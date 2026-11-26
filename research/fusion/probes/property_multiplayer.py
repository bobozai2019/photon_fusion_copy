"""Run installed Godot property rooms through a measured TCP relay."""

import argparse
import asyncio
from functools import partial
import json
from pathlib import Path
import socket
import struct
from textwrap import dedent
import time
from zipfile import ZipFile

from property_profile import replace_once
from rigid_scale import ADDON, HERE, ROOT, budget, create_job, job_lock, local_trial, native_setup, open_trials, remove_owned

GAME_LANE = 1
AUTHORITY_STATE = 67
GAME_HEADER_SIZE = 17
AUTHORITY_VALUE_OFFSET = GAME_HEADER_SIZE + 16


def common_actor(source):
    source = replace_once(source, "var number := 0", """var number := 0:
        set(value):
            number = value
            if value > 0: index = ((value - 1) & 0xffffffff)""")
    source = replace_once(source, "var index := 0", "var index := -1")
    source = replace_once(source, "var list: Array[int] = []", "var list := PackedInt32Array()")
    source = replace_once(source, "var dictionary: Dictionary = {}", "var series := PackedFloat32Array()")
    source = source.replace("(wave << 34) + index", "(wave << 34) + index + 1")
    source = source.replace("(WAVES << 34) + index", "(WAVES << 34) + index + 1")
    source = source.replace("list = [index, wave, -wave]", "list = PackedInt32Array([index, wave, -wave])")
    source = source.replace('dictionary = {"index": index, "wave": wave}', 'series = PackedFloat32Array([index, wave])')
    return (source.replace("list == [index, WAVES, -WAVES]", "list == PackedInt32Array([index, WAVES, -WAVES])")
            .replace('dictionary == {"index": index, "wave": WAVES}', 'series == PackedFloat32Array([index, WAVES])'))


def fusion_files(source, files):
    actor = dedent(source.split("class Actor extends Node3D:\n", 1)[1].split("\nclass Predicted", 1)[0])
    actor = ("extends Node3D\n\nconst PLAYERS := 8\nconst DELEGATED := 24\nconst WAVES := 60\n"
             "static var test_player := 0\nstatic var received_ages := PackedInt32Array()\n"
             "static var version_errors := 0\n" + actor)
    actor = actor.replace("owner_for(index) != player", "index >= 0 and owner_for(index) != test_player")
    actor = actor.replace("errors += 1", "version_errors += 1").replace("ages.append(", "received_ages.append(")
    fields = ("number", "scalar", "enabled", "label", "vector", "list", "series", "sample")
    for area in range(3):
        config = ["[gd_scene format=3]", '[ext_resource type="Script" path="res://actor.gd" id="Actor"]',
                  '[sub_resource type="FusionReplicationConfig" id="Config"]']
        for i, field in enumerate(fields):
            config.append(f'properties/{i}/path = NodePath(".:{field}")')
            if field in ("list", "series"):
                config.append(f"properties/{i}/max_array_capacity = 4")
        config.extend(['[node name="Actor" type="Node3D"]', 'script = ExtResource("Actor")',
                       '[node name="Sync" type="FusionSharedReplicator" parent="."]',
                       "owner_mode = 1", "root_replication_mode = 0",
                       'replication_config = SubResource("Config")'])
        if area:
            config.extend(["interest_mode = 2", f"interest_key = {area}", "interest_destroy_on_exit = true"])
        files[f"area{area}.tscn"] = "\n".join(config) + "\n"
    files["actor.gd"] = actor
    files["main.gd"] = (HERE / "property_mixed_fusion.gd").read_text(encoding="utf-8")


def project_files(scenario, web, profile_zip=None, portable=False, narrow_tcp_queue=False):
    files = {
        "main.gd": (HERE / f"property_{scenario}.gd").read_text(encoding="utf-8"),
        "main.tscn": ('[gd_scene format=3]\n[ext_resource type="Script" path="res://main.gd" id="Main"]\n'
                      '[node name="Main" type="Node"]\nscript = ExtResource("Main")\n'),
    }
    if portable or web:
        files["main.gd"] = common_actor(files["main.gd"]).replace('^":dictionary"', '^":series"')
    if web:
        fusion_files(files["main.gd"], files)
        return files
    if narrow_tcp_queue:
        files["main.gd"] = replace_once(files["main.gd"], "% port, PLAYERS)",
            '% port, PLAYERS, [], false, "tcp", "", "queue_bytes=65538".to_utf8_buffer())')
    if profile_zip:
        base = "addons/netcode/replicated_object.gd"
        replicator_path = "addons/netcode/scene_replicator.gd"
        room_path = "addons/netcode/scene_room.gd"
        with ZipFile(profile_zip) as archive:
            script = archive.read(base).decode("utf-8").replace("\r\n", "\n")
            replicator = archive.read(replicator_path).decode("utf-8").replace("\r\n", "\n")
            room = archive.read(room_path).decode("utf-8").replace("\r\n", "\n")
        meter = 'const Meter = preload("res://mixed_meter.gd")\n'
        files["main.gd"] = replace_once(files["main.gd"], "extends Node\n", "extends Node\n" + meter)
        script = replace_once(script, "extends Node\n", "extends Node\n" + meter)
        script = replace_once(script, "    var sampled := _sample_for_sync()",
                              "    var begin := Time.get_ticks_usec()\n    var sampled := _sample_for_sync()\n"
                              "    var sampled_at := Time.get_ticks_usec()")
        script = replace_once(script, "    if change.is_empty():",
                              "    var synced_at := Time.get_ticks_usec()\n    if Meter.active:\n"
                              "        Meter.samples += 1\n        Meter.sample_us += sampled_at - begin\n"
                              "        Meter.sync_us += synced_at - sampled_at\n    if change.is_empty():")
        files[base] = replace_once(script, "    if not initialized: game.unbind_replica(object_id)",
                                   "    if not initialized: game.unbind_replica(object_id)\n"
                                   "    if Meter.active: Meter.apply_us += Time.get_ticks_usec() - synced_at")
        marker = "    if replication_root() == null:\n        unbind()"
        replicator = replace_once(replicator, marker, "    var route_begin := Time.get_ticks_usec()\n" + marker)
        files[replicator_path] = replace_once(replicator, "    super._physics_process(dt)",
                                              "    if Meter.active: Meter.route_us += Time.get_ticks_usec() - route_begin\n"
                                              "    super._physics_process(dt)")
        room = replace_once(room, "extends RefCounted\n", "extends RefCounted\n" + meter)
        files[room_path] = replace_once(room, "    var frame:=game.advance()",
                                        "    var advance_begin := Time.get_ticks_usec()\n    var frame:=game.advance()\n"
                                        "    if Meter.active: Meter.advance_native_us += Time.get_ticks_usec() - advance_begin")
        files["main.gd"] = replace_once(files["main.gd"], "        start_us = now",
                                        "        Meter.active = true\n        start_us = now")
        files["main.gd"] = replace_once(files["main.gd"], "        write_end_us = Time.get_ticks_usec()",
                                        "        Meter.active = false\n        write_end_us = Time.get_ticks_usec()")
        files["main.gd"] = replace_once(files["main.gd"], '    emit("final",',
                                        '    emit("profile", [Meter.samples, Meter.sample_us, Meter.sync_us, Meter.apply_us,\n'
                                        '        Meter.route_us, Meter.advance_native_us])\n'
                                        '    emit("final",')
        files["mixed_meter.gd"] = ("extends RefCounted\nstatic var active := false\nstatic var samples := 0\n"
                                  "static var sample_us := 0\nstatic var sync_us := 0\nstatic var apply_us := 0\n"
                                  "static var route_us := 0\nstatic var advance_native_us := 0\n")
    return files


async def forward(reader, writer, stats, delay, hold=None, throttle=None):
    pending = asyncio.Queue(maxsize=512)
    dequeued = asyncio.Event() if throttle else None
    loop = asyncio.get_running_loop()

    async def read_frames():
        try:
            while True:
                # Keep the slow interval close to the actual socket. Once the
                # cap is lifted, allow the ordinary propagation-delay pipeline.
                if throttle:
                    _, started, restored = throttle
                    while started.is_set() and not restored.is_set() and pending.qsize() >= 1:
                        dequeued.clear()
                        await dequeued.wait()
                header = await reader.readexactly(3)
                lane, size = struct.unpack(">BH", header)
                payload = await reader.readexactly(size)
                stats["frames"] += 1
                lag = delay + (stats["frames"] % 5) * .004 if delay else 0
                await pending.put((loop.time() + lag, header + payload, lane, payload))
                stats["queue_peak_frames"] = max(stats["queue_peak_frames"], pending.qsize())
        except asyncio.IncompleteReadError:
            await pending.put(None)

    async def write_frames():
        while (frame := await pending.get()) is not None:
            if dequeued:
                dequeued.set()
            due, wire, lane, payload = frame
            if delay:
                await asyncio.sleep(max(0, due - loop.time()))
            if throttle:
                rate, started, restored = throttle
                if started.is_set() and not restored.is_set():
                    try:
                        await asyncio.wait_for(restored.wait(), len(wire) / rate)
                    except TimeoutError:
                        stats["throttled_bytes"] = stats.get("throttled_bytes", 0) + len(wire)
            # Hold the writer's encoded value while the host changes the grant.
            is_stale_sample = (hold and lane == GAME_LANE and len(payload) >= AUTHORITY_VALUE_OFFSET + 10
                               and payload[0] == AUTHORITY_STATE
                               and struct.unpack_from("<Hq", payload, AUTHORITY_VALUE_OFFSET) == (8, 777))
            if hold and is_stale_sample:
                folder, held, released = hold
                if (folder / "hold_state").exists() and not released.is_set():
                    if not held:
                        held.extend(struct.unpack_from("<II", payload, GAME_HEADER_SIZE))
                        (folder / "state_held").touch()
                    await released.wait()
                    stats["held_state_delivered"] = stats.get("held_state_delivered", 0) + 1
            writer.write(wire)
            await writer.drain()
            if hold and is_stale_sample:
                (hold[0] / "state_forwarded").touch()
            stats["bytes"] += len(wire)
            stats["game_bytes"] += len(wire) if lane == GAME_LANE else 0

    await asyncio.gather(read_frames(), write_frames())


def authority_trial(peers, wait, command, release):
    for role in peers:
        wait(role, "baseline")
    command("B", "claim")
    original = wait("B", "writer_ready")
    assert wait("A", "committed") == 42
    command("B", "hold_state")
    wait("B", "second_write")
    command("A", "reclaim_when_held")
    new = wait("A", "reclaimed")
    assert new["epoch"] == original["epoch"] + 1 and new["value"] == 42
    assert wait("B", "reclaim_seen") == new["epoch"]
    release()
    command("A", "check_stale")
    assert wait("A", "stale_rejected") == 43
    assert wait("C", "stale_rejected") == 43
    command("A", "freeze")
    wait("A", "frozen")
    command("B", "race")
    assert [wait(role, "predicted")["value"] for role in ("B", "C")] == [71, 72]
    command("A", "unfreeze")
    result = {role: wait(role, "race_done") for role in peers}
    assert result["B"]["rejected"] != result["C"]["rejected"]
    winner = "C" if result["B"]["rejected"] else "B"
    assert all(row["value"] == result[winner]["value"] and row["writer"] == result[winner]["writer"]
               for row in result.values())
    return {"reclaim": new, "stale_value_after_delivery": 43, "prediction": result}


def mixed_trial(peers, wait, command, count, counters, profile=False, throttle=None):
    roles = tuple(peers)
    players = {role: wait(role, "baseline") for role in roles}
    assert len(set(players.values())) == len(players) and players["A"] == 1
    before = counters()
    start = time.monotonic()
    if throttle:
        throttle[0]()
    command("A", "go")
    for role in roles:
        wait(role, "run_started")
    wait("A", "halfway")
    command("A", "switch_area")
    for role in roles[1:]:
        if role != "B" or not throttle:
            wait(role, "switched")
    written = sum(wait(role, "written") for role in roles)
    assert written == count * 60
    seconds = time.monotonic() - start
    after = counters()
    if throttle:
        throttle[1]()
        wait("B", "switched")
    recovery = time.monotonic() - start - seconds
    command("A", "final_area")
    result = {role: wait(role, "final") for role in roles}
    assert all(row["checked"] == count * 8 for row in result.values())
    return {"players": result, "write_window_seconds": seconds,
            **({"slow_switch_recovery_seconds": recovery,
                "restored_to_final_seconds": time.monotonic() - start - seconds} if throttle else {}),
            **({"profile": {role: wait(role, "profile") for role in roles}} if profile else {}),
            "connection_bytes_per_second": [
                {lane: round((b[lane] - a[lane]) / seconds) for lane in ("up", "down")}
                for a, b in zip(before, after, strict=True)]}


def fusion_trial(peers, wait, _command, _workload):
    def broadcast(op):
        for peer in peers.values():
            peer.page.evaluate("op => {window.mixedFlags = {...window.mixedFlags, [op]: true}}", op)

    broadcast("spawn")
    baselines = {role: wait(role, "baseline") for role in peers}
    count = baselines["A"]
    if any(visible_count != count for visible_count in baselines.values()):
        raise ValueError(f"Inconsistent room layouts: {baselines}")
    start = time.monotonic()
    broadcast("go")
    for role in peers:
        wait(role, "run_started")
    wait("A", "halfway")
    broadcast("switch_area")
    for role in peers:
        if role != "A": wait(role, "switched")
    written = sum(wait(role, "written") for role in peers)
    if written != count * 60:
        raise ValueError(f"Incomplete writer input: {written}")
    seconds = time.monotonic() - start
    broadcast("final_area")
    players = {role: wait(role, "final") for role in peers}
    if any(row["checked"] != count * 8 or row["errors"] for row in players.values()):
        raise ValueError(f"Invalid terminal values or version order: {players}")
    return {"players": players, "write_window_seconds": seconds, "offered_object_versions": written}


async def run_room(project, folder, scenario, count, delay, profile=False, portable=False, slow_rate=0):
    stats = []
    held = []
    released = asyncio.Event()
    started = asyncio.Event()
    loop = asyncio.get_running_loop()
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        host_port = port_socket.getsockname()[1]

    def counters():
        return {"frames": 0, "bytes": 0, "game_bytes": 0, "queue_peak_frames": 0}

    async def relay(reader, writer):
        link = {"up": counters(), "down": counters()}
        stats.append(link)
        throttle = None
        # Local peers connect in role order. The first guest is B.
        if len(stats) == 1 and slow_rate:
            throttle = (slow_rate, started, released)
            upstream_socket = socket.socket()
            try:
                upstream_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                upstream_socket.setblocking(False)
                await loop.sock_connect(upstream_socket, ("127.0.0.1", host_port))
                upstream_reader, upstream = await asyncio.open_connection(sock=upstream_socket, limit=4096)
            except BaseException:
                upstream_socket.close()
                raise
        else:
            upstream_reader, upstream = await asyncio.open_connection("127.0.0.1", host_port)
        hold = (folder, held, released) if scenario == "authority" else None
        tasks = [asyncio.create_task(forward(reader, upstream, link["up"], delay, hold)),
                 asyncio.create_task(forward(upstream_reader, writer, link["down"], delay, throttle=throttle))]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            upstream.close()
            writer.close()

    async with await asyncio.start_server(relay, "127.0.0.1", 0) as server:
        (folder / "relay_port").write_text(str(server.sockets[0].getsockname()[1]), encoding="ascii")

        def connection_bytes():
            return [{direction: link[direction]["bytes"] for direction in ("up", "down")} for link in stats]

        def controller(peers, wait, command, _workload):
            if scenario == "mixed":
                throttle = (lambda: loop.call_soon_threadsafe(started.set),
                            lambda: loop.call_soon_threadsafe(released.set)) if slow_rate else None
                return mixed_trial(peers, wait, command, count, connection_bytes, profile, throttle)
            return authority_trial(peers, wait, command, lambda: loop.call_soon_threadsafe(released.set))

        roles = tuple("ABC" if scenario == "authority" else "ABCDEFGH")
        try:
            result = await asyncio.to_thread(local_trial, folder, project, count, "portable" if portable else scenario, controller=controller,
                                             roles=roles, port=host_port)
        finally:
            released.set()
    assert len(stats) == len(roles) - 1
    if slow_rate:
        assert stats[0]["down"].get("throttled_bytes", 0) > 0
    if scenario == "authority":
        assert held and result["reclaim"]["epoch"] == held[1] + 1
        assert sum(link["up"].get("held_state_delivered", 0) for link in stats) >= 1
        result["held_state"] = {"object": held[0], "epoch": held[1]}
    result["relay"] = stats
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk_zip", type=Path)
    parser.add_argument("scenario", choices=("authority", "mixed"))
    parser.add_argument("--count", type=int, default=128)
    parser.add_argument("--one-way-ms", type=int, default=0)
    parser.add_argument("--slow-down-kib", type=int, default=0, help="Throttle guest B's TCP downlink during writes")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--portable", action="store_true")
    parser.add_argument("--fusion", action="store_true", help="Use the Preview 555 browser SDK with the portable layout")
    parser.add_argument("--addon", type=Path, default=ADDON)
    parser.add_argument("--browser", type=Path, default=Path("C:/Program Files/Google/Chrome/Application/chrome.exe"))
    args = parser.parse_args()
    if not 32 <= args.count <= 1024 or args.count % 16 or not 0 <= args.one_way_ms <= 150:
        parser.error("Use a count divisible by 16 in 32..1024 and latency in 0..150 ms")
    if args.profile and args.scenario != "mixed":
        parser.error("--profile measures mixed rooms")
    if args.slow_down_kib and (args.scenario != "mixed" or args.fusion or not 1 <= args.slow_down_kib <= 64):
        parser.error("Use 1..64 KiB/s for the native mixed room")
    if (args.fusion or args.portable) and args.scenario != "mixed":
        parser.error("The common layout is for mixed rooms")
    if args.fusion:
        import os
        if not os.environ.get("FUSION_PROBE_APP_ID", "").strip():
            parser.error("Set FUSION_PROBE_APP_ID in the process environment")
        if args.profile or args.one_way_ms:
            parser.error("Run the Fusion reference on its normal Cloud connection")
        args.backend = "fusion"
        args.roles = tuple("ABCDEFGH")
        with open_trials(args, partial(project_files, "mixed"), "property-mixed") as run:
            result = run(0, args.count, "mixed", controller=fusion_trial)
            print("RESULT", json.dumps(result), flush=True)
        return
    if not budget(ROOT, 96 * 1024**2)["allowed"]:
        raise RuntimeError("Output space budget exceeded")
    work = create_job(ROOT, "tmp/property-multiplayer-" + args.scenario, "temp")
    try:
        with job_lock(work, "temp"):
            fixture = partial(project_files, args.scenario, profile_zip=args.sdk_zip if args.profile else None,
                              portable=args.portable, narrow_tcp_queue=args.slow_down_kib > 0)
            project = native_setup(work, fixture, args.sdk_zip)
            room = work / "room"
            room.mkdir()
            result = asyncio.run(run_room(project, room, args.scenario, args.count, args.one_way_ms / 1000,
                                          args.profile, args.portable, args.slow_down_kib * 1024))
            print("RESULT", json.dumps(result), flush=True)
    finally:
        remove_owned(ROOT, work, "temp")


if __name__ == "__main__":
    main()
