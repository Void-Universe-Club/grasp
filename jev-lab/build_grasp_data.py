#!/usr/bin/env python3
"""Build NanoJev choice-finetuning JSONL from grasp session histories.

Supervision tiers (label_source):
  trajectory   - consecutive history nodes (A -> B) that follow a real edge: the strongest signal
  version-edge - edge present only in a newer snapshot (.vN -> vM/vfinal): a lesson the graph learned
  heuristic    - never-walked forks, labeled with grasp's own auto-walk rule (non-fallback first)

The instruction and option-text rules MUST stay byte-identical to src/jev.cpp / jev.h.
"""
import argparse
import collections
import json
from pathlib import Path

INSTRUCTION = ("You are the grasp agent standing at the graph node described in the state above. "
               "To advance the current task most efficiently, which edge should you follow next?")
STATE_MAX_BYTES = 480
DESC_MAX_BYTES = 80


def trunc_utf8(s: str, n: int) -> str:
    # byte-for-byte mirror of os::trunc_utf8 in src/os.cpp
    raw = s.encode("utf-8")
    if len(raw) <= n:
        return s
    cut = n
    while cut > 0 and (raw[cut] & 0xC0) == 0x80:
        cut -= 1
    return raw[:cut].decode("utf-8", errors="ignore") + "..."


def option_text(label: str, target_id: str, target_desc: str) -> str:
    # mirrors src/jev.cpp jev_option_text (updated 2026-09-23 after the duel experiment:
    # options must carry the target node summary or System One walks into known traps)
    desc = trunc_utf8(target_desc, DESC_MAX_BYTES) if target_desc else ""
    if not label:
        return desc or target_id
    return f"{label}。目标节点: {desc}" if desc else label


def load_snapshot(path):
    session = json.loads(path.read_text(encoding="utf-8"))
    graph = session["graph"]
    nodes = {n["id"]: n for n in graph["nodes"]}
    adj = collections.defaultdict(list)
    for e in graph["edges"]:
        adj[e["from"]].append(e)
    return session, nodes, adj


def trajectory_pairs(session):
    hist = [h["node"] for h in session["history"]]
    return [(a, b) for a, b in zip(hist, hist[1:]) if a != b]


def collect(sessions_dir: Path):
    files = sorted(p for p in sessions_dir.glob("*.json"))
    versions = collections.defaultdict(list)
    for p in sorted(sessions_dir.glob("*.json.v*")):
        versions[p.name.split(".json")[0] + ".json"].append(p)

    samples = []
    for path in files:
        session, nodes, adj = load_snapshot(path)
        sid = path.stem
        reachable = {a: [e for e in adj[a] if len(adj[a]) >= 2] for a in adj}
        pairs = trajectory_pairs(session)
        pair_set = set(pairs)
        older_edges = set()
        for vp in versions.get(path.name, []):
            _, _, older_adj = load_snapshot(vp)
            older_edges |= {(a, e["to"]) for a in older_adj for e in older_adj[a]}
        for a, outs in reachable.items():
            if not outs:
                continue
            desc = nodes[a].get("desc", "") or a
            options = {e["to"]: option_text(e.get("label", ""), e["to"],
                                            nodes.get(e["to"], {}).get("desc", ""))
                       for e in outs}
            gold = None
            source = None
            walked = [b for (x, b) in pairs if x == a]
            if walked:
                gold, source = walked[0], "trajectory"
            else:
                fresh = [e["to"] for e in outs if (a, e["to"]) not in older_edges
                         and (a, e["to"]) not in pair_set]
                if fresh:
                    gold, source = fresh[0], "version-edge"
                else:
                    non_fb = [e["to"] for e in outs if not e.get("fallback")]
                    gold, source = (non_fb or [outs[0]["to"]])[0], "heuristic"
            samples.append(dict(session=sid, node=a, gold=gold, source=source,
                                state=trunc_utf8(desc, STATE_MAX_BYTES), options=options,
                                order=pairs.index((a, gold)) if (a, gold) in pair_set else -1))
        # one sample per distinct walked fork edge (repeat forks yield extra trajectory rows)
        seen = {(s["node"], s["gold"]) for s in samples if s["session"] == sid}
        for (a, b) in pairs:
            if (a, b) in seen or a not in reachable or not reachable[a] or b not in [e["to"] for e in adj[a]]:
                continue
            outs = reachable[a]
            desc = nodes[a].get("desc", "") or a
            options = {e["to"]: option_text(e.get("label", ""), e["to"],
                                            nodes.get(e["to"], {}).get("desc", ""))
                       for e in outs}
            samples.append(dict(session=sid, node=a, gold=b, source="trajectory",
                                state=trunc_utf8(desc, STATE_MAX_BYTES), options=options,
                                order=pairs.index((a, b))))
    return samples


def assign_split(sample, max_length, tokenizer):
    if tokenizer is not None:
        prefix = tokenizer.encode(f"State:\n{sample['state']}\n", add_special_tokens=False) \
            + tokenizer.encode(f"Question type: choice\nQuestion:\n{INSTRUCTION}\n", add_special_tokens=False)
        for key, text in sample["options"].items():
            leaf = prefix + tokenizer.encode(f"Candidate:\n{key}: {text}\nDecision:", add_special_tokens=False)
            if len(leaf) + 1 > max_length:
                return "dropped"
    if sample["source"] == "heuristic":
        return "train"
    if sample["session"] == "qf_ops":
        return "ood"
    if sample["session"] == "qps" and sample["order"] >= 4:
        return "test"
    if sample["order"] == 4:
        return "calibration"
    if sample["order"] == 3:
        return "dev"
    return "train"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sessions-dir", default="../sessions")
    ap.add_argument("--out", default="grasp_choice.jsonl")
    ap.add_argument("--max-length", type=int, default=1024)
    ap.add_argument("--tokenizer", default="NanoJev/checkpoints/NanoJev-unified/tokenizer")
    ap.add_argument("--no-token-check", action="store_true")
    args = ap.parse_args()

    tokenizer = None
    if not args.no_token_check:
        from transformers import AutoTokenizer
        tokenizer = AutoTokenizer.from_pretrained(args.tokenizer, local_files_only=True)

    samples = collect(Path(args.sessions_dir))
    counts = collections.Counter()
    rows = []
    for s in samples:
        split = assign_split(s, args.max_length, tokenizer)
        counts[(split, s["source"])] += 1
        if split == "dropped":
            continue
        probs = {k: (1.0 if k == s["gold"] else 0.0) for k in s["options"]}
        rows.append({
            "id": f"{s['session']}.{s['node']}->{s['gold']}",
            "state_id": f"{s['session']}:{s['node']}",
            "family_id": f"{s['session']}:{s['node']}",
            "split": split,
            "state": s["state"],
            "questions": {"next-edge": {"type": "choice", "instructions": INSTRUCTION,
                                        "criteria": s["options"]}},
            "gold": {"next-edge": s["gold"]},
            "teacher": {"native_probs": {"next-edge": probs}},
            "label_source": s["source"],
        })
    by_split = collections.Counter(r["split"] for r in rows)
    Path(args.out).write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in rows) + "\n",
                              encoding="utf-8")
    print("wrote", args.out, dict(by_split))
    print("by (split,source):", {f"{k[0]}/{k[1]}": v for k, v in sorted(counts.items())})


if __name__ == "__main__":
    main()
