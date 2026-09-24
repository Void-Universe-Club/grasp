#!/usr/bin/env python3
"""Battle-test the two watchdog wake paths (loop / exec-fail) that live episodes never hit.

Drives handle_wake directly against the grown qf-grow session with a real LLM keeper,
then restores the session file so the episode corpus stays clean.
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


def park(node):
    """Production invariant: handle_wake's steps act on the REAL session node."""
    p = g.SESSIONS_DIR / "qf-grow.json"
    s = json.loads(p.read_text(encoding="utf-8"))
    s["node"] = node
    p.write_text(json.dumps(s, ensure_ascii=False), encoding="utf-8")


def run_case(reason, cur, exclude, jev_choice=None):
    assert cur == "qf_pos", "harness parks the session at qf_pos; keep cur in sync"
    park(cur)
    log = lambda *a: print(*a, flush=True)
    args = argparse.Namespace(min_p=0.5, max_steps=30, max_wakes=8, no_llm=False,
                              flywheel=False, fresh=False, episodes=2, server=g.JEV_DEFAULT)
    s = g.load_session()
    es = g.edges_from(s, cur)
    assert es, f"用例要求 {cur} 有可走的边"
    before = len(es)
    res = g.handle_wake(reason, s, cur, es, args.server, BASE, KEY, MODEL, args, log,
                        jev_choice=jev_choice, exclude=exclude)
    report = {"reason": reason, "cur": cur, "exclude": exclude, "return": res}
    if res is False:
        report["verdict"] = "FAIL: wake returned abort"
        return report
    if isinstance(res, dict):
        stepped, lane = res["stepped"], res["lane"]
        ok_edge = stepped in {e["to"] for e in es}
        ok_not_excluded = stepped != exclude
        moved = g.load_session()["node"] == stepped
        report.update(stepped=stepped, lane=lane, stepped_is_real_edge=ok_edge,
                      respected_exclude=ok_not_excluded, session_actually_moved=moved)
        if not (ok_edge and ok_not_excluded and moved):
            report["verdict"] = "FAIL: wake-step contract broken"
        else:
            report["verdict"] = "PASS: drive advanced"
    else:
        now = g.load_session()
        report["graph_delta"] = (f"{len(now['graph']['nodes'])}n/{len(now['graph']['edges'])}e"
                                 f" (was {len(s['graph']['nodes'])}n/{before}e@cur)")
        report["verdict"] = ("PASS: graph op applied, caller will re-fork"
                             if (len(now["graph"]["edges"]) != len(s["graph"]["edges"])
                                 or len(now["graph"]["nodes"]) != len(s["graph"]["nodes"]))
                             else "FAIL: returned True but graph unchanged AND no step")
    return report


def main():
    if not KEY:
        sys.exit("DASHSCOPE_API_KEY missing")
    backup = g.SESSIONS_DIR / "_pretest.qf-grow.json"
    shutil.copy(g.SESSIONS_DIR / "qf-grow.json", backup)
    try:
        reports = [
            # a-bounce a-bounce a -> loop_detected fires while parked at qf_pos_detail
            run_case("watchdog:loop: qf_pos<->qf_pos_detail 已往返3次", "qf_pos",
                     exclude="qf_pos_detail", jev_choice="qf_pos_detail"),
            # echo cmd timeout simulation on the branch Jev wanted
            run_case("exec-fail: 目标节点命令执行失败(模拟 echo 超时)", "qf_pos",
                     exclude="qf_pos_detail", jev_choice=None),
        ]
    finally:
        shutil.move(backup, g.SESSIONS_DIR / "qf-grow.json")
    print("\n== wake-path battle test ==")
    for r in reports:
        print(json.dumps(r, ensure_ascii=False, default=str))
    print("session restored from backup (journal keeps the evidence trail)")


if __name__ == "__main__":
    main()
