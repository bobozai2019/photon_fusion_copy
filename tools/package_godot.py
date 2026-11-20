"""Export, smoke-test and rotate the standalone Godot Windows x64 delivery."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
from zipfile import ZipFile

from artifacts import ROOT, budget, create_job, job_lock, remove_owned
from godot_build import CPP_DIRECTORY, GODOT_PROJECT, REPO, console_godot, import_project, resolve_godot
from package_yard import BUILD, archive_payload, publish, slot_digest


SLOT = Path("packages/godot-win-x64")
PACKAGE = "netcode-godot-win-x64.zip"
GAME = "netcode-godot.exe"
LIBRARY = "netcode_godot.dll"
PROJECT_FILES = (
    "project.godot", "export_presets.cfg", "netcode_yard.gdextension",
    "yard.tscn", "yard.gd", "paint.gd", "replicator.gd", "replicators.cfg", LIBRARY,
)


def godot_command(godot, cwd, *args):
    result = subprocess.run(
        [godot, *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=90, creationflags=subprocess.CREATE_NO_WINDOW,
    )
    output = result.stdout + result.stderr
    print(output, end="", flush=True)
    if result.returncode or "SCRIPT ERROR" in output or "ERROR:" in output:
        raise RuntimeError(f"Godot command failed ({result.returncode}): {args}")
    return output


def assemble(work, godot, build_project):
    project = work / "project"
    project.mkdir()
    for filename in PROJECT_FILES:
        shutil.copyfile(build_project / filename, project / filename)
    for uid in build_project.glob("*.uid"):
        shutil.copyfile(uid, project / uid.name)
    import_project(godot, project)
    payload = work / "payload"
    payload.mkdir()

    console = console_godot(godot)
    version = godot_command(console, work, "--version").splitlines()[0]
    release = re.match(r"(4\.\d+(?:\.\d+)?)\.stable\.", version)
    if not release:
        raise RuntimeError(f"Use a stable Godot editor with matching export templates: {version}")
    godot_command(console, project, "--headless", "--path", project,
                  "--export-release", "Windows Desktop", payload / GAME)
    for name in (GAME, LIBRARY):
        if not (payload / name).is_file():
            raise FileNotFoundError(f"Godot export did not produce {name}")
    shutil.copyfile(build_project.parent / "netcode-guest.exe", payload / "netcode-guest.exe")
    shutil.copyfile(REPO / "examples/godot-yard/README.md", payload / "README.md")
    default_config = REPO / "examples/car-yard/replicators.cfg"
    shutil.copyfile(default_config, payload / "replicators.cfg")

    licenses = payload / "licenses"
    licenses.mkdir()
    source = f"https://raw.githubusercontent.com/godotengine/godot/{release[1]}-stable"
    for filename in ("LICENSE.txt", "COPYRIGHT.txt"):
        subprocess.run(["curl.exe", "-fLsS", "--retry", "2", "--max-time", "60",
                        f"{source}/{filename}", "-o", licenses / f"Godot-{filename}"], check=True)
    shutil.copyfile(BUILD / "vendor" / CPP_DIRECTORY / "LICENSE.md", licenses / "godot-cpp-LICENSE.md")
    return archive_payload(work, payload, PACKAGE)


def smoke(staged, work, verify_play):
    unpack = work / "unpacked"
    with ZipFile(staged / PACKAGE) as archive:
        if archive.testzip() is not None:
            raise ValueError("Package CRC mismatch")
        archive.extractall(unpack)
    for option, expected in (("--smoke", "Godot scene rendered"), ("--binding-smoke", "passed")):
        if expected not in godot_command(unpack / GAME, unpack, "--", option):
            raise RuntimeError(f"Package smoke output did not contain {expected!r}")
    if verify_play:
        subprocess.run([sys.executable, "-B", REPO / "tools/verify_yard.py", "play",
                        "--platform", "godot", "--godot-export", unpack / GAME,
                        "--bin", unpack, "--port", "43245"], timeout=45, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot-exe", type=Path)
    parser.add_argument("--godot-project", type=Path, default=GODOT_PROJECT)
    parser.add_argument("--verify-play", action="store_true")
    args = parser.parse_args()
    if not budget(ROOT, 512 * 1024 ** 2)["allowed"]:
        print("Output budget exceeded")
        return 1
    godot = resolve_godot(args.godot_exe)
    work = create_job(ROOT, "tmp/package-godot-win-x64", "temp")
    published = False
    try:
        with job_lock(work, "temp"):
            staged = assemble(work, godot, args.godot_project.resolve())
            digest = slot_digest(ROOT, staged, PACKAGE)
            smoke(staged, work, args.verify_play)
            changed = publish(ROOT, work, staged, slot=SLOT, package=PACKAGE)
            published = True
            print(f"{'published' if changed else 'unchanged'} {ROOT / SLOT / 'current' / PACKAGE} {digest}")
    finally:
        if not published and (work / "rollback-previous").exists():
            print(f"Retained package rollback at {work / 'rollback-previous'}", flush=True)
        else:
            print(f"remove {work} (finished package staging)", flush=True)
            remove_owned(ROOT, work, "temp")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
