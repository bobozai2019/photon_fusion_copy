"""Build, install and optionally package one Windows x64 SDK and its Core dependency."""
import argparse
from contextlib import contextmanager
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

from artifacts import GIB, ROOT, budget, create_job, item_at, job_lock, owned, remove_owned
from godot_build import CPP_TAG, sdk_sources
from sdk_packages import publish, sha256

REPO = ROOT.parent
SOURCES = {"core": "core", "godot": "engineplatform/godot",
           "cocos": "engineplatform/cocos2dx", "unity": "engineplatform/unity"}


def cmake_executable():
    path = shutil.which("cmake")
    if path:
        return Path(path)
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([vswhere, "-latest", "-products", "*", "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                  "-property", "installationPath"], text=True).strip()
    return Path(vs) / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"


@contextmanager
def sdk_lock():
    path = item_at(ROOT, ".sdk-build.lock")
    try:
        stream = path.open("x")
    except FileExistsError:
        raise SystemExit(f"An SDK build owns {path}. Check its PID before recovering an interrupted build.")
    try:
        with stream:
            stream.write(str(os.getpid()))
        yield
    finally:
        path.unlink()


def archive(prefix, output, product):
    files = sorted(p for p in prefix.rglob("*") if p.is_file())
    if product == "unity":
        with output.open("wb") as stream, gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w") as tgz:
                for path in files:
                    info = tarfile.TarInfo("package/" + path.relative_to(prefix).as_posix())
                    info.size = path.stat().st_size
                    with path.open("rb") as member:
                        tgz.addfile(info, member)
        with tarfile.open(output) as tgz:
            for path, member in zip(files, tgz.getmembers(), strict=True):
                if hashlib.file_digest(tgz.extractfile(member), "sha256").hexdigest() != sha256(path):
                    raise ValueError(f"Archive verification failed: {member.name}")
    else:
        with ZipFile(output, "w") as z:
            for path in files:
                info = ZipInfo(path.relative_to(prefix).as_posix())
                z.writestr(info, path.read_bytes(), compress_type=ZIP_DEFLATED)
        with ZipFile(output) as z:
            if z.testzip() is not None:
                raise ValueError(f"Invalid archive: {output}")


def package(prefix, product, config, work):
    suffix = "tgz" if product == "unity" else "zip"
    tag = "" if config == "Release" else "-debug"
    name = f"netcode-{product}-win-x64{tag}.{suffix}"
    staged = work / "staged"
    staged.mkdir()
    archive(prefix, staged / name, product)
    (staged / "SHA256SUMS.txt").write_text(f"{sha256(staged / name)}  {name}\n", encoding="ascii")
    slot = Path(f"packages/{product}-win-x64{tag}")
    publish(ROOT, work, staged, slot=slot, package=name)
    print(ROOT / slot / "current" / name, flush=True)


def sync_install(staging, destination, manifest):
    """Refresh only files owned by this SDK. Unrecognized extra files stay in place."""
    files = sorted(p.relative_to(staging) for p in staging.rglob("*") if p.is_file())
    old = json.loads(manifest.read_text(encoding="utf-8")) if manifest.exists() else {}
    hashes = {}
    for rel in files:
        src = staging / rel
        out = item_at(destination, rel)
        digest = sha256(src)
        if not out.is_file() or sha256(out) != digest:
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, out)
        hashes[rel.as_posix()] = digest
    for key in old.keys() - hashes.keys():
        orphan = item_at(destination, key)
        if orphan.is_file() and sha256(orphan) == old[key]:
            orphan.unlink()
    pending = manifest.with_suffix(".tmp")
    pending.write_text(json.dumps(hashes, ensure_ascii=False, indent=2), encoding="utf-8")
    pending.replace(manifest)


def build(product, args, install, cmake):
    config = args.config
    result = budget(ROOT, GIB if product == "godot" else 256 * 1024**2)
    if not result["allowed"]:
        raise RuntimeError(f"Output space budget exceeded: {result}")
    build_dir = item_at(ROOT, Path("build/win-x64/sdk") / product / config)
    if build_dir.exists():
        owned(ROOT, build_dir, "build")
    else:
        create_job(ROOT, build_dir.relative_to(ROOT), "build")
    with job_lock(build_dir, "build"):
        build_and_install(product, args, install, cmake, config, build_dir)


def build_and_install(product, args, install, cmake, config, build_dir):
    destination = install / product
    flags = [f"-DCMAKE_CONFIGURATION_TYPES={config}", f"-DCMAKE_INSTALL_PREFIX={destination}",
             "-DCMAKE_MSVC_RUNTIME_LIBRARY=" + ("MultiThreadedDebug" if config == "Debug" else "MultiThreaded")]
    if product == "core":
        flags.append("-DBUILD_TESTING=ON")
        flags.append("-DNETCODE_STEAM=" + ("ON" if args.steam_sdk else "OFF"))
        if args.steam_sdk:
            flags.append(f"-DNETCODE_STEAMWORKS_ROOT={args.steam_sdk.resolve()}")
    else:
        core = (args.core_prefix or install / "core").resolve()
        core_config = core / "lib/cmake/NetcodeCore"
        if not (core_config / "NetcodeCoreConfig.cmake").is_file():
            raise FileNotFoundError(f"Missing installed Core SDK: {core}")
        flags.append(f"-DNetcodeCore_DIR={core_config}")
    if product == "godot":
        cpp = args.godot_cpp
        if not cpp:
            legacy = ROOT / "build/win-x64/Release"
            default_cache = legacy if (legacy / "vendor" / f"{CPP_TAG}.zip").is_file() else ROOT / "cache"
            cache = Path(os.environ.get("NETCODE_DEPS_CACHE", default_cache)).resolve()
            cache.mkdir(parents=True, exist_ok=True)
            cpp = sdk_sources(cache)
        flags += [f"-DNETCODE_GODOT_CPP={cpp.resolve()}", "-DGODOTCPP_TARGET=" +
                  ("template_debug" if config == "Debug" else "template_release")]
    elif product == "cocos":
        flags += [f"-DCOCOS2DX_ROOT={args.cocos_root.resolve()}",
                  f"-DNETCODE_COCOS_PHYSICS2D={args.physics2d}", f"-DNETCODE_COCOS_PHYSICS3D={args.physics3d}"]
    configure = [str(cmake), "-S", str(REPO / SOURCES[product]), "-B", str(build_dir),
                 "-G", "Visual Studio 18 2026", "-A", "x64", *flags]
    signature = build_dir / "sdk-configure.json"
    if not (build_dir / "CMakeCache.txt").exists() or not signature.exists() or json.loads(signature.read_text(encoding="utf-8")) != configure:
        subprocess.run(configure, check=True)
        signature.write_text(json.dumps(configure), encoding="utf-8")
    subprocess.run([cmake, "--build", build_dir, "--config", config, "--parallel", str(args.jobs)], check=True)
    if product == "core":
        subprocess.run([cmake.with_name("ctest.exe"), "--test-dir", build_dir, "-C", config, "--output-on-failure"], check=True)
    work = create_job(ROOT, f"tmp/sdk-{product}", "temp")
    completed = False
    try:
        with job_lock(work, "temp"):
            staging = work / "install"
            subprocess.run([cmake, "--install", build_dir, "--config", config, "--prefix", staging], check=True)
            destination.mkdir(parents=True, exist_ok=True)
            sync_install(staging, destination, build_dir / "sdk-install.json")
            if args.package:
                package(staging, product, config, work)
            completed = True
    finally:
        if not completed and (work / "rollback-previous").exists():
            print(f"Retained package rollback at {work / 'rollback-previous'}", flush=True)
        else:
            remove_owned(ROOT, work, "temp")
    print(f"{product}: {destination}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("product", nargs="?", default="core", choices=[*SOURCES, "all"])
    parser.add_argument("--config", choices=["Release", "Debug"], default="Release")
    parser.add_argument("--core-prefix", type=Path, help="Use this external Core SDK without rebuilding it")
    parser.add_argument("--package", action="store_true", help="Publish a checked ZIP or Unity UPM tarball")
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--godot-cpp", type=Path, help="Already extracted godot-cpp 4.5 source")
    parser.add_argument("--cocos-root", type=Path, default=REPO.parent / "cocos2d-x-4")
    parser.add_argument("--physics2d", choices=["ON", "OFF"], default="ON")
    parser.add_argument("--physics3d", choices=["ON", "OFF"], default="ON")
    parser.add_argument("--steam-sdk", type=Path, help="Enable optional Steam transport in Core")
    args = parser.parse_args()
    if args.jobs < 1 or (args.core_prefix and args.product in ("all", "core")):
        parser.error("Use positive --jobs and reserve --core-prefix for a single platform")
    ROOT.mkdir(exist_ok=True)
    install = item_at(ROOT, Path("install/win-x64-msvc-mt") / args.config)
    products = list(SOURCES) if args.product == "all" else [args.product]
    if not args.core_prefix and "core" not in products:
        products.insert(0, "core")
    cmake = args.cmake or cmake_executable()
    with sdk_lock():
        for product in products:
            build(product, args, install, cmake)


if __name__ == "__main__":
    main()
