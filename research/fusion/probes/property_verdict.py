"""Score property_matrix JSONL attempts and print the per-cell release gates."""

import argparse
from collections import defaultdict
import json
from pathlib import Path
from statistics import median


PRODUCTS = ("old", "candidate", "fusion")
WORKLOADS = ("idle", "sparse", "dense")
COUNTS = (1000, 2000, 3000, 4000, 5000)


def cell_verdict(count, workload, rooms):
    valid = {product: [record["metrics"] for record in rooms[product]
                       if record.get("metrics", {}).get("correct")] for product in PRODUCTS}
    attempts = {product: {"valid": len(valid[product]), "total": len(rooms[product])} for product in PRODUCTS}
    failures = [product + " correctness" for product in ("old", "candidate")
                if any(record.get("metrics", {}).get("correct") is False or record.get("failure_stage") == "observer"
                       for record in rooms[product])]
    missing = [name for name in PRODUCTS if len(valid[name]) < 5]
    if missing:
        return {"status": "fail" if failures else "pending", "attempts": attempts,
                "failures": failures, "missing": missing}

    def measure(name, cap=None, lower=True):
        values = {}
        for product in PRODUCTS:
            series = [r[name] for r in valid[product] if r[name] is not None]
            scored = [min(x, cap) for x in series] if cap is not None else series
            values[product] = {"median": median(scored) if scored else None,
                               "worst": (min if lower else max)(series) if series else None}
        return values

    metrics = {}
    gates = {}
    for role in ("owner", "remote"):
        name = role + "_tick_hz"
        values = measure(name, cap=60)
        metrics[name] = values
        gates[name] = {"minimum": max(48, .8*values["fusion"]["median"])}
        if values["candidate"]["median"] < gates[name]["minimum"]:
            failures.append(name)
        name = role + "_tick_gap_p95_ms"
        metrics[name] = measure(name, lower=False)
    if workload != "idle":
        for name in ("observed_per_second_while_writing", "visible_version_fraction"):
            values = measure(name)
            metrics[name] = values
            gates[name] = {"minimum": max(.8*values["fusion"]["median"], .95*values["old"]["median"])}
            if values["candidate"]["median"] < gates[name]["minimum"]:
                failures.append(name)
        name = "after_stop_to_all_ms"
        values = measure(name, lower=False)
        metrics[name] = values
        gates[name] = {"maximum": min(1.25*values["fusion"]["median"], 1.10*values["old"]["median"])}
        if values["candidate"]["median"] > gates[name]["maximum"]:
            failures.append(name)
    if count == 1000 and workload == "sparse":
        name = "remote_setter_amplification"
        values = measure(name, lower=False)
        metrics[name] = values
        gates[name] = {"maximum": 1.10}
        if values["candidate"]["worst"] > gates[name]["maximum"]:
            failures.append(name)
    return {"status": "fail" if failures else "pass", "attempts": attempts,
            "failures": failures, "metrics": metrics, "gates": gates}


def verdict(records):
    rooms = defaultdict(lambda: defaultdict(list))
    builds = defaultdict(set)
    seen = set()
    for record in records:
        identity = record.get("room_id") or tuple(record[key] for key in (
            "build", "product", "width", "properties", "workload", "repeat", "attempt"))
        if identity in seen:
            raise ValueError(f"Duplicate room record: {identity}")
        seen.add(identity)
        builds[record["product"]].add(record["build"])
        if record["width"] == 20:
            rooms[record["properties"], record["workload"]][record["product"]].append(record)
    if any(len(fingerprints) > 1 for fingerprints in builds.values()):
        raise ValueError("A product changed builds during the matrix")
    cells = [dict(cell_verdict(count, workload, rooms[count, workload]), properties=count, workload=workload)
             for count in COUNTS for workload in WORKLOADS]
    return {"builds": {name: next(iter(versions)) for name, versions in builds.items()}, "cells": cells}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path, nargs="+")
    parser.add_argument("--reference", type=Path, action="append", default=[],
                        help="Reuse only old and Fusion rooms from an earlier matrix")
    args = parser.parse_args()

    def read(paths):
        return (json.loads(line) for path in paths for line in path.read_text(encoding="utf-8").splitlines())

    records = list(read(args.evidence))
    records.extend(record for record in read(args.reference) if record["product"] in ("old", "fusion"))
    result = verdict(records)
    print(json.dumps(result, indent=2))
    return 0 if all(cell["status"] == "pass" for cell in result["cells"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
