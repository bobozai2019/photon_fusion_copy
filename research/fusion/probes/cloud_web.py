"""Exercise the shipped Preview 555 Web client in a randomly named test room."""

import argparse
from contextlib import ExitStack
from functools import partial
from hashlib import sha256
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import sys
from threading import Thread
import time
import uuid
from zipfile import ZipFile

from playwright.sync_api import sync_playwright

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools"))
from artifacts import ROOT, budget, create_job, job_lock, remove_owned


def scene_text(kind):
    predicted = kind == "predicted"
    cls = "FusionServerReplicator" if predicted else "FusionSharedReplicator"
    owner_mode = 5 if predicted else {"tracked": 1, "control": 1, "declined": 0, "transfer": 0, "interest": 1,
                                      "body": 1, "master_item": 3, "attached": 1}[kind]
    fields = ["label", "marks"] if kind == "control" else ["score", "label", "marks"]
    rows = []
    for i, field in enumerate(fields):
        rows.append(f'properties/{i}/path = NodePath(".:{field}")')
        if field == "marks":
            rows += [f"properties/{i}/max_array_capacity = 4", f"properties/{i}/element_type = 3"]
    interest = "\ninterest_mode = 2\ninterest_key = 42\ninterest_destroy_on_exit = true" if kind == "interest" else ""
    part_resource = '[ext_resource type="Script" path="res://part.gd" id="2"]\n' if kind == "tracked" else ""
    part_node = '[node name="Part" type="Node" parent="."]\nscript = ExtResource("2")\n' if kind == "tracked" else ""
    if kind == "tracked":
        rows.append(f'properties/{len(fields)}/path = NodePath("Part:value")')
    if kind == "body":
        return ('[gd_scene load_steps=3 format=3]\n[ext_resource type="Script" path="res://actor.gd" id="1"]\n'
                '[sub_resource type="SphereShape3D" id="Ball"]\nradius = 0.25\n'
                '[node name="Body" type="RigidBody3D"]\nscript = ExtResource("1")\nkind = "body"\n'
                'gravity_scale = 0.0\nlinear_damp_mode = 1\nlinear_damp = 0.0\n'
                'max_contacts_reported = 4\ncontact_monitor = true\n'
                '[node name="Shape" type="CollisionShape3D" parent="."]\nshape = SubResource("Ball")\n'
                '[node name="Sync" type="FusionSharedReplicator" parent="."]\nowner_mode = 1\n'
                'root_replication_mode = 1\nroot_interpolation_mode = 2\nroot_correction_mode = 2\n'
                'root_teleport_threshold = 10.0\nroot_rotation_teleport_threshold = 90.0\nroot_forecast_gravity = false\n')
    return (f'[gd_scene load_steps={4 if part_node else 3} format=3]\n[ext_resource type="Script" path="res://actor.gd" id="1"]\n'
            + part_resource + '[sub_resource type="FusionReplicationConfig" id="Cfg"]\n' + "\n".join(rows)
            + f'\n[node name="{kind.capitalize()}" type="Node3D"]\nscript = ExtResource("1")\nkind = "{kind}"\n'
            + part_node + f'[node name="Sync" type="{cls}" parent="."]\nowner_mode = {owner_mode}\n'
              'root_replication_mode = 0\nreplication_config = SubResource("Cfg")' + interest + "\n")


def export_web(folder, addon, mode):
    template = next(addon.glob("export_templates/**/godot.web.template_debug*.zip"))
    library = next((addon / "bin").glob("libfusion.web.template_debug*.so"))
    expected = {template: "8e0c5993d79d9172033a4b3dc9b08f4617965b44e5a6edb2be6dc842c99190ad",
                library: "0330c28f94ea5b361805ac7d66d2d427076553aa536f1d357ca37333558bb19a"}
    for path, digest in expected.items():
        if sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Expected Preview 555 original Web file: {path.name}")
    with ZipFile(template) as archive:
        for name in ("godot.js", "godot.wasm", "godot.side.wasm", "godot.audio.worklet.js",
                     "godot.audio.position.worklet.js"):
            (folder / name).write_bytes(archive.read(name))
        html = archive.read("godot.html").decode()
    options = {"args": [], "canvasResizePolicy": 1, "executable": "godot", "mainPack": "fixture.zip",
               "gdextensionLibs": [library.name], "focusCanvas": False}
    for key, value in {"CONFIG": json.dumps(options), "THREADS_ENABLED": "false", "HEAD_INCLUDE": "",
                       "PROJECT_NAME": "Fusion client probe", "URL": "godot.js", "SPLASH_CLASSES": "",
                       "SPLASH_COLOR": "#222222", "SPLASH": ""}.items():
        html = html.replace("$GODOT_" + key, value)
    (folder / "index.html").write_text(html, encoding="utf-8")
    shutil.copyfile(library, folder / library.name)
    with ZipFile(folder / "fixture.zip", "w") as pack:
        pack.writestr("project.godot", 'config_version=5\n[application]\nconfig/name="ClientProbe"\n'
                      'run/main_scene="res://main.tscn"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
                      f'[fusion]\nsimulation/mode={int(mode == "host")}\nsimulation/process_mode=1\n')
        pack.writestr("main.tscn", '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" '
                      'path="res://main.gd" id="1"]\n[node name="Main" type="Node"]\nscript = ExtResource("1")\n')
        pack.write(HERE / "cloud_room.gd", "main.gd")
        pack.write(HERE / "cloud_actor.gd", "actor.gd")
        pack.write(HERE / "cloud_part.gd", "part.gd")
        kinds = {"shared": ("tracked", "control", "declined", "transfer", "interest"), "host": ("predicted",),
                 "session": ("body", "master_item", "attached"), "spawn": ("attached",)}
        for kind in kinds[mode]:
            pack.writestr(kind + ".tscn", scene_text(kind))
        pack.writestr("map.tscn", '[gd_scene format=3]\n[node name="ProbeMap" type="Node3D"]\n')
        pack.write(addon / "fusion.gdextension", "addons/fusion/fusion.gdextension")
        pack.writestr(".godot/extension_list.cfg", "res://addons/fusion/fusion.gdextension\n")
        pack.writestr(".godot/global_script_class_cache.cfg", "list=Array[Dictionary]([])\n")


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def guess_type(self, path):
        if path.endswith((".wasm", ".so")):
            return "application/wasm"
        return super().guess_type(path)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


class Peer:
    def __init__(self, browser, url, config):
        self.name = config["role"]
        self.events = []
        self.printed_decisions = set()
        self.context = browser.new_context()
        self.page = self.context.new_page()
        self.commands = 0
        self.page.add_init_script("window.fusionProbe=" + json.dumps(config))
        self.page.on("console", self.console)
        self.page.on("pageerror", lambda err: print(self.name, "page error", str(err)[:130], flush=True))
        self.page.goto(url)
        self.join = self.wait("joined", timeout=40)["value"]
        print(self.name, "joined", self.join, flush=True)

    def console(self, message):
        line = message.text
        if line.startswith("CLIENT_EVENT "):
            event = json.loads(line.removeprefix("CLIENT_EVENT "))
            self.events.append(event)
            if event["event"] in ("response", "request"):
                key = (event["event"], *event["value"])
                if key not in self.printed_decisions:
                    self.printed_decisions.add(key)
                    print(self.name, event, flush=True)
            elif event["event"] in ("failed", "rpc", "sample", "left"):
                print(self.name, event, flush=True)
        elif "SCRIPT ERROR" in line or "Parse Error" in line or "Invalid call" in line:
            print(self.name, line[:190], flush=True)

    def wait(self, kind, after=0, timeout=8, accept=None):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for event in self.events[after:]:
                if event["event"] == "failed":
                    raise RuntimeError(f"{self.name} connection: {event['value']}")
                if event["event"] == kind and (accept is None or accept(event["value"])):
                    return event
            self.page.wait_for_timeout(60)
        raise TimeoutError(f"{self.name} waiting for {kind}")

    def command(self, op, **kwargs):
        self.commands += 1
        before = len(self.events)
        self.page.evaluate("command => {window.fusionCommand = command}", {"id": self.commands, "op": op, **kwargs})
        result = self.wait("command", before)
        if result["value"] != self.commands:
            raise RuntimeError("Command acknowledgement mismatch")
        return self.events[before:]

    def until(self, op, event, predicate, timeout=10, **kwargs):
        end = time.monotonic() + timeout
        last = None
        while time.monotonic() < end:
            for item in self.command(op, **kwargs):
                if item["event"] == event:
                    last = item["value"]
                    if predicate(last):
                        return last
            self.page.wait_for_timeout(110)
        raise TimeoutError(f"{self.name} {op}/{event}, last value {str(last)[:210]}")


def shared(a, b):
    b.until("members", "members", lambda names: "tracked" in names and "control" in names)
    before = b.until("inspect", "state", lambda s: s[1] == 0.75, kind="tracked")
    print("late join score / authority", before[1], before[4:6], flush=True)
    message = "多端同步🚗" * 54
    for score, label, marks in [(-0.5, message, [0.25, -1.5, 2.75, 4]), (0.25, "", [3.5, -2])]:
        a.command("state", score=score, label=label, marks=marks)
        b.until("inspect", "state", lambda s: s[1:4] == [score, label, marks], kind="tracked")
        print("received exact score, Unicode text and array:", score, len(label), len(marks), flush=True)
    for hz in (60, 30):
        offsets = [len(p.events) for p in (a, b)]
        for peer in (a, b):
            peer.command("measure", hz=hz, ticks=60)
        for peer, offset in zip((a, b), offsets):
            sample = peer.wait("sample", offset, 15)["value"]
            expected_reads = 60 if peer is a else 0
            if len(sample) != 7 or sample[0:2] != [hz, 60] or sample[3:6] != [expected_reads, 0, 0]:
                raise ValueError(f"Invalid timing control: {sample}")
    offset = len(b.events)
    a.command("rpc", player=b.join[0])
    b.until("members", "members", lambda _: len([e for e in b.events[offset:] if e["event"] == "rpc"]) >= 4)
    messages = [e["value"] for e in b.events[offset:] if e["event"] == "rpc"]
    if messages != [["tracked", i, a.join[0]] for i in range(4)]:
        raise ValueError(f"RPC delivery: {messages}")
    offset = len(b.events)
    a.command("mixed_rpc", player=b.join[0])
    b.until("members", "members", lambda _: sum(e["event"] == "rpc" for e in b.events[offset:]) >= 4)
    b.page.wait_for_timeout(350)
    delivered = [(e["event"], e["value"][:2]) for e in b.events[offset:] if e["event"] in ("rpc", "rpc_u")]
    print("two-object reliable / unreliable arrival sample", delivered, flush=True)
    for kind, expected in (("tracked", [10, 11]), ("control", [20, 21])):
        if [v[1] for event, v in delivered if event == "rpc" and v[0] == kind] != expected:
            raise ValueError(f"Incomplete reliable sequence for {kind}")
    b.command("watch_score", yes=True)
    writes = [e["value"] for e in a.command("write_three") if e["event"] == "write"]
    if len(writes) != 3 or len({row[0] for row in writes}) != 1:
        raise ValueError(f"Expected three writes within one physics frame: {writes}")
    b.until("score_window", "scores", lambda scores: any(abs(value - 0.875) < 0.011 for value in scores))
    b.page.wait_for_timeout(300)
    b.command("watch_score", yes=False)
    seen = next(e["value"] for e in b.command("score_window") if e["event"] == "scores")
    print("single-frame writes / remote setters", writes, seen, flush=True)
    if not seen or any(abs(value - 0.875) >= 0.011 for value in seen):
        raise ValueError(f"Unexpected value in the same-frame remote setter window: {seen}")
    precision = []
    for value in (-0.124, 0.376, -0.876):
        a.command("state", score=value, label="", marks=[])
        observed = b.until("inspect", "state", lambda s: abs(s[1] - value) < 0.011, kind="tracked")
        precision.append((value, observed[1]))
    print("float setting, fractional round trips", a.command("float_setting")[0]["value"], precision, flush=True)


def interests(a, b):
    a.command("spawn", kind="interest")
    a.command("state", kind="interest", score=3, label="current", marks=[])
    names = next(item["value"] for item in b.command("members") if item["event"] == "members")
    if "interest" in names:
        raise ValueError("Unsubscribed peer already sees the interest object")
    print("before subscribing", names, flush=True)
    b.command("subscribe", yes=True)
    first = b.until("inspect", "state", lambda s: s[1] == 3, kind="interest")
    b.command("subscribe", yes=False)
    b.until("members", "members", lambda names: "interest" not in names)
    a.command("state", kind="interest", score=7, label="changed", marks=[])
    b.command("subscribe", yes=True)
    again = b.until("inspect", "state", lambda s: s[1] == 7, kind="interest")
    if again[-1] == first[-1]:
        raise ValueError("Expected a new local node on interest re-entry")
    print("interest re-entry, new node and current baseline", first[1], again[1], flush=True)


def ownership(a, b):
    # Each decision uses a fresh root; response signals on a given root carry no request sequence.
    for kind, allow in (("declined", False), ("transfer", True)):
        a.command("spawn", kind=kind)
        a.until("inspect", "state", lambda s: s[4] and s[5] == a.join[0], kind=kind)
        b.until("inspect", "state", lambda s: not s[4] and s[5] == a.join[0], kind=kind)
        a.command("allow", kind=kind, yes=allow)
        start = len(b.events)
        b.command("want", kind=kind, yes=True)
        if not allow:
            b.wait("response", start, 15, accept=lambda v: v == [kind, False])
            b.command("want", kind=kind, yes=False)
            b.until("inspect", "state", lambda s: s[5] == a.join[0] and not s[4], kind=kind)
            a.command("despawn", kind=kind)
            b.until("members", "members", lambda names: kind not in names)
        else:
            b.until("inspect", "state", lambda s: s[4] and s[5] == b.join[0], timeout=20, kind=kind)
            a.until("inspect", "state", lambda s: not s[4] and s[5] == b.join[0], kind=kind)
            b.page.wait_for_timeout(500)
            replies = [e["value"] for e in b.events[start:] if e["event"] == "response" and e["value"][0] == kind]
            print("owner verified on both ends, observed response values", sorted({v[1] for v in replies}), flush=True)
    print("transfer to second peer verified", flush=True)


def child_binding(a, b):
    b.until("child_state", "child", lambda state: state[0] == 1.0)
    a.command("swap_child")
    b.page.wait_for_timeout(1200)
    before = b.until("child_state", "child", lambda state: state[0] == 11.0)
    counters = a.until("child_state", "child", lambda state: state[2] >= 0)
    if counters[1] != 0:
        raise ValueError(f"Replacement was sampled before the old target was freed: {counters}")
    a.command("free_old")
    after = b.until("child_state", "child", lambda state: state[0] == 22.0)
    print("same path replacement: remote before/after freeing old, owner [new value,new reads,old reads]",
          before[0], after[0], counters, flush=True)


def prediction(a, b):
    a.command("spawn", kind="predicted")
    b.until("members", "members", lambda names: "predicted" in names)
    a.command("input_owner", kind="predicted", player=b.join[0])
    b.until("inputs", "inputs", lambda s: s[1], kind="predicted")
    b.command("input_start")
    local = b.until("inputs", "inputs", lambda s: s[2] == 60, kind="predicted")
    remote = a.until("inputs", "inputs", lambda s: s[7] == 60, timeout=15, kind="predicted")
    # Wait for pending state updates before reporting cumulative reset/replay counters.
    b.page.wait_for_timeout(1200)
    local = b.until("inputs", "inputs", lambda s: s[2] == 60, kind="predicted")
    print("input authority, has input authority, submitted, callbacks, distinct, repeated, resets, max, pending")
    print("A", remote, "B", local, flush=True)
    if remote[4:6] != [60, 0] or local[4] != 60 or local[5] == 0 or local[6] == 0:
        raise ValueError("This run did not demonstrate 60 unique inputs and a prediction reset with repeated callbacks")


def rigidbody(a, b):
    a.command("spawn", kind="body")
    b.until("members", "members", lambda names: "body" in names)
    a.command("launch_body", kind="body")
    moving = b.until("body_state", "body", lambda s: s[0] > 0.1 and s[1] > 0.1, kind="body")
    wall = a.until("body_state", "body", lambda s: s[3], kind="body")
    proxy = b.until("body_state", "body", lambda s: s[3] and s[0] > 2.3, kind="body")
    a.command("warp_body", kind="body")
    warped = b.until("body_state", "body", lambda s: s[0] > 24, kind="body")
    a.command("wake_body", kind="body")
    awake = b.until("body_state", "body", lambda s: not s[2] and s[1] > 0.1, kind="body")
    a.command("sleep_body", kind="body")
    asleep = b.until("body_state", "body", lambda s: s[2], kind="body")
    print("rigid body [x,vx,sleeping,ever in contact,largest step]:",
          "moving", moving, "owner wall", wall, "proxy wall", proxy, "warp", warped, "awake", awake, "sleep", asleep, flush=True)


def maps(a, b):
    start = len(b.events)
    records = a.command("load_map")
    sequence = next(item["value"] for item in records if item["event"] == "map_seq")
    b.wait("map_ready", start, accept=lambda value: value == sequence)
    loaded = b.until("maps", "maps", lambda value: value[0] > 0 and value[1] > 0)
    start = len(b.events)
    a.command("unload_map", sequence=sequence)
    b.wait("map_unloaded", start, accept=lambda value: value == sequence)
    empty = b.until("maps", "maps", lambda value: value[1] == 0)
    print("remote map ready/unloaded:", sequence, loaded, empty, flush=True)


def master_change(a, b, browser, addr, config):
    for kind in ("master_item", "attached"):
        a.command("spawn", kind=kind)
        a.command("state", kind=kind, score=8, label="before leave", marks=[])
        b.until("inspect", "state", lambda s: s[1] == 8, kind=kind)
    start = len(b.events)
    a.command("leave")
    b.wait("master", start, accept=lambda ids: ids == [b.join[0], a.join[0]])
    new_owner = b.until("inspect", "state", lambda s: s[4] and s[5] == b.join[0], kind="master_item")
    room = b.until("session", "room", lambda v: v[1] and v[2] == 1)
    b.until("members", "members", lambda names: "attached" not in names)
    print("master migration, new owner / room", new_owner[5], room, flush=True)
    c = Peer(browser, addr, {**config, "role": "C", "identity": "A"})
    c.until("inspect", "state", lambda s: s[1] == 8 and s[5] == b.join[0], kind="master_item")
    print("fresh join with same user ID after graceful leave: old/new player ID", a.join[0], c.join[0], flush=True)
    before = len(b.events)
    c.context.close()
    loss = b.wait("player_left", before, 35, accept=lambda info: info[0] == c.join[0])
    print("browser context closed without leave_room:", loss["value"], flush=True)


def spawn_order(a, b):
    for pre in (False, True):
        starts = [len(peer.events) for peer in (a, b)]
        b.command("spawn_order", kind="attached", pre=pre)
        local = b.wait("spawn_return", starts[1])
        a.wait("spawner_spawned", starts[0])
        a.wait("spawn_setter", starts[0], accept=lambda value: value == 0.75)
        names = {"spawn_ready", "spawn_setter", "replicator_spawned", "spawner_spawned", "spawn_return"}
        traces = [[(event["event"], event["value"]) for event in peer.events[start:] if event["event"] in names]
                  for peer, start in zip((a, b), starts, strict=True)]
        for peer, trace in zip((a, b), traces, strict=True):
            print(f"spawn order pre={pre} {peer.name} {trace}", flush=True)
        if traces[0][:4] != [("spawn_ready", 0.0), ("spawn_setter", 0.75),
                              ("replicator_spawned", 0.75), ("spawner_spawned", 0.75)]:
            raise ValueError("Returned-node write was not the first visible remote value")
        if local["value"] != (0.25 if pre else 0.0):
            raise ValueError("Spawn returned the wrong local initial value")
        b.command("despawn", kind="attached")
        a.until("members", "members", lambda names: "attached" not in names)


def run(folder, args, app):
    export_web(folder, args.addon, args.mode)
    with ExitStack() as stack:
        server = stack.enter_context(ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(folder))))
        Thread(target=server.serve_forever, daemon=True).start()
        stack.callback(server.shutdown)
        playwright = stack.enter_context(sync_playwright())
        browser = stack.enter_context(playwright.chromium.launch(executable_path=str(args.browser), headless=True,
            args=["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--disable-background-timer-throttling",
                  "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows"]))
        config = {"app": app, "region": args.region, "room": "nc-" + uuid.uuid4().hex[:12], "mode": args.mode}
        addr = f"http://127.0.0.1:{server.server_port}/index.html"
        a = Peer(browser, addr, {**config, "role": "A"})
        if args.mode == "shared":
            for kind in ("tracked", "control"):
                a.command("spawn", kind=kind)
            a.command("state", score=0.75, label="baseline", marks=[1])
        b = Peer(browser, addr, {**config, "role": "B"})
        tests = {"shared": [shared, child_binding, interests, ownership],
                 "host": [prediction], "session": [rigidbody, maps], "spawn": [spawn_order]}[args.mode]
        failures = 0
        for test in tests:
            try:
                test(a, b)
                print("PASS", test.__name__, flush=True)
            except (ValueError, RuntimeError, TimeoutError) as error:
                failures += 1
                print("INCOMPLETE", test.__name__, error, flush=True)
        if args.mode == "shared":
            a.command("despawn", kind="tracked")
            b.until("members", "members", lambda names: "tracked" not in names)
            print("PASS remote despawn", flush=True)
        if args.mode == "session":
            try:
                master_change(a, b, browser, addr, config)
                print("PASS master_change", flush=True)
            except (ValueError, RuntimeError, TimeoutError) as error:
                failures += 1
                print("INCOMPLETE master_change", error, flush=True)
        for peer in (b, a):
            if any(e["event"] == "left" for e in peer.events):
                continue
            start = len(peer.events)
            peer.command("leave")
            peer.wait("left", start)
        return int(failures != 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--addon", type=Path, default=REPO.parent / "references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion")
    parser.add_argument("--browser", type=Path, default=Path("C:/Program Files/Google/Chrome/Application/chrome.exe"))
    parser.add_argument("--region", default="asia")
    parser.add_argument("--mode", choices=["shared", "host", "session", "spawn"], default="shared")
    args = parser.parse_args()
    app = os.environ.get("FUSION_PROBE_APP_ID", "").strip()
    if not app:
        parser.error("Set FUSION_PROBE_APP_ID in the process environment")
    if not budget(ROOT, 100 * 1024 ** 2)["allowed"]:
        return 1
    work = create_job(ROOT, f"tmp/fusion-web-{args.mode}", "temp")
    try:
        with job_lock(work, "temp"):
            return run(work, args, app)
    finally:
        print(f"remove {work} (finished original Web client probe)", flush=True)
        remove_owned(ROOT, work, "temp")


if __name__ == "__main__":
    raise SystemExit(main())

