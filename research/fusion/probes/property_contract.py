"""Exercise property bindings and authority with two instances of an installed Godot addon."""

import argparse
from pathlib import Path

from rigid_scale import HERE, open_trials


def fixture(_web):
    return {
        "main.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://main.gd" id="Main"]\n'
                     '[node name="Main" type="Node"]\nscript = ExtResource("Main")\n',
        "main.gd": (HERE / "property_contract.gd").read_text(encoding="utf-8"),
    }


def trial(_peers, wait, command, _workload):
    def step(role, op):
        command(role, op)
        return wait(role, op)

    for role in ("A", "B"):
        wait(role, "baseline")
    result = {"wire": step("A", "seed"), "mixed": wait("B", "seed")}
    step("A", "collection_edit")
    result["collections"] = wait("B", "collection_edit")
    result["local_correction"] = step("B", "edit_proxy")
    step("A", "sibling")
    result["sibling"] = wait("B", "sibling")
    step("A", "target")
    result["target"] = wait("B", "target")
    step("A", "equal_value")
    result["equal_value"] = wait("B", "equal_value")
    step("B", "detach")
    step("A", "update_detached")
    result["replacement"] = step("B", "replace")
    result["replacement_after_free"] = step("B", "replace_freed")
    step("B", "claim")
    result["partial_write"] = wait("A", "partial_write")
    step("A", "reclaim")
    result["handoff"] = wait("B", "host_reclaimed")
    step("A", "reclaimed")
    result["reclaimed"] = wait("B", "reclaimed")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk_zip", type=Path)
    args = parser.parse_args()
    args.backend = "native"
    with open_trials(args, fixture, "property-contract") as run:
        print(run(0, 1, "contract", controller=trial))


if __name__ == "__main__":
    main()
