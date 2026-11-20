"""Run the car yard's dual-window or four-player acceptance scenario on Windows."""

import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import re
import subprocess
import threading
import time

from artifacts import ROOT, budget, create_job, job_lock, rotate_run
from godot_build import GODOT_PROJECT, resolve_godot


BUILD = ROOT / "build/win-x64/Release/bin/Release"


class Process:
    def __init__(self, output, name, executable, args):
        self.name = name
        self.lines = []
        self.proc = subprocess.Popen(
            [executable, *args], cwd=executable.parent, text=True, encoding="utf-8",
            errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.thread = threading.Thread(target=self.read, args=(output,), daemon=True)
        self.thread.start()

    def read(self, output):
        folder = output / self.name
        folder.mkdir()
        with (folder / "stdout.log").open("w", encoding="utf-8") as log:
            for line in self.proc.stdout:
                if len(self.lines) < 8000 and len(line) <= 4096:
                    self.lines.append(line.strip())
                    log.write(line)
                    log.flush()
                else:
                    log.write("Detailed output reached the line cap. Further output was discarded.\n")
                    for _ in self.proc.stdout:
                        pass
                    break

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        self.proc.wait(timeout=6)
        self.thread.join(timeout=2)
        self.proc.stdout.close()

    def get_line(self, needle):
        return next((line for line in self.lines if needle in line), None)

    def await_line(self, needle, seconds=12):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            line = self.get_line(needle)
            if line is not None:
                return line
            if self.proc.poll() is not None and not self.thread.is_alive():
                break
            time.sleep(0.025)
        raise RuntimeError(f"{self.name} missed {needle!r} (exit={self.proc.poll()}): {self.lines[-5:]}")


def stat_sample(processes):
    pids = ",".join(str(p.proc.pid) for p in processes)
    query = (
        f"Get-Process -Id {pids} -ErrorAction SilentlyContinue | "
        "Select-Object Id,CPU,WorkingSet64,PeakWorkingSet64,PrivateMemorySize64,MainWindowTitle | "
        "ConvertTo-Json -Compress"
    )
    raw = subprocess.check_output(
        ["powershell.exe", "-NoProfile", "-Command", query], text=True, encoding="utf-8",
        errors="replace", creationflags=subprocess.CREATE_NO_WINDOW,
    )
    records = json.loads(raw) if raw.strip() else []
    if isinstance(records, dict):
        records = [records]
    return {row["Id"]: row for row in records}


def values(line):
    return {key: int(value) for key, value in re.findall(r"([a-z_]+)=(\d+)", line)}


def require(condition, explanation):
    if not condition:
        raise RuntimeError(explanation)


def execute(args, output):
    with ExitStack() as stack:
        processes = []
        flags = ["--scenario", args.mode, "--port", str(args.port), "--seconds", str(args.seconds), "--trace"]
        if args.fault:
            flags.append("--fault")

        guest_platform = args.guest_platform or args.platform
        guest_name = "godot-guest" if guest_platform == "godot" else "cocos-guest"

        def launch(name, platform, connection):
            capture = ["--capture", str(output / "yard.png")] if args.capture and name == guest_name else []
            if platform == "godot" and args.godot_export:
                executable, engine_args = args.godot_export, ["--"]
            elif platform == "godot":
                executable, engine_args = resolve_godot(args.godot_exe), ["--path", str(args.godot_project), "--"]
            else:
                executable = args.bin / ("netcode-guest.exe" if platform == "light" else "netcode-yard.exe")
                engine_args = []
            process = Process(output, name, executable, [*engine_args, *connection, *flags, *capture])
            stack.callback(process.close)
            processes.append(process)
            return process

        host = launch("host", args.platform, ["--host"])
        host.await_line("listening")
        guest = launch(guest_name, guest_platform, ["--join", "127.0.0.1"])
        if args.mode == "stress":
            guest.await_line("waiting player=2")
            light_one = launch("light-guest-1", "light", ["--join", "127.0.0.1"])
            light_one.await_line("waiting player=3")
            launch("light-guest-2", "light", ["--join", "127.0.0.1"])
        host.await_line("measurement_begin")
        measured_from = time.monotonic()
        before = stat_sample(processes)
        require(all(p.proc.pid in before for p in processes), "Process died before measurement")
        freeze_line = host.await_line("measurement_frozen", args.seconds + 8)
        after = stat_sample(processes)
        measured_seconds = time.monotonic() - measured_from
        require(len(after) == len(processes), "Process died during measurement")
        for process, platform in ((host, args.platform), (guest, guest_platform)):
            title = "Netcode Godot Car Yard" if platform == "godot" else "Netcode Car Yard"
            require(title in after[process.proc.pid]["MainWindowTitle"], f"Missing window on {process.name}")
            for error in ("failed to link program", "SCRIPT ERROR"):
                require(not process.get_line(error), f"{error} on {process.name}")

        observed = values(freeze_line)
        result = {"scenario": args.mode, "faults": args.fault, "host_platform": args.platform,
                  "guest_platform": guest_platform, "host_interval": observed, "peers": {}}
        digests = set()
        for p in processes:
            summary = values(p.await_line("summary"))
            pid = p.proc.pid
            summary["cpu_seconds"] = round(after[pid]["CPU"] - before[pid]["CPU"], 3)
            summary["cpu_one_core_pct"] = round(100 * summary["cpu_seconds"] / measured_seconds, 2)
            summary["working_set_bytes"] = after[pid]["WorkingSet64"]
            summary["peak_working_set_bytes"] = after[pid]["PeakWorkingSet64"]
            summary["private_bytes"] = after[pid]["PrivateMemorySize64"]
            summary["window"] = after[pid]["MainWindowTitle"]
            result["peers"][p.name] = summary
            digests.add(summary["digest"])
            require(p.proc.wait(timeout=8) == 0, f"{p.name} returned an error")
        require(len(digests) == 1, "Final object/field digest mismatch")
        require(len({r["player"] for r in result["peers"].values()}) == len(processes), "Player identity collision")
        guest.await_line("visual id=")
        if args.capture:
            guest.await_line("render_capture_ok")
            require((output / "yard.png").is_file(), "Missing rendered image")
        if args.mode == "stress":
            host.await_line("door_open player=2")
            for p in processes:
                p.await_line("The yard gate is open")
            require(all(row["objects"] == 100 for row in result["peers"].values()), "Expected 100 objects per peer")
            require(all(row["udp_rx"] and row["udp_tx"] for row in result["peers"].values()),
                    "Every process must use the UDP channel")
            if args.fault:
                require(all(any(row[key] for row in result["peers"].values())
                            for key in ("udp_drop", "udp_dup", "udp_reorder")),
                        "Fault test must deliver drops, duplicates and reordering")
        else:
            host.await_line("input player=2")
            host.await_line("net_input_x=0.750000")
            guest.await_line("net_input_x=0.750000")
            guest.await_line("public_rpc rejected")
            host.await_line("door_open player=2")
            guest.await_line("The gate is already open")
            barrier = values(host.await_line("barrier_removed object="))["object"]
            guest.await_line(f"replica_removed object={barrier}")
            if guest_platform == "godot":
                guest.await_line(f"visual_created object={barrier}")
                guest.await_line(f"visual_removed object={barrier}")
            host.await_line("peer_lost player=2")
            guest.await_line("recovered player=2")
            require(result["peers"][guest_name]["recovery_ms"] > 0, "Missing reconnect measurement")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("play", "stress"))
    parser.add_argument("--bin", type=Path, default=BUILD)
    parser.add_argument("--port", type=int, default=43220)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--fault", action="store_true")
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--platform", choices=("cocos2dx", "godot"), default="cocos2dx")
    parser.add_argument("--guest-platform", choices=("cocos2dx", "godot"))
    parser.add_argument("--godot-exe", type=Path)
    parser.add_argument("--godot-project", type=Path, default=GODOT_PROJECT)
    parser.add_argument("--godot-export", type=Path)
    args = parser.parse_args()
    args.bin = args.bin.resolve()
    args.godot_project = args.godot_project.resolve()
    if args.godot_export:
        args.godot_export = args.godot_export.resolve()
    if not budget(ROOT, 256 * 1024 ** 2)["allowed"]:
        print("Output budget exceeded")
        return 1
    output = create_job(ROOT, "runs/current", "run")
    ok = False
    with job_lock(output, "run"):
        try:
            metrics = execute(args, output)
            (output / "metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
            print(json.dumps(metrics, indent=2))
            ok = True
        except Exception as error:
            print(error)
            (output / "failure.txt").write_text(str(error), encoding="utf-8")
    print(f"run evidence {rotate_run(ROOT, ok)}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
