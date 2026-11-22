"""Apply the property line's per-cell verdict to pinned native A/B/C and Fusion evidence."""
import argparse
from collections import defaultdict
from hashlib import file_digest
import json
from pathlib import Path
import sys


def read(path):
    return [json.loads(row) for row in path.read_text(encoding="utf-8").splitlines() if row.strip()]


def evaluate(native, reference, verdict):
    rooms = defaultdict(lambda: defaultdict(list))
    builds = defaultdict(set)
    for record in native:
        variant = record["variant"]
        if variant not in ("A", "B", "C"):
            raise ValueError(f"Unexpected variant {variant}")
        builds[variant].add(record["sdk_sha256"])
        cell = record["fields"], record["workload"]
        rooms[cell][variant].append({"metrics": record.get("result", {"correct": False})})
    for record in reference:
        if record["product"] == "fusion" and record["width"] == 20:
            builds["fusion"].add(record["build"])
            rooms[record["properties"], record["workload"]]["fusion"].append(record)
    if any(len(fingerprints) > 1 for fingerprints in builds.values()):
        raise ValueError("A variant changed builds during the matrix")
    if builds["B"] != builds["C"]:
        raise ValueError("B and C require the same candidate SDK hash")

    variants = ("A", "B", "C")
    cells = []
    for fields in verdict.COUNTS:
        for workload in verdict.WORKLOADS:
            seen = rooms[fields, workload]
            results = {
                variant: verdict.cell_verdict(fields, workload,
                                              {"old": seen["A"], "candidate": seen[variant], "fusion": seen["fusion"]})
                for variant in variants
            }
            cells.append({"fields": fields, "workload": workload, "variants": results,
                          "inherited_failures": results["A"]["failures"],
                          "added_by_direct": [item for item in results["B"]["failures"]
                                              if item not in results["A"]["failures"]],
                          "added_by_tcp": [item for item in results["C"]["failures"]
                                           if item not in results["B"]["failures"]]})
    return {"builds": {key: next(iter(value), None) for key, value in builds.items()}, "cells": cells,
            "tcp_release_passes": all(cell["variants"]["C"]["status"] == "pass" for cell in cells)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probes", type=Path, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--fusion", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.probes.resolve()))
    import property_verdict
    result = evaluate(read(args.native), read(args.fusion), property_verdict)
    with args.fusion.open("rb") as evidence:
        result["fusion_evidence_sha256"] = file_digest(evidence, "sha256").hexdigest()
    print(json.dumps(result, indent=2))
    return 0 if result["tcp_release_passes"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
