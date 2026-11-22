"""Apply the property line's per-cell verdict to pinned native A/B/C and Fusion evidence."""
import argparse
from collections import defaultdict
from hashlib import sha256
import json
from pathlib import Path
import sys

QUEUE_METRICS = ("queued_bytes", "provider_peak_queued_bytes", "core_peak_queued_bytes", "oldest_queued_us")


def read(data):
    return [json.loads(row) for row in data.decode("utf-8-sig").splitlines() if row.strip()]


def queue_high_water(rooms):
    peaks = {}
    metrics = [room.get("metrics", {}) for room in rooms]
    for role in ("owner", "remote"):
        samples = [room.get(f"{role}_maximum_queue") for room in metrics if room.get("correct")]
        samples = [sample for sample in samples if sample]
        if samples:
            peaks[role] = {metric: max(sample[metric] for sample in samples) for metric in QUEUE_METRICS}
    return peaks or None


def require_queue_metrics(metrics, key):
    for role in ("owner", "remote"):
        queue = metrics[f"{role}_maximum_queue"]
        if any(not isinstance(queue.get(field), int) or queue[field] < 0 for field in QUEUE_METRICS):
            raise ValueError(f"Missing or invalid {role} queue metrics in {key}")


def attach_baseline_queues(result, records):
    cells = defaultdict(list)
    builds = set()
    trials = set()
    for record in records:
        cell = record["fields"], record["workload"]
        key = (*cell, record["trial"])
        metrics = record["result"]
        if (key in trials or record["variant"] != "A" or not metrics["correct"] or
                any(metrics[f"{role}_transport"] != "direct" for role in ("owner", "remote"))):
            raise ValueError(f"Invalid baseline queue run {key}")
        require_queue_metrics(metrics, key)
        trials.add(key)
        builds.add(record["sdk_sha256"])
        cells[cell].append({"metrics": metrics})
    expected = {(cell["fields"], cell["workload"]) for cell in result["cells"]}
    if len(builds) != 1 or result["builds"]["A"] in builds:
        raise ValueError("Baseline queue runs require one separately instrumented SDK")
    if cells.keys() != expected or any(len(rooms) < 5 for rooms in cells.values()):
        raise ValueError("Baseline queue runs require five independent rooms in every cell")
    result["A_queue_build"] = builds.pop()
    for cell in result["cells"]:
        cell["variants"]["A"]["queue_high_water"] = queue_high_water(cells[cell["fields"], cell["workload"]])


def evaluate(native, reference, verdict, baseline_sha=None, direct_module_sha=None):
    rooms = defaultdict(lambda: defaultdict(list))
    builds = defaultdict(set)
    providers = defaultdict(set)
    native_keys = set()
    direct_modules = set()
    for record in native:
        variant = record["variant"]
        if variant not in ("A", "B", "C"):
            raise ValueError(f"Unexpected variant {variant}")
        builds[variant].add(record["sdk_sha256"])
        if variant == "B":
            direct_modules.add(record.get("provider_sha256"))
        cell = record["fields"], record["workload"]
        key = (variant, *cell, record["trial"])
        if key in native_keys:
            raise ValueError(f"Duplicate native room {key}")
        native_keys.add(key)
        metrics = record.get("result", {"correct": False})
        if metrics["correct"]:
            owner, remote = metrics["owner_transport"], metrics["remote_transport"]
            if variant == "C":
                expected = owner.startswith("tcp/") and owner.endswith("/v1")
            elif variant == "B" and record.get("provider_sha256"):
                expected = owner.startswith("direct/") and owner.endswith("/v1")
            else:
                expected = owner == "direct"
            if owner != remote or not expected:
                raise ValueError(f"Unexpected providers in {key}: {owner!r}, {remote!r}")
            providers[variant].add(owner)
            if variant in ("B", "C"):
                require_queue_metrics(metrics, key)
        rooms[cell][variant].append({"metrics": metrics})

    reference_keys = set()
    measured_a = bool(builds["A"])
    for record in reference:
        if record["width"] != 20:
            continue
        product = record["product"]
        if product == "candidate" and not measured_a:
            product = "A"
        elif product not in ("old", "fusion"):
            continue
        cell = record["properties"], record["workload"]
        key = (product, *cell, record["repeat"])
        if key in reference_keys:
            raise ValueError(f"Duplicate reference room {key}")
        reference_keys.add(key)
        builds[product].add(record["build"])
        rooms[cell][product].append(record)

    if any(len(fingerprints) > 1 for fingerprints in builds.values()):
        raise ValueError("A variant changed builds during the matrix")
    if any(len(labels) > 1 for labels in providers.values()):
        raise ValueError("A variant changed runtime providers during the matrix")
    if baseline_sha and builds["A"] != {baseline_sha}:
        raise ValueError("A does not match the fixed optimized Direct SDK hash")
    if len(direct_modules) > 1 or (direct_module_sha and direct_modules != {direct_module_sha}):
        raise ValueError("B does not match the fixed Direct module hash")
    if builds["B"] != builds["C"]:
        raise ValueError("B and C require the same candidate SDK hash")

    variants = ("A", "B", "C")
    cells = []
    for fields in verdict.COUNTS:
        for workload in verdict.WORKLOADS:
            seen = rooms[fields, workload]
            results = {
                variant: verdict.cell_verdict(fields, workload,
                                              {"old": seen["old"], "candidate": seen[variant], "fusion": seen["fusion"]})
                for variant in variants
            }
            for variant in variants:
                results[variant]["queue_high_water"] = queue_high_water(seen[variant])
            cells.append({"fields": fields, "workload": workload, "variants": results,
                          "inherited_failures": results["A"]["failures"],
                          "added_by_direct": [item for item in results["B"]["failures"]
                                              if item not in results["A"]["failures"]],
                          "added_by_tcp": [item for item in results["C"]["failures"]
                                           if item not in results["B"]["failures"]]})
    return {"builds": {key: next(iter(value), None) for key, value in builds.items()},
            "providers": {key: next(iter(value)) for key, value in providers.items()},
            "A_source": "transport-run" if measured_a else "property-reference",
            "direct_module_sha256": next(iter(direct_modules), None),
            "cells": cells,
            "tcp_release_passes": all(cell["variants"]["C"]["status"] == "pass" for cell in cells)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probes", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", "--fusion", dest="reference", type=Path, required=True,
                        help="Property-line matrix with the old release, optimized Direct and Fusion cohorts")
    parser.add_argument("--baseline-sha", help="SHA-256 of the fixed optimized Direct archive")
    parser.add_argument("--baseline-queues", type=Path, help="Supplemental queue-only A run from the instrumented baseline")
    parser.add_argument("--direct-module-sha", help="SHA-256 of B's standalone Direct ABI module")
    args = parser.parse_args()
    sys.path.insert(0, str(args.probes.resolve()))
    import property_verdict
    native = args.native.read_bytes()
    reference = args.reference.read_bytes()
    result = evaluate(read(native), read(reference), property_verdict, args.baseline_sha,
                      args.direct_module_sha)
    result["native_results_sha256"] = sha256(native).hexdigest()
    result["property_reference_sha256"] = sha256(reference).hexdigest()
    if args.baseline_queues:
        baseline_queues = args.baseline_queues.read_bytes()
        attach_baseline_queues(result, read(baseline_queues))
        result["A_queue_results_sha256"] = sha256(baseline_queues).hexdigest()
    print(json.dumps(result, indent=2))
    return 0 if result["tcp_release_passes"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
