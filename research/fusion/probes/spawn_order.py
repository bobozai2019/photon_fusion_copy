"""Trace the first replicated field of a guest spawn on Fusion Preview 555 for Windows."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import shutil
import subprocess
import sys
from time import monotonic, sleep
from uuid import uuid4

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "tools"))
from artifacts import ROOT, create_job, job_lock, remove_owned

EDITOR_DLL = "libfusion.windows.editor.x86_64.release.dll"
PREVIEW_555 = "104b1e2c3a0289b7391584c75a90f1480a9dab750099114c99356a813ce69752"


def prepare(project, lab, mode, region):
    source = lab / "addons/fusion"
    library = source / "bin" / EDITOR_DLL
    if sha256(library.read_bytes()).hexdigest() != PREVIEW_555:
        raise ValueError("Expected Fusion Preview 555 Windows editor DLL")
    target = project / "addons/fusion"
    (target / "bin").mkdir(parents=True)
    shutil.copy2(library, target / "bin")
    shutil.copy2(source / "fusion.gdextension", target)
    (project / ".godot").mkdir()
    (project / ".godot/extension_list.cfg").write_text("res://addons/fusion/fusion.gdextension\n")
    app = next((line for name in ("override.cfg", "project.godot") if (lab / name).is_file()
                for line in (lab / name).read_text(encoding="utf-8").splitlines()
                if line.startswith("connection/app_id=")), None)
    if app is None:
        raise ValueError("Set a test App ID in the sync-lab project or its local override")
    (project / "project.godot").write_text(
        f'config_version=5\n[fusion]\n{app}\nconnection/default_region="{region}"\n'
        f'connection/mode={int(mode == "local")}\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n',
        encoding="utf-8")
    shutil.copy2(HERE / "spawn_order.gd", project / "main.gd")
    shutil.copy2(HERE / "spawn_order_orb.gd", project / "orb.gd")
    (project / "orb.tscn").write_text(
        '[gd_scene format=3]\n[ext_resource type="Script" path="res://orb.gd" id="Orb"]\n'
        '[sub_resource type="FusionReplicationConfig" id="Config"]\n'
        'properties/0/path = NodePath(".:value")\n'
        '[node name="Orb" type="Node2D"]\nscript = ExtResource("Orb")\n'
        '[node name="Sync" type="FusionSharedReplicator" parent="."]\n'
        'owner_mode = 1\nroot_replication_mode = 0\nreplication_config = SubResource("Config")\n')


def trial(project, godot, mode, after_return):
    command = [str(godot), "--headless", "--path", str(project), "--script", "res://main.gd"]
    subprocess.run([*command, "--", "defaults"], timeout=15, check=True)
    if mode == "defaults":
        return 0
    room = "spawn-order-" + uuid4().hex[:10]
    ready = project / "host.ready"
    processes = {}
    logs = {}
    try:
        for role in ("host", "guest"):
            logs[role] = (project / (role + ".log")).open("wb")
            extra = ["--after-return"] if after_return else []
            processes[role] = subprocess.Popen([*command, "--", role, room, str(ready), *extra],
                                               stdout=logs[role], stderr=subprocess.STDOUT)
            if role == "host":
                deadline = monotonic() + 30
                while not ready.exists() and processes[role].poll() is None and monotonic() < deadline:
                    sleep(0.1)
                if not ready.exists():
                    break
        for process in processes.values():
            process.wait(timeout=30)
        if len(processes) != 2 or any(p.returncode != 0 for p in processes.values()):
            return 1
        check_trace(project, after_return)
        return 0
    finally:
        for role, process in processes.items():
            if process.poll() is None:
                process.kill()
                process.wait()
            logs[role].close()
            print(role, "exit", process.returncode, flush=True)
            trace = (project / (role + ".log")).read_text(encoding="utf-8", errors="replace")
            print("\n".join(line for line in trace.splitlines() if line.startswith(
                ("SPAWN_ORDER ", "SCRIPT ERROR", "ERROR:", "Godot Engine", "CrashHandler"))), flush=True)


def check_trace(project, after_return):
    expected = {
        "guest": [
            ("pre_spawn_arguments", "FusionSharedReplicator,Node2D"),
            ("pre_spawn", "-1"), ("setter", "731"), ("enter_tree", "731"), ("ready", "731"),
            ("replicator_spawned", "731"), ("spawner_spawned", "731"),
            ("spawn_return", "731"), ("authority_changed", "731"),
        ],
        "host": [
            ("enter_tree", "-1"), ("ready", "-1"), ("setter", "731"),
            ("replicator_spawned", "731"), ("spawner_spawned", "731"),
        ],
    }
    if after_return:
        expected["guest"][-1:] = [("setter", "741"), ("authority_changed", "741")]
    for side, events in expected.items():
        lines = (project / (side + ".log")).read_text(encoding="utf-8", errors="replace").splitlines()
        trace = [json.loads(line.removeprefix("SPAWN_ORDER ")) for line in lines if line.startswith("SPAWN_ORDER ")]
        names = {event for event, _ in events}
        observed = [row for row in trace if row["event"] in names]
        pairs = [(row["event"], row["value"]) for row in observed]
        accepted = [events]
        if after_return and side == "host":
            accepted = [events + [("setter", "741")],
                        [(event, "741" if value == "731" else value) for event, value in events]]
        if pairs not in accepted:
            raise ValueError(f"{side} spawn sequence differs from {events}")
        if any(row["authority"] != (side == "guest") for row in observed if row["event"] != "pre_spawn_arguments"):
            raise ValueError(f"{side} observed unexpected authority")


def main():
    lab = ROOT.parent.parent / "examples/sync-lab"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("defaults", "local", "cloud"))
    parser.add_argument("--region", default="asia")
    parser.add_argument("--after-return", action="store_true", help="Write a second value immediately after spawn returns")
    parser.add_argument("--godot", type=Path,
                        default=lab / "runtime/godot-4.6.1/Godot_v4.6.1-stable_win64_console.exe")
    args = parser.parse_args()
    work = create_job(ROOT, Path("tmp/fusion-spawn-order"), "probe")
    try:
        with job_lock(work, "probe"):
            prepare(work, lab, args.mode, args.region)
            return trial(work, args.godot, args.mode, args.after_return)
    finally:
        remove_owned(ROOT, work, "probe")


if __name__ == "__main__":
    raise SystemExit(main())
