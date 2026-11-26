"""Verify the native Godot node/resource and room API from an isolated project."""

import argparse
from contextlib import ExitStack, contextmanager
from pathlib import Path
import shutil
import socket
import subprocess
from threading import Thread
from time import monotonic, sleep
from zipfile import ZipFile

from artifacts import ROOT, create_job, item_at, job_lock, remove_owned


def wait_for(path, host):
    deadline = monotonic() + 17
    while monotonic() < deadline and host.poll() is None:
        if path.exists():
            return
        sleep(0.05)
    raise RuntimeError(f"Missing marker {path.name}, host status {host.poll()}")


def install(work, addon):
    if addon.is_dir():
        shutil.copytree(addon, work / "addons/netcode")
        return
    with ZipFile(addon) as archive:
        for name in archive.namelist():
            path = Path(name)
            if path.parts[:2] != ("addons", "netcode"):
                raise ValueError(f"Unexpected SDK entry: {name}")
            item_at(work, path)
        archive.extractall(work)


def verify(work, godot, addon, case, legacy, export):
    install(work, addon)
    for source in (ROOT.parent / "tests/godot_native").iterdir():
        shutil.copy2(source, work)
    orb = (work / "orb.tscn").read_text(encoding="utf-8")
    for name in ("other", "third"):
        (work / (name + ".tscn")).write_text(orb.replace('name="Orb"', f'name="{name.title()}"'), encoding="utf-8")
    extras = {
        "transaction": 'owner_mode = 0\nrpc_methods = PackedStringArray("note_value", "note_remote", "set_user_keys")',
        "key_one": "owner_mode = 0\nuser_keys = PackedInt64Array(1)\ninterest_destroy_on_exit = true",
        "key_two": "owner_mode = 0\nuser_keys = PackedInt64Array(2)",
        "spatial": "owner_mode = 0\nspatial_interest = true\ninterest_destroy_on_exit = true\nroot_replication_mode = 1",
    }
    for name, attributes in extras.items():
        scene = orb.replace("owner_mode = 1", attributes)
        if name == "transaction":
            scene = scene.replace("orb_config.tres", "transaction_config.tres")
        elif name == "key_one":
            scene = scene.replace("orb_config.tres", "proxy_config.tres")
        (work / f"{name}.tscn").write_text(scene, encoding="utf-8")
    mixed_config = (work / "orb_config.tres").read_text(encoding="utf-8").replace(
        'NodePath(":value")', 'NodePath(":value"), NodePath(":trail")')
    (work / "transaction_config.tres").write_text(mixed_config, encoding="utf-8")
    (work / "proxy_config.tres").write_text(mixed_config, encoding="utf-8")
    (work / "detaching_proxy_config.tres").write_text(mixed_config + 'apply_proxy_changes_only = true\n', encoding="utf-8")
    (work / "detaching_proxy.tscn").write_text(orb.replace("orb_config.tres", "detaching_proxy_config.tres"), encoding="utf-8")
    for name in ("fragile", "setter_reject", "signal_reject"):
        scene = orb.replace("orb_config.tres", "transaction_config.tres") if name == "setter_reject" else orb
        (work / f"{name}.tscn").write_text(scene, encoding="utf-8")
    (work / "reference.tscn").write_text(orb.replace("orb_config.tres", "reference_config.tres"), encoding="utf-8")
    (work / "reference_config.tres").write_text((work / "orb_config.tres").read_text(encoding="utf-8")
        .replace('NodePath(":value")', 'NodePath(":value"), NodePath(":target"), NodePath(":levels"), NodePath(":ledger")'),
        encoding="utf-8")
    (work / "server.tscn").write_text(orb.replace("NetcodeSharedReplicator", "NetcodeServerReplicator")
        .replace("owner_mode = 1", "neutral_input = PackedByteArray(0)")
        .replace("orb_config.tres", "server_config.tres")
        .replace('script = ExtResource("1")\n', ""), encoding="utf-8")
    (work / "server_config.tres").write_text((work / "orb_config.tres").read_text(encoding="utf-8")
        .replace('NodePath(":value")', 'NodePath(":value"), NodePath(":smoothed")')
        + 'smooth_properties = Array[NodePath]([NodePath(":smoothed")])\n', encoding="utf-8")
    (work / "project.godot").write_text('config_version=5\n[application]\nrun/main_scene="res://export_runner.tscn"\n'
                                       '[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
    (work / ".godot").mkdir()
    (work / ".godot/extension_list.cfg").write_text("res://addons/netcode/netcode.gdextension\n")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = str(listener.getsockname()[1])
    command = [str(godot), "--headless", "--path", str(work), "--max-fps", "120", "--script", "res://check.gd"]
    advanced = [*command[:-1], "res://advanced.gd"]
    if case == "editor":
        verify_editor(work, godot)
        return
    if export:
        player = export_player(work, godot)
        for scenario in ("lifecycle", "shared", "authority_detach", "simulation"):
            advanced_pair(work, [str(player), "--headless", "--max-fps", "120"], scenario)
        return
    if case == "roster":
        verify_roster(work, command, port)
        return
    if case:
        advanced_pair(work, advanced, case)
        return

    def run(role, endpoint=port):
        result = subprocess.run([*command, "--", role, endpoint], timeout=20, capture_output=True, text=True, encoding="utf-8")
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode or f"NATIVE {role} PASS" not in result.stdout or "SCRIPT ERROR" in result.stderr:
            raise RuntimeError(f"{role} failed with exit {result.returncode}")

    run("nodes")
    verify_editor(work, godot)
    run("unreachable")
    with (work / "host.log").open("wb") as log:
        host = subprocess.Popen([*command, "--", "host", port, str(work)], stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(work / "host.ready", host)
            run("occupied")
            run("wrong_catalog")
            with delayed_link(port, 0.035) as guest_port:
                run("guest", guest_port)
            wait_for(work / "host.updated", host)
            run("late")
            (work / "leave").touch()
            host.wait(timeout=20)
        finally:
            if host.poll() is None:
                host.kill()
                host.wait()
            log.flush()
            trace = (work / "host.log").read_text(encoding="utf-8", errors="replace")
            print(trace, end="", flush=True)
    if host.returncode or "NATIVE host PASS" not in trace or "SCRIPT ERROR" in trace:
        raise RuntimeError(f"host failed with exit {host.returncode}")
    verify_roster(work, command, port)
    for scenario in ("reject", "cancel", "lifecycle", "shared", "authority_detach", "simulation"):
        advanced_pair(work, advanced, scenario)
    if legacy:
        verify_incompatible(work, godot, command, legacy)


def export_player(work, godot):
    source = ROOT.parent.parent / "examples/yard/godot/export_presets.cfg"
    shutil.copy2(source, work / "export_presets.cfg")
    player = work / "player/Native.exe"
    player.parent.mkdir()
    subprocess.run([str(godot), "--headless", "--editor", "--import", "--path", str(work)], check=True, timeout=60)
    subprocess.run([str(godot), "--headless", "--path", str(work), "--export-release", "Windows Desktop", str(player)],
                   check=True, timeout=100)
    if not player.is_file():
        raise RuntimeError("Native export did not produce a player")
    return player


def verify_roster(work, command, port):
    folder = work / "roster"
    folder.mkdir()
    with ExitStack() as stack:
        def start(role):
            log = folder / f"{role}.log"
            stream = stack.enter_context(log.open("wb"))
            process = subprocess.Popen([*command, "--", role, port, str(folder)], stdout=stream,
                                       stderr=subprocess.STDOUT)

            def stop():
                if process.poll() is None:
                    process.kill()
                    process.wait()

            stack.callback(stop)
            return process, log

        host, host_log = start("roster_host")
        wait_for(folder / "roster.host", host)
        observer, observer_log = start("roster_observer")
        wait_for(folder / "observer.paused", observer)
        visitor, visitor_log = start("roster_visitor")
        for role, process, log in (("roster_visitor", visitor, visitor_log), ("roster_observer", observer, observer_log),
                                   ("roster_host", host, host_log)):
            status = process.wait(timeout=19)
            trace = log.read_text(encoding="utf-8", errors="replace")
            print(trace, end="", flush=True)
            if status or f"NATIVE {role} PASS" not in trace or "ERROR:" in trace:
                raise RuntimeError(f"{role} failed with exit {status}")


def verify_incompatible(work, godot, command, legacy):
    project = work / "legacy"
    project.mkdir()
    install(project, legacy)
    shutil.copy2(work / "legacy_host.gd", project)
    (project / "project.godot").write_text('config_version=5\n')
    (project / ".godot").mkdir()
    (project / ".godot/extension_list.cfg").write_text("res://addons/netcode/netcode.gdextension\n")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = str(listener.getsockname()[1])
    ready = project / "ready"
    host = subprocess.Popen([str(godot), "--headless", "--path", str(project), "--script",
                             "res://legacy_host.gd", "--", port, str(ready)])
    try:
        wait_for(ready, host)
        result = subprocess.run([*command, "--", "incompatible", port], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=18)
        print(result.stdout + result.stderr, flush=True)
        if result.returncode or "NATIVE incompatible PASS" not in result.stdout or "ERROR:" in result.stderr:
            raise RuntimeError("The new client did not reject the old handshake")
        ready.with_suffix(".done").touch()
        if host.wait(timeout=7):
            raise RuntimeError("Legacy host exited with an error")
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
    verify_native_version_pair(godot, project, work)
    verify_native_version_pair(godot, work, project)


def verify_native_version_pair(godot, host_project, guest_project):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = str(listener.getsockname()[1])
    marker = host_project / "native.ready"

    def command(project, role):
        return [str(godot), "--headless", "--path", str(project), "--script", "res://legacy_host.gd",
                "--", port, str(marker), role]

    host = subprocess.Popen(command(host_project, "host"))
    try:
        try:
            wait_for(marker, host)
        except RuntimeError:
            if host.poll() == 77:
                return
            raise
        guest = subprocess.run(command(guest_project, "guest"), timeout=18, capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
        if guest.returncode != 77:
            print(guest.stdout + guest.stderr, end="", flush=True)
            if guest.returncode or "NATIVE MIXED REJECTED" not in guest.stdout or "ERROR:" in guest.stderr:
                raise RuntimeError("Mixed native protocol versions incorrectly admitted")
        marker.with_suffix(".ready.done").touch()
        if host.wait(timeout=7):
            raise RuntimeError("Native version probe host exited with an error")
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()


@contextmanager
def delayed_link(port, delay):
    if not delay:
        yield port
        return
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(3)

        def transfer(source, destination):
            try:
                while data := source.recv(8192):
                    sleep(delay)
                    destination.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    destination.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        def relay():
            try:
                guest, _ = listener.accept()
                with guest, socket.create_connection(("127.0.0.1", int(port))) as host:
                    upstream = Thread(target=transfer, args=(guest, host), daemon=True)
                    upstream.start()
                    transfer(host, guest)
                    upstream.join(timeout=2)
            except OSError:
                pass

        bridge = Thread(target=relay, daemon=True)
        bridge.start()
        yield str(listener.getsockname()[1])
        bridge.join(timeout=2)


def advanced_pair(work, command, case):
    folder = work / case
    folder.mkdir()
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = str(listener.getsockname()[1])
    host_role, guest_role = f"{case}_host", f"{case}_guest"
    with (folder / "host.log").open("wb") as log:
        host = subprocess.Popen([*command, "--", host_role, port, str(folder)], stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(folder / "host.ready", host)
            with delayed_link(port, 0.035 if case == "cancel" else 0) as guest_port:
                guest = subprocess.run([*command, "--", guest_role, guest_port], timeout=35,
                                       capture_output=True, text=True, encoding="utf-8", errors="replace")
            print(guest.stdout + guest.stderr, end="", flush=True)
            if guest.returncode or f"NATIVE {guest_role} PASS" not in guest.stdout or "ERROR:" in guest.stderr:
                raise RuntimeError(f"{guest_role} failed with exit {guest.returncode}")
            (folder / "leave").touch()
            host.wait(timeout=20)
        finally:
            if host.poll() is None:
                host.kill()
                host.wait()
            trace = (folder / "host.log").read_text(encoding="utf-8", errors="replace")
            print(trace, end="", flush=True)
    if host.returncode or f"NATIVE {host_role} PASS" not in trace or "ERROR:" in trace:
        raise RuntimeError(f"{host_role} failed with exit {host.returncode}")


def verify_editor(work, godot):
    settings = work / "project.godot"
    project = settings.read_bytes()
    config = work / "orb_config.tres"
    saved = config.read_bytes()
    probe = work / "addons/probe"
    probe.mkdir()
    shutil.copy2(work / "editor.gd", probe)
    (probe / "plugin.cfg").write_text(
        '[plugin]\nname="Native editor probe"\nscript="editor.gd"\n')
    settings.write_bytes(project + b'\n[editor_plugins]\nenabled=PackedStringArray("res://addons/probe/plugin.cfg")\n')
    try:
        result = subprocess.run([str(godot), "--headless", "--editor", "--path", str(work),
                                 "--max-fps", "60", "--quit-after", "600"],
                                timeout=24, capture_output=True, text=True, encoding="utf-8", errors="replace")
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode or "NATIVE editor PASS" not in result.stdout or "ERROR:" in result.stderr:
            raise RuntimeError(f"editor failed with exit {result.returncode}")
    finally:
        settings.write_bytes(project)
        config.write_bytes(saved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, default=ROOT.parent.parent /
                        "examples/sync-lab/runtime/godot-4.6.1/Godot_v4.6.1-stable_win64_console.exe")
    parser.add_argument("--addon", type=Path, default=ROOT /
                        "install/win-x64-msvc-mt/Release/godot/addons/netcode")
    parser.add_argument("--case", choices=("editor", "reject", "cancel", "lifecycle", "roster", "shared", "authority_detach", "simulation"))
    parser.add_argument("--legacy-sdk", type=Path, help="Verify the new native handshake against an old SDK")
    parser.add_argument("--export", action="store_true", help="Verify exported native project using installed templates")
    args = parser.parse_args()
    work = create_job(ROOT, Path("tmp/verify-godot-native"), "test")
    try:
        with job_lock(work, "test"):
            verify(work, args.godot.resolve(), args.addon.resolve(), args.case, args.legacy_sdk, args.export)
    finally:
        remove_owned(ROOT, work, "test")


if __name__ == "__main__":
    main()
