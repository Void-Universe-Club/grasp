#!/usr/bin/env python3
"""RSI v0 on the real work graph: Jev drives, watchdog wakes, waking me updates the map.

Architecture (per user design 2026-09-24):
  jev lane  - v2 flywheel server picks outgoing edges at forks (0.17s, no cost)
  watchdog  - loop-detection / stale-frontier / exec-failure / low-confidence crossing
              a threshold FORCE-wakes the LLM keeper
  wake      - context pack = journal tail (persisted "sleep memory") ++ fixed-layout
              meta-graph trace ++ current fork; LLM may only answer with verifiable
              graph ops (keep/add-node/add-edge/prune-edge), executed via grasp CLI;
              the wake writes a journal entry before returning to sleep
  flywheel  - every fork decision whose episode outcome is known becomes a hindsight
              row appended to grow_flywheel.jsonl (same lane schema as the maze)

Environment: session qf_ops copied fresh per run; all node cmds are echoes (safe).

Usage: python grasp_grow_qfops.py [--server http://127.0.0.1:8767] [--min-p 0.5]
                                  [--max-steps 24] [--episodes 2] [--fresh] [--no-llm]
"""
import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

LAB = Path(__file__).resolve().parent
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, str(LAB))
from build_grasp_data import INSTRUCTION, option_text, trunc_utf8, STATE_MAX_BYTES  # noqa: E402

GRASP_EXE = str(LAB.parent / "build" / "Release" / "grasp.exe")
SOURCE_SESSION = LAB.parent / "sessions" / "qf_ops.json"
SESSIONS_DIR = LAB / "qfops_sessions"
SESSION_ID = "qf-grow"
JOURNAL = LAB / "qfops_journal.jsonl"
TRACE = LAB / "qfops_trace.txt"
FLYWHEEL = LAB / "grow_flywheel.jsonl"
JEV_DEFAULT = "http://127.0.0.1:8767"

WAKE_SYSTEM = ("你是grasp元图的守图人(System Two),图坏了由你修,决策犹豫时由你拍板。你睡着时"
               "Jev(System One)在图上推理,watchdog把你叫醒处理它搞不定的局面。读上下文包"
               "(值班日志+定式轨迹+图名单+当前分叉),只做一个最小、可验证、基于证据的动作。"
               "死胡同的标准修法:add_edge把当前节点接到语义合理的下一跳;"
               "Jev不自信时你直接take_edge选定一条可走的边;"
               "修剪错误分支用prune_edge;节点命令跑坏(超时/报错)用update_cmd把该节点node_cmd改成一条正确的只读命令。禁止编造事实;"
               '只输出JSON: {"assessment":"<=40字形势判断",'
               '"action":"keep|take_edge|add_node|add_edge|prune_edge|update_cmd",'
               '"node_id":"","node_desc":"","node_cmd":"",  '
               '"edge_from":"","edge_to":"","edge_label":"",'
               '"note":"<=40字,写给下次唤醒的你"}')


def grasp(*args, stdin_text=None):
    env = dict(os.environ, GRASP_CPP_SESSIONS=str(SESSIONS_DIR))
    try:  # grasp's own 60s command-kill should fire before this outer guard
        result = subprocess.run([GRASP_EXE, *args], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", env=env,
                                input=stdin_text, timeout=90)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"grasp {args[0]} outer timeout: {str(error)[:120]}")
    if result.returncode != 0:
        raise RuntimeError(f"grasp {args[0]}: {result.stderr.strip()[:200]}")
    return result.stdout


def load_session():
    s = json.loads((SESSIONS_DIR / f"{SESSION_ID}.json").read_text(encoding="utf-8"))
    return s


def edges_from(s, node, include_fallback=False):
    return [e for e in s["graph"]["edges"]
            if e["from"] == node and (include_fallback or not e.get("fallback"))]


# Read-only sandbox for keeper-authored exec cmds (mirrors the architect gate):
# only inspection verbs, no redirection/deletion — a keeper must never be able to
# turn a repair into a destructive command.
_CMD_OK = re.compile(
    r"^(echo |git |curl |where |dir |type |find |findstr |netstat|stat |ls |cat "
    r"|python |if exist |cd |test )")
_CMD_BAD = re.compile(
    r">|\brm \b|del |erase |move |ren |mkdir|rmdir|format |git push|git commit")


def cmd_is_readonly(cmd):
    if _CMD_BAD.search(cmd):
        return False
    segs = [x.strip() for x in re.split(r"&&|\|", cmd) if x.strip()]
    return bool(segs) and all(_CMD_OK.match(x) for x in segs)


def fresh_copy():
    SESSIONS_DIR.mkdir(exist_ok=True)
    target = SESSIONS_DIR / f"{SESSION_ID}.json"
    if target.exists():
        target.unlink()
    src = json.loads(SOURCE_SESSION.read_text(encoding="utf-8"))
    graph = {"id": SESSION_ID, "version": 1, "entry": src["graph"]["entry"],
             "nodes": src["graph"]["nodes"], "edges": src["graph"]["edges"]}
    tmp = SESSIONS_DIR / "_import.graph.json"
    tmp.write_text(json.dumps(graph, ensure_ascii=False), encoding="utf-8")
    grasp("new", str(tmp), "--id", SESSION_ID)
    tmp.unlink()


def ask_jev(server, state, criteria):
    payload = {"states": [{"id": "g", "state": state,
                           "questions": {"action": {"type": "choice",
                                                   "instructions": INSTRUCTION,
                                                   "criteria": criteria}}}]}
    r = json.loads(urllib.request.urlopen(urllib.request.Request(
        server.rstrip("/") + "/api/evaluate", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}), timeout=120).read())
    ans = r["states"][0]["answers"]["action"]
    return ans["choice"], ans.get("probabilities", {})


def render_trace(s, window=14):
    """Fixed layout trail the keeper (and flywheel rows) always see."""
    seq = [h["node"] for h in s["history"] if h.get("kind") in ("walk", "step")]
    tail = seq[-window:]
    return f"trail: {' -> '.join(tail)} | steps={len(seq)}"


def render_state(s, cur):
    node = next(n for n in s["graph"]["nodes"] if n["id"] == cur)
    es = edges_from(s, cur)
    untried = sum(1 for e in es if e["to"] not in
                  [h["node"] for h in s["history"] if h.get("kind") in ("walk", "step")])
    return trunc_utf8(f"[meta] graph={SESSION_ID} cur={cur}\ncur_desc: {node['desc']}\n"
                      f"{render_trace(s)}\nout_edges={len(es)} newly_seen={untried}",
                      STATE_MAX_BYTES)


def journal_tail(n=3):
    if not JOURNAL.exists():
        return "(第一次值班,没有历史日志)"
    lines = [l for l in JOURNAL.read_text(encoding="utf-8").splitlines() if l.strip()]
    packed = []
    for l in lines[-n:]:
        e = json.loads(l)
        packed.append(f"- 唤醒({e['reason']}) 判断:{e['assessment']} 动作:{e['action_taken']} 留言:{e['note']}")
    return "\n".join(packed)


def journal_write(entry):
    with JOURNAL.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry, ensure_ascii=False) + "\n")


def ask_keeper(server_state, wake_reason, evidence, fork_lines, journal, base, key, model,
               retry_feedback=None, cur="", fail_node=""):
    if wake_reason.startswith("dead-end"):
        task = ('本轮任务: 当前节点是死胡同,必须由你修路。给出action="add_edge",'
                f'edge_from="{cur}",edge_to从"图上现有节点"名单选一个语义合理的下一跳'
                '(禁止选自己),edge_label用一句中文写走这条边的理由。'
                '只有名单里确实完全找不到可续接节点时才允许keep。')
    elif wake_reason.startswith("low-confidence"):
        task = ('本轮任务: Jev在这个分叉置信度低,需要你拍板。优先action="take_edge",'
                'edge_to从"当前分叉可走的边"的to id里选一个(也可prune_edge剪掉明显错误的分支,'
                '或补一条更好的边)。只有局面确实无需干预时才keep。')
    elif wake_reason.startswith("exec-fail"):
        task = ('本轮任务: 上一步进入的目标节点命令执行失败(见上面"刚失败的目标节点"及其命令)。'
                '你必须二选一,禁止keep: '
                f'(a) action="update_cmd" 把该节点命令修成一条能跑通的只读命令'
                f'(node_id="{fail_node}", node_cmd=修正后的完整命令; 例如数行数应写 '
                '`type 路径\\文件 | find /c /v ""` 而不是 find /c ""); '
                '(b) 若该节点本质修不好, action="take_edge" 从可走的边里选一条不含它的绕行分支'
                '(edge_to=某个可走的to id)。')
    else:
        task = ("本轮任务: 基于值班日志+轨迹+当前分叉,只做一个最小可验证图操作;"
                "拿不准就keep并在note说明理由。")
    prompt = (f"== 值班上下文包 ==\n你上次的记忆(值班日志尾部):\n{journal}\n\n"
              f"{evidence}\n\n唤醒原因: {wake_reason}\n\n当前分叉可走的边:\n{fork_lines}\n\n"
              f"{task}\n"
              "校验规则: add_node的node_cmd必须echo开头; update_cmd的node_cmd须是只读检查命令"
              "(echo/git/curl/where/dir/type/find/findstr/netstat/stat/ls/cat/python/cd等,禁重定向与删除);"
              " edge端点必须是名单里真实存在的id; prune_edge只能剪已存在的边。只输出JSON,无关字段留空。"
              '例: {"assessment":"...","action":"add_edge","node_id":"","node_desc":"",'
              '"node_cmd":"","edge_from":"节点id1","edge_to":"节点id2","edge_label":"一句话理由",'
              '"note":"给下次唤醒的你"}')
    if retry_feedback:
        prompt += f"\n\n你上一份提案被校验拒绝: {retry_feedback}\n请修正后重新提案。"
    payload = {"model": model, "temperature": 0.7 if retry_feedback else 0, "max_tokens": 300,
               "messages": [{"role": "system", "content": WAKE_SYSTEM},
                            {"role": "user", "content": prompt}]}
    request = urllib.request.Request(
        base.rstrip("/") + "/chat/completions", data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    text = json.loads(urllib.request.urlopen(request, timeout=90).read()
                      )["choices"][0]["message"]["content"]
    m = re.search(r"\{.*\}", text, re.S)
    if not m:
        raise ValueError(f"keeper reply unparseable: {text[:120]!r}")
    d = json.loads(m.group(0))
    if d.get("action") not in ("keep", "take_edge", "add_node", "add_edge", "prune_edge",
                               "update_cmd"):
        raise ValueError(f"keeper picked unknown action: {d.get('action')!r}")
    if d["action"] == "add_node" and not str(d.get("node_cmd", "")).startswith("echo "):
        raise ValueError("add_node cmd must be echo-prefixed (sandbox rule)")
    return d


def apply_keeper_op(d):
    """Keeper may only do verifiable graph ops; validates against the CURRENT graph file.
    Returns human string of what happened ('提案被拒: ...' on refusal)."""
    s = load_session()  # fresh: never validate a proposal against a stale snapshot
    act = d["action"]
    if act == "keep":
        return "keep(未改图,观察)"
    if act == "add_node":
        nid = d.get("node_id") or ""
        if not re.fullmatch(r"[a-z0-9_]{3,32}", nid):
            return f"提案被拒: node_id非法 {nid!r}"
        if any(n["id"] == nid for n in s["graph"]["nodes"]):
            return f"提案被拒: 节点 {nid} 已存在"
        body = {"id": nid, "desc": (d.get("node_desc") or "")[:80], "kind": "exec",
                "cmd": d.get("node_cmd", f"echo {nid}")[:80]}
        grasp("insert", SESSION_ID, "--stdin", stdin_text=json.dumps(body, ensure_ascii=False))
        return f"新增节点 {nid}"
    if act == "add_edge":
        f, t = d.get("edge_from", ""), d.get("edge_to", "")
        ids = {n["id"] for n in s["graph"]["nodes"]}
        if f not in ids or t not in ids:
            return f"提案被拒: 端点不存在 {f}->{t}"
        if any(e["from"] == f and e["to"] == t for e in s["graph"]["edges"]):
            return f"提案被拒: 边 {f}->{t} 已存在"
        grasp("add-edge", SESSION_ID, f, t, "--label", (d.get("edge_label") or "keeper")[:40])
        return f"新增边 {f}->{t}"
    if act == "prune_edge":
        f, t = d.get("edge_from", ""), d.get("edge_to", "")
        if not any(e["from"] == f and e["to"] == t for e in s["graph"]["edges"]):
            return f"提案被拒: 边不存在 {f}->{t}"
        grasp("remove-edge", SESSION_ID, f, t)
        return f"剪除边 {f}->{t}"
    if act == "update_cmd":
        nid = d.get("node_id") or ""
        node = next((n for n in s["graph"]["nodes"] if n["id"] == nid), None)
        if node is None:
            return f"提案被拒: 节点 {nid!r} 不存在"
        if node.get("kind") != "exec":
            return f"提案被拒: 节点 {nid} 非exec(kind={node.get('kind')}),改cmd无意义"
        new_cmd = (d.get("node_cmd") or "").strip()
        if not new_cmd:
            return "提案被拒: update_cmd需要非空node_cmd"
        if new_cmd == (node.get("cmd") or "").strip():
            return "提案被拒: 新命令与原命令相同,没修任何东西"
        if not cmd_is_readonly(new_cmd):
            return f"提案被拒: 命令不在只读白名单 {new_cmd[:60]!r}"
        grasp("update", SESSION_ID, nid, "--cmd", new_cmd[:200])
        return f"改好节点 {nid} 的命令 -> {new_cmd[:60]}"
    return f"提案被拒: 未知动作 {act!r}"


def loop_detected(history_seq, window=8, repeat=3):
    if len(history_seq) < repeat:
        return False
    recent = collections.Counter(history_seq[-window:])
    return recent.most_common(1)[0][1] >= repeat


# Mid-run oracle: grasp only RAISES on a command timeout, but a fast non-zero
# exit is returned as normal output with an "ERROR: command exit code N" tail.
# The drive must not call such a check a pass just because it didn't throw.
_ERR_SIGNS = ("ERROR: command exit code", "command timeout", "ASK INPUT UNAVAILABLE")


def exec_errored(output):
    return any(sig in (output or "") for sig in _ERR_SIGNS)


def run_episode(server, base, key, model, args, ep, log):
    s = load_session()
    if not s.get("node"):
        first = s["graph"]["entry"]
        grasp("step", SESSION_ID, first)
        s = load_session()
    stats = collections.Counter()
    decisions = []
    loop_wake_seen = set()
    exec_failed = []  # exec nodes whose command errored this episode (mid-run oracle)
    silent_repair_tried = set()  # nodes already given one keeper-repair shot
    exec_fail_strikes = collections.Counter()  # per-node hang count -> abort if no progress


    def _node_cmd(snap, nid):
        return next((n.get("cmd", "") for n in snap["graph"]["nodes"] if n["id"] == nid), None)

    for step_i in range(args.max_steps):
        s = load_session()
        cur = s["node"]
        node = next(n for n in s["graph"]["nodes"] if n["id"] == cur)
        if node.get("kind") == "conclude":
            stats["reached_conclude"] = 1
            if exec_failed:
                stats["false_success"] = 1
                log(f"[ep{ep}] 抵达conclude,但{len(set(exec_failed))}个检查失败 "
                    f"{sorted(set(exec_failed))} -> 判 false_success(不计真完成)")
            else:
                stats["success"] = 1
            break
        es = edges_from(s, cur)
        if not es:
            es_fb = edges_from(s, cur, include_fallback=True)
            wake_reason = "dead-end(所有出口被剪)"
            if es_fb:  # show keeper what was pruned as evidence
                wake_reason += f"; 历史fallback: {[e['to'] for e in es_fb]}"
            stats["wakes"] += 1
            res = handle_wake(wake_reason, s, cur, [], server, base, key, model, args, log)
            if res is False:
                break
            if not edges_from(load_session(), cur, include_fallback=True):
                log(f"[ep{ep}] dead-end at {cur} 未被修复,abort本episode(不烧wake预算)")
                break
            continue
        state = render_state(s, cur)
        seq = [h["node"] for h in s["history"] if h.get("kind") in ("walk", "step")]
        if loop_detected(seq):
            if (cur, len(seq)) in loop_wake_seen:
                log(f"[ep{ep}] watchdog:loop 同局面已叫醒过守图人,本轮信任Jev继续驱动")
            else:
                loop_wake_seen.add((cur, len(seq)))
                stats["wakes"] += 1
                res = handle_wake("watchdog:loop", s, cur, es, server, base, key, model,
                                  args, log, exclude=cur)
                if res is False:
                    break
                if isinstance(res, dict):
                    decisions.append((state, None, res["stepped"], res["lane"], {}))
                    log(f"[ep{ep} step{step_i}] {cur} -[{res['lane']}]-> {res['stepped']} (wake)")
                    continue
                continue  # graph changed; fresh iteration re-renders state
        if len(es) == 1:
            target, probs, lane = es[0]["to"], {}, "forced"
            criteria = None
        else:
            criteria = {e["to"]: option_text(e.get("label") or e["to"], e["to"], next(
                n["desc"] for n in s["graph"]["nodes"] if n["id"] == e["to"])) for e in es}
            target, probs = ask_jev(server, state, criteria)
            lane = "jev"
            stats["jev_calls"] += 1
            top = max(probs.values()) if probs else 1.0
            if top < args.min_p:
                stats["wakes"] += 1
                res = handle_wake(f"low-confidence(p={top:.2f})", s, cur, es,
                                  server, base, key, model, args, log, jev_choice=target)
                if res is False:
                    break
                if isinstance(res, dict):
                    decisions.append((state, criteria, res["stepped"], res["lane"], dict(probs)))
                    log(f"[ep{ep} step{step_i}] {cur} -[{res['lane']}]-> {res['stepped']} "
                        f"p={top:.2f}(wake)")
                    continue
                continue  # graph changed; re-fork with new edges
        decisions.append((state, criteria, target, lane, dict(probs)))
        log(f"[ep{ep} step{step_i}] {cur} -[{lane}]-> {target} "
            f"p={max(probs.values()) if probs else 1.0:.2f}")
        try:
            grasp("step", SESSION_ID, target)
        except RuntimeError as error:
            exec_failed.append(target)
            stats["exec_fails"] += 1
            exec_fail_strikes[target] += 1
            cmd_before = _node_cmd(s, target)
            stats["wakes"] += 1
            res = handle_wake(f"exec-fail: {str(error)[:80]}", s, cur, es,
                              server, base, key, model, args, log, exclude=target)
            if res is False:
                break
            if isinstance(res, dict):
                decisions.append((state, criteria, res["stepped"], res["lane"], dict(probs)))
                log(f"[ep{ep} step{step_i}] {cur} -[{res['lane']}]-> {res['stepped']} (wake)")
                continue
            # res is True: re-forking. Only worth retrying this node if the keeper
            # actually rewrote its command; otherwise re-entering just hangs again.
            cmd_after = _node_cmd(load_session(), target)
            if cmd_after == cmd_before:
                log(f"[ep{ep}] {target} 执行挂死且守图人未能修复/绕行 -> 判不可修, "
                    f"abort本episode(不烧穿wake预算)")
                stats["blocked"] += 1
                break
            if exec_fail_strikes[target] >= 3:
                log(f"[ep{ep}] {target} 已多次改写命令仍挂死 -> abort(止损)")
                stats["blocked"] += 1
                break
            log(f"[ep{ep}] {target} 命令已被守图人改写,重试该节点(第{exec_fail_strikes[target]}次)")
        else:
            # fast non-zero exit: step committed, no exception -> scan the real output
            s2 = load_session()
            tn = next((n for n in s2["graph"]["nodes"] if n["id"] == target), None)
            if tn and tn.get("kind") == "exec" and exec_errored(s2.get("last_output", "")):
                exec_failed.append(target)
                stats["exec_fails"] += 1
                log(f"[ep{ep} step{step_i}] 事中预言机: {target} 退出码非零/超时(未抛异常),记为失败检查")
                if target not in silent_repair_tried and not args.no_llm:
                    silent_repair_tried.add(target)
                    stats["wakes"] += 1
                    handle_wake(f"exec-fail: {target} 命令非零退出(未抛异常)", s2, target,
                                edges_from(s2, target), server, base, key, model, args, log,
                                exclude=target)
    else:
        log(f"[ep{ep}] step budget exhausted at {cur}")
    s = load_session()
    stats["steps"] = len([h for h in s["history"] if h.get("kind") in ("walk", "step")])
    if args.flywheel:
        emit_rows(decisions, bool(stats["success"]), ep)
    return stats


WAKE_STATE = {"count": 0}


def handle_wake(reason, s, cur, es, server, base, key, model, args, log,
                jev_choice=None, exclude=None):
    """Wake protocol. Return contract for run_episode:
      False                  -> abort episode
      {"stepped","lane"}     -> drive already moved (keeper take_edge or forced escape)
      True                   -> graph may have changed; re-fork this node
    """
    WAKE_STATE["count"] += 1
    if WAKE_STATE["count"] > args.max_wakes:
        log(f"[wake] budget {args.max_wakes} exhausted, episode abort (reason={reason})")
        return False
    fork_lines = "\n".join(
        f"- {e['to']}({next(n['desc'] for n in s['graph']['nodes'] if n['id'] == e['to'])[:60]})"
        for e in es) or "(无出口)"
    visited = {h["node"] for h in s["history"] if h.get("kind") in ("walk", "step")}
    srcs = {e["from"] for e in s["graph"]["edges"]}
    inventory = "\n".join(
        f"- {n['id']}: {n['desc'][:28]}"
        f"{' [conclude=终点]' if n.get('kind') == 'conclude' else ''}"
        f"{' [叶子!无出口]' if n['id'] not in srcs else ''}{' [已走过]' if n['id'] in visited else ''}"
        for n in s["graph"]["nodes"])
    evidence = (f"{render_trace(s)}\ncurrent: {cur}\n"
                f"edges_total={len(s['graph']['edges'])}\n"
                f"图上现有节点({len(s['graph']['nodes'])}):\n{inventory}")
    if reason.startswith("exec-fail") and exclude:
        fn = next((n for n in s["graph"]["nodes"] if n["id"] == exclude), None)
        if fn and fn.get("kind") == "exec":
            evidence += (f"\n刚失败的目标节点: {exclude}\n它的命令: {fn.get('cmd','')}\n"
                         f"(执行超时或报错。可用update_cmd修这条命令,node_id={exclude})")
    taken = collections.Counter(h["node"] for h in s["history"]
                                if h.get("kind") in ("walk", "step"))
    if args.no_llm:
        log(f"[wake#{WAKE_STATE['count']}:{reason}] --no-llm: forced keep")
        journal_write({"ts": int(time.time()), "reason": reason,
                       "assessment": "(no-llm)", "action_taken": "keep", "note": "-"})
        return True
    feedback = None
    d = did = None
    for attempt in (1, 2):
        try:
            d = ask_keeper(server, reason, evidence, fork_lines, journal_tail(),
                           base, key, model, retry_feedback=feedback, cur=cur,
                           fail_node=(exclude or ""))
        except Exception as error:
            log(f"[wake:{reason}] keeper failed: {str(error)[:140]}")
            d = {"action": "keep", "assessment": "keeper失败", "note": str(error)[:60]}
        if d["action"] == "take_edge":
            t = d.get("edge_to") or ""
            if t == exclude:
                feedback = (f"take_edge目标{t!r}正是刚失败/死循环的目标,禁止再选;"
                            f"可走的其余边: {[e['to'] for e in es if e['to'] != exclude]}")
            elif t in {e["to"] for e in es}:
                did = f"裁决通行 {cur}->{t}"
                log(f"[wake#{WAKE_STATE['count']}:{reason}] {d['assessment']} -> {did}")
                journal_write({"ts": int(time.time()), "reason": reason,
                               "assessment": d.get("assessment", ""), "action_taken": did,
                               "note": d.get("note", ""), "raw": d})
                try:
                    grasp("step", SESSION_ID, t)
                    return {"stepped": t, "lane": "keeper"}
                except RuntimeError as error:  # graph diverged under us; caller re-forks
                    log(f"[wake] take_edge执行失败: {str(error)[:100]}")
                    return True
            else:
                feedback = f"take_edge目标{t!r}不在可走的边 {[e['to'] for e in es]}"
        else:
            did = apply_keeper_op(d)
            if not did.startswith("提案被拒"):
                break
            feedback = did
        if attempt == 2:
            d = {"action": "keep", "assessment": d.get("assessment", ""),
                 "note": f"两轮提案被拒: {feedback[:60]}"}
            did = "keep(两轮被拒,降级)"
            log(f"[wake#{WAKE_STATE['count']}:{reason}] 两轮被拒({feedback}) -> 降级keep")
            break
        log(f"[wake#{WAKE_STATE['count']}:{reason}] {d.get('assessment','')} -> 拒({feedback}),重试")
    log(f"[wake#{WAKE_STATE['count']}:{reason}] {d.get('assessment','')} -> {did}")
    journal_write({"ts": int(time.time()), "reason": reason,
                   "assessment": d.get("assessment", ""), "action_taken": did,
                   "note": d.get("note", ""), "raw": d})
    if d["action"] == "keep" and es:  # never let a wake stall the drive
        cands = [e["to"] for e in es if e["to"] != exclude]
        alt = jev_choice if jev_choice in cands else (
            min(cands, key=lambda t: taken[t]) if cands else None)
        if alt:
            lane = "jev" if alt == jev_choice else "escape"
            log(f"[wake] keep -> 强制通行 {cur}->{alt} ({lane})")
            try:
                grasp("step", SESSION_ID, alt)
                return {"stepped": alt, "lane": lane}
            except RuntimeError:
                pass
    return True


def emit_rows(decisions, success, ep):
    if not success:
        return  # no hindsight without a finished trail (v0 credit rule)
    kept = [row for row in decisions if row[3] == "jev" and row[1]]
    if len(kept) < 2:
        return
    rows = []
    for state, criteria, action, lane, probs in kept:
        h = hashlib.md5(state.encode("utf-8")).hexdigest()[:8]
        rows.append({"id": f"qfops-ep{ep}.{h}->{action}",
                     "state_id": f"qfops:{ep}:{h}",
                     "family_id": f"qfops-ep{ep}", "split": "train",
                     "state": state,
                     "questions": {"next-edge": {"type": "choice",
                                                 "instructions": INSTRUCTION,
                                                 "criteria": criteria}},
                     "gold": {"next-edge": action},
                     "teacher": {"native_probs": probs},
                     "label_source": "outcome-hindsight", "lane": "jev"})
    with FLYWHEEL.open("a", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"flywheel: +{len(rows)} qf_ops rows -> {FLYWHEEL.name}")


def main():
    global SESSION_ID
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=JEV_DEFAULT)
    ap.add_argument("--session", default=SESSION_ID, help="session in SESSIONS_DIR to drive")
    ap.add_argument("--min-p", type=float, default=0.5)
    ap.add_argument("--max-steps", type=int, default=24)
    ap.add_argument("--max-wakes", type=int, default=8)
    ap.add_argument("--episodes", type=int, default=2)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--no-llm", action="store_true", help="plumbing test: wakes log only")
    ap.add_argument("--flywheel", action="store_true")
    args = ap.parse_args()
    SESSION_ID = args.session
    base = os.environ.get("LLM_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
    key = os.environ.get("DASHSCOPE_API_KEY", "")
    model = os.environ.get("LLM_MODEL", "qwen-turbo")
    if not args.no_llm and not key:
        sys.exit("DASHSCOPE_API_KEY missing (or run --no-llm)")

    if args.fresh or not (SESSIONS_DIR / f"{SESSION_ID}.json").exists():
        fresh_copy()
    log = lambda *a: print(*a, flush=True)
    total = collections.Counter()
    for ep in range(1, args.episodes + 1):
        total.update(run_episode(args.server, base, key, model, args, ep, log))
    s = load_session()
    print(f"\n== qf_ops grow v0 ==  success={total['success']}/{args.episodes}"
          f"  false_success={total['false_success']}  blocked={total['blocked']}"
          f"  jev_calls={total['jev_calls']}  wakes={total['wakes']}"
          f"  exec_fails={total['exec_fails']}")
    print(f"graph now: {len(s['graph']['nodes'])} nodes / {len(s['graph']['edges'])} edges"
          f" (source had {len(json.loads(SOURCE_SESSION.read_text(encoding='utf-8'))['graph']['nodes'])})")
    print(f"journal -> {JOURNAL}")


if __name__ == "__main__":
    main()
