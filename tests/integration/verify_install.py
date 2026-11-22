"""Test the released Core archive from a relocated, standalone CMake consumer."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
from zipfile import ZipFile
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
from build import cmake_executable
from room_processes import run


def verify(archive, cmake):
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if [digest, archive.name] not in [row.split() for row in archive.with_name("SHA256SUMS.txt").read_text().splitlines()]:
        raise ValueError("Core package checksum mismatch")
    with TemporaryDirectory(prefix="netcode-core-consumer-") as tmp:
        root = Path(tmp)
        sdk = root / "relocated sdk"
        with ZipFile(archive) as z:
            z.extractall(sdk)
        src = root / "app"
        src.mkdir()
        for name in ("CMakeLists.txt", "room_probe.cpp"):
            shutil.copy2(HERE / name, src / name)
        args = [cmake, "-S", src, "-G", "Visual Studio 18 2026", "-A", "x64",
                f"-DNetcodeCore_DIR={(sdk/'lib/cmake/NetcodeCore').as_posix()}", "-DCMAKE_CONFIGURATION_TYPES=Release"]
        good = root / "good"
        subprocess.run([*args, "-B", good, "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"], check=True)
        subprocess.run([cmake, "--build", good, "--config", "Release"], check=True)
        for port, mode in ((43802, "rounds"), (43803, "timeout")):
            run(good / "Release/room_probe.exe", port, mode)
        bad = root / "wrong-crt"
        subprocess.run([*args, "-B", bad, "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"], check=True)
        result = subprocess.run([cmake, "--build", bad, "--config", "Release"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode == 0 or b"NetcodeCore CRT mismatch" not in result.stdout:
            raise AssertionError("Expected an explicit CRT mismatch diagnostic")
        print("PASS relocated Core, two-process room rounds/timeout and /MD rejection")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    verify(parser.parse_args().archive.resolve(), cmake_executable())
