"""Exercise the same room state machine over separate TCP/UDP processes."""

import argparse
from contextlib import ExitStack
import queue
import subprocess
import threading


def expect_line(lines, expected):
    while True:
        line = lines.get(timeout=6)
        if line is None:
            raise RuntimeError(f"Process exited before {expected!r}")
        if expected in line:
            return


def run(executable, port, mode, guest_executable=None):
    with ExitStack() as stack:
        def launch(role, expectation):
            binary = guest_executable if role == "guest" and guest_executable else executable
            process = stack.enter_context(subprocess.Popen(
                [binary, role, str(port), expectation], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            ))

            def stop():
                if process.poll() is None:
                    process.terminate()

            stack.callback(stop)
            lines = queue.Queue()

            def read():
                for line in process.stdout:
                    print(f"{role}/{expectation} {line}", end="", flush=True)
                    lines.put(line)
                lines.put(None)

            threading.Thread(target=read, daemon=True).start()
            return process, lines

        host, host_lines = launch("host", mode)
        expect_line(host_lines, "listening")
        if mode == "rounds":
            wrong, _ = launch("guest", "wrong")
            assert wrong.wait(timeout=6) == 0
        guest, _ = launch("guest", mode)
        if mode == "rounds":
            expect_line(host_lines, "round=1")
            locked, _ = launch("guest", "locked")
            assert locked.wait(timeout=6) == 0
        assert guest.wait(timeout=12) == 0
        assert host.wait(timeout=6) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe")
    parser.add_argument("--guest-exe")
    parser.add_argument("--port", type=int, default=43216)
    args = parser.parse_args()
    run(args.exe, args.port, "rounds", args.guest_exe)
    run(args.exe, args.port + 1, "timeout", args.guest_exe)
