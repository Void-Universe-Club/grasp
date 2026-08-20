# grasp

A zero-dependency C++ binary that gives an LLM agent a **deterministic external memory**: a session graph where nodes are steps (with optional shell commands), edges are transitions. The agent strolls the topology to think; grasp stitches node descriptions into a sentence, asks at multi-edge forks, executes commands, and persists every lesson (visit counts, unexplored edges, new nodes) so nothing is forgotten and nothing is re-traced.

> **The graph session topology replaces the agent loop.** No fragile in-context loop: state lives in a persistent, deterministic graph — the agent strolls, chooses, executes, and the topology accumulates what it learned. **Continuous learning is built in**: every step writes visits, every lesson appends a node or edge, every fork branches the mind — the graph grows with the session and stays correct across runs.

> **Self-improvement through graph evolution.** The topology is the agent's evolving brain, not a static config: every pitfall is persisted as a new node/edge, every reflection upgrades the principle chain, and the same agent walking the same graph gets measurably better over rounds — the graph itself is the improvement.

> **Live demo & docs:** [void-universe-club.github.io/grasp-site](https://void-universe-club.github.io/grasp-site/) — interactive proof that feedback alone is not enough; the topology is the memory.

## grasp market

A marketplace of fine-tuned **graph topologies**: install a topology (fine-tuned by agents or hand-written by humans) and instantly give your agent that skill — no training, no prompt engineering, just a JSON graph.

```bash
# 1. pick a topology from the market (e.g. Galois Analyst: a mathematician's thinking topology)
curl -O https://void-universe-club.github.io/grasp-site/market/galois_meta.json
./grasp new galois_meta.json --id my-math
# 2. use it like any session: walk / step / drive
./grasp walk my-math
```

Each market entry ships a versioned, append-only graph JSON — the same format as `graphs/*.json` — so any topology is interchangeable and auditable.

**Submit your own topology:** author a versioned graph JSON (nodes / edges / entry / version), fork [`Void-Universe-Club/grasp-site`](https://github.com/Void-Universe-Club/grasp-site), add it as `market/<name>.json`, register it in `market/index.json` (id / name / version / author / tags / desc / file), and open a pull request — merged entries appear on the market automatically.

The market lives at [void-universe-club.github.io/grasp-site/market.html](https://void-universe-club.github.io/grasp-site/market.html) — searchable, sorted, installable in one command.

## Build & test

```bash
# CMake (recommended, cross-platform)
cmake -S . -B build && cmake --build build      # Linux/macOS (g++/clang)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release            # Windows (MSVC)
# MinGW: cmake -S . -B build -G "MinGW Makefiles"

# plain Makefile (Linux/macOS only)
make          # produces ./grasp
make test     # end-to-end suite (uses a mock LLM server; no API key needed)
```

All OS-specific code lives in `src/os.cpp` behind the `os::` API (`src/os.h`).

## Quick start (for agents)

Build the binary first (see [Build & test](#build--test)): `./grasp` on Linux/macOS, `grasp.exe` on Windows. Everything below is a subcommand — scriptable from any agent loop. Sessions persist as JSON in `sessions/` (override with `GRASP_CPP_SESSIONS`).

```bash
# 1. create a session from a graph file
./grasp new graphs/example.json --id my-session

# 2. stroll: grasp stitches node descriptions into a sentence;
#    at multi-edge nodes it asks "which do you choose?"
./grasp walk my-session
./grasp walk my-session --choose 2      # pick the 2nd option and continue

# 3. execute a node (runs its cmd, records the output into history)
./grasp step my-session n_start

# 4. persist new knowledge: insert a node (+ optional edge), append-only, version++
#    three forms: inline JSON | flag form (no quoting hell) | file
./grasp insert my-session '{"id":"n_lesson","desc":"lesson: ...","kind":"exec","cmd":"echo hi"}' --edge n_start,n_lesson
./grasp insert my-session n_lesson --desc "lesson: ..." --cmd "echo hi" --edge n_start,n_lesson
./grasp insert my-session --file node.json --edge n_start,n_lesson

# 5. let the LLM drive itself (optional; needs OPENAI_API_KEY)
./grasp drive my-session --max-steps 10
```

Graph files: `graphs/example.json` (minimal), `graphs/walk_demo.json` (stroll demo), `graphs/meta.json` (a full "thinking principles" topology).

## Core concepts

### Graph schema

```json
{
  "id": "example", "version": 1, "entry": "n_start",
  "nodes": [
    {"id": "n_start", "desc": "start: output a greeting", "kind": "exec", "cmd": "echo hello"},
    {"id": "n_done",  "desc": "end session", "kind": "conclude", "message": "task done"}
  ],
  "edges": [
    {"from": "n_start", "to": "n_ask", "label": "go ask"},
    {"from": "n_start", "to": "n_done", "label": "finish now", "fallback": true}
  ]
}
```

- Node `kind`: `exec` (run `cmd`, capture output) / `ask` (read a line from stdin) / `conclude` (end session) / `fork` (spawn a child session)
- Edge `label` is the option text shown during `walk`; `fallback` is the default exit
- Validation on write: entry must exist, node ids unique, edge references valid

### How the memory accumulates

1. **Visits**: every walk/step/travel updates `graph.meta.visits`; `status` reports unexplored nodes and edges
2. **History**: every executed node's output is stored in the session history and fed back into observations
3. **Fork**: `fork <sid>` deep-copies the graph and inherits history — explore risky branches on a child, keep the parent clean
4. **Unexplored steering**: `walk` marks unwalked edges `[unexplored]`; the agent should prefer them (they carry the highest information gain)

### drive loop (optional LLM autonomy)

`drive` builds an observation (current node / successors / recent history / unexplored / visit counts / fork info), asks the LLM for a JSON decision, executes it, and loops:

```
{"action": "step"|"travel"|"set_target"|"insert"|"fork"|"done", "node": "...", ...}
```

Invalid decisions are fed back with the error and retried (≤3 per round). Needs `OPENAI_API_KEY` (any OpenAI-compatible endpoint works).

## Command reference

| Command | Description |
|---|---|
| `new <graph.json> [--id NAME]` | create a session from a graph file |
| `list` | list sessions |
| `status <sid> [--json]` | current state, visits, unexplored nodes + edges |
| `list-next <sid> [--node ID]` | outgoing edges of current (or given) node |
| `walk <sid> [--from ID] [--choose N] [--auto] [--auto N] [--steps N]` | stroll: stitch descriptions into a sentence; multi-edge nodes stop and ask; `--auto` prefers unexplored edges (else fallback, else first); `--auto N` picks option N at every fork (non-interactive) |
| `show <sid> [<node-id>]` | node detail: desc / cmd / kind / visits / outgoing + incoming edges |
| `step <sid> <node-id>` | jump to a successor and execute it |
| `travel <sid> [--from ID] [--target ID]` | execute along edges (BFS toward target) |
| `set-target <sid> <node-id>` | set the target node for travel |
| `insert <sid> '<node-json>' [--edge from,to]` | insert node (+optional edge), version++ |
| `insert <sid> <node-id> --desc '...' [--cmd '...'] [--kind kind] [--edge from,to]` | flag form — no JSON quoting needed |
| `insert <sid> --file node.json [--edge from,to]` | insert node from a JSON file; a JSON array inserts many nodes at once (each element may carry `edge_from`/`edge_to` to auto-link) |
| `add-edge <sid> <from> <to> [--label '...']` | append an edge |
| `remove-edge <sid> <from> <to>` | remove an edge |
| `remove <sid> <node-id>` | remove a node and its edges |
| `dump-svg <sid> [--out FILE] [--width W] [--height H]` | render the session graph as a standalone SVG (layered layout, kind colors, visit badges) |
| `fork <sid>` | fork a child session (deep-copy graph + history) |
| `merge <dst-sid> <src-sid>` | merge src's topology into dst — append-only union; same-id conflicts keep dst, version++ |
| `rebase <sid>` | sync the parent session's newest topology into this session (git-rebase style) |
| `delete <sid>` | delete a session |
| `drive <sid> [--max-steps N]` | LLM decision loop (needs OPENAI_API_KEY) |
| `repl [--session sid]` | interactive REPL |

## Environment variables

| Variable | Purpose |
|---|---|
| `OPENAI_API_KEY` | LLM API key (required by `drive`; expanded by sh, never lands in argv) |
| `OPENAI_BASE_URL` | OpenAI-compatible endpoint (default `https://api.openai.com/v1`; local ollama: `http://127.0.0.1:11434/v1`) |
| `OPENAI_MODEL` | model name (default `gpt-4o-mini`) |
| `GRASP_CPP_SESSIONS` | session store directory (default `sessions/`) |

## Repository layout

```
src/            main · cli · model · store · os · session · llm · driver · repl
third_party/    nlohmann/json single header (v3.11.3)
graphs/         example · walk_demo · meta (thinking-principle topology)
tests/          end-to-end suite + mock LLM server
reports/        generated reports (git-ignored)
```

## Contributing

Contributions are welcome — new market topologies, docs, or core fixes.

- Fork `Void-Universe-Club/grasp` (code) or `Void-Universe-Club/grasp-site` (web site).
- To submit a topology to the market: author a versioned graph JSON, add it as `market/<name>.json` in `grasp-site`, register it in `market/index.json`, and open a pull request.
- Run the test suite before opening a pull request (78 end-to-end tests): `cmake --build build && bash tests/run_tests.sh` (or `make test` on Linux/macOS).

## Citation

If you use or reference this project in a paper, please cite the repository:

```
https://github.com/Void-Universe-Club/grasp
```

## License

MIT

## Copyright

© 2026 www.void-universe.com — all rights reserved.
