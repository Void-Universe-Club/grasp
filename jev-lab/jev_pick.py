#!/usr/bin/env python3
"""Pick a grasp graph edge with a local NanoJev (System One) server; no provider calls.

Usage:
  python jev_pick.py --session qps [--node ps_ground] [--server http://127.0.0.1:8765]
  python jev_pick.py --session qps --node <id> --apply   # rewrite session "node" to the pick
"""
import argparse
import json
import sys
import urllib.request
from collections import defaultdict
from pathlib import Path

INSTRUCTION = (
    "你是 grasp 智能体，正站在上面这段经验描述所在的图谱节点。"
    "为了最高效地推进当前任务，下一步应沿哪条边走？"
)


def edge_label(edge):
    for key in ("label", "when", "desc"):
        value = edge.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return "走向 " + edge["to"]


def load_session(sessions_dir, session_id):
    path = Path(sessions_dir) / f"{session_id}.json"
    session = json.loads(path.read_text(encoding="utf-8"))
    graph = session["graph"]
    nodes = {node["id"]: node for node in graph["nodes"]}
    incoming = defaultdict(list)
    for edge in graph["edges"]:
        incoming[edge["from"]].append(edge)
    return session, nodes, incoming


def build_request(nodes, outgoing, node_id):
    node = nodes[node_id]
    criteria = {edge["to"]: edge_label(edge) for edge in outgoing[node_id]}
    return {"states": [{
        "id": node_id,
        "state": node.get("desc") or node_id,
        "questions": {"next-edge": {
            "type": "choice", "instructions": INSTRUCTION, "criteria": criteria}}}],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--session", required=True)
    parser.add_argument("--node", help="default: session current node, else graph entry")
    parser.add_argument("--sessions-dir", default=str(Path(__file__).resolve().parent.parent / "sessions"))
    parser.add_argument("--server", default="http://127.0.0.1:8765")
    parser.add_argument("--apply", action="store_true", help="move the session pointer to the chosen node")
    args = parser.parse_args()

    session, nodes, outgoing = load_session(args.sessions_dir, args.session)
    node_id = args.node or session.get("node") or session["graph"].get("entry")
    if node_id not in nodes:
        sys.exit(f"node {node_id!r} not in session {args.session}")
    if node_id not in outgoing or len(outgoing[node_id]) < 2:
        sys.exit(f"node {node_id!r} is not a fork (<2 outgoing edges)")

    request = build_request(nodes, outgoing, node_id)
    response = json.loads(urllib.request.urlopen(
        urllib.request.Request(f"{args.server}/api/evaluate",
                               data=json.dumps(request).encode(),
                               headers={"Content-Type": "application/json"}),
        timeout=120).read())
    answer = response["states"][0]["answers"]["next-edge"]
    execution = response["execution"]
    print(f"{args.session} @ {node_id}: {len(outgoing[node_id])} edges, "
          f"{execution['forward_passes']} forward pass, "
          f"{execution['server_evaluation_seconds']:.2f}s, decode steps 0")
    for target, probability in sorted(answer["probabilities"].items(), key=lambda x: -x[1]):
        label = next(edge_label(e) for e in outgoing[node_id] if e["to"] == target)
        print(f"  {probability:.3f}  {target:<28} {label}")
    print(f"=> {answer['choice']}")

    if args.apply:
        session["node"] = answer["choice"]
        path = Path(args.sessions_dir) / f"{args.session}.json"
        path.write_text(json.dumps(session, ensure_ascii=False), encoding="utf-8")
        print(f"session pointer moved to {answer['choice']}")


if __name__ == "__main__":
    main()
