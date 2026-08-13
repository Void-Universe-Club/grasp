#!/usr/bin/env python3
"""Mock OpenAI-compatible server for drive-loop end-to-end testing.

basic 模式：按调用次数返回 step n_plan -> travel -> done，验证基础链路。
grow 模式：返回 insert(新分支) -> fork -> done，验证拓扑扩展与 fork 切换。
用法: mock_llm.py <port> [basic|grow]
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

COUNTER = {"n": 0}
MODE = sys.argv[2] if len(sys.argv) > 2 else "basic"


def next_decision():
    COUNTER["n"] += 1
    n = COUNTER["n"]
    if MODE == "grow":
        if n == 1:
            # 不带 edge：验证驱动自动从当前节点接边（探索插桩）
            return {"action": "insert",
                    "nodes": [{"id": "n_new", "desc": "探索新分支",
                                "kind": "exec", "cmd": "echo new"}]}
        if n == 2:
            return {"action": "fork"}
        return {"action": "done"}
    if n == 1:
        return {"action": "step", "node": "n_plan"}
    if n == 2:
        return {"action": "travel"}
    return {"action": "done"}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = json.loads(self.rfile.read(length) or b"{}")
        if not body.get("messages"):
            self.send_error(400, "bad request")
            return
        resp = {"choices": [{"message": {"content": json.dumps(next_decision())}}]}
        data = json.dumps(resp).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
