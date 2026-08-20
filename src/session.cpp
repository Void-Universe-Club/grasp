#include "session.h"

#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

#include "os.h"

namespace {

const int MAX_TRAVEL_STEPS = 200;

// node description (placeholder when missing)
std::string node_desc(const Graph& g, const std::string& id) {
    const Node* n = g.find_node(id);
    return n ? n->desc : "(missing node)";
}

// next hop by edge order: non-fallback first, else the first edge
std::string pick_next(const Graph& g, const std::string& from) {
    std::vector<const Edge*> es = g.edges_from(from);
    if (es.empty()) return "";
    for (size_t i = 0; i < es.size(); ++i) {
        if (!es[i]->fallback) return es[i]->to;
    }
    return es[0]->to;
}

// BFS path from start to target; next[from] = first hop toward target
std::map<std::string, std::string> bfs_next_hop(const Graph& g,
                                                const std::string& start,
                                                const std::string& target) {
    std::map<std::string, std::string> next;
    if (start == target) return next;
    std::map<std::string, std::string> prev;   // child -> parent
    std::set<std::string> seen;
    std::queue<std::string> q;
    seen.insert(start);
    q.push(start);
    bool found = false;
    while (!q.empty() && !found) {
        std::string cur = q.front();
        q.pop();
        std::vector<const Edge*> es = g.edges_from(cur);
        for (size_t i = 0; i < es.size(); ++i) {
            if (seen.count(es[i]->to) > 0) continue;
            seen.insert(es[i]->to);
            prev[es[i]->to] = cur;
            if (es[i]->to == target) {
                found = true;
                break;
            }
            q.push(es[i]->to);
        }
    }
    if (!found) return next;   // target unreachable, no guide
    // build the first-hop guide by backtracking from target
    std::string cur = target;
    while (prev.count(cur) > 0 && prev[cur] != start) {
        next[prev[cur]] = cur;
        cur = prev[cur];
    }
    if (prev.count(cur) > 0) next[start] = cur;
    return next;
}

}  // namespace

// ---------- node execution ----------

StepOutcome session_execute_node(Session& s, const Node& n, SessionStore* store) {
    StepOutcome o;
    o.node = n.id;
    o.desc = n.desc;
    o.kind = n.kind;
    o.reached_target = false;
    o.terminated = false;

    std::string output;
    if (n.kind == "exec") {
        output = os::run_shell(n.cmd, n.timeout_secs);
    } else if (n.kind == "ask") {
        // read one input line as the output (drive / REPL / piped input)
        std::cout << n.prompt << std::endl;
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            output = "ASK INPUT UNAVAILABLE (stdin closed)";
            s.state = "error";
        } else {
            output = line;
        }
    } else if (n.kind == "conclude") {
        output = n.message;
        o.terminated = true;
        s.state = "done";
    } else if (n.kind == "fork") {
        if (store == NULL) {
            throw std::runtime_error("fork node needs a session store (run via CLI/drive)");
        }
        Session child = store->fork(s.id);
        output = "FORKED " + child.id;
        o.forked_session = child.id;
    } else {
        throw std::runtime_error("unknown node kind '" + n.kind + "'");
    }

    // successor descriptions (fed back to the meta session graph after execution)
    std::vector<const Edge*> es = s.graph.edges_from(n.id);
    for (size_t i = 0; i < es.size(); ++i) {
        std::string label = es[i]->label.empty()
                                ? es[i]->to
                                : es[i]->label + " -> " + es[i]->to;
        o.next_desc.push_back(label +
                              (es[i]->fallback ? " (fallback)" : "") +
                              " - " + node_desc(s.graph, es[i]->to));
    }

    // record + topo accumulation
    StepRecord rec;
    rec.node = n.id;
    rec.kind = n.kind;
    rec.output = output;
    rec.at_ms = os::now_ms();
    s.history.push_back(rec);
    s.last_output = output;
    s.graph.mark_visited(n.id);
    o.output = output;
    return o;
}

  // ---------- single step ----------

StepOutcome session_step(Session& s, const std::string& node_id, SessionStore* store) {
    if (node_id.empty()) {
        throw std::runtime_error("step requires a node id");
    }
    s.state = "running";  // operating on a finished/waiting session re-activates it
    if (!s.started()) {
        if (node_id != s.graph.entry) {
            throw std::runtime_error("session not started, can only step to entry '" +
                                     s.graph.entry + "'");
        }
    } else {
        bool is_next = false;
        std::vector<const Edge*> es = s.graph.edges_from(s.node);
        for (size_t i = 0; i < es.size(); ++i) {
            if (es[i]->to == node_id) {
                is_next = true;
                break;
            }
        }
        if (!is_next) {
            throw std::runtime_error("'" + node_id +
                                     "' is not a successor of current node '" + s.node +
                                     "' is not a successor of the current node (see list-next)");
        }
    }
    const Node* n = s.graph.find_node(node_id);
    if (n == NULL) {
        throw std::runtime_error("node '" + node_id + "' does not exist");
    }
  // record the entering edge (evidence for not retracing); no entering edge on first start
    if (!s.node.empty()) {
        s.graph.mark_edge_visited(s.node, node_id);
    }
    s.node = node_id;
    StepOutcome o = session_execute_node(s, *n, store);
    if (s.target == node_id) o.reached_target = true;
    return o;
}

  // ---------- traversal ----------

std::vector<StepOutcome> session_travel(Session& s, const std::string& from,
                                        const std::string& target,
                                        SessionStore* store) {
  // start: explicit from > current node > entry; travel always begins at the given start
    std::string cur = !from.empty() ? from : (s.started() ? s.node : s.graph.entry);
    if (s.graph.find_node(cur) == NULL) {
        throw std::runtime_error("node '" + cur + "' does not exist");
    }
    s.state = "running";  // traversing a finished session re-activates it
    s.node = cur;

    std::string tgt = !target.empty() ? target : s.target;
    std::map<std::string, std::string> hop = bfs_next_hop(s.graph, cur, tgt);

    std::vector<StepOutcome> outs;
    for (int steps = 0; steps < MAX_TRAVEL_STEPS; ++steps) {
        const Node* n = s.graph.find_node(s.node);
        if (n == NULL) {
            throw std::runtime_error("node '" + s.node + "' does not exist");
        }
        StepOutcome o = session_execute_node(s, *n, store);
        outs.push_back(o);

        if (!tgt.empty() && s.node == tgt) {
            outs.back().reached_target = true;
            break;
        }
        if (s.state == "done" || s.state == "error") break;
        if (!o.forked_session.empty()) break;  // after a fork, drive decides whether to switch
  // next hop: BFS guide > default edge order; dead end / self-loop stops
        std::string next;
        if (!tgt.empty() && hop.count(s.node) > 0) {
            next = hop[s.node];
        } else {
            next = pick_next(s.graph, s.node);
        }
        if (next.empty() || next == s.node) break;
        s.graph.mark_edge_visited(s.node, next);  // edge-visit accumulation
        s.node = next;
    }
    return outs;
}

  // ---------- walk: topology stroll (external thinking tool for AI agents) ----------

namespace {

  // record walk dialogue actions (ask sentences / choices) for conversation replay
void push_walk_record(Session& s, const std::string& node, const std::string& kind,
                      const std::string& text) {
    StepRecord rec;
    rec.node = node;
    rec.kind = kind;
    rec.output = text;
    rec.at_ms = os::now_ms();
    s.history.push_back(rec);
}

}  // namespace

std::string session_walk(Session& s, const std::string& from, int choose,
                         int auto_choose, int max_steps, SessionStore* store) {
    (void)store;  // walk executes nothing, no store needed
    if (max_steps <= 0) max_steps = 50;
    std::string cur = !from.empty() ? from : (s.started() ? s.node : s.graph.entry);
    if (s.graph.find_node(cur) == NULL) {
        throw std::runtime_error("node '" + cur + "' does not exist");
    }

    std::stringstream path;  // path description chain (node desc joined by ->)
    std::stringstream stmt;  // final assembled sentence
    stmt << "Walking from " << cur << ": ";
    bool first = true;
    for (int step = 0; step < max_steps; ++step) {
        const Node* n = s.graph.find_node(cur);
        if (n == NULL) {
            throw std::runtime_error("node '" + cur + "' does not exist");
        }
  // 1. append the node description
        if (!first) path << " → ";
        path << n->desc;
        first = false;
  // 2. advance the thinking position + topo accumulation (walk = considered the node)
        s.node = cur;
        s.graph.mark_visited(cur);
        StepRecord rec;
        rec.node = cur;
        rec.kind = "walk";
        rec.output = n->desc;
        rec.at_ms = os::now_ms();
        s.history.push_back(rec);
  // 3. reached conclude: terminal
        if (n->kind == "conclude") {
            stmt << path.str() << ". Reached conclusion: " << n->message;
            push_walk_record(s, cur, "walk_ask", stmt.str());
            return stmt.str();
        }
  // 4. edges: single edge advances automatically; multiple edges stop and ask (choose applies to the start only)
        std::vector<const Edge*> es = s.graph.edges_from(cur);
        if (es.empty()) {
            stmt << path.str()
                 << ". This node has no outgoing edges (end of path). Would you like to add new options (insert)?";
            push_walk_record(s, cur, "walk_ask", stmt.str());
            return stmt.str();
        }
        if (es.size() > 1) {
            int pick_idx = 0;
            if (step == 0 && choose >= 1 && choose <= static_cast<int>(es.size())) {
                pick_idx = choose;  // start choose: explicit option for the entry node
            } else if (auto_choose == -1) {
  // auto (smart): prefer the first never-visited edge (exploration), else the
  // fallback edge, else the first — keeps walking to conclude without stopping
                for (size_t i = 0; i < es.size(); ++i) {
                    if (s.graph.edge_visit_count(cur, es[i]->to) == 0) {
                        pick_idx = static_cast<int>(i) + 1;
                        break;
                    }
                    if (es[i]->fallback && pick_idx == 0) {
                        pick_idx = static_cast<int>(i) + 1;  // remember fallback
                    }
                }
                if (pick_idx == 0) pick_idx = 1;
            } else if (auto_choose >= 1 && auto_choose <= static_cast<int>(es.size())) {
                pick_idx = auto_choose;  // auto: same option index at every multi-edge node
            }
            if (pick_idx >= 1) {
  // follow the chosen edge, record edge visit + the choice action
                const Edge* pick = es[pick_idx - 1];
                s.graph.mark_edge_visited(cur, pick->to);
                const Node* t = s.graph.find_node(pick->to);
                std::string opt = !pick->label.empty()
                                      ? pick->label
                                      : (t ? t->desc : pick->to);
                push_walk_record(s, cur, "walk_choose",
                                 "choice: " + opt);
                cur = pick->to;
                continue;
            }
  // assemble options: edge label (event language), fallback to the target node desc;
  // never-walked edges are marked [unexplored] to surface unexplored edges
            stmt << path.str() << ". There are " << es.size() << " options: ";
            for (size_t i = 0; i < es.size(); ++i) {
                if (i > 0) {
                    stmt << " ";
                }
                const Node* t = s.graph.find_node(es[i]->to);
                std::string opt = !es[i]->label.empty() ? es[i]->label
                                                         : (t ? t->desc : es[i]->to);
                stmt << (i + 1) << ". " << opt;
                if (s.graph.edge_visit_count(cur, es[i]->to) == 0) {
                    stmt << "[unexplored]";
                }
            }
            stmt << ". Which do you choose? Or would you like to add more options?";
            push_walk_record(s, cur, "walk_ask", stmt.str());
            return stmt.str();
        }
  // single edge: advance and record the edge visit (evidence for not retracing)
        s.graph.mark_edge_visited(cur, es[0]->to);
        cur = es[0]->to;
    }
    stmt << path.str() << ". Walk step budget (" << max_steps << ") exceeded, stopped.";
    return stmt.str();
}

  // ---------- topology operations ----------

void session_insert(Session& s, const Node& n, const Edge* e) {
    std::string err = validate_new_node(s.graph, n);
    if (!err.empty()) throw std::runtime_error(err);
    s.graph.nodes.push_back(n);
    if (e != NULL) {
        err = validate_new_edge(s.graph, *e);
        if (!err.empty()) {
            s.graph.nodes.pop_back();  // rollback
            throw std::runtime_error(err);
        }
        s.graph.edges.push_back(*e);
    }
    s.graph.version++;
}

void session_add_edge(Session& s, const Edge& e) {
    std::string err = validate_new_edge(s.graph, e);
    if (!err.empty()) throw std::runtime_error(err);
    s.graph.edges.push_back(e);
    s.graph.version++;
}

void session_remove_edge(Session& s, const std::string& from, const std::string& to) {
    for (size_t i = 0; i < s.graph.edges.size(); ++i) {
        if (s.graph.edges[i].from == from && s.graph.edges[i].to == to) {
            s.graph.edges.erase(s.graph.edges.begin() + static_cast<long>(i));
            s.graph.version++;
            return;
        }
    }
    throw std::runtime_error("edge '" + from + " -> " + to + "' does not exist");
}

void session_remove(Session& s, const std::string& node_id) {
    const Node* n = s.graph.find_node(node_id);
    if (n == NULL) {
        throw std::runtime_error("node '" + node_id + "' does not exist");
    }
    if (node_id == s.graph.entry) {
        throw std::runtime_error("cannot remove entry node '" + node_id + "'");
    }
    if (node_id == s.node) {
        throw std::runtime_error("cannot remove the current node '" + node_id + "'");
    }
  // remove the node
    for (size_t i = 0; i < s.graph.nodes.size(); ++i) {
        if (s.graph.nodes[i].id == node_id) {
            s.graph.nodes.erase(s.graph.nodes.begin() + static_cast<long>(i));
            break;
        }
    }
  // drop edges that reference it
    for (size_t i = 0; i < s.graph.edges.size();) {
        if (s.graph.edges[i].from == node_id || s.graph.edges[i].to == node_id) {
            s.graph.edges.erase(s.graph.edges.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    if (s.target == node_id) s.target = "";
    s.graph.version++;
}

void session_set_target(Session& s, const std::string& target) {
    if (s.graph.find_node(target) == NULL) {
        throw std::runtime_error("node '" + target + "' does not exist");
    }
    s.target = target;
}

std::string session_merge(Session& dst, const Session& src) {
    int merged_nodes = 0, merged_edges = 0, conflicts = 0;
  // nodes: append src nodes absent in dst; same-id keeps dst (append-only, never rewrite)
    for (size_t i = 0; i < src.graph.nodes.size(); ++i) {
        const Node& n = src.graph.nodes[i];
        const Node* existing = dst.graph.find_node(n.id);
        if (existing == NULL) {
            dst.graph.nodes.push_back(n);
            merged_nodes++;
        } else if (existing->desc != n.desc || existing->kind != n.kind ||
                   existing->cmd != n.cmd) {
            conflicts++;  // divergent definition: dst wins, counted for visibility
        }
    }
  // edges: append src edges absent in dst (same from->to keeps dst)
    for (size_t i = 0; i < src.graph.edges.size(); ++i) {
        const Edge& e = src.graph.edges[i];
        bool found = false;
        for (size_t j = 0; j < dst.graph.edges.size(); ++j) {
            if (dst.graph.edges[j].from == e.from && dst.graph.edges[j].to == e.to) {
                found = true;
                break;
            }
        }
        if (!found) {
            dst.graph.edges.push_back(e);
            merged_edges++;
        }
    }
  // visit stats: union — carry src's visits for newly merged nodes, keep max for shared ones
    if (src.graph.meta.is_object() && src.graph.meta.contains("visits") &&
        src.graph.meta["visits"].is_object()) {
        if (!dst.graph.meta.is_object()) dst.graph.meta = nlohmann::json::object();
        if (!dst.graph.meta.contains("visits")) dst.graph.meta["visits"] = nlohmann::json::object();
        const nlohmann::json& sv_meta = src.graph.meta["visits"];
        for (nlohmann::json::const_iterator it = sv_meta.begin(); it != sv_meta.end(); ++it) {
            int sv = it.value().get<int>();
            int dv = dst.graph.meta["visits"].value(it.key(), 0);
            if (sv > dv) dst.graph.meta["visits"][it.key()] = sv;
        }
    }
    dst.graph.version++;
    std::stringstream ss;
    ss << "merged " << merged_nodes << " node(s), " << merged_edges << " edge(s)"
       << (conflicts > 0 ? ", " + std::to_string(conflicts) + " conflict(s) kept dst" : "")
       << " | graph v" << dst.graph.version;
    return ss.str();
}

  // ---------- queries and status ----------

std::vector<std::string> session_list_next(const Session& s, const std::string& node_id) {
    std::string nid = node_id.empty() ? (s.started() ? s.node : s.graph.entry) : node_id;
    if (s.graph.find_node(nid) == NULL) {
        throw std::runtime_error("node '" + nid + "' does not exist");
    }
    std::vector<const Edge*> es = s.graph.edges_from(nid);
    std::vector<std::string> lines;
    for (size_t i = 0; i < es.size(); ++i) {
        std::string label = es[i]->label.empty()
                                ? es[i]->to
                                : es[i]->label + " -> " + es[i]->to;
        lines.push_back(label +
                        (es[i]->fallback ? " (fallback)" : "") +
                        " - " + node_desc(s.graph, es[i]->to));
    }
    if (lines.empty()) {
        lines.push_back("(no outgoing edges - dead end)");
    }
    return lines;
}

std::string session_status(const Session& s, bool as_json) {
    if (as_json) {
        nlohmann::json j = s;
        return j.dump(2);
    }
    std::stringstream ss;
    ss << "session: " << s.id << "\n";
    ss << "parent : " << (s.parent.empty() ? "(root)" : s.parent) << "\n";
    ss << "graph  : " << s.graph.id << " v" << s.graph.version
       << " | entry " << s.graph.entry
       << " | " << s.graph.nodes.size() << " nodes / " << s.graph.edges.size() << " edges\n";
    ss << "state  : " << s.state << "\n";
    ss << "node   : " << (s.started() ? s.node : "(not started)") << "\n";
    ss << "target : " << (s.target.empty() ? "(none)" : s.target) << "\n";
    ss << "history: " << s.history.size() << " steps\n";
    ss << "last   : " << s.last_output << "\n";
  // topo accumulation summary
    std::stringstream v;
    if (s.graph.meta.is_object() && s.graph.meta.contains("visits") &&
        s.graph.meta["visits"].is_object()) {
        bool first = true;
        for (nlohmann::json::const_iterator it = s.graph.meta["visits"].begin();
             it != s.graph.meta["visits"].end(); ++it) {
            if (!first) v << ", ";
            v << it.key() << ":" << it.value();
            first = false;
        }
    }
    ss << "visits : " << (v.str().empty() ? "(none)" : v.str()) << "\n";
    std::vector<std::string> un = s.graph.unexplored_reachable();
    ss << "unexpl : " << (un.empty() ? "(none)" : un[0]);
    for (size_t i = 1; i < un.size(); ++i) ss << ", " << un[i];
    ss << "\n";
  // unexplored-edge summary (helps the agent see unwalked paths)
    std::vector<std::string> ue = s.graph.unexplored_edges();
    ss << "unexplE : " << (ue.empty() ? "(none)" : ue[0]);
    for (size_t i = 1; i < ue.size(); ++i) ss << ", " << ue[i];
    ss << "\n";
    return ss.str();
}
