"""Build a clean Windows SDK consumer and exercise both providers in the same executable."""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tarfile
import time
from uuid import uuid4
from zipfile import ZipFile

FIXTURES = Path(__file__).resolve().parent / "players"
TCP_NAME = "netcode_transport_tcp.dll"
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
GODOT_PRESET = """[preset.0]
name="Windows Desktop"
platform="Windows Desktop"
runnable=true
export_filter="all_resources"
include_filter="consumer.cfg"
exclude_filter=""
export_path=""
script_export_mode=2

[preset.0.options]
debug/export_console_wrapper=0
binary_format/embed_pck=true
binary_format/architecture="x86_64"
application/modify_resources=false
codesign/enable=false
"""


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def archive(root, product):
    suffix = "tgz" if product == "unity" else "zip"
    path = root / f"{product}-win-x64/current/netcode-{product}-win-x64.{suffix}"
    assert [digest(path), path.name] in [line.split() for line in path.with_name("SHA256SUMS.txt").read_text().splitlines()]
    print(f"ARCHIVE {product} {digest(path)}", flush=True)
    return path


def export_godot(args, work):
    project = work / "project"
    project.mkdir()
    with ZipFile(archive(args.packages, "godot")) as package:
        package.extractall(project)
    expected = (project / "addons/netcode/build-id.txt").read_text().strip()
    for file in ("consumer.cfg", "consumer.gd"):
        shutil.copy2(FIXTURES / file, project / file)
    (project / "project.godot").write_text(
        'config_version=5\n[application]\nconfig/name="Transport Consumer"\n'
        'run/main_scene="res://consumer.tscn"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
    (project / "consumer.tscn").write_text(
        '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://consumer.gd" id="1"]\n'
        '[node name="Consumer" type="Node"]\nscript=ExtResource("1")\n')
    (project / "export_presets.cfg").write_text(GODOT_PRESET, encoding="utf-8")
    cache = project / ".godot"
    cache.mkdir()
    (cache / "extension_list.cfg").write_text("res://addons/netcode/netcode.gdextension\n", encoding="utf-8")
    player = work / "player/Consumer.exe"
    player.parent.mkdir()
    prefix = [args.godot, "--headless", "--path", project]
    with (work / "editor.log").open("w") as log:
        subprocess.run([*prefix, "--editor", "--import", "--language", "en"], cwd=project,
                       stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True, creationflags=CREATE_NO_WINDOW)
        subprocess.run([*prefix, "--export-release", "Windows Desktop", player],
                       cwd=project, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True,
                       creationflags=CREATE_NO_WINDOW)
    return player, expected, player.parent / "netcode_godot.dll", player.parent / TCP_NAME


def export_unity(args, work):
    project = work / "project"
    assets = project / "Assets"
    for directory in (assets / "Editor", assets / "StreamingAssets", project / "Packages", project / "ProjectSettings"):
        directory.mkdir(parents=True)
    source = archive(args.packages, "unity")
    local = work / source.name
    shutil.copy2(source, local)
    with tarfile.open(local) as package:
        expected = package.extractfile("package/build-id.txt").read().decode().strip()
    for file, destination in (("ProviderConsumer.cs", assets), ("ProviderConsumerBuild.cs", assets / "Editor"),
                              ("consumer.cfg", assets / "StreamingAssets")):
        shutil.copy2(FIXTURES / file, destination / file)
    dependencies = {"com.netcode.runtime": "file:" + local.as_posix(),
                    "com.unity.modules.physics": "1.0.0", "com.unity.modules.physics2d": "1.0.0"}
    (project / "Packages/manifest.json").write_text(json.dumps({"dependencies": dependencies}), encoding="utf-8")
    (project / "ProjectSettings/ProjectVersion.txt").write_text("m_EditorVersion: 6000.4.0f1\n", encoding="utf-8")
    player = work / "player/Consumer.exe"
    player.parent.mkdir()
    subprocess.run([args.unity, "-batchmode", "-nographics", "-quit", "-projectPath", project, "-buildTarget", "Win64",
                    "-executeMethod", "ProviderConsumerBuild.Export", "-sdkOutput", player, "-logFile", work / "editor.log"],
                   timeout=900, check=True, creationflags=CREATE_NO_WINDOW)
    plugins = player.parent / "Consumer_Data/Plugins/x86_64"
    return player, expected, plugins / "netcode_unity.dll", plugins / TCP_NAME


def build_cocos(args, work):
    sdk = work / "relocated SDK"
    for product in ("core", "cocos"):
        with ZipFile(archive(args.packages, product)) as package:
            package.extractall(sdk / product)
    source = work / "consumer"
    shutil.copytree(FIXTURES / "cocos", source)
    shutil.copy2(FIXTURES / "consumer.cfg", source)
    build = work / "build"
    subprocess.run([args.cmake, "-S", source, "-B", build, "-G", "Visual Studio 18 2026", "-A", "x64",
                    "-DCMAKE_CONFIGURATION_TYPES=Release", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                    f"-DNETCODE_HOST_TARGETS={args.cocos_host_targets.resolve()}",
                    f"-DNetcodeCore_DIR={sdk / 'core/lib/cmake/NetcodeCore'}",
                    f"-DNetcodeCocos_DIR={sdk / 'cocos/lib/cmake/NetcodeCocos'}"], check=True)
    subprocess.run([args.cmake, "--build", build, "--config", "Release"], check=True)
    player = build / "Release/Consumer.exe"
    expected = (player.parent / "sdk-build-id.txt").read_text().strip()
    return player, expected, player, player.parent / TCP_NAME


def stop(process):
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=10)


def launch(stack, engine, player, log, arguments):
    if engine == "godot":
        switches = ["--headless", "--"]
    elif engine == "unity":
        switches = ["-batchmode", "-nographics", "-logFile", log, "--"]
    else:
        switches = ["--headless"]
    output = subprocess.DEVNULL if engine == "unity" else stack.enter_context(log.open("wb"))
    process = subprocess.Popen([player, *switches, *arguments], cwd=log.parent, stdout=output,
                               stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
    stack.callback(stop, process)
    return process


def require_line(process, log, expected, timeout=35):
    deadline = time.monotonic() + timeout
    while True:
        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        for line in text.splitlines():
            if expected in line:
                print(line, flush=True)
                return
        if time.monotonic() >= deadline or process.poll() is not None:
            raise RuntimeError(f"{log.name}: missing {expected!r}, exit={process.poll()}\n{text[-6000:]}")
        time.sleep(0.05)


def tcp_endpoint():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return "{}:{}".format(*sock.getsockname())


def verify_run(args, work, player, platform, expected, kind, module=None):
    room = work / kind
    room.mkdir()
    address = tcp_endpoint() if kind == "tcp" else "accept-" + uuid4().hex
    ticket = room / "seat.bin"
    provider = [] if kind == "tcp" else ["--provider", kind, "--module", module]
    common = ["--endpoint", address, "--ticket", ticket, *provider]
    sdk = f"OPEN sdk={expected} provider={kind}/"
    hashes = digest(player), digest(platform)
    print(f"BINARY {kind} player={hashes[0]} platform={hashes[1]}", flush=True)
    with ExitStack() as processes:
        host_log = room / "host.log"
        host = launch(processes, args.engine, player, host_log, ["--host", *common])
        require_line(host, host_log, sdk)
        for attempt in (0, 1):
            guest_log = room / f"guest-{attempt}.log"
            guest = launch(processes, args.engine, player, guest_log, [*common, *(["--resume"] if attempt else [])])
            require_line(guest, guest_log, sdk)
            require_line(guest, guest_log, f"GUEST_PASS fields=8 player=2 resumed={attempt}")
            require_line(host, host_log, f"HOST_RPC phase={attempt + 1} player=2")
            assert guest.wait(timeout=10) == 0
            if attempt == 0:
                assert ticket.stat().st_size == 36
                time.sleep(0.3)
        require_line(host, host_log, "HOST_PASS")
        assert host.wait(timeout=10) == 0
    ticket.unlink()
    assert hashes == (digest(player), digest(platform)), "Player or platform binary changed during provider swap"
    return hashes


def verify_failure(args, work, player, name, module, message):
    log = work / f"{name}.log"
    with ExitStack() as processes:
        process = launch(processes, args.engine, player, log,
                         ["--host", "--endpoint", tcp_endpoint(), "--module", module, "--expect-error", message])
        require_line(process, log, "OPEN_ERROR")
        assert process.wait(timeout=10) == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", choices=("godot", "unity", "cocos"))
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--external-dll", type=Path, required=True)
    parser.add_argument("--bad-abi", type=Path)
    parser.add_argument("--godot", type=Path)
    parser.add_argument("--unity", type=Path)
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--cocos-host-targets", type=Path)
    args = parser.parse_args()
    if args.engine == "godot" and not args.godot:
        parser.error("Godot requires --godot")
    if args.engine == "unity" and not args.unity:
        parser.error("Unity requires --unity")
    if args.engine == "cocos" and not (args.cmake and args.cocos_host_targets):
        parser.error("Cocos requires --cmake and --cocos-host-targets")
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=False)
    build = {"godot": export_godot, "unity": export_unity, "cocos": build_cocos}[args.engine]
    player, expected, platform, tcp = build(args, args.work)
    print(f"PROVIDER_SHA tcp={digest(tcp)} example-pipe={digest(args.external_dll)}", flush=True)
    a = verify_run(args, args.work, player, platform, expected, "tcp")
    b = verify_run(args, args.work, player, platform, expected, "example-pipe", args.external_dll.resolve())
    assert a == b
    verify_failure(args, args.work, player, "missing", args.work / "absent.dll", "Cannot load transport tcp")
    if args.bad_abi:
        verify_failure(args, args.work, player, "abi", args.bad_abi.resolve(), "ABI v1 negotiation failed")
    print(f"PASS {args.engine}: same player and platform hashes on both providers", flush=True)


if __name__ == "__main__":
    main()
