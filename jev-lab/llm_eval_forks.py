#!/usr/bin/env python3
"""Pure-LLM baseline on the same fork exam (grasp_choice_v2.jsonl test split)."""
import argparse
import json
import os
import statistics
import sys
import time
import urllib.request

SYSTEM = ("你是图谱选边器。给定当前状态和若干候选边(每条边有ID和说明),"
          "选出最可能完成任务的那条边。只输出选项ID,不要任何解释。")


def ask_llm(base, key, model, row):
    q = row["questions"]["next-edge"]
    listing = "\n".join(f"{k}: {text}" for k, text in q["criteria"].items())
    prompt = f"状态:\n{row['state']}\n\n问题: {q['instructions']}\n\n候选边:\n{listing}"
    payload = {"model": model, "temperature": 0, "max_tokens": 16,
               "messages": [{"role": "system", "content": SYSTEM},
                            {"role": "user", "content": prompt}]}
    request = urllib.request.Request(
        base.rstrip("/") + "/chat/completions", data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    body = json.loads(urllib.request.urlopen(request, timeout=60).read())
    reply = body["choices"][0]["message"]["content"].strip()
    usage = body.get("usage", {})
    keys = list(q["criteria"])
    choice = reply if reply in keys else next((k for k in keys if k in reply), "UNPARSED")
    return choice, reply, usage.get("total_tokens", 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default="grasp_choice_v2.jsonl")
    ap.add_argument("--split", default="test")
    args = ap.parse_args()
    key = os.environ.get("DASHSCOPE_API_KEY", "")
    if not key:
        sys.exit("DASHSCOPE_API_KEY missing")
    base = os.environ.get("LLM_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
    model = os.environ.get("LLM_MODEL", "qwen-turbo")
    rows = [json.loads(l) for l in open(args.data, encoding="utf-8")
            if json.loads(l)["split"] == args.split]
    stats = {"maze": [0, 0], "work": [0, 0]}
    lat, tokens = [], 0
    for i, row in enumerate(rows, 1):
        t0 = time.perf_counter()
        choice, reply, tok = ask_llm(base, key, model, row)
        lat.append(time.perf_counter() - t0)
        tokens += tok
        gold = row["gold"]["next-edge"]
        bucket = "maze" if row["family_id"].startswith("maze-") else "work"
        stats[bucket][1] += 1
        stats[bucket][0] += int(choice == gold)
        if choice != gold:
            print(f"  miss[{bucket}] {row['family_id']} gold={gold} llm={choice!r} reply={reply[:30]!r}")
        if i % 20 == 0:
            print(f"[{i}/{len(rows)}] maze {stats['maze'][0]}/{stats['maze'][1]}"
                  f" | work {stats['work'][0]}/{stats['work'][1]}", flush=True)
    hit = sum(s[0] for s in stats.values())
    n = sum(s[1] for s in stats.values())
    print(f"\nLLM({model}) {args.split}: maze {stats['maze'][0]}/{stats['maze'][1]}"
          f" | work {stats['work'][0]}/{stats['work'][1]} | ALL {hit}/{n}"
          f" ({100.0 * hit / n:.0f}%)")
    print(f"latency p50={statistics.median(lat):.2f}s p90={sorted(lat)[int(len(lat)*0.9)]:.2f}s"
          f" | total tokens={tokens}")


if __name__ == "__main__":
    main()
