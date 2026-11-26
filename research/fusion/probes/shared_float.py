"""Observe one declaratively replicated float in two Fusion Godot processes."""

import argparse
from contextlib import ExitStack
from pathlib import Path
import sys
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
from artifacts import ROOT, budget, create_job, job_lock, remove_owned

sys.path.insert(0, str(REPO.parent / "examples" / "tools"))
from verify_yard import Process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", required=True, type=Path)
    parser.add_argument("--racing", type=Path, default=REPO.parent / "references/photon/photon-fusion-godot-starter-kit-racing_3.0.0")
    args = parser.parse_args()
    if not budget(ROOT, 32 * 1024 ** 2)["allowed"]:
        return 1
    work = create_job(ROOT, "tmp/fusion-shared-float", "temp")
    result = 1
    try:
        with job_lock(work, "temp"), ExitStack() as stack:
            room = "nc-" + uuid.uuid4().hex[:12]
            trigger = work / "send-markers"
            flags = ["--headless", "--path", str(args.racing.resolve()), "--script",
                     str(Path(__file__).with_suffix(".gd").resolve()), "--"]

            def start(name):
                process = Process(work, name, args.godot.resolve(), [*flags, name, room, str(trigger)])
                stack.callback(process.close)
                return process

            host = start("A")
            print(host.await_line("PROBE_JOINED", 14))
            guest = start("B")
            print(guest.await_line("PROBE_JOINED", 14))
            trigger.touch()
            for marker in ("0.75", "-0.5"):
                print(host.await_line(f"PROBE_WRITE marker={marker}", 8))
                print(guest.await_line(f"PROBE_READ marker={marker}", 8))
            result = 0 if host.proc.wait(timeout=8) == guest.proc.wait(timeout=8) == 0 else 1
    except Exception as error:
        print(error)
        for name in ("A", "B"):
            log = work / name / "stdout.log"
            if log.is_file():
                for line in log.read_text(encoding="utf-8").splitlines():
                    if line.startswith(("PROBE_", "SCRIPT ERROR", "Parse Error")):
                        print(line)
    finally:
        print(f"remove {work} (finished Fusion probe)", flush=True)
        remove_owned(ROOT, work, "temp")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
