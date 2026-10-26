#!/usr/bin/env python3
"""Build the NOS atlas. Python 3 / stdlib only; no network or firmware access.

Run from any directory. --check is read-only: checks layout, references, text
budget, membership and byte-for-byte freshness against the current inputs.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import textwrap

ROOT = Path(__file__).resolve().parents[1]
TITLES = ("NOS System Map", "NOS Flows", "NOS Schedule", "NOS Gaps")
STATUS = {"working": "4", "partial": "3", "stub": "1", "broken": "1",
          "dead-code": "#808890"}
SEVERITY = {"critical": "1", "high": "2", "medium": "3", "low": "5"}
GUTTER = 56
CARD = 400
SYSTEMS = ("Health and activity", "Sports and workouts", "Power and battery",
           "Services hub", "Storage and persistence", "Engine and scheduling",
           "Connectivity and voice", "Shell and faces", "Utility apps", "Settings and developer")
EDGE_COLORS = {"data": "5", "control": "2", "persist": "4", "schedule": "6", "ui": "3", "event": "1"}
STRATEGY = ("Real component arrows, capped at 140: retain every app/face ↔ service link, "
            "service → store link and driver/task → service link; fill remaining places "
            "with inter-lane relationships, prioritising flow pairs, gap endpoints, "
            "persistence and app/face coverage. Parallel directed pairs collapse into "
            "one labelled arrow. Directions and MISSING labels remain canonical. "
            "Focused canvases retain EVERY edge incident to their members, with one-hop "
            "neighbours as ghost cards. Cyan data · orange control · green persist · "
            "purple schedule · yellow UI · red event. Red card borders mark high/critical gaps.")

# Only unambiguous renamed components are reconciled. Broad obsolete aggregates
# (e.g. store-sd, store-nvs, hw-strap) stay explicitly unresolved, never guessed.
GAP_ALIASES = {
    "eng-frame": "eng-nos-frame", "eng-nos": "eng-nos-frame",
    "eng-ota": "svc-ota", "eng-power": "eng-power-states",
    "hw-battery": "hw-battery-adc", "hw-ble": "drv-ble",
    "hw-env": "hw-strap-env", "hw-gps": "hw-strap-gps",
    "hw-haptic": "hw-strap-haptic", "hw-imu": "hw-qmi8658-imu",
    "hw-light": "hw-strap-light", "hw-lte": "hw-strap-lte",
    "hw-mag": "hw-strap-mag", "hw-ppg": "hw-strap-hr",
    "hw-rtc": "hw-pcf85063-rtc", "hw-speaker": "hw-strap-audio",
    "hw-touch": "hw-ft3168-touch", "hw-wifi": "drv-wifi",
    "store-ledger": "store-nvs-sportsfood", "store-prefs": "store-nos-prefs",
    "store-sports-journal": "store-sd-sports", "svc-alarm": "svc-alarms",
    "svc-notif": "svc-notifications", "task-display": "task-flush",
    "eng-blur": "eng-blur-live", "eng-flash-tools": "ext-host-tools",
    "eng-usb-host": "drv-usb-serial", "hw-strap": "drv-ext-hal",
    "store-nvs": "drv-hal-store", "store-sd": "drv-sd", "svc-phone": "svc-phone-link",
}


def resolve_gaps(gaps, lookup):
    resolved = []
    for gap in gaps:
        item = dict(gap)
        item["original_nodes"] = gap.get("original_nodes", gap.get("nodes", []))
        item["nodes"] = list(dict.fromkeys(i if i in lookup else GAP_ALIASES.get(i, i)
                                           for i in gap.get("nodes", [])))
        resolved.append(item)
    return resolved


def canvas_path(root, title):
    return root / ("" if title in TITLES else "canvases") / (title + ".canvas")


def dump(value):
    """Compact JSON: one canvas node/edge (or report entry) per line, to keep diffs small."""
    def one(item):
        return json.dumps(item, ensure_ascii=False, separators=(",", ":"))
    if not isinstance(value, dict):
        return one(value) + "\n"
    parts = []
    for key, item in value.items():
        if isinstance(item, list) and item:
            parts.append(one(key) + ":[\n" + ",\n".join(one(i) for i in item) + "\n]")
        elif isinstance(item, dict) and item:
            parts.append(one(key) + ":{\n" + ",\n".join(one(k) + ":" + one(v) for k, v in item.items()) + "\n}")
        else:
            parts.append(one(key) + ":" + one(item))
    return "{\n" + ",\n".join(parts) + "\n}\n"


def plain(value):
    value = re.sub(r"\[\[([^\]|]+)(?:\|([^\]]+))?\]\]",
                   lambda m: m[2] or m[1], value)
    return value.replace("**", "").replace("`", "")


def lines(value, width):
    count = max(1, int((width - 48) / 7.5))
    return [part for line in plain(value).split("\n")
            for part in (textwrap.wrap(line, count, break_long_words=True,
                                      break_on_hyphens=False) or [""])]


def height(value, width=CARD):
    return max(160, math.ceil((len(lines(value, width)) * 24 + 56) / 8) * 8)


def short(value, limit=45):
    return textwrap.shorten(" ".join(str(value).split()), limit, placeholder="…")


def link(note, label="Open note"):
    return f"[[{str(note).removesuffix('.md')}|{label}]]" if note else "Note not supplied"


def prose(value):
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False, sort_keys=True)
    return str(value or "Not supplied")


class Canvas:
    def __init__(self, title):
        self.title = title
        self.nodes = []
        self.edges = []
        self.parents = {}
        self.meta = {}

    def card(self, ident, text, x, y, width=CARD, h=None, color=None,
             parent=None, **meta):
        node = dict(id=ident, type="text", x=int(x), y=int(y), width=int(width),
                    height=int(h or height(text, width)), text=text)
        if color:
            node["color"] = color
        self.nodes.append(node)
        if parent:
            self.parents[ident] = parent
        self.meta[ident] = meta
        return node

    def group(self, ident, label, x, y, width, h, color="5", **meta):
        node = dict(id=ident, type="group", x=int(x), y=int(y), width=int(width),
                    height=int(h), label=label, color=color)
        self.nodes.append(node)
        self.meta[ident] = meta
        return node

    def edge(self, source, target, label="", side="right", end="left", color="5"):
        self.edges.append(dict(id=f"edge-{len(self.edges):04d}", fromNode=source,
                               fromSide=side, toNode=target, toSide=end,
                               toEnd="arrow", color=color, label=label))

    def data(self):
        # Groups first: JSON Canvas specifies array order as z-order.
        return {"nodes": sorted(self.nodes, key=lambda n: n["type"] != "group"),
                "edges": self.edges}

    def banner(self, subtitle, width=3600):
        text = f"**{self.title} / NOS · 1.43-B**\n\n{subtitle}"
        node = self.card("atlas-intro", text, 0, 0, width=width, color="5")
        return node["height"] + 96


def load_inputs(root=ROOT):
    warnings = []
    graph_path = root / "data/graph.json"
    gap_path = root / "data/gaps.json"
    hashes = {}
    def read(path):
        if not path.exists():
            hashes[path.name] = None
            return None
        data = path.read_bytes()
        hashes[path.name] = hashlib.sha256(data).hexdigest()
        if not data.strip():
            return None
        return json.loads(data.decode("utf-8-sig"))
    graph = read(graph_path)
    if graph is None:
        warnings.append("graph.json is missing or empty; component maps are pending.")
        graph = {key: [] for key in ("nodes", "edges", "lanes", "flows", "schedule")}
    if not isinstance(graph, dict):
        raise ValueError("graph.json must be an object")
    for key in ("nodes", "edges", "lanes", "flows", "schedule"):
        if key not in graph or not isinstance(graph[key], list):
            raise ValueError(f"graph.json.{key} must be an array")
    gaps = read(gap_path)
    available = gaps is not None
    if gaps is None:
        warnings.append("gaps.json is missing or empty; verified gap annotations are pending.")
        gaps = []
    elif isinstance(gaps, dict):
        if "gaps" in gaps:
            gaps = gaps["gaps"]
        elif "id" in gaps:
            gaps = [gaps]
        elif not gaps:
            gaps = []
        else:
            raise ValueError("gaps.json must be a gap array or an object containing gaps[]")
    if not isinstance(gaps, list):
        raise ValueError("gaps.json must contain an array")
    return graph, gaps, available, warnings, hashes


def audit(graph, gaps, root=ROOT):
    issues = []
    for key in ("nodes", "lanes", "flows"):
        ids = [item.get("id") for item in graph[key]]
        if any(not isinstance(i, str) or not i for i in ids):
            raise ValueError(f"Missing or invalid {key} id")
        duplicate = [i for i, count in Counter(ids).items() if count > 1]
        if duplicate:
            raise ValueError(f"Duplicate {key} ids: {duplicate}")
    ids = {n["id"] for n in graph["nodes"]}
    lanes = {n["id"] for n in graph["lanes"]}
    notes = {p.stem for p in root.rglob("*.md")}
    degree = Counter()
    for n in graph["nodes"]:
        if n.get("lane") not in lanes:
            issues.append(f"Unknown/missing lane on {n['id']}; displayed in Unassigned.")
        if n.get("status") not in {*STATUS, "unknown"}:
            issues.append(f"Unknown status on {n['id']}: {n.get('status')}")
        note = n.get("note", "")
        if not note or Path(note).stem not in notes:
            issues.append(f"Missing note for {n['id']}: {note or '(none)'}")
    for e in graph["edges"]:
        if e.get("from") not in ids or e.get("to") not in ids:
            issues.append(f"Dangling graph edge omitted: {e}")
        else:
            degree.update([e["from"], e["to"]])
    for n in graph["nodes"]:
        if not degree[n["id"]]:
            issues.append(f"Orphan component (no canonical edges): {n['id']}")
    seen = Counter((e.get("from"), e.get("to"), e.get("kind"), e.get("label"))
                   for e in graph["edges"])
    for key, count in seen.items():
        if count > 1:
            issues.append(f"Duplicate graph relationship x{count}: {key}")
    for f in graph["flows"]:
        for s in f.get("steps", []):
            if s.get("node") not in ids:
                issues.append(f"Unknown component in {f['id']}: {s.get('node')}")
    for s in graph["schedule"]:
        for ident in s.get("runs", []):
            if ident not in ids:
                issues.append(f"Unknown schedule component in {s['context']}: {ident}")
    gap_ids = [g.get("id") for g in gaps]
    if any(not isinstance(i, str) or not i for i in gap_ids) or len(set(gap_ids)) != len(gap_ids):
        raise ValueError("Gap ids must be nonempty unique strings")
    for gap in gaps:
        if not isinstance(gap.get("nodes", []), list):
            raise ValueError(f"{gap['id']}.nodes must be an array")
        if gap.get("severity", "").lower() not in SEVERITY:
            issues.append(f"Unknown severity on {gap['id']}: {gap.get('severity')}")
        for ident in gap.get("nodes", []):
            if ident not in ids:
                issues.append(f"Unknown affected component in {gap['id']}: {ident}")
    return issues


def component_text(n, gap_index, available, full=False):
    summary = n.get("summary", "Summary not supplied")
    text = (f"**{n['label']}**\n{summary if full else short(summary)}\n\n"
            f"runs: {n.get('runs', 'Not supplied')}\n\n"
            f"`{n.get('status', 'unknown').upper()}`")
    if available:
        count = len(gap_index[n["id"]])
        if count:
            text += f" · ⚠ [[Gaps|{count} gap{'s' if count != 1 else ''}]]"
    return text + "\n" + link(n.get("note"))


def lane_order(graph, edges=None):
    lanes = [dict(l) for l in graph["lanes"]]
    known = {l["id"] for l in lanes}
    if any(n.get("lane") not in known for n in graph["nodes"]):
        lanes.append({"id": "unassigned", "title": "Unassigned / data needs repair",
                      "description": "Components with unknown or missing lane ids."})
    rows = {l["id"]: [] for l in lanes}
    membership = {}
    for n in graph["nodes"]:
        key = n.get("lane") if n.get("lane") in known else "unassigned"
        rows[key].append(n["id"])
        membership[n["id"]] = key
    neighbours = defaultdict(list)
    for e in graph["edges"] if edges is None else edges:
        if e.get("from") in membership and e.get("to") in membership:
            neighbours[e["from"]].append(e["to"])
            neighbours[e["to"]].append(e["from"])
    lane_ids = [l["id"] for l in lanes]
    # Fixed number of stable barycentre sweeps; normalised ranks work across
    # lanes of different lengths. Disconnected nodes keep their previous rank.
    for sweep in range(8):
        order = range(len(lanes)) if sweep % 2 == 0 else reversed(range(len(lanes)))
        for i in order:
            row = rows[lane_ids[i]]
            ranks = {ident: j / max(1, len(r) - 1) for r in rows.values()
                     for j, ident in enumerate(r)}
            adjacent = set(lane_ids) - {lane_ids[i]}
            def score(ident):
                weights = [ranks[k] for k in neighbours[ident] if membership[k] in adjacent]
                return (sum(weights) / len(weights) if weights else ranks[ident], ranks[ident], ident)
            row.sort(key=score)
    return lanes, rows, membership


def select_spine(graph, lookup, gap_index, limit=140):
    pairs = defaultdict(list)
    for e in graph["edges"]:
        if e["from"] in lookup and e["to"] in lookup:
            pairs[e["from"], e["to"]].append(e)
    flow_pairs = set()
    for flow in graph["flows"]:
        steps = [s["node"] for s in flow["steps"]]
        flow_pairs.update(zip(steps, steps[1:]))
    def priority(pair):
        a, b = pair
        kinds = {e["kind"] for e in pairs[pair]}
        return (-(pair in flow_pairs or (b, a) in flow_pairs),
                -(bool(gap_index[a]) + bool(gap_index[b])),
                -("persist" in kinds), a, b)
    mandatory = set()
    for pair in pairs:
        a, b = (lookup[i]["lane"] for i in pair)
        if ((a in ("apps", "faces") and b == "services") or
            (b in ("apps", "faces") and a == "services") or
            (a == "services" and b == "stores") or
            (a in ("drivers", "tasks") and b == "services")):
            mandatory.add(pair)
    # Also retain at least one real relationship for every connected app/face,
    # including faces fed through the shared registry rather than a service.
    for ident, node in lookup.items():
        if node["lane"] in ("apps", "faces") and not any(ident in p for p in mandatory):
            choices = [p for p in pairs if ident in p]
            if choices:
                mandatory.add(min(choices, key=priority))
    chosen = sorted(mandatory, key=priority)
    extras = sorted((p for p in pairs if p not in mandatory and
                     lookup[p[0]]["lane"] != lookup[p[1]]["lane"]), key=priority)
    chosen += extras[:max(0, limit - len(chosen))]
    result = []
    for a, b in chosen:
        originals = pairs[a, b]
        first = min(originals, key=lambda e: (e["kind"] != "data", e["kind"], e["label"]))
        result.append(dict(first, label=" / ".join(dict.fromkeys(
            f"{e['kind']}: {e['label']}" for e in originals)), originals=originals))
    return result


def add_graph_edges(c, edges):
    positions = {n["id"]: n for n in c.nodes}
    for e in edges:
        a, b = positions[e["from"]], positions[e["to"]]
        if a["y"] == b["y"]:
            side = end = "top"  # same-lane arcs use the clear upper gutter
        else:
            side, end = ("bottom", "top") if a["y"] < b["y"] else ("top", "bottom")
        label = e["label"] if "originals" in e else f"{e['kind']}: {e['label']}"
        c.edge(e["from"], e["to"], label, side, end, EDGE_COLORS.get(e["kind"], "5"))


def lane_cards(c, graph, lookup, gap_index, available, edges, y, members=None):
    lanes, rows, _ = lane_order(graph, edges)
    # One physical row per lane prevents arrows crossing cards in a wrapped row.
    columns = max([len(r) for r in rows.values()] or [1])
    width = max(1800, columns * (CARD + GUTTER) - GUTTER + 96)
    for i, lane in enumerate(lanes):
        if not rows[lane["id"]]:
            continue
        texts = {ident: component_text(lookup[ident], gap_index, available) for ident in rows[lane["id"]]}
        if members is not None:
            texts = {ident: text + ("\n↗ GHOST · one-hop neighbour" if ident not in members else "\n● SYSTEM MEMBER")
                     for ident, text in texts.items()}
        h = max([height(t) for t in texts.values()] or [160])
        gh = h + 148
        group = "lane-" + lane["id"]
        c.group(group, f"{i + 1:02d} / {lane['title']} · {len(texts)}", 0, y, width, gh,
                description=lane.get("description", ""), lane=lane["id"])
        offset = (width - (len(texts) * (CARD + GUTTER) - GUTTER)) // 2
        for j, ident in enumerate(rows[lane["id"]]):
            n = lookup[ident]
            ghost = members is not None and ident not in members
            c.card(ident, texts[ident], offset + j * (CARD + GUTTER), y + 100, h=h,
                   color="#808890" if ghost else STATUS.get(n.get("status")),
                   parent=group, source=ident, ghost=ghost)
        y += gh + 320
    add_graph_edges(c, edges)


def overview(graph, lookup, gap_index, available):
    c = Canvas(TITLES[0])
    edges = select_spine(graph, lookup, gap_index)
    y = c.banner(f"{len(lookup)} components · {len(graph['edges'])} canonical relationships · {len(edges)} visible arrows\n"
                 f"{STRATEGY}\nGreen working · yellow partial · red stub/broken · grey dead code. "
                 + ("Verified gap badges included." if available else "Gap verification pending."))
    text = "**Explore a system / all incident edges**\n\n" + "\n".join(
        f"[[canvases/{title}.canvas|{title}]]" for title in SYSTEMS)
    index = c.card("system-index", text, 0, y, 800, color="6", system_index=True)
    lane_cards(c, graph, lookup, gap_index, available, edges, y + index["height"] + 160)
    return c


def system_members(graph, title):
    notes = {
        "Health and activity": {"Activity", "Heart rate", "Breathe"},
        "Sports and workouts": {"Sports", "IMU", "Energy"},
        "Power and battery": {"Battery settings", "Power - screen-off screen", "Jobs"},
        "Services hub": {"Alarms", "Timer", "Stopwatch", "Notifications", "Incoming call screen", "Music", "Find phone", "Weather"},
        "Storage and persistence": set(),
        "Engine and scheduling": {"Rendering pipeline"},
        "Connectivity and voice": {"Chat", "Cellular", "Connectivity settings"},
        "Shell and faces": {"Home pager", "Launcher", "Boot screen", "Face picker", "Control Center"},
        "Utility apps": {"Barometer", "Compass", "Navigate", "Recorder", "World clock", "Torch", "Hardware and drivers"},
        "Settings and developer": {"Settings", "Developer tools", "Diagnostics", "Modules", "About"},
    }[title] | {title}
    followups = {"Storage and persistence": ("Followup 3",),
                 "Engine and scheduling": ("Followup 4",),
                 "Connectivity and voice": ("Followup 5", "Followup 6"),
                 "Services hub": ("Followup 1",)}.get(title, ())
    return {n["id"] for n in graph["nodes"] if
            Path(n.get("note", "")).stem in notes or Path(n.get("note", "")).stem.startswith(followups) or
            (title == "Shell and faces" and n["lane"] == "faces") or
            (title == "Storage and persistence" and n["lane"] == "stores")}


def system_canvas(title, graph, lookup, gap_index, available):
    members = system_members(graph, title)
    edges = [e for e in graph["edges"] if e["from"] in lookup and e["to"] in lookup
             and (e["from"] in members or e["to"] in members)]
    included = members | {i for e in edges for i in (e["from"], e["to"])}
    subgraph = dict(graph, nodes=[n for n in graph["nodes"] if n["id"] in included])
    c = Canvas(title)
    y = c.banner(f"{len(members)} system members · {len(included - members)} one-hop ghost neighbours · {len(edges)} canonical edges\n"
                 "Every incoming and outgoing edge of a member is shown, including parallel edges. "
                 "Ghost cards retain status text and gap badges; grey borders mark neighbours unless a high/critical gap requires red. "
                 "Arrows retain source direction; MISSING means an absent connection, not working data flow.\n"
                 "Cyan data · orange control · green persist · purple schedule · yellow UI · red event.\n"
                 "[[NOS System Map.canvas|Return to overview]]")
    lane_cards(c, subgraph, lookup, gap_index, available, edges, y, members)
    return c


def flows_canvas(graph, lookup, gap_index, available):
    c = Canvas(TITLES[1])
    y = c.banner("Read each flow left → right. Arrows show narrative step order, not additional canonical graph edges. "
                 "Red steps touch a verified gap; a badge lists how many. The three reported symptoms lead the atlas.")
    complaints = [
        ("01 / Badminton recorded no calories", "workout.energy requires a nonzero body mass. The default is unset; the Start screen does not prompt for it. Actual session settings are not established by this source audit.", "flow-workout"),
        ("02 / No movement graph for the game", "The session retains aggregate motion counters, not a time series, and Sports has no session chart. Whole-day step bins do not describe court movement.", "flow-workout"),
        ("03 / Passive calories missing from today", "The daily estimate is steps × 0.04. There is no basal-energy calculation, and workout energy is not merged into that daily total.", "flow-passive-calories")]
    texts = [f"**{title}**\n\n{body}\n\n[[Calorie trace|Source trace]]\nTrace: `{flow}`" for title, body, flow in complaints]
    h = max(height(t, 640) for t in texts)
    c.group("reported-symptoms", "START HERE / Three reported symptoms · source audit, not recovered session telemetry", 0, y, 2128, h + 148, "2")
    for i, text in enumerate(texts):
        c.card(f"complaint-{i+1}", text, 48 + i * 696, y + 100, 640, h, "2", "reported-symptoms")
    y += h + 268
    ordered = sorted(enumerate(graph["flows"]), key=lambda p: (0 if p[1]["id"] == "flow-workout" else 1 if p[1]["id"] == "flow-passive-calories" else 2, p[0]))
    for _, flow in ordered:
        gid = "flow-" + flow["id"]
        texts = []
        for i, step in enumerate(flow.get("steps", [])):
            n = lookup.get(step["node"], {"label": step["node"] + " / UNRESOLVED"})
            badges = gap_index[step["node"]]
            text = f"**{i + 1:02d} / {n['label']}**\n\n{step['action']}\n\n{link(n.get('note'))}"
            if badges:
                text += f"\n⚠ [[Gaps|{len(badges)} verified gaps]]: " + ", ".join(g["id"] for g in badges)
            texts.append(text)
        h = max([height(t) for t in texts] or [160])
        width = max(1400, len(texts) * (CARD + GUTTER) - GUTTER + 96)
        c.group(gid, flow["title"], 0, y, width, h + 148, "6")
        for i, (step, text) in enumerate(zip(flow.get("steps", []), texts)):
            ident = f"{gid}-step-{i:02d}"
            n = lookup.get(step["node"], {})
            c.card(ident, text, 48 + i * (CARD + GUTTER), y + 100, h=h,
                   color="1" if gap_index[step["node"]] else STATUS.get(n.get("status")),
                   parent=gid, source=step["node"], action=step["action"])
            if i:
                c.edge(f"{gid}-step-{i-1:02d}", ident, "next", color="6")
        y += h + 268
    return c


def schedule_canvas(graph, lookup, gap_index, available):
    c = Canvas(TITLES[2])
    y = c.banner("Execution atlas / each schedule record owns a column. UI modes and inline callbacks are separate records, "
                 "not extra FreeRTOS tasks. Core 'n/a' is unspecified, not proof of affinity. "
                 "Cadences and screen-off conditions are copied from graph.json; dormant and C-only paths remain labelled.")
    buckets = [("0", "CORE 0"), ("1", "CORE 1"), ("any", "UNPINNED"), ("other", "AFFINITY UNSPECIFIED / timers, ISRs and deep sleep")]
    for core, title in buckets:
        records = [(i, s) for i, s in enumerate(graph["schedule"])
                   if (str(s.get("core")) if str(s.get("core")) in ("0", "1", "any") else "other") == core]
        if not records:
            continue
        texts = []
        for i, s in records:
            components = "\n".join("• " + lookup.get(k, {}).get("label", k + " / UNRESOLVED") for k in s.get("runs", []))
            texts.append(f"**{s['context']}**\n\nCore: {s.get('core', 'unspecified')}\nPriority: {s.get('priority', 'unspecified')}\n\nCadence: {s.get('cadence', 'unspecified')}\n\nScreen off: {s.get('screen_off', 'unspecified')}\n\n**Runs**\n{components}\n\n[[Engine and scheduling|Execution notes]]")
            affected = {g['id'] for k in s.get('runs', []) for g in gap_index[k]}
            if affected:
                texts[-1] += f"\n⚠ [[Gaps|{len(affected)} gaps in scheduled components]]"
        h = max(height(t, 440) for t in texts)
        gid = "core-" + core
        c.group(gid, f"{title} · {len(records)} schedule records", 0, y, len(records) * 496 + 40, h + 148)
        for j, ((i, s), text) in enumerate(zip(records, texts)):
            c.card(f"context-{i:02d}", text, 48 + j * 496, y + 100, 440, h,
                   "5", gid, sources=s.get("runs", []), schedule=s)
        y += h + 268
    # Classification is deliberately labelled as an index derived from cadence
    # text. Original conditions stay visible, including one-shots and dormancy.
    categories = {"Every frame": [], "Periodically": [], "Event-driven / one-shot": [], "While the screen is off": [], "Dormant / no execution": []}
    for s in graph["schedule"]:
        cadence = s.get("cadence", "")
        low = cadence.lower()
        label = f"• {s['context']} — {cadence}"
        dormant = any(k in low for k in ("not created", "never created", "no execution"))
        if dormant:
            categories["Dormant / no execution"].append(label)
        elif "every frame" in low or "1/fps_cap" in low:
            categories["Every frame"].append(label)
        elif any(k in low for k in ("event driven", "on demand", "one shot", "one-shot", "boot only", "on edge", "per dirty", "per http", "per /ota", "per vibration", "per dma")):
            categories["Event-driven / one-shot"].append(label)
        else:
            categories["Periodically"].append(label)
        off = s.get("screen_off", "")
        if not dormant and (off.startswith("yes") or off.startswith("only if")):
            categories["While the screen is off"].append(f"• {s['context']} — {off}; {cadence}")
    texts = [f"**{title}**\n\n" + "\n\n".join(items or ["No entries supplied."]) for title, items in categories.items()]
    h = max(height(t, 640) for t in texts)
    c.group("behaviour-strip", "RHYTHM INDEX / derived from cadence text · conditions preserved · categories may overlap", 0, y, 3520, h + 148, "6")
    for i, text in enumerate(texts):
        c.card(f"rhythm-{i}", text, 48 + i * 696, y + 100, 640, h, "6", "behaviour-strip")
    y += h + 268
    stores = [n for n in graph["nodes"] if n.get("lane") == "stores"]
    for kinds, title in [(('ram', 'dram'), 'RAM / DRAM'), (('psram',), 'PSRAM'), (('nvs',), 'NVS'), (('sd',), 'SD'), (('rtc', 'flash'), 'RTC / Flash')]:
        selected = [n for n in stores if n.get("kind") in kinds]
        if not selected:
            continue
        texts = [component_text(n, gap_index, available, full=True) for n in selected]
        h = max(height(t, 480) for t in texts)
        rows = math.ceil(len(texts) / 4)
        gid = "storage-" + kinds[0]
        c.group(gid, f"DATA STORES / {title} · contents, persistence and flush policy from source", 0, y, 2184, 148 + rows * (h + GUTTER) - GUTTER, "2")
        for j, (n, text) in enumerate(zip(selected, texts)):
            c.card("storage-copy-" + n["id"], text, 48 + (j % 4) * 536,
                   y + 100 + (j // 4) * (h + GUTTER), 480, h,
                   STATUS.get(n.get("status")), gid, source=n["id"])
        y += 268 + rows * (h + GUTTER) - GUTTER
    return c


def gaps_canvas(gaps, lookup, gap_index, available):
    c = Canvas(TITLES[3])
    y = c.banner("Verified gaps / clustered by area. Critical red · high orange · medium yellow · low cyan. "
                 "Arrows identify affected components; repeated components are local copies, not additional systems.", 2184)
    complaints = [
        ("Badminton workout had no calories", "Body mass is unset by default; workout calories require it.", ("G01",)),
        ("No per-minute movement graph for a session", "Aggregate motion counters do not retain the session time series needed for a graph.", ("G06",)),
        ("No basal / passive calories", "No BMR calculation or age/sex/height profile; daily energy uses steps × 0.04.", ("G04", "G05")),
    ]
    by_id = {g["id"]: g for g in gaps}
    texts = []
    for title, body, ids in complaints:
        matches = [by_id[k] for k in ids if k in by_id]
        state = "Verified: " + ", ".join(g["id"] for g in matches) if matches else "Reported symptom · verification entry pending"
        texts.append(f"**{title}**\n\n{body}\n\n{state}\n[[Gaps|Gap register]] · [[Calorie trace|Source trace]]")
    h = max(height(t, 640) for t in texts)
    c.group("today", "The three things the user hit today", 0, y, 2184, h + 148, "2")
    for i, text in enumerate(texts):
        c.card(f"today-{i}", text, 48 + i * 696, y + 100, 640, h, "2", "today")
    y += h + 268
    if not gaps:
        text = ("**No verified entries supplied**\n\nThe gaps dataset is present but has no entries. This is not a claim that the OS has no gaps."
                if available else "**Verification pending**\n\ngaps.json is missing or empty. Rerun the generator when it arrives. No gap badges or verified gap cards have been inferred.\n\nThe Flows canvas still shows the three reported symptoms from [[Calorie trace]].")
        c.card("gap-data-state", text, 0, y, 800, color="3")
        return c
    areas = defaultdict(list)
    for gap in gaps:
        areas[prose(gap.get("area", "Unassigned"))].append(gap)
    rank = {key: i for i, key in enumerate(SEVERITY)}
    for ai, (area, items) in enumerate(sorted(areas.items())):
        gid = f"gap-area-{ai:02d}"
        group = c.group(gid, area, 0, y, 2184, 160, "2")
        cy = y + 100
        for gap in sorted(items, key=lambda g: (rank.get(g.get("severity", "").lower(), 4), g["id"])):
            severity = gap.get("severity", "unknown").lower()
            text = (f"**{gap['id']} / {gap.get('title', 'Untitled')}**\n\n`{severity.upper()}`\n\n"
                    f"Impact: {prose(gap.get('user_impact'))}\n\nEvidence: {prose(gap.get('evidence'))}\n\n"
                    f"Suggested fix: {prose(gap.get('suggested_fix'))}\n\nVotes: {prose(gap.get('votes'))}\n\n"
                    f"Nodes: {', '.join(gap.get('nodes', [])) or 'None supplied'}\n\n[[Gaps|Full gap register]]")
            if gap.get("original_nodes", []) != gap.get("nodes", []):
                text += "\nSource IDs (before rename mapping): " + ", ".join(gap["original_nodes"])
            gn = c.card("gap-" + gap["id"], text, 48, cy, 640, color=SEVERITY.get(severity), parent=gid, gap=gap)
            ids = list(dict.fromkeys(gap.get("nodes", [])))
            copies = []
            for ident in ids:
                n = lookup.get(ident, {"id": ident, "label": ident + " / UNRESOLVED", "summary": "Referenced by the gaps dataset but absent from graph.json.", "status": "unknown"})
                copies.append((ident, n, component_text(n, gap_index, available)))
            h = max([height(t) for _, _, t in copies] or [160])
            for j, (ident, n, ct) in enumerate(copies):
                cid = f"gap-{gap['id']}-component-{j}"
                c.card(cid, ct, 768 + j % 3 * 456, cy + j // 3 * (h + GUTTER), h=h,
                       color=STATUS.get(n.get("status")), parent=gid, source=ident)
                c.edge(gn["id"], cid, "affects", color=SEVERITY.get(severity, "5"))
            cy += max(gn["height"], math.ceil(len(copies) / 3) * (h + GUTTER) - GUTTER) + 96
        group["height"] = cy - y - 48
        y = cy + 72
    return c


def build(graph, gaps, available):
    lookup = {n["id"]: n for n in graph["nodes"]}
    gaps = resolve_gaps(gaps, lookup)
    gap_index = defaultdict(list)
    for gap in gaps:
        for ident in set(gap.get("nodes", [])):
            gap_index[ident].append(gap)
    canvases = [overview(graph, lookup, gap_index, available),
            flows_canvas(graph, lookup, gap_index, available),
            schedule_canvas(graph, lookup, gap_index, available),
            gaps_canvas(gaps, lookup, gap_index, available)]
    canvases += [system_canvas(title, graph, lookup, gap_index, available) for title in SYSTEMS]
    for c in canvases:
        for n in c.nodes:
            meta = c.meta[n["id"]]
            sources = [meta["source"]] if "source" in meta else meta.get("sources", [])
            severe = {g["id"] for ident in sources for g in gap_index[ident]
                      if g.get("severity", "").lower() in ("critical", "high")}
            if severe:
                n["color"] = "1"
                meta["severe_gaps"] = sorted(severe)
    return canvases


def contains(a, b):
    return (a["x"] <= b["x"] and a["y"] <= b["y"] and
            a["x"] + a["width"] >= b["x"] + b["width"] and
            a["y"] + a["height"] >= b["y"] + b["height"])


def validate(data, parents):
    errors = []
    nodes, edges = data.get("nodes", []), data.get("edges", [])
    ids = [n.get("id") for n in nodes] + [e.get("id") for e in edges]
    if len(ids) != len(set(ids)) or any(not isinstance(i, str) or not i for i in ids):
        errors.append("Ids must be unique, nonempty strings (nodes and edges).")
    lookup = {n["id"]: n for n in nodes}
    for n in nodes:
        if n.get("type") not in ("text", "file", "group"):
            errors.append(f"Invalid node type: {n['id']}")
        for key in ("x", "y", "width", "height"):
            if type(n.get(key)) is not int or (key in ("width", "height") and n[key] <= 0):
                errors.append(f"Invalid geometry: {n['id']}.{key}")
        if "color" in n and not re.fullmatch(r"[1-6]|#[0-9a-fA-F]{6}", n["color"]):
            errors.append(f"Invalid colour: {n['id']}")
        required = {"text": "text", "file": "file", "group": "label"}.get(n.get("type"))
        if required and not isinstance(n.get(required), str):
            errors.append(f"Missing {required}: {n['id']}")
        if n.get("type") == "text" and height(n.get("text", ""), n["width"]) > n["height"]:
            errors.append(f"Text exceeds estimated card capacity: {n['id']}")
    cards = [n for n in nodes if n["type"] != "group"]
    groups = [n for n in nodes if n["type"] == "group"]
    for i, a in enumerate(cards):
        for b in cards[i + 1:]:
            dx = max(a["x"], b["x"]) - min(a["x"] + a["width"], b["x"] + b["width"])
            dy = max(a["y"], b["y"]) - min(a["y"] + a["height"], b["y"] + b["height"])
            if dx < 0 and dy < 0:
                errors.append(f"Overlapping cards: {a['id']} / {b['id']}")
            elif (dx < 0 and 0 <= dy < 40) or (dy < 0 and 0 <= dx < 40):
                errors.append(f"Card gutter under 40 px: {a['id']} / {b['id']}")
        for g in groups:
            intersects = (max(a["x"], g["x"]) < min(a["x"] + a["width"], g["x"] + g["width"]) and
                          max(a["y"], g["y"]) < min(a["y"] + a["height"], g["y"] + g["height"]))
            if intersects and not contains(g, a):
                errors.append(f"Card crosses group boundary: {a['id']} / {g['id']}")
    for child, parent in parents.items():
        if child not in lookup or parent not in lookup:
            errors.append(f"Missing group/member: {parent} / {child}")
        elif lookup[parent]["type"] != "group" or not contains(lookup[parent], lookup[child]):
            errors.append(f"Child outside its assigned group: {child} / {parent}")
    for e in edges:
        if e.get("fromNode") not in lookup or e.get("toNode") not in lookup:
            errors.append(f"Dangling edge: {e.get('id')}")
        if e.get("fromSide") not in ("top", "right", "bottom", "left") or e.get("toSide") not in ("top", "right", "bottom", "left") or e.get("toEnd") != "arrow":
            errors.append(f"Invalid edge endpoints: {e.get('id')}")
    return errors


def render_html(canvases, graph, gaps, available, warnings):
    payload = dict(canvases={c.title: c.data() for c in canvases},
                   metadata={c.title: c.meta for c in canvases},
                   graph=graph, gaps=resolve_gaps(gaps, {n['id']: n for n in graph['nodes']}), gapsAvailable=available,
                   warnings=warnings, strategy=STRATEGY)
    # Escape '<' so a source string cannot close the application/json script.
    encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    template = (Path(__file__).parent / "preview_template.html").read_text(encoding="utf-8")
    return template.replace("__ATLAS_JSON__", encoded)


def artifacts(root=ROOT):
    graph, gaps, available, warnings, hashes = load_inputs(root)
    resolved = resolve_gaps(gaps, {n['id']: n for n in graph['nodes']})
    warnings += audit(graph, resolved, root)
    canvases = build(graph, gaps, available)
    for c in canvases:
        errors = validate(c.data(), c.parents)
        if errors:
            raise ValueError(c.title + ":\n" + "\n".join(errors))
    report = dict(inputs=hashes, gap_data_available=available, gaps=len(gaps),
                  source_counts={k: len(graph[k]) for k in ("nodes", "edges", "flows", "schedule")},
                  edge_strategy=STRATEGY, warnings=warnings,
                  gap_id_aliases={k: v for k, v in GAP_ALIASES.items() if any(k in g.get('nodes', []) for g in gaps)},
                  unresolved_gap_nodes=sorted({i for g in resolved for i in g['nodes']} - {n['id'] for n in graph['nodes']}),
                  canvases={c.title: dict(nodes=len(c.nodes), cards=sum(n["type"] != "group" for n in c.nodes),
                                         groups=sum(n["type"] == "group" for n in c.nodes), edges=len(c.edges),
                                         parents=c.parents) for c in canvases})
    output = {canvas_path(root, c.title): dump(c.data()) for c in canvases}
    output[root / "preview/system_map.html"] = render_html(canvases, graph, gaps, available, warnings)
    output[root / "canvas-tools/build_report.json"] = dump(report)
    return output, canvases, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Validate existing artifacts and freshness; write nothing")
    args = parser.parse_args()
    try:
        output, canvases, report = artifacts()
        errors = []
        if args.check:
            for c in canvases:
                path = canvas_path(ROOT, c.title)
                if path.exists():
                    try:
                        errors += [f"{c.title}: {e}" for e in validate(json.loads(path.read_text(encoding="utf-8")), c.parents)]
                    except (ValueError, KeyError, TypeError) as exc:
                        errors.append(f"{c.title}: invalid canvas: {exc}")
            for path, expected in output.items():
                if not path.exists():
                    errors.append(f"Missing generated file: {path.relative_to(ROOT)}")
                elif path.read_bytes() != expected.encode("utf-8"):
                    errors.append(f"Stale or edited output: {path.relative_to(ROOT)}; rerun generator")
        else:
            for path, value in output.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value.encode("utf-8"))
                print(f"WROTE {path.relative_to(ROOT)}")
        for name, count in report["canvases"].items():
            print(f"{name}: {count['nodes']} nodes ({count['cards']} cards + {count['groups']} groups), {count['edges']} edges")
        for warning in report["warnings"]:
            print("DATA: " + warning)
        if errors:
            print("\n".join("FAIL: " + e for e in errors), file=sys.stderr)
            return 1
        print("PASS: JSON Canvas structure, unique ids, endpoints, card gutters, text budget, group containment" +
              (" and byte-for-byte freshness." if args.check else "."))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
