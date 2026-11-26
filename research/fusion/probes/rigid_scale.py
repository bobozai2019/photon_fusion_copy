"""Two-player active RigidBody3D sweep. Run native or original Fusion Web separately."""
import argparse
from contextlib import contextmanager, ExitStack
from functools import partial
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
from threading import Thread
import time
import uuid
from zipfile import ZipFile

from cloud_web import Handler, Peer, ThreadingHTTPServer, export_web, sync_playwright
from artifacts import ROOT, budget, create_job, job_lock, remove_owned
from godot_build import resolve_godot, import_project

HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parents[3]
ADDON = WORKSPACE / "references/photon/photon-fusion-godot-starter-kit-racing_3.0.0/addons/fusion"


class ScalePeer(Peer):
    def wait(self, kind, after=0, timeout=8, accept=None):
        try:
            return super().wait(kind, after, timeout, accept)
        except TimeoutError:
            progress = [e for e in self.events if e["event"] in ("loading", "prepared", "writer", "observer", "converged", "authority_final")]
            print(self.name, "last benchmark events", progress[-4:], flush=True)
            raise

    def console(self, message):
        super().console(message)
        if message.type in ("error", "warning") or message.text.startswith("CLIENT_EVENT "):
            line = message.text.replace(os.environ["FUSION_PROBE_APP_ID"], "[app]")
            if not line.startswith("CLIENT_EVENT ") or '"event":"loading"' not in line:
                print(self.name, line[:350], flush=True)


def body_scene(web):
    sync = "FusionSharedReplicator" if web else "Node"
    resources = ('[sub_resource type="FusionReplicationConfig" id="config"]\nproperties/0/path = NodePath(".:stamp")\n' if web
                 else '[ext_resource type="Script" path="res://addons/netcode/scene_replicator.gd" id="sync"]\n')
    properties = ('owner_mode = 1\nroot_replication_mode = 1\nroot_interpolation_mode = 2\nroot_correction_mode = 2\nroot_forecast_gravity = false\nroot_teleport_threshold = 10.0\nreplication_config = SubResource("config")' if web
                  else 'script = ExtResource("sync")\ndelegate_root_motion = true\nplayer_attached = true\nteleport_distance = 10.0\nproperties = Array[NodePath]([NodePath(":stamp")])\nclient_writable = Array[NodePath]([NodePath(":stamp")])')
    return ('[gd_scene format=3]\n[ext_resource type="Script" path="res://actor.gd" id="actor"]\n' + resources +
            '[sub_resource type="SphereShape3D" id="ball"]\nradius = 0.25\n[node name="Body" type="RigidBody3D"]\n'
            'collision_layer = 0\ncollision_mask = 0\ngravity_scale = 0.0\nlinear_damp_mode = 1\nlinear_damp = 0.0\n'
            'script = ExtResource("actor")\n[node name="Shape" type="CollisionShape3D" parent="."]\nshape = SubResource("ball")\n'
            f'[node name="Sync" type="{sync}" parent="."]\n{properties}\n')


def fixture(web):
    return {"body.tscn": body_scene(web), "actor.gd": "extends RigidBody3D\nvar stamp: int = 0\n",
            "scale.gd": (HERE / "rigid_scale.gd").read_text(encoding="utf-8"),
            "main.gd": (HERE / ("rigid_scale_fusion.gd" if web else "rigid_scale_native.gd")).read_text(encoding="utf-8"),
            "main.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://main.gd" id="1"]\n[node name="Main" type="Node"]\nscript = ExtResource("1")\n'}


def web_setup(work, addon, project_files=fixture):
    export_web(work, addon, "session")
    updates = project_files(True)
    with ZipFile(work / "fixture.zip") as old, ZipFile(work / "scale.zip","w") as new:
        for name in old.namelist():
            if name not in updates:
                new.writestr(name,old.read(name))
        for name,source in updates.items(): new.writestr(name,source)
    (work / "scale.zip").replace(work / "fixture.zip")


def native_setup(work, project_files=fixture, sdk_zip=None):
    project = work / "project"
    project.mkdir()
    if sdk_zip:
        with ZipFile(sdk_zip) as archive:
            members = [name for name in archive.namelist() if name.startswith("addons/netcode/")]
            if "addons/netcode/netcode.gdextension" not in members:
                raise ValueError("Expected an installable Godot addon archive")
            archive.extractall(project, members)
    else:
        shutil.copytree(ROOT / "install/win-x64-msvc-mt/Release/godot/addons/netcode", project / "addons/netcode",
                        ignore=shutil.ignore_patterns("~*"))
    for name,source in project_files(False).items(): (project / name).write_text(source,encoding="utf-8")
    (project / "project.godot").write_text('config_version=5\n[application]\nrun/main_scene="res://main.tscn"\n'
        'run/max_fps=60\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n',encoding="utf-8")
    import_project(resolve_godot(),project)
    return project


RIGID_EVENTS = (("A", "writer"), ("B", "observer"), ("B", "converged"), ("A", "authority_final"))


def local_trial(folder, project, count, workload, events=RIGID_EVENTS, coordinate_observer=False,
                controller=None, roles=("A", "B"), port=None):
    def wait_for(role, name):
        limit = time.monotonic() + 35
        while time.monotonic() < limit:
            output = (folder / role).read_text(encoding="utf-8",errors="replace")
            if peers[role].poll() is not None or "SCRIPT ERROR" in output:
                raise RuntimeError(output[-900:])
            for line in output.splitlines():
                if line.startswith("CLIENT_EVENT "):
                    try:
                        event = json.loads(line[13:])
                    except json.JSONDecodeError:
                        continue  # The engine may still be writing its last line.
                    if event["event"] == name: return event["value"]
                    if event["event"] == "failed": raise ValueError(event)
            time.sleep(.08)
        raise TimeoutError(f"{role}: no {name}, log: {output[-500:]}")

    if port is None:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1",0))
            port = sock.getsockname()[1]
    peers = {}
    with ExitStack() as stack:
        for role in roles:
            log = stack.enter_context((folder / role).open("wb",buffering=0))
            p = subprocess.Popen([resolve_godot(),"--headless","--path",project,"--",role,str(count),str(port),folder,workload],
                stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            peers[role] = p
            stack.callback(lambda p=p: (p.kill(),p.wait()) if p.poll() is None else None)
            wait_for(role,"opened")
        if controller:
            return controller(peers, wait_for, lambda role, op: (folder / op).touch(), workload)
        (folder / "spawn").touch()
        for role in peers: wait_for(role,"prepared")
        (folder / "go").touch()
        result = []
        for role, event in events:
            result.append(wait_for(role, event))
            if coordinate_observer and role == "A" and event == "writer":
                (folder / "writer_done").touch()
        for role in peers:
            lines = (folder / role).read_text(encoding="utf-8", errors="replace").splitlines()
            errors = [line for line in lines if line.startswith(("ERROR:", "SCRIPT ERROR:"))]
            if errors: print(role, "engine errors", len(errors), sorted(set(errors))[:3], flush=True)
        return result


def browser_trial(browser, url, count, workload, events=RIGID_EVENTS, coordinate_observer=False,
                  controller=None, roles=("A", "B")):
    config = {"app":os.environ["FUSION_PROBE_APP_ID"],"region":"asia","room":"scale-"+uuid.uuid4().hex[:12],"count":count,"pairs":workload=="pairs","workload":workload}
    with ExitStack() as stack:
        peers = {}
        for role in roles:
            peer = ScalePeer(browser, url, {**config, "role": role})
            stack.callback(peer.context.close)
            peers[role] = peer
        if controller:
            return controller(peers, lambda role, event: peers[role].wait(event, timeout=150 if len(roles)>2 else 35)["value"],
                              lambda role, op: peers[role].page.evaluate("op => window.scaleCommand = op", op), workload)
        a, b = peers["A"], peers["B"]
        a.page.evaluate("window.scaleCommand='spawn'")
        for peer in (a,b): peer.wait("prepared",timeout=40)
        a.page.evaluate("window.scaleCommand='go'")
        if coordinate_observer:
            b.page.evaluate("window.scaleCommand='go'")
        peers = {"A": a, "B": b}
        result = []
        for role, event in events:
            result.append(peers[role].wait(event, timeout=30)["value"])
            if coordinate_observer and role == "A" and event == "writer":
                b.page.evaluate("window.scaleCommand='writer_done'")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend",choices=("native","fusion"))
    parser.add_argument("counts",type=int,nargs="+")
    parser.add_argument("--addon",type=Path,default=ADDON)
    parser.add_argument("--workload",choices=("lanes","pairs"),default="lanes")
    parser.add_argument("--browser", type=Path, default=Path("C:/Program Files/Google/Chrome/Application/chrome.exe"))
    args = parser.parse_args()
    if any(n < 1 or (args.workload == "pairs" and n % 2) for n in args.counts):
        parser.error("Provide positive counts and even counts for pairs")
    run_trials(args, fixture, RIGID_EVENTS, "body-scale")


@contextmanager
def open_trials(args, project_files, job_name):
    if not budget(ROOT,180*1024**2)["allowed"]: raise RuntimeError("Output budget exceeded")
    work = create_job(ROOT,f"tmp/{job_name}-{args.backend}-{uuid.uuid4().hex[:8]}","temp")
    try:
        with job_lock(work,"temp"), ExitStack() as stack:
            if args.backend == "fusion":
                web_setup(work,args.addon,project_files)
                server = stack.enter_context(ThreadingHTTPServer(("127.0.0.1",0),partial(Handler,directory=str(work))))
                Thread(target=server.serve_forever,daemon=True).start()
                stack.callback(server.shutdown)
                pw = stack.enter_context(sync_playwright())
                browser = stack.enter_context(pw.chromium.launch(executable_path=str(args.browser),
                    headless=True,args=["--use-angle=swiftshader","--enable-unsafe-swiftshader","--disable-background-timer-throttling","--disable-renderer-backgrounding","--disable-backgrounding-occluded-windows"]))
            else:
                project = native_setup(work,project_files,getattr(args,"sdk_zip",None))
            def run(index, count, workload, events=(), coordinate_observer=False, controller=None):
                if args.backend == "fusion":
                    return browser_trial(browser, f"http://127.0.0.1:{server.server_port}/index.html",
                                         count, workload, events, coordinate_observer, controller,
                                         getattr(args, "roles", ("A", "B")))
                room = work / str(index)
                room.mkdir()
                return local_trial(room, project, count, workload, events, coordinate_observer, controller)
            yield run
    finally:
        assert work.resolve().parent == (ROOT/"tmp").resolve()
        remove_owned(ROOT,work,"temp")


def run_trials(args, project_files, events, job_name, summarize=None, coordinate_observer=False, controller=None):
    with open_trials(args, project_files, job_name) as run:
        for i,n in enumerate(args.counts):
            result = run(i, n, args.workload, events, coordinate_observer, controller)
            if summarize:
                result = summarize(n, args.workload, *result)
            print("RESULT",args.backend,args.workload,n,json.dumps(result),flush=True)


if __name__ == "__main__":
    main()
