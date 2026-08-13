#ifndef GRASP_CPP_SESSION_H
#define GRASP_CPP_SESSION_H

  // Session state machine: execute nodes, jump along edges, traverse, plus insert/remove topology ops.
  // All operations write back to the session (the caller persists via store.save).
  // step/travel update history and graph.meta (visits) after every node -- topo accumulation.

#include <string>
#include <vector>
#include "store.h"

  // execution outcome of one node (also the feedback fed to the meta session graph / LLM)
struct StepOutcome {
    std::string node;
    std::string desc;
    std::string kind;
    std::string output;
    std::vector<std::string> next_desc;  // successor descriptions after this node (id + desc)
    bool reached_target;  // whether travel hit the target node
    bool terminated;  // conclude ended the session
    std::string forked_session;  // new session id produced by a fork node (empty = no fork)
};

  // single step: jump to node_id and execute. Before start only entry is allowed; otherwise node_id must be a successor of the current node.
StepOutcome session_step(Session& s, const std::string& node_id, SessionStore* store);

  // traverse from the current node (or from) along edges. With target, BFS a path toward it;
  // otherwise follow the first non-fallback edge (or the first edge if all fallback). Stop at conclude / dead end / target / fork.
std::vector<StepOutcome> session_travel(Session& s, const std::string& from,
                                        const std::string& target, SessionStore* store);

  // execute one node and record it (history + visits accumulation)
StepOutcome session_execute_node(Session& s, const Node& n, SessionStore* store);

  // topology ops: insert (optionally with one edge) / add-edge / remove-edge / remove / set-target
void session_insert(Session& s, const Node& n, const Edge* e);
void session_add_edge(Session& s, const Edge& e);
void session_remove_edge(Session& s, const std::string& from, const std::string& to);
void session_remove(Session& s, const std::string& node_id);
void session_set_target(Session& s, const std::string& target);

  // merge src's topology into dst (append-only union, git-merge style): nodes/edges absent in dst
  // are appended; same-id conflicts keep dst and are counted; version++. returns a one-line summary.
  // merge never rewrites existing nodes/edges, so any session state stays valid.
std::string session_merge(Session& dst, const Session& src);

  // jumpable nodes of the current (or given) node, as "to (fallback) - desc" lines
std::vector<std::string> session_list_next(const Session& s, const std::string& node_id);

  // walk: topology stroll (no execution, pure thinking navigation). Single edges advance automatically; multiple edges / dead end / conclude
  // stop and return the path node descriptions joined with edge event language as a natural-language sentence (external thinking tool).
  // choose selects the Nth outgoing edge at the start (1-based); auto_choose selects the Nth edge at EVERY multi-edge node
  // (non-interactive stroll); max_steps guards cycles (default 50). walk updates visits accumulation and advances the current node.
std::string session_walk(Session& s, const std::string& from, int choose,
                         int auto_choose, int max_steps, SessionStore* store);

  // status summary (full session JSON with --json)
std::string session_status(const Session& s, bool as_json);

#endif // GRASP_CPP_SESSION_H
