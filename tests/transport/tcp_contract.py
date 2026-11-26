"""Verify the separately installed transport DLL, including its raw TCP boundary."""
import ctypes as c
from contextlib import contextmanager
from pathlib import Path
import socket
import struct
import sys
import time

OK, WOULD_BLOCK, PEER_CLOSED, TOO_LARGE, INVALID_CONFIG = range(5)
LISTEN, CONNECT = 1, 2
CONTROL, GAME = 0, 1
OPEN, MESSAGE, CLOSED = 1, 2, 3
LOCAL, REMOTE, PROTOCOL, REFUSED = 1, 2, 5, 6
MAX_CONTROL = 65535
U32, U64, PTR = c.c_uint32, c.c_uint64, c.c_void_p


class Bytes(c.Structure):
    _fields_ = [("data", c.c_char_p), ("size", U32)]


def span(value):
    return Bytes(value, len(value))


class Args(c.Structure):
    _fields_ = [("role", U32), ("endpoint", Bytes), ("config", Bytes)]


class Event(c.Structure):
    _fields_ = [("type", U32), ("lane", U32), ("peer", U64), ("payload", Bytes), ("reason", U32)]


class LaneCaps(c.Structure):
    _fields_ = [("max_payload", U32), ("flags", U32)]


class LaneStats(c.Structure):
    _fields_ = [(name, U64) for name in ("sent", "received", "queued", "peak", "oldest_us")]


Receive = c.CFUNCTYPE(None, PTR, c.POINTER(Event))
Caps = LaneCaps * 2
Stats = LaneStats * 2


class API(c.Structure):
    _fields_ = [
        ("abi", U32), ("size", U32), ("provider_id", Bytes), ("build_id", Bytes),
        ("open", c.CFUNCTYPE(U32, c.POINTER(Args), c.POINTER(PTR))),
        ("send_batch", c.CFUNCTYPE(U32, PTR, U64, U32, c.POINTER(Bytes), U32, c.POINTER(U32))),
        ("poll", c.CFUNCTYPE(U32, PTR, U64, U32, Receive, PTR)),
        ("close_peer", c.CFUNCTYPE(U32, PTR, U64)),
        ("destroy", c.CFUNCTYPE(None, PTR)),
        ("get_caps", c.CFUNCTYPE(U32, PTR, c.POINTER(Caps))),
        ("get_stats", c.CFUNCTYPE(U32, PTR, U64, c.POINTER(Stats))),
    ]


def read_span(value):
    # Read the pointer from the structure. c_char_p field access stops at a NUL.
    pointer = PTR.from_address(c.addressof(value)).value
    return c.string_at(pointer, value.size) if value.size else b""


class Instance:
    def __init__(self, api, role, endpoint, config=b""):
        self.api = api
        self.handle = PTR()
        args = Args(role, span(endpoint.encode("ascii")), span(config))
        result = api.open(c.byref(args), c.byref(self.handle))
        assert result == OK and self.handle, (endpoint, result)
        self.events = []

        @Receive
        def receive(_, pointer):
            event = pointer.contents
            self.events.append((event.type, event.peer, event.lane, read_span(event.payload), event.reason))

        self.receive = receive

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.api.destroy(self.handle)

    def poll(self, budget=16):
        before = len(self.events)
        assert self.api.poll(self.handle, time.monotonic_ns() // 1000, budget, self.receive, None) == OK
        assert len(self.events) - before <= budget

    def send(self, peer, lane, *values):
        accepted = U32(99)
        messages = (Bytes * len(values))(*(span(value) for value in values))
        status = self.api.send_batch(self.handle, peer, lane, messages, len(messages), c.byref(accepted))
        return status, accepted.value

    def close(self, peer):
        return self.api.close_peer(self.handle, peer)

    def stats(self, peer=0):
        out = Stats()
        assert self.api.get_stats(self.handle, peer, c.byref(out)) == OK
        return out


def wait(instances, predicate, budget=16):
    end = time.monotonic() + 5
    while not predicate():
        assert time.monotonic() < end, [entry.events for entry in instances]
        for entry in instances:
            entry.poll(budget)
        time.sleep(0.001)


@contextmanager
def endpoint():
    with socket.socket() as reserve, socket.socket(type=socket.SOCK_DGRAM) as udp:
        reserve.bind(("127.0.0.1", 0))
        address = reserve.getsockname()
        udp.bind(address)
        reserve.close()
        yield address, "{}:{}".format(*address)


def tcp_clients(api):
    config = b"queue_bytes=65538"
    with endpoint() as (_, address), Instance(api, LISTEN, address) as host, \
            Instance(api, CONNECT, address, config) as a:
        caps = Caps()
        assert api.get_caps(a.handle, c.byref(caps)) == OK
        assert [(lane.max_payload, lane.flags) for lane in caps] == [(65535, 1), (1176, 1)]
        wait([host, a], lambda: host.events and a.events, budget=1)
        a_peer = a.events[0][1]
        host_a = host.events[0][1]
        assert host.events[0][0] == a.events[0][0] == OPEN
        with Instance(api, CONNECT, address) as b:
            wait([host, a, b], lambda: len(host.events) == 2 and b.events, budget=1)
            host_b = host.events[1][1]
            assert host.events[1][0] == b.events[0][0] == OPEN and host_a != host_b
            first = b"\x00A" * (MAX_CONTROL // 2) + b"\x80"
            second = b"later\x00frame"
            assert a.send(a_peer, GAME, b"x", b"_" * 1177) == (TOO_LARGE, 0)
            assert a.send(a_peer, CONTROL, first, second) == (WOULD_BLOCK, 1)
            assert a.send(a_peer, CONTROL, second) == (WOULD_BLOCK, 0)
            a.poll(0)
            before = a.stats(a_peer)[CONTROL]
            assert before.queued == MAX_CONTROL and before.peak == MAX_CONTROL and before.oldest_us > 0
            assert host.send(host_b, GAME, b"", b"for b") == (OK, 2)
            wait([host, b], lambda: len(b.events) == 3, budget=1)
            assert [event[3] for event in b.events[1:]] == [b"", b"for b"]
            assert len(host.events) == 2
            wait([host, a], lambda: len(host.events) == 3, budget=1)
            assert host.events[-1] == (MESSAGE, host_a, CONTROL, first, 0)
            assert a.send(a_peer, CONTROL, second) == (OK, 1)
            assert a.close(a_peer) == OK and a.close(a_peer) == PEER_CLOSED
            assert a.send(a_peer, GAME, b"x") == (PEER_CLOSED, 0)
            wait([host, a], lambda: len(host.events) == 5 and len(a.events) == 2, budget=1)
            assert host.events[-2:] == [(MESSAGE, host_a, CONTROL, second, 0),
                                        (CLOSED, host_a, CONTROL, b"", REMOTE)]
            assert a.events[-1] == (CLOSED, a_peer, CONTROL, b"", LOCAL)
            assert host.stats()[CONTROL].received == len(first) + len(second)
            assert a.stats()[CONTROL].sent == len(first) + len(second)
        with Instance(api, CONNECT, address) as replacement:
            wait([host, replacement], lambda: replacement.events and
                 any(event[0] == OPEN and event[1] not in (host_a, host_b) for event in host.events))
            assert replacement.events[0][0] == OPEN
        print("TCP: one host, two guests; no UDP bind; partial acceptance 1/2; retry preserves bytes; reconnect uses fresh peer")


def frame(lane, content):
    return struct.pack("!BH", lane, len(content)) + content


def tcp_poll_burst(api):
    with endpoint() as (address, text), Instance(api, LISTEN, text) as host:
        with socket.create_connection(address, timeout=3) as raw:
            wait([host], lambda: len(host.events) == 1)
            peer = host.events[0][1]
            frames = tuple(bytes([i]) * 8192 for i in range(6))
            assert host.send(peer, CONTROL, *frames) == (OK, len(frames))
            host.poll()
            sent = host.stats(peer)[CONTROL]
            assert sent.sent == 49152 and sent.queued == 0
            expected = b"".join(frame(CONTROL, payload) for payload in frames)
            received = bytearray()
            while len(received) < len(expected):
                packet = raw.recv(len(expected) - len(received))
                assert packet
                received.extend(packet)
            assert received == expected
    print("TCP: one poll drains a 48 KiB send burst with intact frames")


def tcp_mixed_batch(api):
    with endpoint() as (address, text), Instance(api, LISTEN, text) as host:
        with socket.create_connection(address, timeout=3) as raw:
            wait([host], lambda: len(host.events) == 1)
            peer = host.events[0][1]
            expected = bytearray()
            totals = [0, 0]
            for group in range(4):
                games = (b"", *(bytes([group, index, 0, 255]) * (220 + index) for index in range(20)))
                control = (bytes([group, 0, 255]) * 10921,)
                for lane, payloads in ((GAME, games), (CONTROL, control)):
                    assert host.send(peer, lane, *payloads) == (OK, len(payloads))
                    for payload in payloads:
                        totals[lane] += len(payload)
                        expected.extend(frame(lane, payload))

            # The burst crosses the per-poll byte budget within a frame.
            host.poll()
            pending = host.stats(peer)
            assert sum(lane.sent for lane in pending) > 0 and sum(lane.queued for lane in pending) > 0
            assert [lane.sent + lane.queued for lane in pending] == totals
            assert host.close(peer) == OK

            received = bytearray()
            deadline = time.monotonic() + 5
            raw.setblocking(False)
            while len(received) < len(expected) or len(host.events) < 2:
                assert time.monotonic() < deadline
                host.poll(budget=1)
                try:
                    received.extend(raw.recv(65536))
                except BlockingIOError:
                    time.sleep(0.001)

            assert received == expected and host.events[-1] == (CLOSED, peer, CONTROL, b"", LOCAL)
            assert [(lane.sent, lane.queued) for lane in host.stats()] == [(n, 0) for n in totals]
    print("TCP: mixed lanes across send batches and poll budgets, frame bytes and close order verified")


def tcp_raw(api):
    with endpoint() as (address, text), Instance(api, LISTEN, text) as host:
        with socket.create_connection(address, timeout=3) as raw:
            wait([host], lambda: len(host.events) == 1)
            peer = host.events[0][1]
            first = frame(GAME, b"a\x00b")
            for cut in (first[:1], first[1:4]):
                raw.sendall(cut)
                for _ in range(5):
                    host.poll()
                    time.sleep(0.002)
                assert len(host.events) == 1
            raw.sendall(first[4:] + frame(CONTROL, b"two") + frame(GAME, b"three"))
            wait([host], lambda: len(host.events) == 4, budget=1)
            assert host.events[1:] == [(MESSAGE, peer, GAME, b"a\x00b", 0),
                                        (MESSAGE, peer, CONTROL, b"two", 0),
                                        (MESSAGE, peer, GAME, b"three", 0)]
            raw.sendall(struct.pack("!BH", GAME, 1177))
            wait([host], lambda: len(host.events) == 5)
            assert host.events[-1] == (CLOSED, peer, CONTROL, b"", PROTOCOL)
        with socket.create_connection(address, timeout=3) as raw:
            raw.sendall(frame(GAME, b"truncated")[:-2])
            raw.shutdown(socket.SHUT_WR)
            wait([host], lambda: len(host.events) == 7)
            assert host.events[-2][0] == OPEN
            assert host.events[-1] == (CLOSED, host.events[-2][1], CONTROL, b"", PROTOCOL)
        host.poll()
        assert len(host.events) == 7
    with endpoint() as (_, text), Instance(api, CONNECT, text) as missing:
        wait([missing], lambda: len(missing.events) == 1)
        assert missing.events[0][0] == CLOSED and missing.events[0][4] == REFUSED
        missing.poll()
        assert len(missing.events) == 1
    print("TCP: fragmented header/payload, coalesced frames, protocol close, refused connection")


def load_api(path, expected):
    library = c.CDLL(str(path.resolve()))
    get_api = library.netcode_transport_get_api
    get_api.argtypes = [U32, U32, PTR]
    get_api.restype = U32
    api = API()
    assert get_api(1, c.sizeof(api), c.byref(api)) == OK
    assert api.abi == 1 and api.size == c.sizeof(api) and read_span(api.provider_id) == expected
    print("Provider build", read_span(api.build_id).decode("ascii"))
    return library, api


def main(path):
    library, api = load_api(path, b"tcp")
    invalid = Args(CONNECT, span(b"missing-port"), span(b""))
    handle = PTR()
    assert api.open(c.byref(invalid), c.byref(handle)) == INVALID_CONFIG and not handle
    tcp_clients(api)
    tcp_poll_burst(api)
    tcp_mixed_batch(api)
    tcp_raw(api)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
