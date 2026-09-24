#!/usr/bin/env python3
"""A/B demo on NanoJev's own turf: Jev zero-shot vs Jev + grasp persistent map memory.

Arm A (jev-only): every episode starts from scratch. Jev is asked at every fork
      (fresh env per episode; the env's own memory resets, so does the agent's).
Arm B (jev+grasp): every physical outcome is written into a grasp session
      (cells=nodes, open moves=edges, collisions=fallback edges). Next episode
      walks the remembered shortest path and only asks Jev where memory is blank.

Usage: python grasp_play_maze.py [--size 8] [--seed 20260923] [--episodes 3]
                                 [--server http://127.0.0.1:8765] [--arm both|a|b]
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from collections import deque
from pathlib import Path

LAB = Path(__file__).resolve().parent
SCRIPTS = LAB / "NanoJev" / "scripts"
sys.path.insert(0, str(SCRIPTS))
from unified_grid_envs import UnifiedMazeEnv, REVERSE  # noqa: E402

GRASP_EXE = str(LAB.parent / "build" / "Release" / "grasp.exe")
SESSIONS_DIR = LAB / "maze_sessions"
SESSION_ID = "maze-map"

# verbatim from NanoJev unified_game_pipeline.py so the request is in-distribution
POLICY_QUESTION = ("Choose the next action that maximizes the probability of completing "
                   "the stated task successfully before its deadline. Use the visible state, "
                   "action descriptions, remaining time, and recorded history.")


def ask_jev(server, state, candidates):
    action_question = {"type": "choice", "instructions": POLICY_QUESTION,
                       "criteria": candidates}
    one_state = {"id": "maze", "state": state, "questions": {"action": action_question}}
    payload = {"states": [one_state]}
    try:
        response = json.loads(urllib.request.urlopen(
            urllib.request.Request(f"{server}/api/evaluate",
                                   data=json.dumps(payload).encode(),
                                   headers={"Content-Type": "application/json"}),
            timeout=120).read())
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", "replace")
        raise RuntimeError(f"jev 400 for state {len(state)} chars, "
                           f"{len(candidates)} candidates:\n{body}") from None
    return response["states"][0]["answers"]["action"]["choice"]


def grasp(*args):
    env = dict(os.environ, GRASP_CPP_SESSIONS=str(SESSIONS_DIR))
    result = subprocess.run([GRASP_EXE, *args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", env=env)
    if result.returncode != 0:
        raise RuntimeError(f"grasp {' '.join(args[:2])} failed: {result.stderr.strip()}")
    return result.stdout


class GraspMemory:
    """grasp session as the authoritative cross-episode map store."""

    def __init__(self):
        self.open = {}      # (pos, action) -> dest
        self.blocked = {}   # (pos, action) -> reason
        self.nodes = set()
        self.edges = set()
        self.session_ready = SESSIONS_DIR.exists() and (SESSIONS_DIR / f"{SESSION_ID}.json").exists()
        if self.session_ready:
            self._load()
        else:
            SESSIONS_DIR.mkdir(exist_ok=True)

    def _load(self):
        session = json.loads((SESSIONS_DIR / f"{SESSION_ID}.json").read_text(encoding="utf-8"))
        for node in session["graph"]["nodes"]:
            self.nodes.add(node["id"])
        for edge in session["graph"]["edges"]:
            self.edges.add((edge["from"], edge["to"], edge.get("label", "")))

    @staticmethod
    def _cell(pos):
        return f"c_{pos[0]}_{pos[1]}"

    def _ensure_node(self, node_id, desc, kind, cmd=""):
        if node_id in self.nodes:
            return
        if not self.session_ready:
            graph = {"id": SESSION_ID, "version": 1, "entry": node_id,
                     "nodes": [{"id": node_id, "desc": desc, "kind": kind, "cmd": cmd}], "edges": []}
            tmp = LAB / "maze-map.graph.json"
            tmp.write_text(json.dumps(graph, ensure_ascii=False), encoding="utf-8")
            grasp("new", str(tmp), "--id", SESSION_ID)
            tmp.unlink()
            self.session_ready = True
        else:
            grasp("insert", SESSION_ID, json.dumps({"id": node_id, "desc": desc,
                                                    "kind": kind, "cmd": cmd}))
        self.nodes.add(node_id)

    def _add_edge(self, src, dst, label, fallback):
        key = (src, dst, label)
        if key in self.edges or src == dst:
            return
        args = ["add-edge", SESSION_ID, src, dst, "--label", label]
        if fallback:
            args.append("--fallback")
        grasp(*args)
        self.edges.add(key)

    def learn(self, events):
        for event in events:
            pos, action = tuple(event["position"]), event["action"]
            if event["collision"]:
                if (pos, action) in self.blocked:
                    continue
                reason = "boundary" if event["collision_reason"] == "boundary" else "wall"
                self.blocked[(pos, action)] = reason
                dead = f"w_{pos[0]}_{pos[1]}_{action}"
                self._ensure_node(dead, f"dead end: {action} from ({pos[0]},{pos[1]}) hits a {reason}",
                                  "conclude")
                self._ensure_node(self._cell(pos), f"cell ({pos[0]},{pos[1]})", "exec",
                                  f"echo cell_{pos[0]}_{pos[1]}")
                self._add_edge(self._cell(pos), dead, action, True)
            else:
                dest = tuple(event["next_position"])
                if (pos, action) in self.open:
                    continue
                self.open[(pos, action)] = dest
                self.open[(dest, REVERSE[action])] = pos
                back = f"w_{dest[0]}_{dest[1]}_{REVERSE[action]}"
                for cell in (pos, dest):
                    self._ensure_node(self._cell(cell), f"cell ({cell[0]},{cell[1]})", "exec",
                                      f"echo cell_{cell[0]}_{cell[1]}")
                self._add_edge(self._cell(pos), self._cell(dest), action, False)
                self._add_edge(self._cell(dest), self._cell(pos), REVERSE[action], False)
                if back in self.nodes and (self._cell(dest), back, REVERSE[action]) in self.edges:
                    # memory was wrong about a block; drop the stale dead-end edge
                    grasp("remove-edge", SESSION_ID, self._cell(dest), back)
                    self.edges.discard((self._cell(dest), back, REVERSE[action]))

    def dist_to_goal(self, goal):
        dist = {goal: 0}
        queue = deque([goal])
        while queue:
            node = queue.popleft()
            for (pos, action), dest in self.open.items():
                if dest == node and pos not in dist:
                    dist[pos] = dist[node] + 1
                    queue.append(pos)
        return dist

    def known_size(self):
        return len(self.open) // 2, len(self.blocked)


def decide(candidates, obs, env, memory, server, use_memory):
    """Arm B: follow the remembered shortest path; None = memory blank, ask System One."""
    if not use_memory:
        return None
    cur, goal = env._position, env._goal
    dist = memory.dist_to_goal(goal)
    if cur not in dist or dist[cur] == 0:
        return None
    for action in candidates:
        dest = memory.open.get((cur, action))
        if dest is not None and dist.get(dest) == dist[cur] - 1:
            return action
    return None


def run_episode(server, env, seed, use_memory, memory):
    obs, info = env.reset(seed)
    jev_calls = forced_calls = 0
    while not info["terminated"]:
        candidates = obs["candidates"]
        if len(candidates) == 1:  # no fork: server requires >=2 options, nothing to decide
            action = next(iter(candidates))
            forced_calls += 1
        else:
            action = decide(candidates, obs, env, memory, server, use_memory)
            if action is None:
                action = ask_jev(server, obs["state"], candidates)
                jev_calls += 1
            else:
                forced_calls += 1
            if action not in candidates:
                raise RuntimeError(f"picked {action!r}, not among candidates {list(candidates)}")
        obs, reward, terminated, truncated, info = env.step(action)
        if use_memory:
            memory.learn(info["physical_events"])
    metrics = info["episode_metrics"]
    metrics["jev_calls"] = jev_calls
    metrics["forced_calls"] = forced_calls
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--size", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20260923)
    parser.add_argument("--episodes", type=int, default=3)
    parser.add_argument("--server", default="http://127.0.0.1:8765")
    parser.add_argument("--arm", choices=["both", "a", "b"], default="both")
    args = parser.parse_args()

    spec = {"task": "maze", "size": args.size}
    results = {}

    if args.arm in ("both", "a"):
        env = UnifiedMazeEnv(spec)
        rows = [run_episode(args.server, env, args.seed, False, None) for _ in range(args.episodes)]
        results["A_jev_only"] = rows
    if args.arm in ("both", "b"):
        memory = GraspMemory()
        env = UnifiedMazeEnv(spec)
        rows = [run_episode(args.server, env, args.seed, True, memory) for _ in range(args.episodes)]
        results["B_jev_grasp"] = rows
        opened, blocked = memory.known_size()
        print(f"grasp session '{SESSION_ID}': {opened} open edges + {blocked} collisions persisted "
              f"under {SESSIONS_DIR}")

    print(f"\nmaze size={args.size} seed={args.seed} episodes={args.episodes} server={args.server}")
    print(f"{'arm':<14}{'ep':<4}{'success':<9}{'steps':<7}{'collisions':<12}{'jev calls'}")
    for arm, rows in results.items():
        for index, m in enumerate(rows, 1):
            print(f"{arm:<14}{index:<4}{str(m['success']):<9}{m['physical_steps']:<7}"
                  f"{m['collisions']:<12}{m['jev_calls']}")
    out = LAB / "maze_ab_results.json"
    out.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"results written to {out}")


if __name__ == "__main__":
    main()
