#!/usr/bin/env python3
"""Compare two NanoJev servers on held-out grasp forks (top-1 hit rate per split)."""
import argparse
import collections
import json
import urllib.request


def ask(server, row):
    q = row["questions"]["next-edge"]
    payload = {"states": [{"id": "eval", "state": row["state"],
                           "questions": {"next-edge": q}}]}
    resp = json.loads(urllib.request.urlopen(
        urllib.request.Request(f"{server}/api/evaluate",
                               data=json.dumps(payload).encode(),
                               headers={"Content-Type": "application/json"}),
        timeout=120).read())
    return resp["states"][0]["answers"]["next-edge"]["choice"]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default="grasp_choice.jsonl")
    ap.add_argument("--server", action="append", required=True,
                    metavar="NAME=URL", help="repeatable, e.g. game=http://127.0.0.1:8765")
    args = ap.parse_args()
    servers = dict(a.split("=", 1) for a in args.server)
    rows = [json.loads(l) for l in open(args.data, encoding="utf-8")]
    hits = collections.defaultdict(lambda: collections.Counter())
    for row in rows:
        gold = row["gold"]["next-edge"]
        for name, url in servers.items():
            choice = ask(url, row)
            hits[name][row["split"]] += int(choice == gold)
            hits[name][row["split"] + "_n"] += 1
    print(f"{'server':8} " + " ".join(f"{s:>14}" for s in ["train", "dev", "calibration", "test", "ood", "ALL"]))
    for name in servers:
        cells = []
        total = total_n = 0
        for split in ["train", "dev", "calibration", "test", "ood"]:
            n = hits[name][split + "_n"]
            total += hits[name][split]
            total_n += n
            cells.append(f"{hits[name][split]}/{n}" if n else "-")
        cells.append(f"{total}/{total_n} ({100.0 * total / max(1, total_n):.0f}%)")
        print(f"{name:8} " + " ".join(f"{c:>14}" for c in cells))


if __name__ == "__main__":
    main()
