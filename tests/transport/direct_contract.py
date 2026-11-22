"""Exercise the independently built legacy provider through the same C ABI."""
import ctypes as c
from pathlib import Path
import socket
import sys

from tcp_contract import (CLOSED, CONNECT, CONTROL, GAME, LISTEN, LOCAL, MAX_CONTROL,
                          MESSAGE, OK, OPEN, REMOTE, WOULD_BLOCK, Caps, Instance, load_api, wait)


def main(path):
    library, api = load_api(path, b"direct")
    with socket.socket() as port:
        port.bind(("127.0.0.1", 0))
        address = "{}:{}".format(*port.getsockname())
    with Instance(api, LISTEN, address) as host, Instance(api, CONNECT, address) as guest:
        caps = Caps()
        assert api.get_caps(guest.handle, c.byref(caps)) == OK
        assert [(lane.max_payload, lane.flags) for lane in caps] == [(65535, 1), (1176, 0)]
        wait([host, guest], lambda: host.events and guest.events, budget=1)
        remote, local = host.events[0][1], guest.events[0][1]
        assert host.events[0][0] == guest.events[0][0] == OPEN
        batch = [b"\x00" * (MAX_CONTROL - 1) + bytes([index]) for index in range(4)]
        assert guest.send(local, CONTROL, *batch) == (WOULD_BLOCK, 3)
        assert guest.stats()[CONTROL].queued == MAX_CONTROL * 3
        wait([host, guest], lambda: len(host.events) == 4, budget=1)
        assert [item[3] for item in host.events[1:]] == batch[:3]
        assert guest.send(local, CONTROL, batch[-1]) == (OK, 1)
        assert host.send(remote, GAME, b"hello\x00game") == (OK, 1)
        wait([host, guest], lambda: len(host.events) == 5 and len(guest.events) == 2)
        assert host.events[-1] == (MESSAGE, remote, CONTROL, batch[-1], 0)
        assert guest.events[-1] == (MESSAGE, local, GAME, b"hello\x00game", 0)
        assert guest.stats()[CONTROL].sent == host.stats()[CONTROL].received == MAX_CONTROL * 4
        assert host.stats()[GAME].sent == guest.stats()[GAME].received == 10
        assert guest.close(local) == OK
        wait([host, guest], lambda: len(host.events) == 6 and len(guest.events) == 3)
        assert guest.events[-1] == (CLOSED, local, CONTROL, b"", LOCAL)
        assert host.events[-1] == (CLOSED, remote, CONTROL, b"", REMOTE)
    print("Direct ABI: TCP Control, UDP Game, prefix 3/4, business byte counters and close")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
