"""Measure declared Godot integer properties through two real clients."""

import argparse
from functools import partial
from hashlib import sha256
import math
import os
from pathlib import Path

from rigid_scale import ADDON, HERE, run_trials


WAVES = 60


class ObservationFailed(RuntimeError):
    """The writer finished but the observing process failed to report."""


def actor_script(names):
    lines = ['extends "res://property_actor.gd"', f"const FIELD_COUNT := {len(names)}", ""]
    for index, name in enumerate(names):
        lines.extend([
            f"@export var {name}: int = 0:",
            "    get:",
            "        reads += 1",
            f"        return {name}",
            "    set(value):",
            "        sets += 1",
            f"        if {name} != value: applied({index}, value)",
            f"        {name} = value",
            "",
        ])
    lines.extend(["func write_wave(wave: int) -> int:",
                  "    var first_id := slot * FIELD_COUNT + 1", "    var written := 0"])
    for i, name in enumerate(names):
        lines.extend([f"    if wave == 0 or not sparse or (wave - 1) % 10 == (first_id - 1 + {i}) % 10:",
                      f"        {name} = encode(first_id + {i}, wave)", "        written += 1"])
    lines.append("    return written")
    lines.extend(["", "func correct_final(baseline: bool = false) -> int:", "    var correct := 0"])
    for i, name in enumerate(names):
        lines.append(f"    if {name} == encode(slot * FIELD_COUNT + {i + 1}, 0 if baseline else final_wave({i})): correct += 1")
    lines.append("    return correct")
    return "\n".join(lines) + "\n"


def fixture(web, width, omit_wave=0):
    names = [f"field_{i:02d}" for i in range(width)]
    node_script = '[ext_resource type="Script" path="res://actor.gd" id="Actor"]'
    root = '[node name="PropertyObject" type="Node3D"]\nscript = ExtResource("Actor")'
    if web:
        resources = ['[sub_resource type="FusionReplicationConfig" id="Config"]']
        resources.extend(f'properties/{i}/path = NodePath(".:{name}")' for i, name in enumerate(names))
        sync = ['[node name="Sync" type="FusionSharedReplicator" parent="."]', "owner_mode = 1",
                "root_replication_mode = 0", 'replication_config = SubResource("Config")']
    else:
        resources = ['[ext_resource type="Script" path="res://addons/netcode/scene_replicator.gd" id="Replica"]']
        paths = ", ".join(f'NodePath(":{name}")' for name in names)
        sync = ['[node name="Sync" type="Node" parent="."]', 'script = ExtResource("Replica")',
                "root_mode = 0", "player_attached = true", f"properties = Array[NodePath]([{paths}])",
                f"client_writable = Array[NodePath]([{paths}])"]
    platform = "fusion" if web else "native"
    return {
        "property_object.tscn": "\n".join(["[gd_scene format=3]", node_script, *resources, root, *sync, ""]),
        "main.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://main.gd" id="Main"]\n'
                     '[node name="Main" type="Node"]\nscript = ExtResource("Main")\n',
        "actor.gd": actor_script(names),
        "property_actor.gd": (HERE / "property_scale_actor.gd").read_text(encoding="utf-8"),
        "measure.gd": (HERE / "property_scale.gd").read_text(encoding="utf-8").replace(
            "const OMIT_WAVE := 0", f"const OMIT_WAVE := {omit_wave}"),
        "main.gd": (HERE / f"property_scale_{platform}.gd").read_text(encoding="utf-8"),
    }


def p95(values):
    ordered = sorted(values)
    return ordered[math.ceil(len(ordered) * .95) - 1] if ordered else None


def summarize(count, workload, writer, observer, width):
    start, stop = writer["start_us"], writer["stopped_us"]
    duration = (stop - start) / 1e6
    if duration <= 0 or observer["start_us"] > start or observer["ticks_us"][-1] < stop:
        raise ValueError("Both clients must cover the writer's entire measurement window")
    result = {"objects": count // width, "properties": count, "workload": workload,
              "target_tick_hz": 60, "source_seconds": duration,
              "owner_correct": writer["correct"], "remote_correct": observer["correct"],
              "field_errors": observer["errors"], "owner_sets": writer["sets"],
              "remote_sets": observer["sets"], "owner_gets": writer["reads"], "remote_gets": observer["reads"]}
    for label, role in (("owner", writer), ("remote", observer)):
        ticks = [tick for tick in role["ticks_us"] if start <= tick <= stop]
        result[f"{label}_baseline"] = role["baseline"]
        result[f"{label}_ticks"] = len(ticks)
        result[f"{label}_tick_hz"] = len(ticks) / duration
        result[f"{label}_tick_gap_p95_ms"] = p95([(b-a)/1000 for a,b in zip(ticks,ticks[1:])])
    result["correct"] = (writer["baseline"] == observer["baseline"] == writer["correct"]
                         == observer["correct"] == observer["checked"] == count and observer["errors"] == 0)
    if workload == "idle":
        if writer["written"] != 0:
            raise ValueError("Idle workload assigned source values")
        result["correct"] &= writer["sets"] == observer["sets"] == 0
        return result
    expected_wave = count if workload == "dense" else count // 10
    expected = WAVES * expected_wave
    counts = observer["received"][1:]
    observed = sum(counts)
    in_window = max((total for ms, total in observer["history"] if (ms+1)*1000 <= stop), default=0)
    # Report the count in the partial millisecond separately from the conservative count.
    through_end_bucket = max((total for ms, total in observer["history"] if ms <= stop//1000), default=0)
    waves = [t for t in writer["wave_us"][1:] if t]
    if len(waves) != WAVES or writer["written"] != expected:
        raise ValueError(f"Invalid source workload: {len(waves)}/{WAVES} waves, {writer['written']}/{expected} writes")
    first_delays = [(first-t)/1000 for first,t in zip(observer["first_us"][1:], waves) if first]
    result["correct"] &= bool(observer["settled_us"]) and max(counts) <= expected_wave
    result.update({
        "waves": len(waves), "written_changes": writer["written"], "observed_by_stop": in_window,
        "versions_in_stop_bucket": through_end_bucket - in_window, "observed_including_drain": observed,
        "source_wave_gap_p95_ms": p95([(b-a)/1000 for a,b in zip(waves,waves[1:])]),
        "observed_per_second_while_writing": in_window/duration, "waves_seen": len(first_delays),
        "visible_version_fraction": in_window/expected,
        "waves_fully_seen": counts.count(expected_wave), "first_field_p95_ms": p95(first_delays),
        "after_stop_to_all_ms": max(0, observer["settled_us"]-stop)/1000 if observer["settled_us"] else None,
        "remote_setter_amplification": observer["sets"]/observed if observed else None, "wave_counts": counts,
    })
    return result


def prepare_window(peers, wait, command):
    command("A", "spawn")
    count = wait("A", "prepared")
    if wait("B", "prepared") != count:
        raise ValueError("Different baseline sizes")
    return count


def start_window(wait, command):
    command("B", "go_observer")
    wait("B", "armed")
    command("A", "go")


def finish_window(wait, command):
    writer = wait("A", "writer")
    command("B", "writer_done")
    try:
        observer = wait("B", "observer")
    except (TimeoutError, RuntimeError, ValueError) as error:
        raise ObservationFailed(str(error)) from error
    return writer, observer


def trial(peers, wait, command, workload):
    prepare_window(peers, wait, command)
    start_window(wait, command)
    return finish_window(wait, command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("native","fusion"))
    parser.add_argument("counts", type=int, nargs="+")
    parser.add_argument("--workload", choices=("dense","sparse","idle"), default="dense")
    parser.add_argument("--fields-per-object", type=int, default=20)
    parser.add_argument("--addon", type=Path, default=ADDON)
    parser.add_argument("--sdk-zip", type=Path, help="Install and fingerprint this native addon archive")
    parser.add_argument("--omit-wave", type=int, default=0, choices=range(WAVES), metavar="WAVE",
                        help="Deliberately omit a non-final wave to exercise input validation")
    parser.add_argument("--browser", type=Path, default=Path("C:/Program Files/Google/Chrome/Application/chrome.exe"))
    args = parser.parse_args()
    width = args.fields_per_object
    if not 1 <= width <= 64 or any(n <= 0 or n % width or n >= 8192 for n in args.counts):
        parser.error("Use 1..64 fields per object and divisible positive totals below 8192")
    if args.workload == "sparse" and any(n % 10 for n in args.counts):
        parser.error("Rotating 10% needs totals divisible by 10")
    if args.backend == "fusion" and not os.environ.get("FUSION_PROBE_APP_ID", "").strip():
        parser.error("Set FUSION_PROBE_APP_ID in the process environment")
    if args.sdk_zip:
        if args.backend != "native":
            parser.error("--sdk-zip requires the native backend")
        print("SDK_SHA256", sha256(args.sdk_zip.read_bytes()).hexdigest(), flush=True)
    run_trials(args, partial(fixture, width=width, omit_wave=args.omit_wave), (), "property-scale",
               partial(summarize, width=width), controller=trial)


if __name__ == "__main__":
    main()
