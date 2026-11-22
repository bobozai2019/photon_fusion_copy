"""Run the existing native property workload on isolated Direct and TCP SDK installs."""
import argparse
from hashlib import file_digest
import json
import os
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from artifacts import ROOT, RUN_LIMIT, budget, tree_size

LEGACY_OPEN = 'spawner.open(writer, "127.0.0.1", int(args[2]), 2)'
METRICS = """
var maximum_queue := {}

func _physics_process(delta: float) -> void:
    if spawner and spawner.room.game.has_method("transport_stats"):
        var stats: Dictionary = spawner.room.game.transport_stats()
        for field in stats:
            maximum_queue[field] = maxi(maximum_queue.get(field, 0), stats[field])
    super._physics_process(delta)

func emit_result(kind: String, value: Variant) -> void:
    if kind == "writer" or kind == "observer":
        value["maximum_queue"] = maximum_queue
        value["provider"] = spawner.room.game.transport_build_id() if spawner.room.game.has_method("transport_build_id") else "direct"
    super.emit_result(kind, value)
"""


def load_probe(args):
    sys.path.insert(0, str(args.godot_tools.resolve()))
    import godot_build
    if not hasattr(godot_build, "import_project"):
        raise ValueError("Use the examples repository tools directory for --godot-tools")
    if args.godot:
        os.environ["NETCODE_GODOT_EXE"] = str(args.godot.resolve())
    sys.path.insert(0, str(args.probes.resolve()))
    import property_scale
    import rigid_scale
    return property_scale, rigid_scale


def fingerprint(path):
    with path.open("rb") as stream:
        return file_digest(stream, "sha256").hexdigest()


def provider_open(provider, module=None):
    path = module.resolve().as_posix() if module else ""
    return ('spawner.open_with_provider(writer, "127.0.0.1:" + args[2], 2, '
            f'{json.dumps(provider)}, {json.dumps(path, ensure_ascii=False)}, '
            'PackedByteArray(), PackedByteArray(), false)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probes", type=Path, required=True, help="Existing property_scale/rigid_scale probe sources")
    parser.add_argument("--godot-tools", type=Path, required=True)
    parser.add_argument("--godot", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--direct-module", type=Path,
                        help="Direct ABI module for B (defaults to the standalone DLL beside the candidate archive when present)")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--counts", type=int, nargs="+", choices=range(1000, 5001, 1000),
                        default=(1000, 2000, 3000, 4000, 5000))
    parser.add_argument("--workloads", nargs="+", choices=("idle", "sparse", "dense"), default=("idle", "sparse", "dense"))
    parser.add_argument("--rooms", type=int, default=5)
    args = parser.parse_args()
    work = args.work.resolve()
    if not work.is_relative_to(ROOT.resolve()) or args.rooms < 1:
        parser.error("Use a new --work directory under out/ and a positive room count")
    if not budget(ROOT, RUN_LIMIT)["allowed"]:
        parser.error("The existing output budget cannot accommodate this run")
    prop, runner = load_probe(args)
    width = 20
    direct_module = args.direct_module
    if direct_module is None:
        sibling = args.candidate.with_name("netcode_transport_direct.dll")
        direct_module = sibling if sibling.is_file() else None
    direct_digest = fingerprint(direct_module) if direct_module else None
    variants = {"B": (args.candidate, "direct", direct_module), "C": (args.candidate, "tcp", None)}
    if args.baseline:
        variants = {"A": (args.baseline, "direct", None), **variants}

    work.mkdir(parents=True, exist_ok=False)
    projects = {}
    for name, (archive, provider, module) in variants.items():
        job = work / name
        job.mkdir()

        def fixture(web):
            assert not web
            files = prop.fixture(False, width)
            if LEGACY_OPEN not in files["main.gd"]:
                raise ValueError("Property probe open call changed. Review the provider injection point.")
            if provider == "tcp" or module:
                files["main.gd"] = files["main.gd"].replace(LEGACY_OPEN, provider_open(provider, module), 1)
            files["main.gd"] += METRICS
            return files

        projects[name] = runner.native_setup(job, fixture, archive.resolve()), fingerprint(archive)

    failures = 0
    with (work / "results.jsonl").open("w", encoding="utf-8") as summary:
        for count_index, count in enumerate(args.counts):
            for workload_index, workload in enumerate(args.workloads):
                for trial in range(args.rooms):
                    order = list(variants)
                    start = (count_index + workload_index + trial) % len(order)
                    for name in order[start:] + order[:start]:
                        if tree_size(work) >= RUN_LIMIT:
                            raise RuntimeError("Property probe evidence reached the run size limit")
                        project, digest = projects[name]
                        entry = {"variant": name, "sdk_sha256": digest, "fields": count, "workload": workload, "trial": trial + 1}
                        if name == "B" and direct_digest:
                            entry["provider_sha256"] = direct_digest
                        room = work / name / f"{count}-{workload}-{trial + 1}"
                        room.mkdir()
                        try:
                            writer, observer = runner.local_trial(room, project, count, workload, controller=prop.trial)
                            result = prop.summarize(count, workload, writer, observer, width)
                            result["owner_transport"] = writer["provider"]
                            result["remote_transport"] = observer["provider"]
                            result["owner_maximum_queue"] = writer["maximum_queue"]
                            result["remote_maximum_queue"] = observer["maximum_queue"]
                            entry["result"] = result
                            failures += int(not result["correct"])
                        except (ValueError, RuntimeError, TimeoutError) as error:
                            entry["error"] = str(error)
                            failures += 1
                        summary.write(json.dumps(entry, ensure_ascii=False) + "\n")
                        summary.flush()
                        print(name, count, workload, trial + 1,
                              entry.get("error") or {key: entry["result"][key] for key in ("correct", "owner_tick_hz", "remote_tick_hz")},
                              flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
