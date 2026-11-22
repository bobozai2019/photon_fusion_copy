"""Exercise the external message-mode provider built from a copied source directory."""
from pathlib import Path
import sys
from uuid import uuid4

from tcp_contract import (CLOSED, CONNECT, CONTROL, GAME, LISTEN, LOCAL, MAX_CONTROL,
                          MESSAGE, OK, OPEN, PEER_CLOSED, REFUSED, REMOTE, WOULD_BLOCK,
                          Instance, load_api, wait)


def main(path):
    library, api = load_api(path, b"example-pipe")
    name = f"contract-{uuid4().hex}"
    with Instance(api, LISTEN, name) as host:
        with Instance(api, CONNECT, name) as guest:
            wait([host, guest], lambda: host.events and guest.events, budget=1)
            remote, local = host.events[0][1], guest.events[0][1]
            assert host.events[0][0] == guest.events[0][0] == OPEN
            data = [b"\x00" * (MAX_CONTROL - 1) + bytes([index]) for index in range(17)]
            assert guest.send(local, CONTROL, *data) == (WOULD_BLOCK, 16)
            assert guest.send(local, CONTROL, data[-1]) == (WOULD_BLOCK, 0)
            assert guest.stats(local)[CONTROL].queued == 16 * MAX_CONTROL
            guest.poll()
            assert guest.stats(local)[CONTROL].queued > 0
            wait([guest, host], lambda: len(host.events) == 17, budget=4)
            assert [event[3] for event in host.events[1:]] == data[:16]
            assert guest.send(local, CONTROL, data[-1]) == (OK, 1)
            assert host.send(remote, GAME, b"echo\x00", b"") == (OK, 2)
            wait([host, guest], lambda: len(guest.events) == 3)
            assert guest.events[-2:] == [(MESSAGE, local, GAME, b"echo\x00", 0), (MESSAGE, local, GAME, b"", 0)]
            assert guest.close(local) == OK and guest.close(local) == PEER_CLOSED
            wait([guest, host], lambda: len(host.events) == 19 and len(guest.events) == 4)
            assert host.events[-2:] == [(MESSAGE, remote, CONTROL, data[-1], 0),
                                        (CLOSED, remote, CONTROL, b"", REMOTE)]
            assert guest.events[-1] == (CLOSED, local, CONTROL, b"", LOCAL)
            assert host.stats()[CONTROL].received == guest.stats()[CONTROL].sent == 17 * MAX_CONTROL
        with Instance(api, CONNECT, name) as replacement:
            wait([host, replacement], lambda: len(host.events) == 20 and replacement.events)
            assert host.events[-1][0] == OPEN and host.events[-1][1] != remote
    with Instance(api, CONNECT, name) as missing:
        wait([missing], lambda: missing.events)
        assert missing.events[0][0] == CLOSED and missing.events[0][4] == REFUSED
    print("External pipes: 16/17 accepted; real slow-reader backpressure; ordered binary messages and drain; new peer on reopen")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
