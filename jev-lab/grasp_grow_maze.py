#!/usr/bin/env python3
"""RSI smoke test: the grow loop (probe -> verdict -> pave -> prune) on the maze env.

Env contract (unified_grid_envs.UnifiedMazeEnv) that shapes the loop:
  - only *untried-here* directions are ever offered as candidates;
  - when a cell runs dry, the env auto-repositions to the nearest frontier over
    verified edges (each such move consumes one attempt);
  - so the agent's real decisions are: which untried direction to dig next, and
    what to do when our cross-episode graph claims every offer is a stale wall.

Lanes:
  jev    - System One ranks the fresh candidates (self-contained: dest coords are in
           the option text); used iff top probability >= --min-p
  llm    - System Two paver, fires on (a) low Jev confidence, (b) every candidate
           marked blocked by the graph: it picks which stale wall to re-verify. Its
           one-line "why" is stored on the node as a falsifiable hypothesis.
  graph  - knowledge as routing: candidates the graph already knows are walls are
           filtered out before anyone is asked (cross-episode collision savings)
  prune  - physical verdicts contradicting the graph mutate it both ways: collision
           on a learned open edge -> remove it; an open through a learned wall ->
           remove the dead-end and unblock

Usage: python grasp_grow_maze.py [--size 8] [--seed 20260923] [--episodes 3]
                                 [--transfer-seed 0] [--min-p 0.4] [--random]
                                 [--server http://127.0.0.1:8765] [--fresh]
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys
import urllib.error
import urllib.request
from collections import deque
from pathlib import Path

LAB = Path(__file__).resolve().parent
sys.path.insert(0, str(LAB / "NanoJev" / "scripts"))
from unified_grid_envs import UnifiedMazeEnv, REVERSE  # noqa: E402
from build_grasp_data import trunc_utf8, STATE_MAX_BYTES  # noqa: E402
from build_grasp_data import trunc_utf8, STATE_MAX_BYTES  # byte-identical truncation mirror  # noqa: E402

FLYWHEEL = LAB / "grow_flywheel.jsonl"

GRASP_EXE = str(LAB.parent / "build" / "Release" / "grasp.exe")
SESSIONS_DIR = LAB / "grow_sessions"
SESSION_ID = "grow-map"

POLICY_QUESTION = ("Choose the next action that maximizes the probability of completing "
                   "the stated task successfully before its deadline. Use the visible state, "
                   "action descriptions, remaining time, and recorded history.")

PAVE_SYSTEM = ("你是迷宫图谱的修路仲裁者。图谱记录已验证的物理连通性,候选方向是环境给出的"
               "未试过(或图谱认为是旧墙)的挖点。你的职责是在直觉模型没把握时拍板挖哪个,"
               "优先挖可能连通新区域、离goal更近的方向;若判断某条旧墙是误记,也可重挖验证。"
               '只输出JSON: {"pick": 编号, "why": "不超过40字的理由"}')


def cell_id(pos):
    return f"c_{pos[0]}_{pos[1]}"


def grasp(*args, stdin_text=None):
    env = dict(os.environ, GRASP_CPP_SESSIONS=str(SESSIONS_DIR))
    result = subprocess.run([GRASP_EXE, *args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", env=env, input=stdin_text)
    if result.returncode != 0:
        raise RuntimeError(f"grasp {' '.join(args[:2])} failed: {result.stderr.strip()}")
    return result.stdout


def ask_jev(server, state, candidates):
    """Returns (choice, top_prob, probs); choice is among the given candidates."""
    action_question = {"type": "choice", "instructions": POLICY_QUESTION,
                       "criteria": candidates}
    payload = {"states": [{"id": "maze", "state": state,
                           "questions": {"action": action_question}}]}
    try:
        response = json.loads(urllib.request.urlopen(
            urllib.request.Request(f"{server}/api/evaluate",
                                   data=json.dumps(payload).encode(),
                                   headers={"Content-Type": "application/json"}),
            timeout=120).read())
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", "replace")
        raise RuntimeError(f"jev 400 ({len(candidates)} candidates):\n{body}") from None
    answer = response["states"][0]["answers"]["action"]
    probs = answer["probabilities"]
    return answer["choice"], max(probs.values()), probs


def ask_llm_pave(base, key, model, state_text, options):
    """System Two picks one of the offered dig options; returns (dir, why)."""
    listing = "\n".join(f"{i + 1}. {d}: {t}" for i, (d, t) in enumerate(options.items()))
    prompt = f"{state_text}\n\n当前分叉的候选挖点:\n{listing}\n\n选最值得挖的一个。"
    payload = {"model": model, "temperature": 0, "max_tokens": 96,
               "messages": [{"role": "system", "content": PAVE_SYSTEM},
                            {"role": "user", "content": prompt}]}
    request = urllib.request.Request(
        base.rstrip("/") + "/chat/completions", data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    text = json.loads(urllib.request.urlopen(request, timeout=60).read()
                      )["choices"][0]["message"]["content"]
    match = re.search(r'"pick"\s*:\s*(\d+)', text)
    if not match:
        raise ValueError(f"paver reply unparseable: {text[:120]!r}")
    index = int(match.group(1)) - 1
    keys = list(options)
    if not 0 <= index < len(keys):
        raise ValueError(f"paver pick {index + 1} out of range 1..{len(keys)}")
    why_match = re.search(r'"why"\s*:\s*"([^"]{0,120})"', text)
    return keys[index], (why_match.group(1) if why_match else "")


class GrowGraph:
    """Authoritative cross-episode map = grasp session; RAM mirror for routing."""

    def __init__(self):
        self.open = {}      # (pos, dir) -> dest   verified traversable
        self.blocked = {}   # (pos, dir) -> reason  verified wall/boundary
        self.nodes = set()
        self.edges = set()  # (src, dst, label)
        self.added_nodes = 0
        self.added_edges = 0
        self.pruned = 0     # graph facts the env contradicted, both directions
        self.exists = (SESSIONS_DIR / f"{SESSION_ID}.json").exists()
        if self.exists:
            self._load()
        else:
            SESSIONS_DIR.mkdir(exist_ok=True)

    def _load(self):
        s = json.loads((SESSIONS_DIR / f"{SESSION_ID}.json").read_text(encoding="utf-8"))
        for node in s["graph"]["nodes"]:
            self.nodes.add(node["id"])
        for e in s["graph"]["edges"]:
            self.edges.add((e["from"], e["to"], e.get("label", "")))
            m_from = re.fullmatch(r"c_(\d+)_(\d+)", e["from"])
            if not m_from:
                continue
            src = (int(m_from.group(1)), int(m_from.group(2)))
            dead = re.fullmatch(r"w_(\d+)_(\d+)_(\w+)", e["to"])
            m_to = re.fullmatch(r"c_(\d+)_(\d+)", e["to"])
            if e.get("fallback") and dead:
                self.blocked[(src, e["label"])] = "wall"
            elif m_to:
                self.open[(src, e["label"])] = (int(m_to.group(1)), int(m_to.group(2)))

    def _ensure_node(self, node_id, desc, kind, cmd="", hypothesis=""):
        if node_id in self.nodes:
            return
        body = {"id": node_id, "desc": desc, "kind": kind}
        if cmd:
            body["cmd"] = cmd
        if hypothesis:
            body["hypothesis"] = hypothesis
        if not self.exists:
            graph = {"id": SESSION_ID, "version": 1, "entry": node_id,
                     "nodes": [body], "edges": []}
            tmp = LAB / "grow-map.graph.json"
            tmp.write_text(json.dumps(graph, ensure_ascii=False), encoding="utf-8")
            grasp("new", str(tmp), "--id", SESSION_ID)
            tmp.unlink()
            self.exists = True
        else:
            # --stdin keeps Chinese hypothesis bytes out of Windows argv (ANSI codepage)
            grasp("insert", SESSION_ID, "--stdin",
                  stdin_text=json.dumps(body, ensure_ascii=False))
        self.nodes.add(node_id)
        self.added_nodes += 1

    def _add_edge(self, src, dst, label, fallback):
        key = (src, dst, label)
        if key in self.edges or src == dst:
            return
        args = ["add-edge", SESSION_ID, src, dst, "--label", label]
        if fallback:
            args.append("--fallback")
        grasp(*args)
        self.edges.add(key)
        self.added_edges += 1

    def _remove_edge(self, src, dst):
        grasp("remove-edge", SESSION_ID, src, dst)
        self.edges = {e for e in self.edges if not (e[0] == src and e[1] == dst)}

    # ---- verdict / prune: ONLY physical events mutate knowledge ----

    def record(self, events, attribution, hypothesis=""):
        for ev in events:
            pos, action = tuple(ev["position"]), ev["action"]
            if ev["collision"]:
                reason = ev.get("collision_reason") or "wall"
                if (pos, action) in self.open:  # learned edge was a lie -> prune both ways
                    self.pruned += 1
                    dest = self.open.pop((pos, action))
                    self.open.pop((dest, REVERSE[action]), None)
                    self._remove_edge(cell_id(pos), cell_id(dest))
                    self._remove_edge(cell_id(dest), cell_id(pos))
                    self.edges.discard((cell_id(pos), cell_id(dest), action))
                    self.edges.discard((cell_id(dest), cell_id(pos), REVERSE[action]))
                if (pos, action) not in self.blocked:
                    self.blocked[(pos, action)] = reason
                    dead = f"w_{pos[0]}_{pos[1]}_{action}"
                    self._ensure_node(
                        dead, f"dead end: {action} from ({pos[0]},{pos[1]}) hits a {reason}"
                        f" [{attribution}]", "conclude", hypothesis=hypothesis)
                    self._ensure_node(cell_id(pos), f"cell ({pos[0]},{pos[1]})", "exec",
                                      f"echo cell_{pos[0]}_{pos[1]}")
                    self._add_edge(cell_id(pos), dead, action, True)
            else:
                dest = tuple(ev["next_position"])
                if (pos, action) in self.blocked:  # a misrecorded "wall" -> unblock
                    self.pruned += 1
                    self.blocked.pop((pos, action))
                    dead = f"w_{pos[0]}_{pos[1]}_{action}"
                    self._remove_edge(cell_id(pos), dead)
                    self.edges.discard((cell_id(pos), dead, action))
                if (pos, action) in self.open:
                    continue
                self.open[(pos, action)] = dest
                self.open[(dest, REVERSE[action])] = pos
                self.blocked.pop((dest, REVERSE[action]), None)
                back = f"w_{dest[0]}_{dest[1]}_{REVERSE[action]}"
                for cell in (pos, dest):
                    self._ensure_node(cell_id(cell), f"cell ({cell[0]},{cell[1]})", "exec",
                                      f"echo cell_{cell[0]}_{cell[1]}",
                                      hypothesis=hypothesis if cell == dest else "")
                self._add_edge(cell_id(pos), cell_id(dest), action, False)
                self._add_edge(cell_id(dest), cell_id(pos), REVERSE[action], False)
                if (cell_id(dest), back, REVERSE[action]) in self.edges:
                    self.pruned += 1
                    self._remove_edge(cell_id(dest), back)
                    self.edges.discard((cell_id(dest), back, REVERSE[action]))

    def dist_to_goal(self, goal):
        dist = {goal: 0}
        queue = deque([goal])
        while queue:
            node = queue.popleft()
            for (pos, _action), dest in self.open.items():
                if dest == node and pos not in dist:
                    dist[pos] = dist[node] + 1
                    queue.append(pos)
        return dist

    def reachable_cells(self, cur):
        adj = {}
        for (pos, _a), dest in self.open.items():
            adj.setdefault(pos, []).append(dest)
        seen, queue = {cur}, deque([cur])
        while queue:
            node = queue.popleft()
            for nxt in adj.get(node, []):
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append(nxt)
        return seen

    def size(self):
        return len(self.open), len(self.blocked)


def pick_direction(obs, cur, graph, args, stats, log):
    """Returns (action, lane, hypothesis, criteria). Graph filters; Jev orders; LLM arbitrates."""
    cands = obs["candidates"]
    if not cands:
        raise RuntimeError(f"env returned control with zero candidates at {cur} — "
                           "auto-reposition should have fired; env contract violated")
    fresh = {d: t for d, t in cands.items() if (cur, d) not in graph.blocked}
    if args.random:
        pool = fresh if fresh else cands
        return random.choice(list(pool)), "random", "", pool
    if not fresh:
        # every offer is a graph-recorded wall: System Two arbitrates a re-dig
        action, why = ask_llm_pave(args.base, args.key, args.model, obs["state"], cands)
        stats["llm_calls"] += 1
        log(f"[llm redig] ({cur[0]},{cur[1]}) all {len(cands)} known-walls, re-dig "
            f"{action}: {why}")
        return action, "llm", why, cands
    if len(fresh) == 1:
        stats["forced"] += 1
        return next(iter(fresh)), "forced", "", fresh
    choice, top, probs = ask_jev(args.server, obs["state"], fresh)
    stats["jev_calls"] += 1
    if top < args.min_p:
        action, why = ask_llm_pave(args.base, args.key, args.model, obs["state"], fresh)
        stats["llm_calls"] += 1
        stats["escalations"] += 1
        log(f"[llm escal] jev top {choice}={top:.2f} < {args.min_p}; paver chose "
            f"{action}: {why}")
        return action, "llm", why, fresh
    log(f"[jev] {choice}={top:.2f} of "
        f"{[(d, round(p, 2)) for d, p in sorted(probs.items(), key=lambda x: -x[1])]}")
    return choice, "jev", "", fresh


def run_episode(env, seed, graph, args, log=print, ep_no=0):
    obs, info = env.reset(seed)
    stats = dict(steps=0, collisions=0, jev_calls=0, llm_calls=0, escalations=0,
                 forced=0, graph_skips=0, reposition=0, success=False, outcome="",
                 flywheel_rows=0)
    n0, e0, p0 = graph.added_nodes, graph.added_edges, graph.pruned
    decisions = []
    guard = 0
    while not info["terminated"]:
        guard += 1
        if guard > 2000:
            raise RuntimeError("decision-loop guard tripped (env never terminated)")
        cur = env._position
        stats["graph_skips"] += sum(1 for d in obs["candidates"] if (cur, d) in graph.blocked)
        action, lane, why, criteria = pick_direction(obs, cur, graph, args, stats, log)
        decisions.append((cur, obs["state"], dict(criteria), action, lane))
        obs, reward, terminated, truncated, info = env.step(action)
        model_events, macro_events = [], []
        for ev in info["physical_events"]:
            (macro_events if ev.get("actor") == "verified_edge_reposition" else model_events
             ).append(ev)
        graph.record(model_events, lane, why)
        graph.record(macro_events, "map")
        stats["steps"] += len(model_events)
        stats["collisions"] += sum(1 for ev in model_events if ev["collision"])
    stats["success"] = bool(info["success"])
    stats["outcome"] = info["outcome"]
    stats["new_nodes"] = graph.added_nodes - n0
    stats["new_edges"] = graph.added_edges - e0
    stats["prunes"] = graph.pruned - p0
    stats["reposition"] = info["episode_metrics"]["reposition_steps"]
    if args.flywheel:
        stats["flywheel_rows"] = emit_flywheel(graph, env._goal, seed, ep_no, decisions, args)
    return stats


_SEEN_ROWS = set()


def emit_flywheel(graph, goal, seed, ep_no, decisions, args):
    """Hindsight supervision: gold = the unique shortest-path next hop on the
    END-of-episode verified map (perfect maze = spanning tree, so at most one).
    A row is only emitted when the gold hop was among the offered criteria."""
    dist = graph.dist_to_goal(goal)
    rows, written = [], 0
    for cur, state, criteria, chosen, lane in decisions:
        if cur not in dist or dist[cur] == 0 or len(criteria) < 2:
            continue
        gold = next((d for d in criteria
                     if (cur, d) in graph.open and dist.get(graph.open[(cur, d)]) == dist[cur] - 1),
                    None)
        if gold is None:
            continue
        state_id = f"maze-{seed}:c_{cur[0]}_{cur[1]}"
        key = (state_id, tuple(sorted(criteria)), gold)
        if key in _SEEN_ROWS:
            continue
        _SEEN_ROWS.add(key)
        probs = {k: (1.0 if k == gold else 0.0) for k in criteria}
        rows.append({
            "id": f"maze-{seed}.{cell_id(cur)}->{gold}",
            "state_id": state_id,
            "family_id": f"maze-{seed}",
            "split": args.split,
            "state": trunc_utf8(state, STATE_MAX_BYTES),
            "questions": {"next-edge": {"type": "choice", "instructions": POLICY_QUESTION,
                                        "criteria": criteria}},
            "gold": {"next-edge": gold},
            "teacher": {"native_probs": {"next-edge": probs}},
            "label_source": "hindsight",
            "lane": lane,
        })
    with FLYWHEEL.open("a", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
            written += 1
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, default=8)
    ap.add_argument("--seed", type=int, default=20260923)
    ap.add_argument("--episodes", type=int, default=3)
    ap.add_argument("--transfer-seed", type=int, default=0)
    ap.add_argument("--min-p", type=float, default=0.4)
    ap.add_argument("--random", action="store_true", help="control arm: no models, blind dig")
    ap.add_argument("--server", default="http://127.0.0.1:8765")
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--flywheel", action="store_true",
                    help="append hindsight-labeled choice rows to grow_flywheel.jsonl "
                         "(run one session per maze family: the labels assume a pure-family map)")
    ap.add_argument("--split", default="train")
    args = ap.parse_args()

    args.base = os.environ.get("OPENAI_BASE_URL",
                               "https://dashscope.aliyuncs.com/compatible-mode/v1")
    args.key = os.environ.get("DASHSCOPE_API_KEY", "")
    args.model = os.environ.get("OPENAI_MODEL", "qwen-turbo")
    if not args.random and not args.key:
        raise SystemExit("DASHSCOPE_API_KEY missing (paver lane needs it; or run --random)")

    if args.fresh and SESSIONS_DIR.exists():
        import shutil
        shutil.rmtree(SESSIONS_DIR)

    env = UnifiedMazeEnv({"task": "maze", "size": args.size})
    graph = GrowGraph()
    o, b = graph.size()
    print(f"map loaded: {o} open / {b} blocked edges | min_p={args.min_p}"
          f"{' | RANDOM control' if args.random else ''}")
    header = (f"{'ep':<4}{'ok':<6}{'steps':<7}{'coll':<6}{'jev':<5}{'llm':<5}{'esc':<5}"
              f"{'skip':<6}{'newN':<6}{'newE':<6}{'prune':<7}{'repostep'}")

    def row(tag, st):
        print(f"{tag:<4}{str(st['success']):<6}{st['steps']:<7}{st['collisions']:<6}"
              f"{st['jev_calls']:<5}{st['llm_calls']:<5}{st['escalations']:<5}"
              f"{st['graph_skips']:<6}{st['new_nodes']:<6}{st['new_edges']:<6}"
              f"{st['prunes']:<7}{st['reposition']}")

    print(f"== same-maze episodes (seed {args.seed}) ==")
    print(header)
    all_rows = {}
    for i in range(1, args.episodes + 1):
        st = run_episode(env, args.seed, graph, args, ep_no=i)
        row(str(i), st)
        all_rows[f"ep{i}"] = st
    if args.transfer_seed:
        print(f"== transfer episode (fresh maze seed {args.transfer_seed}, grown map) ==")
        print(header)
        st = run_episode(env, args.transfer_seed, graph, args)
        row("tr", st)
        all_rows["transfer"] = st
    out = LAB / ("grow_results_random.json" if args.random else "grow_results.json")
    out.write_text(json.dumps(all_rows, ensure_ascii=False, indent=2), encoding="utf-8")
    o, b = graph.size()
    print(f"map now: {o} open / {b} blocked | flywheel rows this run: "
          f"{sum(r['flywheel_rows'] for r in all_rows.values())} -> {FLYWHEEL.name}")
    print(f"session '{SESSION_ID}' under {SESSIONS_DIR}")
    print(f"results -> {out}")


if __name__ == "__main__":
    main()
