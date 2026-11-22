"""Compare normalized Direct payloads and cross-version room sessions from installed Core archives."""
import argparse
from contextlib import ExitStack
from pathlib import Path
import shutil
import subprocess
import time
from zipfile import ZipFile

from verify_players import CREATE_NO_WINDOW, digest, require_line, stop, tcp_endpoint


def install_consumer(cmake, workspace, archive):
    workspace.mkdir()
    checksum = digest(archive)
    assert [checksum, archive.name] in [line.split() for line in archive.with_name("SHA256SUMS.txt").read_text().splitlines()]
    sdk = workspace / "relocated SDK"
    with ZipFile(archive) as package:
        package.extractall(sdk)
    src = workspace / "consumer"
    shutil.copytree(Path(__file__).with_name("direct_consumer"), src)
    build = workspace / "build"
    subprocess.run([cmake, "-S", src, "-B", build, "-G", "Visual Studio 18 2026", "-A", "x64",
                    "-DCMAKE_CONFIGURATION_TYPES=Release", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                    f"-DNetcodeCore_DIR={sdk / 'lib/cmake/NetcodeCore'}"], check=True)
    subprocess.run([cmake, "--build", build, "--config", "Release"], check=True)
    binary = build / "Release/compat_probe.exe"
    print(f"COMPAT_INPUT archive={checksum} binary={digest(binary)}", flush=True)
    return binary


def run(work, host_binary, guest_binary):
    work.mkdir()
    port = tcp_endpoint().rsplit(":", 1)[1]
    ticket = work / "seat.bin"
    logs = {}
    with ExitStack() as stack:
        def launch(bin_path, role):
            log = logs[role] = work / f"{role}.log"
            stream = stack.enter_context(log.open("wb"))
            process = subprocess.Popen([bin_path, role, port, ticket], cwd=work, stdout=stream,
                                       stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
            stack.callback(stop, process)
            return process, log

        host, log = launch(host_binary, "host")
        require_line(host, log, "LISTEN host")
        for index, role in enumerate(("join", "resume"), 1):
            guest, guest_log = launch(guest_binary, role)
            require_line(guest, guest_log, f"GUEST_OK phase={index} player=2 value=42")
            require_line(host, log, f"HOST_OK phase={index} value=42")
            assert guest.wait(timeout=6) == 0
            if index == 1:
                time.sleep(0.2)
        assert host.wait(timeout=6) == 0
    ticket.unlink()
    traces = {name: [line for line in log.read_text().splitlines() if line.startswith("TRACE ")]
              for name, log in logs.items()}
    assert any(line.startswith("TRACE Control ") for line in traces["host"])
    assert any(line.startswith("TRACE Game 02") for line in traces["host"])
    assert any(line.startswith("TRACE Game 05") for line in traces["host"])
    assert all(traces.values())
    return traces


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--old", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    old = install_consumer(args.cmake, work / "old", args.old)
    candidate = install_consumer(args.cmake, work / "candidate", args.candidate)
    reference = run(work / "old-old", old, old)
    for role, host, guest in (("old-new", old, candidate), ("new-old", candidate, old), ("new-new", candidate, candidate)):
        actual = run(work / role, host, guest)
        if reference != actual:
            raise AssertionError(f"{role} payloads differ\nreference={reference!r}\nactual={actual!r}")
        print("DIRECT_COMPAT", role, {peer: len(trace) for peer, trace in actual.items()}, flush=True)


if __name__ == "__main__":
    main()
