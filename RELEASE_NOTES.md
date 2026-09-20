# Release notes

Append a section per release; the Release workflow injects the section matching
the tag name into the GitHub Release body. Keep entries at `## vX.Y.Z` level.

## v0.1.4

Same binaries as v0.1.3; first release published through the new
release-notes pipeline.

### Infrastructure

- Release notes are now authored in `RELEASE_NOTES.md`: the workflow extracts
  the section matching the tag and injects it into the GitHub Release body
  (replaces the empty auto-generated changelog).

## v0.1.3

First packaged cross-platform release of grasp — a zero-dependency C++11 binary
that gives an LLM agent a deterministic external memory (a session graph of
nodes, edges and transitions).

### Functions

- **Session graph memory**: `new / walk / step / travel / show / status` —
  stroll the topology, stitch node descriptions into sentences, execute node
  commands, track visits and unexplored edges.
- **Append-only learning**: `insert` (inline JSON / flag form / file, batch
  arrays) / `add-edge` / `remove-edge` / `remove` — persist every lesson,
  version++ on each write, validated on save.
- **Branching**: `fork` (deep-copy graph + history), `merge` (append-only
  union), `rebase` (pull parent's newest topology).
- **LLM autonomy**: `drive --max-steps N` decision loop over any
  OpenAI-compatible endpoint (`OPENAI_BASE_URL` / `OPENAI_MODEL`), invalid
  decisions fed back and retried.
- **Visualization**: `dump-svg` renders the session graph as a standalone SVG
  (layered layout, kind colors, visit badges).
- **grasp market**: install versioned graph topologies (JSON) as ready-made
  agent skills — interchangeable and auditable.
- **REPL** (`repl`) and scriptable subcommands for any agent loop.
- 78 end-to-end tests (`tests/run_tests.sh`, mock LLM server, no API key).

### Bug fixes

- `os::trunc_utf8` / `os::utf8_sanitize` were declared only under
  `#ifdef _WIN32` in `src/os.h` while implemented cross-platform, breaking
  compilation on Linux and macOS (`trunc_utf8 is not a member of os`).

### Infrastructure

- New GitHub Actions release workflow: builds and ships binaries for
  Linux (x64), Windows (x64, MSVC) and macOS (arm64) on every `v*` tag;
  build failures surface as per-file error annotations.

## v0.1.0

Initial source import.
