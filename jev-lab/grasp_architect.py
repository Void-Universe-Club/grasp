#!/usr/bin/env python3
"""Meta-graph architect: one-sentence goal -> validated grasp graph, zero hand edits.

The point of the verdict experiment: a human supplies intent, never nodes.
LLM drafts the graph; a machine validator rejects anything structurally unsound
(bad ids, missing endpoints, orphan leaves, unreachable conclude, non-whitelisted
commands) and sends the refusal back to the architect for one correction round.

Usage: python grasp_architect.py [--goal "..."] [--session arch-check]
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

LAB = Path(__file__).resolve().parent
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")
GRASP_EXE = str(LAB.parent / "build" / "Release" / "grasp.exe")
SESSIONS_DIR = LAB / "qfops_sessions"

DEFAULT_GOAL = ("每早对grasp工作区做一次晨检:确认git工作区状态、构建产物grasp.exe存在、"
                "本地Jev服务8767存活、飞轮数据文件grow_flywheel.jsonl行数没缩水,"
                "最后给出晨检结论。")

SYSTEM = ("你是grasp元图架构师。把任务目标编译成一张能跑的元图JSON,只输出JSON不要解释。"
          '格式: {"entry":"节点id","nodes":[{"id":"snake_case_3到32字符","desc":"<=60字中文:这个节点在确认什么","kind":"exec或conclude","cmd":"单行Windows cmd命令"}],'
          '"edges":[{"from":"id1","to":"id2","label":"什么情况下走这条边(人话)"}]}。'
          "规则: 恰好一个kind=conclude的终点节点;除终点外每个节点至少一条出边且不能是死胡同;"
          "cmd只能是只读检查(echo/git status/curl -sf/where/dir/python -c打印),禁止写文件禁止删除禁止网络POST;"
          "检查失败也要有路可走(用标签区分 正常/异常 分支)。工作区根目录 D:/data/gitee/grasp。")

CMD_OK = re.compile(r"^(echo |git |curl |where |dir |python |if exist |cd |type |netstat|findstr |find )")


def cmd_is_readonly(cmd):
    """Every segment of a && / | compound must start with a whitelisted read-only verb."""
    for seg in re.split(r"&&|\|", cmd):
        s = seg.strip()
        if s and not CMD_OK.match(s):
            return False
    return True


def ask_architect(goal, feedback, base, key, model, temperature=0.0):
    prompt = f"任务目标: {goal}"
    if feedback:
        prompt += f"\n\n你上一版图被校验拒绝: {feedback}\n修正后重新输出完整JSON。"
    payload = {"model": model, "temperature": temperature, "max_tokens": 1600,
               "messages": [{"role": "system", "content": SYSTEM},
                            {"role": "user", "content": prompt}]}
    req = urllib.request.Request(base.rstrip("/") + "/chat/completions",
                                 data=json.dumps(payload).encode(),
                                 headers={"Authorization": f"Bearer {key}",
                                          "Content-Type": "application/json"})
    text = json.loads(urllib.request.urlopen(req, timeout=120).read()
                      )["choices"][0]["message"]["content"]
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        raise ValueError(f"architect reply has no JSON: {text[:100]!r}")
    return json.loads(m.group(0))


def validate(graph):
    """Structural gate — returns list of refusals (empty = pass)."""
    errs = []
    nodes = graph.get("nodes") or []
    edges = graph.get("edges") or []
    entry = graph.get("entry")
    ids = [n.get("id", "") for n in nodes]
    if not nodes:
        return ["nodes为空"]
    for nid in ids:
        if not re.fullmatch(r"[a-z0-9_]{3,32}", nid):
            errs.append(f"node_id非法: {nid!r}")
    dupes = [k for k, c in collections.Counter(ids).items() if c > 1]
    if dupes:
        errs.append(f"node_id重复: {dupes}")
    idset = set(ids)
    if entry not in idset:
        errs.append(f"entry {entry!r} 不在节点里")
    concludes = [n["id"] for n in nodes if n.get("kind") == "conclude"]
    if len(concludes) != 1:
        errs.append(f"conclude节点必须恰好1个,实际{len(concludes)}")
    for e in edges:
        if e.get("from") not in idset or e.get("to") not in idset:
            errs.append(f"边端点不存在: {e.get('from')}->{e.get('to')}")
        if not str(e.get("label", "")).strip():
            errs.append(f"边缺label: {e.get('from')}->{e.get('to')}")
    adj = collections.defaultdict(list)
    for e in edges:
        adj[e["from"]].append(e["to"])
    for n in nodes:
        nid, kind = n.get("id"), n.get("kind", "exec")
        if kind != "conclude" and not adj.get(nid):
            errs.append(f"死胡同节点(非conclude却无出边): {nid}")
        cmd = str(n.get("cmd", ""))
        if kind != "conclude" and (not cmd.strip() or not cmd_is_readonly(cmd)):
            errs.append(f"cmd缺失或非只读白名单: {nid}: {cmd[:40]!r}")
    seen, stack = set(), [entry]
    while stack:
        x = stack.pop()
        if x in seen:
            continue
        seen.add(x)
        stack.extend(adj.get(x, []))
    for c in concludes:
        if c not in seen:
            errs.append(f"conclude不可达: {c}")
    for nid in idset - seen:
        errs.append(f"从entry不可达的孤儿节点: {nid}")
    return errs


def import_graph(graph, session):
    SESSIONS_DIR.mkdir(exist_ok=True)
    target = SESSIONS_DIR / f"{session}.json"
    if target.exists():
        target.unlink()
    payload = {"id": session, "version": 1, "entry": graph["entry"],
               "nodes": graph["nodes"], "edges": graph["edges"]}
    tmp = SESSIONS_DIR / "_arch.graph.json"
    tmp.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    env = dict(os.environ, GRASP_CPP_SESSIONS=str(SESSIONS_DIR))
    r = subprocess.run([GRASP_EXE, "new", str(tmp), "--id", session],
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace", env=env, timeout=60)
    tmp.unlink()
    if r.returncode != 0:
        raise RuntimeError(f"grasp new失败: {r.stderr.strip()[:200]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--goal", default=DEFAULT_GOAL)
    ap.add_argument("--session", default="arch-check")
    args = ap.parse_args()
    base = os.environ.get("LLM_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
    key = os.environ.get("DASHSCOPE_API_KEY", "")
    model = os.environ.get("LLM_MODEL", "qwen-turbo")
    if not key:
        sys.exit("DASHSCOPE_API_KEY missing")

    feedback = None
    for attempt in (1, 2):
        # temp 0.7 on retries: at 0 the model reproduces the refused draft verbatim
        graph = ask_architect(args.goal, feedback, base, key, model,
                              temperature=0.0 if attempt == 1 else 0.7)
        errs = validate(graph)
        if not errs:
            break
        feedback = "; ".join(errs[:8])
        print(f"architect v{attempt} 被拒: {feedback}")
    else:
        sys.exit(f"两轮成图均被拒,实验失败于脚手架阶段: {feedback}")

    import_graph(graph, args.session)
    print(f"图已生成并导入: session={args.session} "
          f"{len(graph['nodes'])}节点/{len(graph['edges'])}边 entry={graph['entry']}")
    for n in graph["nodes"]:
        print(f"  [{n.get('kind','exec')}] {n['id']}: {n['cmd'][:64]}")


if __name__ == "__main__":
    main()
