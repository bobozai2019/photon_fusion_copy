"""Publish and rotate verified SDK archives in the local package slots."""
import hashlib
from pathlib import Path
from artifacts import item_at

PACKAGE = "netcode-core-win-x64.zip"
MANIFEST = "SHA256SUMS.txt"
SLOT = Path("packages/core-win-x64")


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
