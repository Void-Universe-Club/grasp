#!/usr/bin/env python3
"""Gardener v0: LLM rewrites node descs into numeric evidence, then A/B Jev hit-rate.

Loop (meta-graph evolution, inference-time only, no retraining):
  1. harvest real traversal stats from session histories (entered/bounced/visits)
  2. LLM rewrites each stats-carrying node desc so numbers survive in <=80 chars
  3. same fork exam (original option text vs gardener text) against the Jev servers
  4. verdict = does numericized text move hit-rate; proposals stay as an audit file

Usage: python grasp_gardener.py [--server NAME=URL ...]
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_grasp_data import option_text  # noqa: E402

LAB = Path(__file__).resolve().parent
SESSION_FILES = [LAB.parent / "sessions" / f"{n}.json" for n in
                 ("qf_ops", "qps", "qf_bt_eval", "qpf", "sn-mtf-uneven")] + \
                [LAB / "gtest2" / "sessions" / "demo3.json"]
EXAMS = [LAB / "grasp_choice.jsonl", LAB / "work_traps.jsonl"]
CACHE = LAB / "gardener_cache.json"
PROPOSALS = LAB / "gardener_proposals.jsonl"

GARDENER_SYSTEM = ("你是图谱园丁。给你一个节点的原描述和真实走法统计，"
                   "把描述重写成一行让小型决策模型一眼用到数值证据的文本。"
                   '只输出JSON: {"desc": "..."}。要求:不超过80字;'
                   "必须原样引用统计里的数字,禁止编造未提供的数字;保留原描述的关键语义。")


def harvest_stats():
    """Per-node walk facts from every real session: who enters, who bounces back."""
    stats = collections.defaultdict(lambda: {"taken_to": collections.Counter(),
                                             "bounced": collections.Counter(),
                                             "visits": 0})
    for path in SESSION_FILES:
        if not path.exists():
            print(f"  (no session: {path.name})")
            continue
        seq = [h["node"] for h in json.loads(path.read_text(encoding="utf-8"))
               .get("history", []) if h.get("kind") == "walk"]
        for i, node in enumerate(seq):
            stats[node]["visits"] += 1
            if i + 1 < len(seq) and seq[i + 1] != node:
                nxt = seq[i + 1]
                stats[node]["taken_to"][nxt] += 1
                stats[nxt]["bounced"][node] += 1  # walked straight back = dead-end signal
    return {k: {"taken_to": dict(v["taken_to"]), "bounced": dict(v["bounced"]),
                "visits": v["visits"]} for k, v in stats.items()}


def node_evidence(node, stats):
    entered = sum(s["taken_to"].get(node, 0) for s in stats.values())
    bounced = sum(s["bounced"].get(node, 0) for s in stats.values())
    visits = stats.get(node, {}).get("visits", 0)
    if entered == 0 and visits == 0:
        return None
    return {"entered": entered, "bounced_back": bounced, "visits": visits}


def llm_rewrite(desc, evidence, base, key, model, cache):
    sig = hashlib.sha1(f"{desc}|{json.dumps(evidence, sort_keys=True)}".encode()).hexdigest()
    if sig in cache:
        return cache[sig], sig
    prompt = (f"原描述: {desc}\n真实走法统计: {json.dumps(evidence, ensure_ascii=False)}\n"
              "字段含义: entered=历史上走入该节点的次数, bounced_back=走入后又立刻折返的次数, "
              "visits=总到访次数。折返多说明该路常是死胡同。重写这一行描述。")
    payload = {"model": model, "temperature": 0, "max_tokens": 120,
               "messages": [{"role": "system", "content": GARDENER_SYSTEM},
                            {"role": "user", "content": prompt}]}
    request = urllib.request.Request(
        base.rstrip("/") + "/chat/completions", data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    text = json.loads(urllib.request.urlopen(request, timeout=60).read()
                      )["choices"][0]["message"]["content"]
    m = re.search(r'"desc"\s*:\s*"([^"]+)"', text)
    if not m:
        raise ValueError(f"gardener reply unparseable: {text[:120]!r}")
    new = m.group(1)
    digits = set(re.findall(r"\d", new))
    allowed = set(f"{evidence['entered']}{evidence['bounced_back']}{evidence['visits']}")
    if not digits:
        raise ValueError(f"rewrite dropped the numbers: {new!r}")
    if digits - allowed:
        raise ValueError(f"gardener invented digits {sorted(digits - allowed)} in {new!r}")
    cache[sig] = new
    return new, sig


def ask_jev(url, row):
    q = row["questions"]["next-edge"]
    payload = {"states": [{"id": "e", "state": row["state"], "questions": {"next-edge": q}}]}
    r = json.loads(urllib.request.urlopen(urllib.request.Request(
        url + "/api/evaluate", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}), timeout=120).read())
    return r["states"][0]["answers"]["next-edge"]["choice"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", action="append", default=None, metavar="NAME=URL")
    args = ap.parse_args()
    servers = (dict(s.split("=", 1) for s in args.server) if args.server else
               {"v2fly": "http://127.0.0.1:8767", "game": "http://127.0.0.1:8765"})
    key = os.environ.get("DASHSCOPE_API_KEY", "")
    if not key:
        sys.exit("DASHSCOPE_API_KEY missing")
    base = os.environ.get("LLM_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
    model = os.environ.get("LLM_MODEL", "qwen-turbo")

    stats = harvest_stats()
    cache = json.loads(CACHE.read_text(encoding="utf-8")) if CACHE.exists() else {}
    rows = [json.loads(l) for f in EXAMS for l in open(f, encoding="utf-8") if l.strip()]
    rows = [r for r in rows if not r["family_id"].startswith("maze-")]

    proposals, exam = [], []
    for row in rows:
        q = row["questions"]["next-edge"]
        new_criteria = {}
        for target, text in q["criteria"].items():
            ev = node_evidence(target, stats)
            if ev is None:
                new_criteria[target] = text  # no real stats: gardener does not touch it
                continue
            desc = re.sub(r"^.*?目标节点[:：]\s*", "", text) or text
            try:
                new_desc, _ = llm_rewrite(desc, ev, base, key, model, cache)
            except Exception as error:
                print(f"  skip {target}: {str(error)[:120]}")
                new_criteria[target] = text
                continue
            cand = option_text(target, target, new_desc)
            if not 2 <= len(cand.encode("utf-8")) <= 255:
                new_criteria[target] = text
                continue
            new_criteria[target] = cand
            proposals.append({"target": target, "evidence": ev, "old": text, "new": cand})
        grown = any(new_criteria[t] != q["criteria"][t] for t in new_criteria)
        exam.append((row, dict(row, questions={"next-edge": dict(q, criteria=new_criteria)}), grown))

    CACHE.write_text(json.dumps(cache, ensure_ascii=False, indent=1), encoding="utf-8")
    PROPOSALS.write_text("\n".join(json.dumps(p, ensure_ascii=False) for p in proposals) + "\n",
                         encoding="utf-8")
    touched = [(o, n) for o, n, g in exam if g]
    print(f"exam rows: {len(exam)} | rows with gardener edits: {len(touched)} | "
          f"options rewritten: {len(proposals)}")

    t0 = time.perf_counter()
    results = {}  # name -> list[(family, gold, hit_original, hit_gardener)] per edited row
    for name, url in servers.items():
        per_row = []
        for row_old, row_new in touched:
            gold = row_old["gold"]["next-edge"]
            co = ask_jev(url, row_old)
            cn = ask_jev(url, row_new)
            per_row.append((row_old["family_id"], gold, co == gold, cn == gold))
        results[name] = per_row
    print(f"{len(servers) * 2 * len(touched)} Jev calls in {time.perf_counter() - t0:.0f}s\n")

    print(f"{'server':8}{'orig':>8}{'gardener':>10}   fixed / broken")
    for name, per_row in results.items():
        oh = sum(1 for r in per_row if r[2])
        nh = sum(1 for r in per_row if r[3])
        fixed = sorted({f for f, g, o, n in per_row if not o and n})
        broken = sorted({f for f, g, o, n in per_row if o and not n})
        print(f"{name:8}{oh:>4}/{len(per_row)}{nh:>7}/{len(per_row)}   "
              f"+{100*(nh-oh)/max(1,len(per_row)):+.0f}pp fixed:{fixed} broken:{broken}")
    print(f"proposals -> {PROPOSALS}")


if __name__ == "__main__":
    main()
