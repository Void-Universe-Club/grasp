#!/usr/bin/env python3
"""Probe the dashscope OpenAI-compatible endpoint without revealing credentials."""
import json
import os
import sys
import urllib.request

key = os.environ.get("DASHSCOPE_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN") or os.environ.get("ANTHROPIC_AUTH_TOKEN_ALIYUN")
if not key:
    sys.exit("no api key in env")
base = os.environ.get("LLM_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
model = os.environ.get("LLM_MODEL", "qwen-turbo")
payload = {"model": model, "messages": [{"role": "user", "content": "只回答一个字：好"}], "max_tokens": 8}
request = urllib.request.Request(
    base.rstrip("/") + "/chat/completions",
    data=json.dumps(payload).encode(),
    headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
try:
    body = json.loads(urllib.request.urlopen(request, timeout=30).read())
    print(f"OK model={model} reply={body['choices'][0]['message']['content'][:12]!r}")
except Exception as error:
    detail = getattr(error, "read", lambda: b"")()
    print(f"FAIL {type(error).__name__}: {str(error)[:120]} {detail[:200]!r}")
    sys.exit(1)
