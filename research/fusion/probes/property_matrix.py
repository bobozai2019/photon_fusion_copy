"""Run release and Fusion property sweeps in serial and retain every attempt."""

import argparse
from contextlib import ExitStack
from functools import partial
from hashlib import sha256
import json
import os
from pathlib import Path
from types import SimpleNamespace
from uuid import uuid4

from property_scale import ObservationFailed, fixture, summarize, trial
from rigid_scale import ADDON, open_trials
from artifacts import ROOT, budget, create_job, job_lock, rotate_run


def matrix(args, stream):
    builds = {}
    for name in ("old", "candidate"):
        archive = getattr(args, name)
        if archive:
            builds[name] = ("native", archive, sha256(archive.read_bytes()).hexdigest())
    if args.fusion:
        builds["fusion"] = ("fusion", None, "Preview 555")
    names = list(builds)
    attempt = 0
    passed = True
    for width in args.widths:
        cells = [(count, workload) for count in (args.counts if width == 20 else [250 * width])
                 for workload in args.workloads]
        with ExitStack() as stack:
            runners = {}
            for name, (backend, archive, _) in builds.items():
                config = SimpleNamespace(backend=backend, sdk_zip=archive, addon=args.addon, browser=args.browser)
                runners[name] = stack.enter_context(open_trials(
                    config, partial(fixture, width=width), "property-matrix"))
            for repeat in range(args.rooms):
                shift = repeat % len(names)
                for count, workload in cells:
                    for name in names[shift:] + names[:shift]:
                        attempt += 1
                        record = {"attempt": attempt, "room_id": uuid4().hex, "repeat": repeat + 1, "product": name,
                                  "build": builds[name][2], "width": width, "properties": count, "workload": workload}
                        try:
                            writer, observer = runners[name](attempt, count, workload, controller=trial)
                            record["metrics"] = summarize(count, workload, writer, observer, width)
                            passed &= record["metrics"]["correct"]
                        except Exception as exc:
                            error = str(exc)
                            app = os.environ.get("FUSION_PROBE_APP_ID")
                            record["error"] = (error.replace(app, "[app]") if app else error)[-2000:]
                            if isinstance(exc, ObservationFailed):
                                record["failure_stage"] = "observer"
                            passed = False
                        line = json.dumps(record)
                        stream.write(line + "\n")
                        stream.flush()
                        print("ATTEMPT", line, flush=True)
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--fusion", action="store_true")
    parser.add_argument("--rooms", type=int, default=5)
    parser.add_argument("--widths", type=int, nargs="+", default=[20, 4, 12], choices=[4, 12, 20])
    parser.add_argument("--counts", type=int, nargs="+", default=[1000, 2000, 3000, 4000, 5000],
                        choices=[1000, 2000, 3000, 4000, 5000])
    parser.add_argument("--workloads", nargs="+", default=["idle", "sparse", "dense"],
                        choices=["idle", "sparse", "dense"])
    parser.add_argument("--addon", type=Path, default=ADDON)
    parser.add_argument("--browser", type=Path, default=Path("C:/Program Files/Google/Chrome/Application/chrome.exe"))
    args = parser.parse_args()
    if args.rooms < 1 or not (args.old or args.candidate or args.fusion):
        parser.error("Select at least one product and a positive room count")
    if args.fusion and not os.environ.get("FUSION_PROBE_APP_ID", "").strip():
        parser.error("Set FUSION_PROBE_APP_ID in the process environment")
    if not budget(ROOT, (len(args.widths) * 3 * 180 + 256) * 1024**2)["allowed"]:
        raise RuntimeError("Output budget exceeded")
    run = create_job(ROOT, "runs/current", "run")
    passed = False
    try:
        with job_lock(run, "run"), (run / "property-matrix.jsonl").open("w", encoding="utf-8") as stream:
            passed = matrix(args, stream)
    finally:
        print(rotate_run(ROOT, passed), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
