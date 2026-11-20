"""Manage generated files under this repository's out directory."""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import stat
import time


ROOT = Path(__file__).resolve().parents[1] / "out"
GIB = 1024 ** 3
DAY = 24 * 60 * 60
RUN_LIMIT = 256 * 1024 ** 2
RECORD = ".netcode-job.json"
LOCK = ".netcode-job.lock"
SLOTS = ("current", "last-pass", "last-fail")


def reparse(info):
    return stat.S_ISLNK(info.st_mode) or (
        getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    )


def item_at(root, relative):
    """Check both the lexical path and every on-disk ancestor."""
    rel = Path(relative)
    if rel.is_absolute() or rel.drive or ".." in rel.parts:
        raise ValueError(f"Path outside output root: {relative}")
    target = root / rel
    for part in (*reversed(target.parents), target):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if reparse(info):
            raise ValueError(f"Reparse point: {part}")
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Resolved path outside output root: {target}")
    return target


def tree_size(path, strict=False):
    item_at(path.parent, path.name)
    return subtree_size(path, strict)


def subtree_size(path, strict):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return 0
    if reparse(info):
        if strict:
            raise ValueError(f"Reparse point: {path}")
        return 0
    if stat.S_ISDIR(info.st_mode):
        return sum(subtree_size(child, strict) for child in path.iterdir())
    return info.st_size


def budget(root, incoming=0, ceiling=16 * GIB, reserve=8 * GIB):
    used = tree_size(root)
    free = shutil.disk_usage(root.parent).free
    allowed = used + incoming <= ceiling and free - incoming >= reserve
    return {"used_bytes": used, "free_bytes": free, "incoming_bytes": incoming,
            "max_bytes": ceiling, "reserved_free_bytes": reserve, "allowed": allowed}


def managed_slot(relative):
    return re.fullmatch(
        r"(?:tmp/[a-zA-Z0-9_-]+|build/win-x64/(?:Debug|Release)"
        r"|runs/(?:current|last-pass|last-fail))", relative.as_posix()
    )


def create_job(root, relative, kind):
    relative = Path(relative)
    if not managed_slot(relative):
        raise ValueError(f"Unmanaged slot: {relative}")
    path = item_at(root, relative)
    path.mkdir(parents=True, exist_ok=False)
    seal_job(path, kind)
    return path


def inventory(path, top=True):
    entries = []
    for child in sorted(path.iterdir()):
        if top and child.name in (RECORD, LOCK):
            continue
        info = child.lstat()
        if reparse(info):
            raise ValueError(f"Reparse point: {child}")
        entries.append(child.name)
        if stat.S_ISDIR(info.st_mode):
            entries.extend(f"{child.name}/{name}" for name in inventory(child, False))
    return entries


def seal_job(path, kind):
    (path / RECORD).write_text(
        json.dumps({"kind": kind, "entries": inventory(path)}), encoding="utf-8"
    )


def owned(root, path, kind):
    rel = path.relative_to(root)
    if not managed_slot(rel):
        raise ValueError(f"Unmanaged slot: {path}")
    record = item_at(root, rel / RECORD)
    if not record.is_file():
        raise ValueError(f"Unknown directory: {path}")
    data = json.loads(record.read_text(encoding="utf-8"))
    if data.get("kind") != kind or data.get("entries") != inventory(path):
        raise ValueError(f"Unregistered contents: {path}")
    return tree_size(path, strict=True)


@contextmanager
def job_lock(path, kind=None):
    lock = path / LOCK
    with lock.open("x", encoding="ascii") as stream:
        stream.write(str(os.getpid()))
    try:
        yield
    finally:
        if kind:
            seal_job(path, kind)
        lock.unlink()


def remove_owned(root, path, kind):
    owned(root, path, kind)
    if (path / LOCK).exists():
        raise ValueError(f"Active job: {path}")
    shutil.rmtree(path)


def clean(root, apply=False, include_build=False):
    tmp = item_at(root, "tmp")
    candidates = [(path, "temp") for path in tmp.iterdir()] if tmp.exists() else []
    candidates += [(root / "runs" / slot, "run") for slot in SLOTS[1:]]
    if include_build:
        candidates += [(root / "build" / "win-x64" / config, "build")
                       for config in ("Debug", "Release")]
    for path, kind in candidates:
        if not path.exists():
            continue
        try:
            item_at(root, path.relative_to(root))
            if kind == "temp" and (path / "rollback-previous").exists():
                raise ValueError("Package recovery files")
            if (path / LOCK).exists():
                raise ValueError("Active job")
            size = owned(root, path, kind)
            if kind != "build" and time.time() - (path / RECORD).stat().st_mtime < DAY:
                raise ValueError("Retained for 24 hours")
        except (OSError, ValueError) as error:
            print(f"skip {path} ({error})")
            continue
        print(f"{'remove' if apply else 'candidate'} {path} {size} bytes ({kind})", flush=True)
        if apply:
            remove_owned(root, path, kind)


def rotate_run(root, passed):
    current = item_at(root, "runs/current")
    target = item_at(root, "runs/last-pass" if passed else "runs/last-fail")
    if owned(root, current, "run") > RUN_LIMIT:
        raise ValueError("Current run exceeds 256 MiB")
    if (current / LOCK).exists():
        raise ValueError(f"Active job: {current}")
    if target.exists():
        remove_owned(root, target, "run")
    current.rename(target)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    usage = sub.add_parser("usage")
    usage.add_argument("--reserve-mib", type=int, default=0, help="Estimated additional output")
    cleanup = sub.add_parser("clean")
    cleanup.add_argument("--apply", action="store_true")
    cleanup.add_argument("--include-build", action="store_true")
    args = parser.parse_args()
    if args.action == "usage":
        if args.reserve_mib < 0:
            parser.error("Estimated additional output must be nonnegative")
        result = budget(ROOT, args.reserve_mib * 1024 ** 2)
        print(json.dumps(result))
        return 0 if result["allowed"] else 1
    clean(ROOT, args.apply, args.include_build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
