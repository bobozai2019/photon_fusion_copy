"""Export original editor schemas in a disposable fixture, or test Preview 684 single-player."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools"))
from artifacts import ROOT, budget, create_job, job_lock, remove_owned
from native_index import DLL_HASH

HASH_684 = "325537b53a183eef35a995bf940f3bffa885764411555cfd1348976199c77d3e"
ACTOR = """extends Node3D
var reads := 0
@export var score: float = 0.5:
    get:
        reads += 1
        return score
@export var marks: Array[float] = [1.0]
@export var label: String = "ok"
"""
SCENE = """[gd_scene load_steps=3 format=3]
[ext_resource type="Script" path="res://actor.gd" id="1"]
[sub_resource type="FusionReplicationConfig" id="Cfg"]
properties/0/path = NodePath(".:score")
properties/0/interface_tag = "IScore"
properties/1/path = NodePath(".:label")
properties/2/path = NodePath(".:marks")
properties/2/max_array_capacity = 4
properties/2/element_type = 3
[node name="{name}" type="Node3D"]
script = ExtResource("1")
[node name="Sync" type="FusionSharedReplicator" parent="."]
root_replication_mode = {mode}
replication_config = SubResource("Cfg")
"""


def prepare(work, args):
    dll = "addons/fusion/bin/libfusion.windows.editor.x86_64.release.dll"
    for member in ("addons/fusion/fusion.gdextension", dll):
        dest = work / member
        dest.parent.mkdir(parents=True, exist_ok=True)
        if args.preview_684:
            dest.write_bytes(subprocess.check_output([str(args.sevenzip), "x", "-so", str(args.preview_684),
                                                      member.replace("/", "\\")]))
        else:
            shutil.copyfile(args.racing / member, dest)
    if hashlib.sha256((work / dll).read_bytes()).hexdigest() != (HASH_684 if args.preview_684 else DLL_HASH):
        raise ValueError("Unexpected editor extension fingerprint")
    (work / ".godot").mkdir()
    (work / ".godot/extension_list.cfg").write_text("res://addons/fusion/fusion.gdextension\n", encoding="utf-8")
    (work / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Fusion fixture"\n'
                                        '[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
    (work / "actor.gd").write_text(ACTOR, encoding="utf-8")
    if args.preview_684:
        workload = ACTOR.replace("score: float = 0.5", "score: int = 0").replace("Array[float] = [1.0]", "Array[int] = []")
        (work / "workload.gd").write_text(workload, encoding="utf-8")
        scene = SCENE.format(name="Workload", mode=0).replace("actor.gd", "workload.gd").replace(
            "max_array_capacity = 4", "max_array_capacity = 64").replace("element_type = 3", "element_type = 2")
        (work / "workload.tscn").write_text(scene, encoding="utf-8")
    for file, name, mode in (("actor", "Actor", 0), ("auto", "AutoActor", 1)):
        (work / (file + ".tscn")).write_text(SCENE.format(name=name, mode=mode), encoding="utf-8")


def run_godot(work, godot, mode):
    command = [str(godot), "--headless"]
    if mode == "editor":
        command.append("--editor")
    proc = subprocess.run([*command, "--path", str(work), "--script", str(HERE / "editor_fixture.gd"), "--", mode],
                          capture_output=True, encoding="utf-8", errors="replace", timeout=45)
    lines = (proc.stdout + proc.stderr).splitlines()
    for line in lines:
        if line.startswith(("FIXTURE", "SCRIPT ERROR", "ERROR: [Fusion Godot]", "[Fusion Godot] Export")):
            print(line)
    if proc.returncode or "FIXTURE_FINISH 0" not in lines:
        raise RuntimeError(f"{mode}: exit {proc.returncode}, missing successful assertions")
    return lines


def check_json(work):
    schema = json.loads((work / "FusionCustomPlugin.Generated.g.json").read_text(encoding="utf-8-sig"))
    observed = {obj["name"]: (obj["words"], [(f["offset"], f["type"]) for f in obj["fields"]])
                for obj in schema["objects"]}
    expected = {"Actor": (8, [(0, "float32"), (1, "string"), (3, "[4]float32")]),
                "Auto": (15, [(0, "vec3<float32>"), (3, "vec3<float32>"), (6, "float32"),
                              (7, "string"), (9, "[4]float32"), (14, "float32")])}
    if observed != expected or {i["name"] for i in schema["interfaces"]} != {"IScore", "transform3d"}:
        raise ValueError(f"Unexpected original schema: {observed}")
    print("JSON, two interfaces and per-field offsets verified:", observed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", required=True, type=Path)
    parser.add_argument("--racing", type=Path, default=REPO.parent / "references/photon/photon-fusion-godot-starter-kit-racing_3.0.0")
    parser.add_argument("--preview-684", type=Path, help="Test the later single-player build separately")
    parser.add_argument("--sevenzip", type=Path, default=Path("C:/Program Files/7-Zip/7z.exe"))
    args = parser.parse_args()
    if not budget(ROOT, 32 * 1024 ** 2)["allowed"]:
        return 1
    work = create_job(ROOT, "tmp/fusion-fixture-" + ("684" if args.preview_684 else "555"), "temp")
    try:
        with job_lock(work, "temp"):
            prepare(work, args)
            if args.preview_684:
                run_godot(work, args.godot, "single")
            else:
                run_godot(work, args.godot, "layout")
                lines = run_godot(work, args.godot, "editor")
                check_json(work)
                if not any("CLI tool not found" in line for line in lines):
                    raise ValueError("Expected the packaged editor to report its absent C# schema compiler")
        return 0
    finally:
        print(f"remove {work} (finished original editor probe)")
        remove_owned(ROOT, work, "temp")


if __name__ == "__main__":
    raise SystemExit(main())

