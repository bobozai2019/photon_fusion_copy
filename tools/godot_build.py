"""Pinned Godot C++ bindings and shared Windows project discovery."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
from zipfile import ZipFile

from artifacts import item_at


CPP_TAG = "godot-4.5-stable"
CPP_HASH = "0316ea168750b5f4f1caea6457657a6c64241f890f78e2f042aee4be2b1085bf"
CPP_DIRECTORY = f"godot-cpp-{CPP_TAG}"
DEFAULT_GODOT = Path("D:/godot/Godot_v4.7.1-stable/Godot_v4.7.1-stable_win64.exe")


def resolve_godot(requested=None):
    location = requested or os.environ.get("NETCODE_GODOT_EXE") or shutil.which("godot") or DEFAULT_GODOT
    location = Path(location).resolve()
    if not location.is_file():
        raise FileNotFoundError("Set NETCODE_GODOT_EXE or use --godot-exe with the Godot Windows executable")
    return location


def console_godot(godot):
    console = godot.with_name(f"{godot.stem}_console{godot.suffix}")
    return console if console.is_file() else godot


def sdk_sources(build):
    vendor = build / "vendor"
    vendor.mkdir(exist_ok=True)
    archive = vendor / f"{CPP_TAG}.zip"

    def verify(path):
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != CPP_HASH:
                raise ValueError(f"Unexpected godot-cpp archive hash: {path}")

    if not archive.exists():
        partial = archive.with_suffix(".partial")
        url = f"https://codeload.github.com/godotengine/godot-cpp/zip/refs/tags/{CPP_TAG}"
        subprocess.run(["curl.exe", "-fLsS", "--retry", "2", "--max-time", "90", url, "-o", partial],
                       check=True)
        verify(partial)
        partial.replace(archive)
    verify(archive)
    destination = vendor / CPP_DIRECTORY
    marker = vendor / f"{CPP_TAG}.sha256"
    if not marker.exists():
        with ZipFile(archive) as zip_file:
            for member in zip_file.infolist():
                rel = Path(member.filename)
                if not rel.parts or rel.parts[0] != CPP_DIRECTORY:
                    raise ValueError(f"Unexpected godot-cpp archive entry: {member.filename}")
                item_at(vendor, rel)
                zip_file.extract(member, vendor)
        marker.write_text(CPP_HASH, encoding="ascii")
    elif marker.read_text(encoding="ascii") != CPP_HASH:
        raise ValueError("godot-cpp extraction marker mismatch")
    return destination


def import_project(godot, project):
    # Load the native class at startup, before the editor's first script scan.
    cache = project / ".godot"
    cache.mkdir(exist_ok=True)
    extensions = cache / "extension_list.cfg"
    descriptor = "res://addons/netcode/netcode.gdextension"
    original = extensions.read_text(encoding="utf-8").splitlines() if extensions.exists() else []
    entries = [line for line in original if line]
    if descriptor not in entries:
        entries.append(descriptor)
    if entries != original:
        extensions.write_text("\n".join(entries) + "\n", encoding="utf-8")
    result = subprocess.run(
        [console_godot(godot), "--headless", "--path", project, "--editor", "--import", "--language", "en"],
        cwd=project, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    output = result.stdout + result.stderr
    print(output, end="", flush=True)
    if result.returncode or "SCRIPT ERROR" in output or "ERROR:" in output:
        raise RuntimeError(f"Godot project import failed (exit {result.returncode})")
