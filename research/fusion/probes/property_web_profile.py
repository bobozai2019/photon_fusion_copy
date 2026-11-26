"""Aggregate the two Chrome main-isolate CPU profiles without collecting packet contents."""
from collections import Counter
import base64
import hashlib
from io import BytesIO
import re
from zipfile import ZipFile


def module_files(addon):
    template = next(addon.glob("export_templates/**/godot.web.template_debug*.zip"))
    with ZipFile(template) as arc:
        sizes = {arc.getinfo(n).file_size: n for n in arc.namelist() if n.endswith(".wasm")}
    plugin = next((addon / "bin").glob("*web.template_debug*.so"))
    sizes[plugin.stat().st_size] = "fusion_extension"
    return sizes, plugin


def export_names(plugin):
    def uint(stream):
        value = shift = 0
        while True:
            byte = stream.read(1)[0]
            value |= (byte & 127) << shift
            if byte < 128: return value
            shift += 7
    stream = BytesIO(plugin.read_bytes())
    if stream.read(8) != b"\x00asm\x01\x00\x00\x00": raise ValueError("Expected Wasm module")
    names = {}
    while section := stream.read(1):
        data = BytesIO(stream.read(uint(stream)))
        if section != b"\x07": continue
        for _ in range(uint(data)):
            name = data.read(uint(data)).decode()
            kind = data.read(1)
            index = uint(data)
            if kind == b"\x00": names[index] = name
        break
    return names


def summarize_samples(profile, scripts, sizes, bounds, names):
    nodes = {node["id"]: node for node in profile["nodes"]}
    parents = {child: node["id"] for node in nodes.values() for child in node.get("children", [])}
    modules = {}
    for sid, script in scripts.items():
        if script.get("scriptLanguage") == "WebAssembly" or script["url"].startswith("wasm:"):
            modules[sid] = sizes.get(script.get("length"), "unmapped_wasm")
            source_map = script.get("sourceMapURL", "")
            if "libfusion.web" in source_map: modules[sid] = "fusion_extension"
    def label(sid, url):
        if sid in modules:
            return modules[sid]
        if url.endswith(".so"): return "fusion_extension"
        if ".wasm" in url or url.startswith("wasm:"): return "unmapped_wasm"
        return "browser_js"
    totals, inclusive, paths, leaf_frames = Counter(), Counter(), Counter(), Counter()
    samples = profile["samples"]
    deltas = profile["timeDeltas"]
    if len(samples) != len(deltas): raise ValueError("Missing correlatable CPU samples")
    lo, hi = bounds
    if not (profile["startTime"] <= lo < hi <= profile["endTime"]): raise ValueError(f"CPU profile misses game window: {profile['startTime']}, {bounds}, {profile['endTime']}")
    selected = 0
    stamp = profile["startTime"]
    for sample, delta in zip(samples, deltas):
        stamp += delta
        if stamp < lo or stamp > hi: continue
        selected += 1
        chain = []
        node = sample
        while node:
            frame = nodes[node]["callFrame"]
            name = frame["functionName"] or "(anonymous)"
            module = label(frame["scriptId"], frame["url"])
            if name in ("(idle)", "(program)", "(garbage collector)", "(root)"):
                module = name
            chain.append((module, name))
            node = parents.get(node)
        totals[chain[0][0]] += 1
        leaf_frames[" / ".join(chain[0])] += 1
        for module in {m for m, name in chain}: inclusive[module] += 1
        fusion = [name for module, name in chain if module == "fusion_extension"]
        if fusion: paths[" <- ".join(fusion[:3])] += 1
    return {
        "duration_s": (profile["endTime"]-profile["startTime"])/1e6,
        "game_window_s": (hi-lo)/1e6, "samples": selected, "uncropped_samples":len(samples),
        "leaf_samples": dict(totals), "inclusive_samples": dict(inclusive),
        "fusion_stacks_top": paths.most_common(12),
        "leaf_frames_top": leaf_frames.most_common(8),
        "matched_exports": {index: names[int(index)] for path, hits in paths.most_common(12)
                            for index in re.findall(r"wasm-function\[(\d+)\]", path) if int(index) in names},
        "wasm_scripts": [{"module": module, "length":scripts[sid].get("length")}
                         for sid, module in modules.items() if module != "unmapped_wasm"],
    }


class BrowserProfile:
    def __init__(self, peers, addon):
        self.roles = {}
        self.sizes, self.plugin = module_files(addon)
        for role, peer in peers.items():
            session = peer.context.new_cdp_session(peer.page)
            session.send("Performance.enable")
            session.send("Profiler.enable")
            session.send("Profiler.setSamplingInterval", {"interval":1000})
            self.roles[role] = session

    @staticmethod
    def metrics(session):
        return {v["name"]:v["value"] for v in session.send("Performance.getMetrics")["metrics"]}

    def start(self):
        self.initial = {}
        for role, session in self.roles.items():
            session.send("Profiler.start")
            self.initial[role] = self.metrics(session)

    def stop(self):
        self.profiles, self.metrics_at_stop = {}, {}
        # Stop both samplers before asking either target for module metadata.
        for role, session in self.roles.items():
            self.metrics_at_stop[role] = self.metrics(session)
            self.profiles[role] = session.send("Profiler.stop")["profile"]

    def summarize(self, game_bounds):
        result = {}
        for role, session in self.roles.items():
            scripts = {}
            session.on("Debugger.scriptParsed", lambda s, scripts=scripts: scripts.update({s["scriptId"]:s}))
            session.send("Debugger.enable")
            # CDP's WebAssembly length can differ from the on-disk size. For ambiguous
            # modules, verify the bytecode of candidates close to the plugin's size.
            plugin_hash = hashlib.sha256(self.plugin.read_bytes()).digest()
            for script in list(scripts.values()):
                size = script.get("length", 0)
                if script.get("scriptLanguage") != "WebAssembly" or size in self.sizes: continue
                if 0 < size < 8_000_000 and "libfusion.web" not in script.get("sourceMapURL", ""):
                    source = session.send("Debugger.getScriptSource", {"scriptId":script["scriptId"]})
                    blob = base64.b64decode(source.get("bytecode",""))
                    if blob and hashlib.sha256(blob).digest() == plugin_hash:
                        self.sizes[size] = "fusion_extension"
            nav_us = self.initial[role]["NavigationStart"] * 1e6
            bounds = [nav_us + ms*1000 for ms in game_bounds[role]]
            row = summarize_samples(self.profiles[role], scripts, self.sizes, bounds, export_names(self.plugin))
            row["cdp_seconds"] = {key: self.metrics_at_stop[role][key]-self.initial[role][key] for key in
                ("Timestamp", "ProcessTime", "ThreadTime", "TaskDuration", "ScriptDuration")
                if key in self.metrics_at_stop[role] and key in self.initial[role]}
            result[role] = row
            session.send("Debugger.disable")
            session.detach()
        return result
