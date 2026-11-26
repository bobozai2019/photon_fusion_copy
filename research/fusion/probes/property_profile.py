"""Profile the normal 250-object property fixture, then separately test proxy-local edits."""
import argparse
import ctypes
from ctypes import wintypes
from functools import partial
from pathlib import Path
import time
from zipfile import ZipFile

from property_scale import fixture, prepare_window, start_window, summarize
from property_web_profile import BrowserProfile
from rigid_scale import ADDON, HERE, ROOT, run_trials


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Profile point no longer matches exactly once: {old}")
    return text.replace(old,new)


def project_files(web, cached_scalar=False, packed_scalar=False, sdk_zip=None):
    files = fixture(web, 20)
    files["meter.gd"] = (HERE / "property_profile_meter.gd").read_text(encoding="utf-8")
    meter = 'const ProfileMeter = preload("res://meter.gd")'
    measure = files["measure.gd"]
    measure = replace_once(measure, "extends Node\n", "extends Node\n" + meter + "\n")
    measure = replace_once(measure, "    if not online or finished: return",
                           "    if online and prepared: profile_tick()\n    if not online or finished: return")
    files["measure.gd"] = measure + (HERE / "property_profile_hooks.gd.inc").read_text(encoding="utf-8")
    query = ('return command() == op' if web else 'return FileAccess.file_exists(folder.path_join(op))')
    files["main.gd"] += f"\nfunc audit_command(op: String) -> bool:\n    {query}\n"
    if web:
        files["main.gd"] += '\nfunc browser_clock_ms() -> float:\n    return JavaScriptBridge.eval("performance.now()", true)\n'
        return files

    sdk = ROOT / "install/win-x64-msvc-mt/Release/godot"
    def installed_source(path):
        if sdk_zip:
            with ZipFile(sdk_zip) as archive:
                return archive.read(path).decode("utf-8").replace("\r\n", "\n")
        return (sdk/path).read_text(encoding="utf-8")

    base = "addons/netcode/replicated_object.gd"
    text = installed_source(base)
    text = replace_once(text, "extends Node\n", "extends Node\n" + meter + "\n")
    sample_call = "    var sampled := _sample_for_sync()" if "_sample_for_sync()" in text else "    var sampled := sample_fields()"
    text = replace_once(text, sample_call,
        "    var started := Time.get_ticks_usec()\n" + sample_call + "\n"
        "    var sampled_at := Time.get_ticks_usec()")
    text = replace_once(text, "    if change.is_empty():",
        "    var synced_at := Time.get_ticks_usec()\n    if ProfileMeter.active:\n"
        "        ProfileMeter.read_us += sampled_at - started\n"
        "        ProfileMeter.sync_us += synced_at - sampled_at\n    if change.is_empty():")
    apply_end = ("    if not initialized: game.unbind_replica(object_id)"
                 if "    initialized = _apply_fields(" in text else "    initialized = true")
    text = replace_once(text, apply_end,
        apply_end + "\n    if ProfileMeter.active: ProfileMeter.apply_us += Time.get_ticks_usec()-synced_at")
    files[base] = text

    replica = "addons/netcode/scene_replicator.gd"
    text = installed_source(replica)
    if cached_scalar or packed_scalar:
        text = replace_once(text,
            "func sample_fields() -> Array:\n    if not _uses_body() and _paths.is_empty(): return [true]\n    return super.sample_fields()",
            """var profile_names: Array[StringName] = []

func sample_fields() -> Array:
    if _paths.is_empty(): return [true]
    if profile_names.is_empty():
        for path in _paths: profile_names.append(path.get_subname(0))
    var target := replication_root()
    var fields := []
    for name in profile_names:
        fields.append(Values.new().encode(target.get(name), "integer64", 8))
    return fields""")
        if packed_scalar:
            text = replace_once(text, '        fields.append(Values.new().encode(target.get(name), "integer64", 8))',
                '        var encoded := PackedByteArray()\n        encoded.resize(8)\n'
                '        encoded.encode_s64(0, target.get(name))\n        fields.append(encoded)')
    signature = "func _physics_process(dt:float) -> void:"
    files[replica] = replace_once(text, signature, signature + """
    var started := Time.get_ticks_usec()
    measured_replica_step(dt)
    if ProfileMeter.active:
        ProfileMeter.calls += 1
        ProfileMeter.replica_us += Time.get_ticks_usec() - started

func measured_replica_step(dt:float) -> void:""")
    spawner = "addons/netcode/scene_spawner.gd"
    text = installed_source(spawner)
    text = replace_once(text, "extends Node\n", "extends Node\n" + meter + "\n")
    files[spawner] = replace_once(text, "    var frame := room.advance()",
        "    var started := Time.get_ticks_usec()\n    var frame := room.advance()\n"
        "    if ProfileMeter.active: ProfileMeter.advance_us += Time.get_ticks_usec()-started")
    return files


def process_seconds(process):
    times = [wintypes.FILETIME() for _ in range(4)]
    get_times = ctypes.windll.kernel32.GetProcessTimes
    get_times.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    if not get_times(int(process._handle), *(ctypes.byref(t) for t in times)):
        raise ctypes.WinError()
    return sum((t.dwHighDateTime << 32) + t.dwLowDateTime for t in times[2:]) / 1e7


class NativeProfile:
    def __init__(self, peers):
        self.peers = peers

    def snapshot(self):
        return {role:(time.perf_counter(), process_seconds(proc)) for role,proc in self.peers.items()}

    def start(self):
        self.initial = self.snapshot()

    def stop(self):
        end = self.snapshot()
        return {role:{"wall_s":t-self.initial[role][0],"process_cpu_s":cpu-self.initial[role][1]}
                for role,(t,cpu) in end.items()}


def trial(peers, wait, command, workload, backend, addon):
    count = prepare_window(peers, wait, command)
    profiler = BrowserProfile(peers, addon) if backend == "fusion" else NativeProfile(peers)
    profiler.start()
    start_window(wait, command)
    idle = workload == "idle"
    writer = wait("A", "writer")
    if backend == "native":
        cpu = profiler.stop()
    command("B", "writer_done")
    clocks = {role:wait(role, "meter") for role in peers}
    if backend == "fusion":
        profiler.stop()
        cpu = profiler.summarize({role: values["browser_bounds"] for role, values in clocks.items()})
    proxy = wait("B", "observer")
    result = {"benchmark":summarize(count, workload, writer, proxy, 20), "profile":cpu,
              "meter":clocks}
    if backend == "fusion":
        result["meter"] = {role: {key: clock[key] for key in ("elapsed_us", "ticks")}
                           for role, clock in result["meter"].items()}
    if idle:
        command("A", "audit_arm")
        command("B", "audit_arm")
        audit = {"baseline": (15485863+32452843)+1, "proxy_local":wait("B","local_assignment"),
                 "proxy_after_750ms":wait("B","quiet_value")}
        command("A","audit_sibling")
        audit["owner_before_sibling"] = wait("A","owner_before")
        audit["proxy_on_sibling"] = wait("B","sibling_arrived")
        command("A","audit_target")
        audit["proxy_on_target"] = wait("B","target_arrived")
        result["local_edit_after_profile"] = audit
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", choices=("native","fusion"))
    parser.add_argument("--workload", choices=("idle","sparse","dense"), default="idle")
    controls = parser.add_mutually_exclusive_group()
    controls.add_argument("--cached-scalar", action="store_true", help="Cached root integer reads, stock integer64 codec")
    controls.add_argument("--packed-scalar", action="store_true", help="Cached root integer reads, identical eight-byte values packed directly")
    parser.add_argument("--addon", type=Path, default=ADDON)
    parser.add_argument("--sdk-zip", type=Path, help="Read and run instrumented sources from this native archive")
    parser.add_argument("--browser", type=Path, default=Path("C:/Program Files/Google/Chrome/Application/chrome.exe"))
    args = parser.parse_args()
    if (args.cached_scalar or args.packed_scalar) and args.backend != "native": parser.error("The comparison patch is native only")
    if args.sdk_zip and args.backend != "native": parser.error("--sdk-zip requires the native backend")
    args.counts = [5000]
    run_trials(args, partial(project_files, cached_scalar=args.cached_scalar, packed_scalar=args.packed_scalar,
                            sdk_zip=args.sdk_zip), (), "property-profile",
               controller=partial(trial, backend=args.backend, addon=args.addon))


if __name__ == "__main__":
    main()
