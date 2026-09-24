#!/usr/bin/env python3
"""Deterministic battle-test for the mid-run oracle (capability A).

Two single-path (no forks) sessions so run_episode needs neither Jev nor an LLM:
  oracle_ok  : every exec check passes           -> success=1, false_success=0
  oracle_bad : one exec check exits non-zero fast -> step commits without raising,
               the silent-output scan must flag it and the conclude gate must refuse
               to call it a real success.
This isolates the oracle from Jev routing and the 60s command-timeout path.
"""
import argparse
import json
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
import grasp_grow_qfops as g  # noqa: E402


def make_session(sid, cmds):
    nodes = [{"id": "chk_a", "desc": "check a", "kind": "exec", "timeout_secs": 15, "cmd": cmds[0]},
             {"id": "chk_b", "desc": "check b", "kind": "exec", "timeout_secs": 15, "cmd": cmds[1]},
             {"id": "conclude", "desc": "end", "kind": "conclude", "message": "done"}]
    graph = {"id": sid, "version": 1, "entry": "chk_a", "nodes": nodes,
             "edges": [{"from": "chk_a", "to": "chk_b", "label": "next"},
                       {"from": "chk_b", "to": "conclude", "label": "done"}]}
    path = g.SESSIONS_DIR / f"_{sid}.graph.json"
    path.write_text(json.dumps(graph, ensure_ascii=False), encoding="utf-8")
    try:
        g.grasp("delete", sid)
    except Exception:
        pass
    g.grasp("new", str(path), "--id", sid)
    path.unlink()


def run(sid, cmds):
    make_session(sid, cmds)
    g.SESSION_ID = sid
    args = argparse.Namespace(min_p=0.5, max_steps=10, max_wakes=4, no_llm=True,
                              flywheel=False, fresh=False, episodes=1, server=g.JEV_DEFAULT)
    log = lambda *a: print("   ", *a, flush=True)
    stats = g.run_episode("http://127.0.0.1:1", "", "", "", args, 1, log)
    return stats


def check(label, cond):
    print(f"[{'PASS' if cond else 'FAIL'}] {label}")
    return cond


def main():
    g.SESSIONS_DIR.mkdir(exist_ok=True)
    ok = True
    print("-- oracle_ok: both checks pass --")
    s_ok = run("oracle_ok", ["echo A_OK", "echo B_OK"])
    print("   stats:", dict(s_ok))
    ok &= check("success=1", s_ok["success"] == 1)
    ok &= check("false_success=0", s_ok["false_success"] == 0)

    print("-- oracle_bad: second check exits non-zero fast (no raise) --")
    s_bad = run("oracle_bad", ["echo A_OK", "type __no_such_file__.zzz"])
    print("   stats:", dict(s_bad))
    ok &= check("success=0 (gate refused false win)", s_bad["success"] == 0)
    ok &= check("false_success=1", s_bad["false_success"] == 1)
    ok &= check("reached_conclude=1", s_bad["reached_conclude"] == 1)
    ok &= check("exec_fails=1 (silent scan caught it)", s_bad["exec_fails"] == 1)

    print("\n== oracle battle test ==", "PASS" if ok else "FAIL")

    for sid in ("oracle_ok", "oracle_bad"):
        try:
            g.grasp("delete", sid)
        except Exception:
            pass


if __name__ == "__main__":
    main()
