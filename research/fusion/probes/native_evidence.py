"""Verify selected call sites in the Racing Linux editor library with LLVM."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess


SDK = (Path(__file__).resolve().parents[4] / "references/photon/photon-fusion-godot-starter-kit-racing_3.0.0"
       / "addons/fusion/bin/libfusion.linux.editor.x86_64.release.so")
SHA256 = "3bcbec5f76344bb98f82f4f65448afdf65206400b4268e28a1d4b95d38ebf61b"

# These are call/jump instruction addresses, not symbol entry addresses.
SITES = {
    "Input": [
        (0x1E72FD, "FusionCore::ObjectRoot::QueueInput"),
        (0x1E7330, "FusionCore::ObjectRoot::ExecuteInputs"),
        (0x4641C8, "FusionCore::Client::InputWritePending"),
        (0x4641D0, "FusionCore::Client::InputFlushToServer"),
        (0x4579D7, "PhotonMatchmaking::RealtimeClient::SendEvent"),
        (0x46683E, "FusionCore::Client::InputPacketReceived"),
        (0x45E503, "FusionCore::Client::FindObjectRoot"),
    ],
    "RPC / Notify": [
        (0x457CD2, "FusionCore::Rpc::Write"),
        (0x458052, "FusionCore::Rpc::Write"),
        (0x45A163, "FusionCore::Notify::Connection::Queue"),
        (0x45A493, "FusionCore::Notify::Connection::Queue"),
        (0x455F70, "PhotonMatchmaking::RealtimeClient::SendEvent"),
        (0x466891, "FusionCore::Notify::Connection::Receive"),
        (0x4686FE, "FusionCore::Client::RpcPacketReceived"),
        (0x468708, "FusionCore::Client::StatePacketReceived"),
        (0x489AB4, "FusionCore::Notify::Connection::ReliableLost"),
    ],
    "Objects": [
        (0x462275, "FusionCore::Client::AllocateObject"),
        (0x462737, "FusionCore::Client::FindObjectRoot"),
        (0x46277E, "FusionCore::Client::AllocateObject"),
        (0x465917, "FusionCore::Client::TryFireObjectReady"),
        (0x46448D, "FusionCore::Client::GetRequiredObjects"),
        (0x4644C2, "FusionCore::Client::FindObject"),
        (0x465C63, "FusionCore::Client::DestroyObjectFromRemote"),
        (0x465C96, "FusionCore::Client::DestroySubObjectFromRemote"),
    ],
    "Interest / ownership": [
        (0x1A6996, "FusionCore::Client::SetAreaInterestKey"),
        (0x1A69C6, "FusionCore::Client::SetUserInterestKey"),
        (0x458B63, "FusionCore::Client::GetTail"),
        (0x459D29, "FusionCore::Client::IsOwner"),
        (0x459E2C, "FusionCore::Client::SendRpcInternal"),
    ],
    "String heap / clock": [
        (0x45294B, "FusionCore::NetworkedStringHeap::AllocateString"),
        (0x45296B, "FusionCore::NetworkedStringHeap::FreeHandle"),
        (0x462DD3, "FusionCore::Client::CheckMutatedStringHeap"),
        (0x462DED, "FusionCore::Client::WriteStringHeap"),
        (0x4656AF, "FusionCore::Client::ReadStringHeap"),
        (0x465FC8, "FusionCore::Client::ServerTimeReceived"),
    ],
}


def plt_targets(objdump):
    slots = {int(address, 16): name for address, name in re.findall(
        r"^([0-9a-f]+)\s+R_X86_64_JUMP_SLOT\s+(.+)$", objdump("-R", "--demangle"), re.MULTILINE)}
    targets = {}
    entry = None
    for line in objdump("-d", "--no-show-raw-insn", "-j", ".plt.sec").splitlines():
        start = re.match(r"\s*([0-9a-f]+):\s+endbr64", line)
        if start:
            entry = int(start[1], 16)
            continue
        jump = re.search(r"\bjmpq?\s+.*# 0x([0-9a-f]+)", line)
        if entry is not None and jump:
            slot = int(jump[1], 16)
            if slot in slots:
                targets[entry] = slots[slot]
            entry = None
    return targets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=SDK)
    parser.add_argument("--llvm-bin", type=Path, help="Directory containing llvm-nm and llvm-objdump")
    parser.add_argument("--inventory", action="store_true", help="Print the named FusionCore text entries")
    args = parser.parse_args()
    if hashlib.sha256(args.sdk.read_bytes()).hexdigest() != SHA256:
        parser.error("The instruction addresses are specific to the Racing Linux x86-64 editor build.")

    def run(tool, *flags):
        command = str(args.llvm_bin / tool) if args.llvm_bin else tool
        return subprocess.check_output([command, *flags, str(args.sdk)], text=True, encoding="utf-8")

    def objdump(*flags):
        return run("llvm-objdump", *flags)

    listing = run("llvm-nm", "-D", "-n", "-C", "--defined-only")
    entries = re.findall(r"^([0-9a-f]+)\s+[TW]\s+(FusionCore::.+)$", listing, re.MULTILINE)
    print(f"{len(entries)} named entries, {len({address for address, _ in entries})} distinct starts")
    if args.inventory:
        for address, name in entries:
            print(f"  {address} {name}")

    targets = plt_targets(objdump)
    failures = 0
    count = 0
    for group, sites in SITES.items():
        print(group)
        for address, expected in sites:
            instruction = objdump("-d", "--no-show-raw-insn", f"--start-address={address:#x}",
                                  f"--stop-address={address + 6:#x}")
            branch = re.search(rf"^\s*{address:x}:\s+(?:callq?|jmpq?)\s+0x([0-9a-f]+)", instruction, re.MULTILINE)
            name = targets.get(int(branch[1], 16), "") if branch else ""
            count += 1
            if name.partition("(")[0] != expected:
                failures += 1
                print(f"  {address:#x} expected {expected}, resolved {name!r}")
            else:
                print(f"  {address:#x} -> {name}")
    print(f"{count - failures}/{count} selected call and tail-jump sites verified")
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
