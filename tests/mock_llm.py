#!/usr/bin/env python3
"""Mock OpenAI-compatible server for drive-loop end-to-end testing.

basic mode: returns step n_plan -> travel -> done per call count, verifying the base chain.
grow mode: returns insert(new branch) -> fork -> done, verifying topology growth and fork switching.
usage: mock_llm.py <port> [basic|grow]
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
            # no edge: verifies drive auto-links from the current node (exploration instrumentation)
            return {"action": "insert",
                    "nodes": [{"id": "n_new", "desc": "explore a new branch",
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
