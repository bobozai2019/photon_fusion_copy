"""Create and verify the Windows x64 delivery, then rotate the package slots."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from artifacts import ROOT, budget, create_job, item_at, job_lock, remove_owned
from build import SOURCE_ROOT, TRIPLET


PACKAGE = "netcode-win-x64.zip"
MANIFEST = "SHA256SUMS.txt"
SLOT = Path("packages/win-x64")
BUILD = ROOT / "build/win-x64/Release"
PAYLOAD = ("netcode-yard.exe", "netcode-guest.exe", "replicators.cfg", "beacon.obj")


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def slot_digest(root, path, package=PACKAGE):
    path = item_at(root, path.relative_to(root))
    if not path.exists():
        return None
    if {child.name for child in path.iterdir()} != {package, MANIFEST}:
        raise ValueError(f"Unknown package slot: {path}")
    archive = item_at(root, path.relative_to(root) / package)
    manifest = item_at(root, path.relative_to(root) / MANIFEST)
    digest = sha256(archive)
    if manifest.read_text(encoding="ascii") != f"{digest}  {package}\n":
        raise ValueError(f"Package hash mismatch: {path}")
    return digest


def publish(root, work, staged, *, slot=SLOT, package=PACKAGE):
    slots = item_at(root, slot)
    slots.mkdir(parents=True, exist_ok=True)
    current, previous = slots / "current", slots / "previous"
    new_digest = slot_digest(root, staged, package)
    if new_digest is None:
        raise ValueError("Staged package is missing")
    current_digest = slot_digest(root, current, package)
    if new_digest == current_digest:
        return False
    old_previous = slot_digest(root, previous, package)
    rollback = item_at(root, (work / "rollback-previous").relative_to(root))
    if rollback.exists():
        raise ValueError("Existing package rollback slot")
    moved_current = False
    if old_previous:
        previous.rename(rollback)
    try:
        if current_digest:
            current.rename(previous)
            moved_current = True
        staged.rename(current)
    except OSError:
        if moved_current:
            previous.rename(current)
        if old_previous:
            rollback.rename(previous)
        raise
    if not moved_current and old_previous:
        rollback.rename(previous)
    return True


def add_notices(payload, cocos_root):
    licenses = payload / "licenses"
    shutil.copytree(cocos_root / "licenses", licenses / "cocos2d-x")
    share = BUILD / "vcpkg-installed" / TRIPLET / "share"
    for lib in ("egl-registry", "freetype", "glew", "glfw3", "libpng", "opengl", "opengl-registry", "zlib"):
        shutil.copyfile(share / lib / "copyright", licenses / f"{lib}.txt")
    source = BUILD / "vendor" / SOURCE_ROOT
    # These source-only dependencies carry notices in their public headers.
    for relative in ("clipper/clipper.hpp", "tinyxml2/tinyxml2.h", "xxhash/xxhash.h", "xxtea/xxtea.h",
                     "json/rapidjson.h", "tinydir/tinydir.h", "rapidxml/rapidxml_sax3.hpp", "unzip/unzip.h"):
        shutil.copyfile(source / relative, licenses / (relative.split("/")[0] + "-notice.txt"))


def assemble(work, cocos_root):
    payload = work / "payload"
    payload.mkdir()
    for name in PAYLOAD:
        shutil.copyfile(BUILD / "bin/Release" / name, payload / name)
    shutil.copyfile(ROOT.parent / "examples/car-yard/README.md", payload / "README.md")
    add_notices(payload, cocos_root)
    return archive_payload(work, payload, PACKAGE)


def archive_payload(work, payload, package):
    staged = work / "staged"
    staged.mkdir()
    with ZipFile(staged / package, "w", ZIP_DEFLATED) as archive:
        for file in sorted(payload.rglob("*")):
            if not file.is_file():
                continue
            entry = ZipInfo(file.relative_to(payload).as_posix())
            entry.compress_type = ZIP_DEFLATED
            archive.writestr(entry, file.read_bytes())
    digest = sha256(staged / package)
    (staged / MANIFEST).write_text(f"{digest}  {package}\n", encoding="ascii")
    return staged


def smoke(staged, work, verify_play):
    unpack = work / "unpacked"
    with ZipFile(staged / PACKAGE) as archive:
        if archive.testzip() is not None:
            raise ValueError("Package CRC mismatch")
        archive.extractall(unpack)
    for option, expected in (("--smoke", "Cocos2d-x scene rendered"), ("--binding-smoke", "passed")):
        result = subprocess.run([unpack / PAYLOAD[0], option], cwd=unpack, timeout=15, check=True,
                                capture_output=True, text=True, errors="replace",
                                creationflags=subprocess.CREATE_NO_WINDOW)
        if expected not in result.stdout:
            raise RuntimeError(f"Package smoke output did not contain {expected!r}")
        print(result.stdout, end="")
    if verify_play:
        subprocess.run([sys.executable, "-B", ROOT.parent / "tools/verify_yard.py", "play",
                        "--bin", unpack, "--port", "43225"], timeout=40, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cocos-root", type=Path, default=ROOT.parent.parent / "cocos2d-x-4")
    parser.add_argument("--verify-play", action="store_true")
    args = parser.parse_args()
    if not budget(ROOT, 256 * 1024 ** 2)["allowed"]:
        print("Output budget exceeded")
        return 1
    work = create_job(ROOT, "tmp/package-win-x64", "temp")
    published = False
    try:
        with job_lock(work, "temp"):
            staged = assemble(work, args.cocos_root)
            digest = slot_digest(ROOT, staged)
            smoke(staged, work, args.verify_play)
            changed = publish(ROOT, work, staged)
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
