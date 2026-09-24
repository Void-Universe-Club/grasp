#!/usr/bin/env python3
"""Battle-test the keeper's update_cmd self-repair op (capability B).

Two layers:
  1. deterministic op-layer checks (no LLM): apply_keeper_op("update_cmd") repairs the
     live broken `find /c ""` node in arch-check, and rejects the unsafe/invalid variants.
  2. one live exec-fail wake to watch the LLM keeper actually reach for update_cmd
     once the failing node + its command are surfaced in the context pack.
The arch-check session is backed up and restored so the episode corpus stays clean.
"""
import argparse
import json
import os
import shutil
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
import grasp_grow_qfops as g  # noqa: E402

BASE = os.environ.get("LLM_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
KEY = os.environ.get("DASHSCOPE_API_KEY", "")
MODEL = os.environ.get("LLM_MODEL", "qwen-turbo")
BROKEN = 'cd /d D:\\data\\gitee\\grasp && type grow_flywheel.jsonl | find /c ""'
FIXED = 'type grow_flywheel.jsonl | find /c /v ""'
BROKEN_NODE = "check_flywheel_file"


def node_cmd():
    s = g.load_session()
    return next(n.get("cmd", "") for n in s["graph"]["nodes"] if n["id"] == BROKEN_NODE)


def check(label, cond):
    print(f"  [{'PASS' if cond else 'FAIL'}] {label}")
    return cond


def op_layer():
    print("\n-- layer 1: deterministic apply_keeper_op('update_cmd') --")
    ok = True
    assert node_cmd() == BROKEN, f"target node cmd is not the broken original: {node_cmd()!r}"
    ok &= check("live node holds the broken find /c \"\"", True)
    # reject: non-existent node
    ok &= check("reject unknown node",
                g.apply_keeper_op({"action": "update_cmd", "node_id": "nope_zzz",
                                   "node_cmd": FIXED}).startswith("提案被拒"))
    # reject: non-exec node (conclude)
    ok &= check("reject non-exec target",
                g.apply_keeper_op({"action": "update_cmd", "node_id": "conclude_morning_check",
                                   "node_cmd": FIXED}).startswith("提案被拒"))
    # reject: empty cmd
    ok &= check("reject empty cmd",
                g.apply_keeper_op({"action": "update_cmd", "node_id": BROKEN_NODE,
                                   "node_cmd": "   "}).startswith("提案被拒"))
    # reject: no-op (same as current)
    ok &= check("reject identical cmd",
                g.apply_keeper_op({"action": "update_cmd", "node_id": BROKEN_NODE,
                                   "node_cmd": BROKEN}).startswith("提案被拒"))
    # reject: destructive (redirect out of the sandbox)
    ok &= check("reject write/destructive cmd",
                g.apply_keeper_op({"action": "update_cmd", "node_id": BROKEN_NODE,
                                   "node_cmd": "type x > y.txt"}).startswith("提案被拒"))
    assert node_cmd() == BROKEN, "a rejected proposal must NOT touch the graph"
    ok &= check("graph untouched after all rejections", True)
    # accept: the real repair
    did = g.apply_keeper_op({"action": "update_cmd", "node_id": BROKEN_NODE,
                             "node_cmd": FIXED})
    ok &= check(f"accept valid repair -> {did!r}", did.startswith("改好节点"))
    ok &= check("node cmd actually changed on disk", node_cmd() == FIXED)
    return ok


def live_wake():
    print("\n-- layer 2: live exec-fail wake (LLM keeper) --")
    log = lambda *a: print(*a, flush=True)
    args = argparse.Namespace(min_p=0.5, max_steps=30, max_wakes=8, no_llm=False,
                              flywheel=False, fresh=False, episodes=1, server=g.JEV_DEFAULT)
    s = g.load_session()
    s["node"] = "check_jev_service"
    for n in s["graph"]["nodes"]:
        if n["id"] == BROKEN_NODE:
            n["cmd"] = BROKEN  # re-break so the keeper sees the broken cmd fresh
    (g.SESSIONS_DIR / "arch-check.json").write_text(
        json.dumps(s, ensure_ascii=False), encoding="utf-8")
    s = g.load_session()
    cur = "check_jev_service"
    es = g.edges_from(s, cur)
    before_cmd = node_cmd()
    res = g.handle_wake(
        'exec-fail: grasp step: error: command timeout (60s): cd /d D:\\data\\gitee\\grasp && type grow',
        s, cur, es, args.server, BASE, KEY, MODEL, args, log, exclude=BROKEN_NODE)
    after_cmd = node_cmd()
    repaired = after_cmd != before_cmd and g.cmd_is_readonly(after_cmd)
    print(f"  handle_wake return: {res!r}")
    print(f"  broken cmd : {before_cmd!r}")
    print(f"  after wake : {after_cmd!r}")
    print("  keeper REPAIRED the node via update_cmd" if repaired
          else "  keeper did NOT repair (chose bypass / keep) — op layer already proved the capability")
    return res, repaired


def main():
    if not KEY:
        sys.exit("DASHSCOPE_API_KEY missing")
    g.SESSION_ID = "arch-check"
    src = g.SESSIONS_DIR / "arch-check.json"
    backup = g.SESSIONS_DIR / "_pretest.arch-check.json"
    shutil.copy(src, backup)
    try:
        op_ok = op_layer()
        if op_ok:
            live_wake()
    finally:
        shutil.move(backup, src)
    print("\n== update_cmd battle test ==")
    print("op-layer:", "PASS" if op_ok else "FAIL")
    print("arch-check restored from backup (journal keeps the evidence trail)")


if __name__ == "__main__":
    main()
