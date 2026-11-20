"""Build the independent Windows x64 core and optional car yard platforms."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from zipfile import ZipFile
import zlib

from artifacts import GIB, ROOT, budget, create_job, item_at, job_lock, owned
from godot_build import GODOT_PROJECT, import_project, resolve_godot, sdk_sources


REPO = ROOT.parent
ARCHIVE = "metal-support-22.zip"
ARCHIVE_URL = ("https://codeload.github.com/cocos2d/cocos2d-x-3rd-party-libs-bin/"
               "zip/refs/tags/metal-support-22")
ARCHIVE_HASH = "721c0588f10c7e445d4b4103efcf3869203fffa9bf7836deb3c6bb51b2f8a764"
SOURCE_ROOT = "cocos2d-x-3rd-party-libs-bin-metal-support-22"
SOURCE_LIBS = {"tinyxml2", "xxhash", "xxtea", "clipper", "edtaa3func", "ConvertUTF",
               "poly2tri", "md5", "unzip", "json", "rapidxml", "tinydir"}
TRIPLET = "x64-windows-static"


def run(*command, **kwargs):
    subprocess.run([str(part) for part in command], check=True, cwd=REPO, **kwargs)


def visual_studio():
    where = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    path = subprocess.check_output(
        [where, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        text=True,
    ).strip()
    if not path:
        raise RuntimeError("Install the Visual Studio Desktop development with C++ workload")
    return Path(path)


def verify_archive(path):
    with path.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != ARCHIVE_HASH:
            raise ValueError(f"Unexpected SHA-256 for {path}")


def vendor_sources(build):
    vendor = build / "vendor"
    vendor.mkdir(exist_ok=True)
    archive = vendor / ARCHIVE
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        run("curl.exe", "-fLsS", "--retry", 2, "--max-time", 240, ARCHIVE_URL, "-o", partial)
        verify_archive(partial)
        partial.replace(archive)
    verify_archive(archive)
    with ZipFile(archive) as zip_file:
        for member in zip_file.infolist():
            parts = member.filename.split("/")
            if len(parts) > 2 and parts[0] == SOURCE_ROOT and parts[1] in SOURCE_LIBS:
                target = item_at(vendor, Path(*parts))
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                elif not target.is_file() or zlib.crc32(target.read_bytes()) != member.CRC:
                    zip_file.extract(member, vendor)
    return vendor / SOURCE_ROOT


def install_dependencies(build, vcpkg):
    env = os.environ.copy()
    downloads = build / "downloads"
    cache = build / "binary-cache"
    for folder in (downloads, cache):
        folder.mkdir(exist_ok=True)
    env.update(VCPKG_DOWNLOADS=str(downloads), VCPKG_BINARY_SOURCES="clear",
               VCPKG_DEFAULT_BINARY_CACHE=str(cache))
    run(vcpkg, "install", "--triplet", TRIPLET,
        f"--x-manifest-root={REPO / 'engineplatform/cocos2dx'}",
        f"--x-install-root={build / 'vcpkg-installed'}",
        f"--x-buildtrees-root={build / 'vcpkg-buildtrees'}",
        f"--x-packages-root={build / 'vcpkg-packages'}", env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cocos", action="store_true")
    parser.add_argument("--godot", action="store_true")
    parser.add_argument("--godot-exe", type=Path)
    parser.add_argument("--cocos-root", type=Path, default=REPO.parent / "cocos2d-x-4")
    parser.add_argument("--parallel", type=int, default=3)
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--vcpkg", type=Path)
    parser.add_argument("--fresh", action="store_true", help="Recreate CMake's configure cache")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("Parallel jobs must be positive")
    expected = 4 * GIB if args.cocos else GIB if args.godot else 128 * 1024 ** 2
    result = budget(ROOT, expected)
    print(json.dumps(result), flush=True)
    if not result["allowed"]:
        return 1

    vs = visual_studio()
    bin_dir = vs / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
    cmake = args.cmake or bin_dir / "cmake.exe"
    if not cmake.is_file():
        cmake = Path(shutil.which("cmake") or cmake)
    ctest = cmake.with_name("ctest.exe")
    config = "Release" if args.cocos or args.godot else "Debug"
    relative = Path("build/win-x64") / config
    build = item_at(ROOT, relative)
    if build.exists():
        owned(ROOT, build, "build")
    else:
        build = create_job(ROOT, relative, "build")
    with job_lock(build, "build"):
        source = REPO / "core"
        options = ["--fresh"] if args.fresh else []
        if args.cocos or args.godot:
            source = REPO
            options += [
                f"-DNETCODE_COCOS={'ON' if args.cocos else 'OFF'}",
                f"-DNETCODE_GODOT={'ON' if args.godot else 'OFF'}",
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>",
            ]
        if args.cocos:
            vcpkg = args.vcpkg or vs / "VC/vcpkg/vcpkg.exe"
            dependencies = vendor_sources(build)
            install_dependencies(build, vcpkg)
            remaining = budget(ROOT, 1024 ** 3)
            print(json.dumps(remaining), flush=True)
            if not remaining["allowed"]:
                return 1
            options += [
                f"-DCOCOS2DX_ROOT={args.cocos_root.resolve()}",
                f"-DNETCODE_COCOS_EXTERNAL={dependencies}",
                f"-DCMAKE_TOOLCHAIN_FILE={vcpkg.parent / 'scripts/buildsystems/vcpkg.cmake'}",
                "-DVCPKG_MANIFEST_MODE=OFF",
                f"-DVCPKG_TARGET_TRIPLET={TRIPLET}",
                f"-DVCPKG_INSTALLED_DIR={build / 'vcpkg-installed'}",
            ]
        if args.godot:
            godot = resolve_godot(args.godot_exe)
            options += [f"-DNETCODE_GODOT_CPP={sdk_sources(build)}", "-DGODOTCPP_TARGET=template_release"]
        run(cmake, "-S", source, "-B", build, "-G", "Visual Studio 18 2026", "-A", "x64", *options)
        run(cmake, "--build", build, "--config", config, "--parallel", args.parallel)
        run(ctest, "--test-dir", build, "-C", config, "--output-on-failure")
        if args.godot:
            import_project(godot, GODOT_PROJECT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
