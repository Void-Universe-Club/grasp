#!/usr/bin/env bash
# grasp end-to-end: full command chain, validation rejections, topo accumulation, fork tree.
# usage: make test (or bash tests/run_tests.sh)

set -u
cd "$(dirname "$0")/.."

BIN=./grasp
WORK=$(mktemp -d /tmp/grasp-test-XXXXXX)
export GRASP_CPP_SESSIONS="$WORK/sessions"
PASS=0
FAIL=0

note()  { echo "--- $1"; }
ok()    { PASS=$((PASS+1)); echo "  ok: $1"; }
fail()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }

# assert_contains <haystack> <needle> <desc>
assert_contains() {
    if echo "$1" | grep -q "$2"; then ok "$3"; else fail "$3 (missing: $2)"; fi
}
assert_exit() { # assert_exit <code> <desc> <cmd...>
    local want=$1 desc=$2; shift 2
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then ok "$desc (exit $got)"; else fail "$desc (want $want, got $got)"; fi
}

note "0. basics: no-arg usage / help"
assert_exit 1 "no-arg exit code 1" "$BIN"
OUT=$($BIN help); assert_contains "$OUT" "list-next" "help lists subcommands"

note "1. new: create a session from a graph file"
OUT=$($BIN new graphs/example.json)
assert_contains "$OUT" "session s-" "new prints session id"
SID=$(echo "$OUT" | grep -o 's-[0-9-]*')
assert_contains "$OUT" "example" "new prints graph id"
ls "$WORK/sessions/$SID.json" >/dev/null 2>&1 && ok "session file persisted" || fail "session file missing"

note "2. open / status"
OUT=$($BIN open "$SID"); assert_contains "$OUT" "(not started)" "open shows not-started state"
OUT=$($BIN status "$SID"); assert_contains "$OUT" "unexpl" "status shows unexplored stats"
OUT=$($BIN status "$SID" --json)
echo "$OUT" | grep -q '"id".*s-' && ok "status --json emits JSON" || fail "status --json"

note "3. list-next: outgoing nodes of the current (entry) node"
OUT=$($BIN list-next "$SID")
assert_contains "$OUT" "n_plan" "list-next includes n_plan"
assert_contains "$OUT" "n_done (fallback)" "list-next marks fallback"
assert_contains "$OUT" "plan: pick a branch" "list-next shows successor descriptions"
OUT=$($BIN list-next "$SID" --node n_plan)
assert_contains "$OUT" "n_ask" "list-next --node n_plan includes n_ask"

note "4. step: single step to a successor and execute"
assert_exit 1 "step to a non-successor rejected" "$BIN" step "$SID" n_done
OUT=$(echo "" | $BIN step "$SID" n_start)
assert_contains "$OUT" "hello from grasp" "step entry executes"
OUT=$(echo "" | $BIN step "$SID" n_plan)
assert_contains "$OUT" "plan: explore" "step n_plan executes"
OUT=$($BIN status "$SID"); assert_contains "$OUT" "n_plan" "status current node becomes n_plan"
OUT=$($BIN status "$SID"); assert_contains "$OUT" "n_plan:1" "visits accumulate n_plan:1"

note "5. travel: full traversal (ask consumes stdin)"
OUT=$(echo "continue" | $BIN travel "$SID")
assert_contains "$OUT" "travel done: 3 node(s)" "travel walks 3 steps from n_plan (plan -> ask -> conclude)"
assert_contains "$OUT" "next:" "travel shows successor descriptions each step"
assert_contains "$OUT" "task done" "travel reaches conclude"
assert_contains "$OUT" "continue" "ask consumes stdin input"

note "6. set-target + travel"
SID2_OUT=$($BIN new graphs/example.json)
SID2=$(echo "$SID2_OUT" | grep -o 's-[0-9-]*')
OUT=$($BIN set-target "$SID2" n_done); assert_contains "$OUT" "target set to n_done" "set-target succeeds"
OUT=$($BIN travel "$SID2")
assert_contains "$OUT" "reached target" "travel hits target"

note "7. insert / remove topology ops"
OUT=$($BIN insert "$SID2" '{"id":"n_new","desc":"new branch","kind":"exec","cmd":"echo new"}' --edge n_start,n_new)
assert_contains "$OUT" "inserted node n_new" "insert node succeeds"
assert_exit 1 "insert duplicate id rejected" "$BIN" insert "$SID2" '{"id":"n_new","desc":"x","kind":"exec","cmd":"echo x"}'
assert_exit 1 "insert invalid edge rejected" "$BIN" insert "$SID2" '{"id":"n_bad","desc":"x","kind":"exec","cmd":"echo x"}' --edge n_start,ghost
OUT=$($BIN list-next "$SID2" --node n_start); assert_contains "$OUT" "n_new" "new branch immediately reachable"
OUT=$($BIN remove "$SID2" n_new); assert_contains "$OUT" "removed node n_new" "remove node succeeds"
assert_exit 1 "remove missing node rejected" "$BIN" remove "$SID2" n_ghost
assert_exit 1 "remove entry rejected" "$BIN" remove "$SID2" n_start
assert_exit 1 "insert illegal kind rejected" "$BIN" insert "$SID2" '{"id":"n_x","desc":"x","kind":"hack","cmd":"echo x"}'

note "8. fork: deep-copied topology, parent recorded"
OUT=$($BIN fork "$SID2"); assert_contains "$OUT" "forked" "fork succeeds"
CHILD=$(echo "$OUT" | grep -o 's-[0-9-]*' | tail -1)
OUT=$($BIN status "$CHILD"); assert_contains "$OUT" "$SID2" "fork child records parent"
OUT=$($BIN status "$CHILD"); assert_contains "$OUT" "n_done:1" "fork inherits visit accumulation"
OUT=$($BIN status "$CHILD"); assert_contains "$OUT" "history: 2 steps" "fork inherits parent conversation history"

note "9. delete"
assert_exit 0 "delete succeeds" "$BIN" delete "$CHILD"
ls "$WORK/sessions/$CHILD.json" >/dev/null 2>&1 && fail "file still exists after delete" || ok "file removed after delete"

note "10. invalid graph files rejected"
echo '{"id":"bad","version":1,"entry":"ghost","nodes":[],"edges":[]}' > "$WORK/bad.json"
assert_exit 1 "graph without entry rejected" "$BIN" new "$WORK/bad.json"
echo 'not json' > "$WORK/notjson.json"
assert_exit 1 "non-JSON file rejected" "$BIN" new "$WORK/notjson.json"

note "11. drive: correct error without LLM key"
assert_exit 1 "drive errors without key" env -u OPENAI_API_KEY "$BIN" drive "$SID" --max-steps 3

note "13. drive: mock-LLM end-to-end (decide -> execute -> feedback -> accumulate)"
PORT=$((20000 + RANDOM % 20000))
python3 tests/mock_llm.py "$PORT" & MOCK_PID=$!
sleep 0.5
D_OUT=$($BIN new graphs/example.json)
D_SID=$(echo "$D_OUT" | grep -o 's-[0-9-]*')
DRIVE_OUT=$(echo "continue" | \
    OPENAI_API_KEY=dummy OPENAI_BASE_URL="http://127.0.0.1:$PORT/v1" \
    OPENAI_MODEL=mock "$BIN" drive "$D_SID" --max-steps 5 2>&1)
kill "$MOCK_PID" 2>/dev/null
assert_contains "$DRIVE_OUT" "decision: {\"action\":\"step\"" "drive parses LLM step decision"
assert_contains "$DRIVE_OUT" "invalid decision: session not started" "failed decision fed back to LLM for retry"
assert_contains "$DRIVE_OUT" "travel executed 4 node(s)" "drive executes travel"
assert_contains "$DRIVE_OUT" "session concluded" "drive reaches conclude"
assert_contains "$DRIVE_OUT" "visit counts: " "drive observation includes visits summary"
D_ST=$(echo "$DRIVE_OUT" | grep -c "outgoing:")
[ "$D_ST" -ge 1 ] && ok "drive observation includes topo description" || fail "drive observation includes topo description"
OUT=$($BIN status "$D_SID"); assert_contains "$OUT" "state  : done" "session done after drive"
OUT=$($BIN status "$D_SID"); assert_contains "$OUT" "n_start:1" "visits accumulated after drive"
OUT=$($BIN status "$D_SID"); assert_contains "$OUT" "n_explore" "unexplored nodes still reported after drive"

echo
note "14. drive: mock-LLM meta-learning (insert branch + self-fork)"
PORT2=$((20000 + RANDOM % 20000))
python3 tests/mock_llm.py "$PORT2" grow & MOCK2_PID=$!
sleep 0.5
G_OUT=$($BIN new graphs/example.json)
G_SID=$(echo "$G_OUT" | grep -o 's-[0-9-]*')
G_DRIVE=$(OPENAI_API_KEY=dummy OPENAI_BASE_URL="http://127.0.0.1:$PORT2/v1" \
    OPENAI_MODEL=mock "$BIN" drive "$G_SID" --max-steps 5 2>&1)
kill "$MOCK2_PID" 2>/dev/null
assert_contains "$G_DRIVE" "inserted node n_new" "LLM insert decision executed"
assert_contains "$G_DRIVE" "auto edge n_start->n_new" "insert without edge auto-links from current node"
assert_contains "$G_DRIVE" "graph now v2" "graph version bumped after insert"
assert_contains "$G_DRIVE" "forked session" "LLM fork decision executed"
assert_contains "$G_DRIVE" "drive switched to fork session" "drive switches context to the fork"
G_CHILD=$(echo "$G_DRIVE" | grep -o 'forked session s-[0-9-]* -> s-[0-9-]*' | grep -o 's-[0-9-]*$')
OUT=$($BIN status "$G_CHILD"); assert_contains "$OUT" "$G_SID" "fork child records parent"
OUT=$($BIN status "$G_SID"); assert_contains "$OUT" "n_new" "original session topo contains the new node"
OUT=$($BIN list-next "$G_SID" --node n_start); assert_contains "$OUT" "n_new" "auto-linked new branch immediately reachable"

echo
note "15. fork node: stepping to a kind=fork node auto-creates a child session"
F_OUT=$($BIN new graphs/example.json)
F_SID=$(echo "$F_OUT" | grep -o 's-[0-9-]*')
OUT=$(echo "" | $BIN step "$F_SID" n_start)
OUT=$(echo "" | $BIN step "$F_SID" n_plan)
OUT=$(echo "" | $BIN step "$F_SID" n_explore)
OUT=$(echo "" | $BIN step "$F_SID" n_fork)
assert_contains "$OUT" "\\[forked\\] new session" "fork node execution creates a child session"
F_CHILD=$(echo "$OUT" | grep -o 's-[0-9-]*' | tail -1)
ls "$WORK/sessions/$F_CHILD.json" >/dev/null 2>&1 && ok "fork child session persisted" || fail "fork child session file missing"
OUT=$($BIN status "$F_CHILD"); assert_contains "$OUT" "$F_SID" "fork-node child records parent"
OUT=$($BIN status "$F_CHILD"); assert_contains "$OUT" "history: 3 steps" "fork-node child inherits the 3 prior steps"
OUT=$(echo "" | $BIN travel "$F_SID" --from n_fork)
assert_contains "$OUT" "\\[forked\\]" "travel stops at fork node and creates a new session"

echo
note "16. walk: stitched stroll sentences (external thinking tool mode)"
W_OUT=$($BIN new graphs/walk_demo.json)
W_SID=$(echo "$W_OUT" | grep -o 's-[0-9-]*')
OUT=$($BIN walk "$W_SID")
assert_contains "$OUT" "at home → hungry" "walk stitches path node descriptions"
assert_contains "$OUT" "There are 2 options" "walk reports option count"
assert_contains "$OUT" "find food" "walk lists option 1 (edge label)"
assert_contains "$OUT" "sleep" "walk lists option 2 (edge label)"
assert_contains "$OUT" "\\[unexplored\\]" "walk marks unwalked edges unexplored"
assert_contains "$OUT" "Which do you choose? Or would you like to add more options?" "walk asks for a choice at multi-edge nodes"
OUT=$($BIN walk "$W_SID" --choose 1)
assert_contains "$OUT" "eating" "walk --choose 1 follows the find-food branch"
assert_contains "$OUT" "end of path" "walk reports dead end"
assert_contains "$OUT" "add new options" "walk suggests insert at dead end"
OUT=$($BIN status "$W_SID"); assert_contains "$OUT" "unexplE : B->D" "walked edges are no longer marked unexplored"
OUT=$($BIN walk "$W_SID" --from B --choose 2)
assert_contains "$OUT" "resting" "walk --from B --choose 2 follows the sleep branch"
OUT=$($BIN status "$W_SID"); assert_contains "$OUT" "A:1" "walk visit accumulation"
OUT=$($BIN list-next "$W_SID" --node B)
assert_contains "$OUT" "find food -> C" "list-next shows edge labels"

echo
note "12. REPL smoke test"
RPL_OUT=$($BIN new graphs/example.json)
RPL_SID=$(echo "$RPL_OUT" | grep -o 's-[0-9-]*')
printf 'open %s\nlist-next\nstep n_start\nstep n_plan\nstep n_ask\ncontinue\nexit\n' "$RPL_SID" | "$BIN" repl > "$WORK/repl.out" 2>&1
assert_contains "$(cat "$WORK/repl.out")" "grasp" "REPL starts"
assert_contains "$(cat "$WORK/repl.out")" "What would you like to do?" "REPL can execute ask node"
assert_contains "$(cat "$WORK/repl.out")" "hello from grasp" "REPL auto-injects session id for step"

echo
note "17. dump-svg: session graph rendered as standalone SVG"
SVG_SID=$(echo "$($BIN new graphs/example.json)" | grep -o 's-[0-9-]*')
OUT=$($BIN dump-svg "$SVG_SID" --out "$WORK/topo.svg")
assert_contains "$OUT" "wrote $WORK/topo.svg" "dump-svg writes the output file"
assert_contains "$(cat "$WORK/topo.svg")" "<svg" "dump-svg output starts with an SVG document"
assert_contains "$(cat "$WORK/topo.svg")" "n_start" "dump-svg embeds node ids"
assert_contains "$(cat "$WORK/topo.svg")" "entry" "dump-svg marks the entry node"

echo
note "18. usability: walk --auto / show / insert flag form / list UNEXPL"
U_SID=$(echo "$($BIN new graphs/walk_demo.json)" | grep -o 's-[0-9-]*')
OUT=$($BIN walk "$U_SID" --auto 2)
assert_contains "$OUT" "resting" "walk --auto follows the same option at every fork"
OUT=$($BIN insert "$U_SID" u_node --desc "flags form node" --cmd "echo hi" --edge A,u_node)
assert_contains "$OUT" "inserted node u_node" "insert flag form (--desc/--cmd) works"
OUT=$($BIN show "$U_SID" u_node)
assert_contains "$OUT" "desc    : flags form node" "show prints the node desc"
assert_contains "$OUT" "<- A" "show lists incoming edges"
OUT=$($BIN list)
assert_contains "$OUT" "UNEXPL" "list reports unexplored node count"

echo
note "19. merge / rebase: fork -> learn -> merge back; parent evolution -> rebase"
M_P=$(echo "$($BIN new graphs/example.json --id mparent)" | grep -o 's-[0-9-]*' || echo mparent)
M_C=$($BIN fork mparent | grep -o 's-[0-9-]*')
OUT=$($BIN insert "$M_C" m_lesson --desc "child lesson" --cmd "echo x" --edge n_start,m_lesson)
assert_contains "$OUT" "inserted node m_lesson" "child session learns a node"
OUT=$($BIN merge mparent "$M_C")
assert_contains "$OUT" "merged 1 node(s), 1 edge(s)" "merge brings child's topology back to parent"
OUT=$($BIN show mparent m_lesson)
assert_contains "$OUT" "child lesson" "parent now sees the merged node"
OUT=$($BIN add-edge mparent n_done n_start --label "loop")
assert_contains "$OUT" "graph v3" "parent evolves independently"
OUT=$($BIN rebase "$M_C")
assert_contains "$OUT" "rebased" "rebase syncs parent topology into child"
OUT=$($BIN show "$M_C" n_done)
assert_contains "$OUT" "loop" "child now has parent's new edge"

echo
echo "========== RESULT: $PASS passed, $FAIL failed =========="
rm -rf "$WORK"
[ "$FAIL" -eq 0 ]
