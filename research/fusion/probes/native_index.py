"""Verify the Godot bindings and both named native function indexes for Preview 555."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from native_evidence import SDK, SHA256, plt_targets


HERE = Path(__file__).resolve().parent
INDEX = HERE.parent / "findings"
DLL_HASH = "104b1e2c3a0289b7391584c75a90f1480a9dab750099114c99356a813ce69752"


class EditorLibrary:
    def __init__(self, path, llvm_bin):
        self.path = path
        self.llvm_bin = llvm_bin
        self.data = path.read_bytes()
        if hashlib.sha256(self.data).hexdigest() != SHA256:
            raise ValueError("Expected the Preview 555 Linux x86-64 editor build")
        program_headers = struct.unpack_from("<Q", self.data, 32)[0]
        size, count = struct.unpack_from("<HH", self.data, 54)
        self.segments = []
        for index in range(count):
            kind, _, offset, addr, _, length, _, _ = struct.unpack_from(
                "<IIQQQQQQ", self.data, program_headers + size * index)
            if kind == 1:
                self.segments.append((addr, addr + length, offset))

    def offset(self, address):
        for start, end, offset in self.segments:
            if start <= address < end:
                return offset + address - start
        raise ValueError(f"Address outside file-backed load segments: {address:#x}")

    def cstring(self, address):
        offset = self.offset(address)
        return self.data[offset:self.data.index(b"\0", offset)].decode("utf-8")

    def pointer(self, address):
        return struct.unpack_from("<Q", self.data, self.offset(address))[0]

    def tool(self, name, *args):
        path = self.llvm_bin / name if self.llvm_bin else name
        return subprocess.check_output([str(path), *args, str(self.path)], text=True, encoding="utf-8")

    def objdump(self, *args):
        return self.tool("llvm-objdump", *args)

    def instructions(self, start, end):
        listing = self.objdump("-d", "--no-show-raw-insn", f"--start-address={start:#x}", f"--stop-address={end:#x}")
        return [(int(addr, 16), op, args.strip()) for addr, op, args in re.findall(
            r"^\s*([0-9a-f]+):[ \t]+(\S+)[ \t]*(.*)$", listing, re.MULTILINE)]


def function_rows(lib, namespace):
    groups = {}
    exported = {}
    for line in lib.tool("llvm-nm", "-D", "-S", "-n", "-C", "--defined-only").splitlines():
        match = re.match(r"^([0-9a-f]+)\s+([0-9a-f]+)\s+([TW])\s+(.+)$", line)
        if not match:
            continue
        address, size, _, name = match.groups()
        address = int(address, 16)
        exported[address] = name
        if name.startswith(namespace):
            signatures, extent = groups.setdefault(address, ([], int(size, 16)))
            if extent != int(size, 16):
                raise ValueError(f"Aliased symbol size mismatch at {address:#x}")
            signatures.append(name)
    named_targets = {**exported, **plt_targets(lib.objdump)}
    yield ["entry", "bytes", "symbol_entries", "instructions", "indirect_calls", "indirect_jumps",
           "signatures", "direct_core_or_realtime_targets"]
    for start, (signatures, size) in sorted(groups.items()):
        ops = lib.instructions(start, start + size)
        if not ops or ops[0][0] != start:
            raise ValueError(f"Missing function at {start:#x}")
        targets = []
        indirect_calls = 0
        indirect_jumps = 0
        for _, op, args in ops:
            if op not in ("callq", "call", "jmpq", "jmp"):
                continue
            if args.startswith("*"):
                indirect_calls += int(op.startswith("call"))
                indirect_jumps += int(op.startswith("jmp"))
                continue
            target = re.match(r"0x([0-9a-f]+)", args)
            if not target:
                continue
            dest = int(target[1], 16)
            name = named_targets.get(dest, "")
            if not start <= dest < start + size and name.startswith(("FusionCore::", "PhotonMatchmaking::")):
                if name not in targets:
                    targets.append(name)
        yield [hex(start), str(size), str(len(signatures)), str(len(ops)), str(indirect_calls), str(indirect_jumps),
               " | ".join(dict.fromkeys(signatures)), " | ".join(targets) or "-"]


def classdb(godot, racing):
    dll = racing / "addons/fusion/bin/libfusion.windows.editor.x86_64.release.dll"
    if hashlib.sha256(dll.read_bytes()).hexdigest() != DLL_HASH:
        raise ValueError("The ClassDB census uses the matching Windows editor build")
    proc = subprocess.run([str(godot), "--headless", "--path", str(racing), "--script", str(HERE / "client_surface.gd")],
                          capture_output=True, encoding="utf-8", errors="replace", timeout=30, check=True)
    return json.loads(next(line.removeprefix("FUSION_CLASSDB ") for line in proc.stdout.splitlines()
                           if line.startswith("FUSION_CLASSDB ")))


def type_rows(api):
    yield ["class", "kind", "name", "declaration"]
    for cls, group in sorted(api.items()):
        yield [cls, "class", cls, group["parent"]]
        for kind, key in (("method", "method_signatures"), ("signal", "signal_signatures"),
                          ("property", "property_descriptors"), ("enum", "enums")):
            for name, declaration in sorted(group[key].items()):
                if kind == "enum":
                    declaration = ", ".join(f"{key}={value}" for key, value in sorted(declaration.items()))
                yield [cls, kind, name, declaration]


def check_index(filename, rows, write):
    path = INDEX / filename
    if write:
        with path.open("w", encoding="utf-8", newline="") as stream:
            csv.writer(stream, delimiter="\t", lineterminator="\n").writerows(rows)
    else:
        with path.open(encoding="utf-8", newline="") as stream:
            if list(csv.reader(stream, delimiter="\t")) != rows:
                raise ValueError(f"{filename} differs from the original build")


def check_managed(api, folder):
    totals = dict.fromkeys(("methods", "properties", "signals"), 0)
    files = sorted(folder.rglob("*.cs"))
    for path in files:
        if path.stem not in api:
            continue
        source = path.read_text(encoding="utf-8-sig")
        for group, key in (("MethodName", "methods"), ("PropertyName", "properties"), ("SignalName", "signals")):
            block = re.search(r"\bclass " + group + r"[^{]*\{(.*?)\n    \}", source, re.DOTALL)
            names = re.findall(r'StringName \w+ = "([^"]+)"', block[1]) if block else []
            missing = set(names) - set(api[path.stem][key])
            if missing:
                raise ValueError(f"{path.name} {group}: {missing}")
            totals[key] += len(names)
    print(f"{len(files)} C# files; native names found for {totals}")


def referenced_address(instruction):
    address, op, args = instruction
    match = re.search(r"# 0x([0-9a-f]+)", args)
    if op != "leaq" or not match:
        raise ValueError(f"Expected RIP-relative address at {address:#x}")
    return int(match[1], 16)


def check_bindings(lib, api):
    with (INDEX / "godot-bindings.tsv").open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    pairs = {(row["class"], row["method"]) for row in rows}
    registered = {(cls, nm) for cls, group in api.items() for nm in group["methods"]}
    if pairs != registered or len(pairs) != len(rows):
        raise ValueError(f"ClassDB / table difference: {pairs ^ registered}")
    for cls in sorted(api):
        members = [row for row in rows if row["class"] == cls]
        if not members:
            continue
        begin = min(int(row["name_ref"], 16) for row in members)
        end = max(int(row["member_ref"], 16) for row in members) + 160
        ops = lib.instructions(begin, end)
        by_address = {op[0]: i for i, op in enumerate(ops)}
        for row in members:
            name_at, member_at, target = (int(row[key], 16) for key in ("name_ref", "member_ref", "entry"))
            if lib.cstring(referenced_address(ops[by_address[name_at]])) != row["method"]:
                raise ValueError(f"Method literal mismatch: {row}")
            i = by_address[member_at]
            if row["reference"] == "virtual":
                # Itanium member pointer encodes the virtual slot byte offset plus one.
                if (ops[i][1] != "movl" or not ops[i][2].startswith("$0x81, %edi")
                        or lib.cstring(lib.pointer(lib.pointer(0x6C58C8) + 8)) != "22FusionServerReplicator"
                        or lib.pointer(0x6C58D0 + 0x80) != target):
                    raise ValueError(f"Virtual slot mismatch: {row}")
                continue
            if referenced_address(ops[i]) != target:
                raise ValueError(f"Member pointer mismatch: {row}")
            reg = re.search(r",\s*(%r\w+)", ops[i][2])[1]
            near = ops[i + 1:i + 23]
            if row["reference"] == "slot":
                if not any(op.startswith("mov") and re.match(re.escape(reg) + r",\s*0x50\(", arg)
                           for _, op, arg in near):
                    raise ValueError(f"MethodBind member assignment missing: {row}")
            elif row["reference"] == "binder":
                first_call = next((arg for _, op, arg in near if op == "callq"), "")
                if reg not in ("%rsi", "%rdi") or not re.match(r"0x(?:1|2[0-3])[0-9a-f]+\b", first_call):
                    raise ValueError(f"MethodBind adapter call missing: {row}")
            else:
                raise ValueError(f"Unknown reference type: {row}")
    print(f"{len(api)} ClassDB classes, {len(rows)} registered methods linked to Linux entry addresses")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--godot", required=True, type=Path)
    parser.add_argument("--racing", type=Path, default=SDK.parents[3])
    parser.add_argument("--write-index", action="store_true", help="Regenerate the function and type indexes")
    args = parser.parse_args()
    addon = args.racing / "addons/fusion"
    lib = EditorLibrary(addon / "bin" / SDK.name, args.llvm_bin)
    api = classdb(args.godot, args.racing)
    totals = {key: sum(len(cls[key]) for cls in api.values()) for key in ("properties", "signals", "enums")}
    print("Direct ClassDB descriptors:", totals)
    check_bindings(lib, api)
    check_managed(api, addon / "cs")
    check_index("godot-types.tsv", list(type_rows(api)), args.write_index)
    for namespace, file in (("FusionCore::", "core-functions.tsv"), ("PhotonMatchmaking::", "realtime-functions.tsv")):
        rows = list(function_rows(lib, namespace))
        check_index(file, rows, args.write_index)
        print(f"{namespace} {len(rows)-1} function starts, {sum(int(row[2]) for row in rows[1:])} named entries, "
              f"{sum(int(row[3]) for row in rows[1:])} decoded instructions")


if __name__ == "__main__":
    main()

