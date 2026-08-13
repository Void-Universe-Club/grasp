#include "driver.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

#include "llm.h"
#include "session.h"

namespace {

const char* SYSTEM_PROMPT =
    "You are the meta-learning loop of a session graph framework.\n"
    "A session holds a graph: nodes are steps with descriptions, edges are jumps.\n"
    "Executing a node produces text output; your job is to explore the topology,\n"
    "reach the conclusion node, and grow the graph when branches are missing.\n"
    "You decide ONE action per turn and reply with ONLY a JSON object, no extra text.\n"
    "\n"
    "Action schema (choose exactly one \"action\"):\n"
    "- {\"action\":\"step\",\"node\":\"<id>\"}: step to an OUTGOING node of the current node and execute it.\n"
    "- {\"action\":\"travel\"}: traverse from the current node along edges (toward target if set) until conclude / dead end / target.\n"
    "- {\"action\":\"set_target\",\"node\":\"<id>\"}: set the target node, then traverse toward it.\n"
    "- {\"action\":\"insert\",\"nodes\":[{...}],\"edge\":{\"from\":\"...\",\"to\":\"...\"}}: insert new node(s) and optionally one edge (use to explore unknown branches). \"edge\" may be omitted.\n"
    "- {\"action\":\"fork\"}: fork this session (clone graph) and continue on the fork.\n"
    "- {\"action\":\"done\"}: finish driving.\n"
    "\n"
    "Insert node JSON format: {\"id\":\"...\",\"desc\":\"...\",\"kind\":\"exec|ask|conclude|fork\",\"cmd\":\"...\"}\n"
    "Rules: new node ids must be unique; step only to nodes listed in outgoing;\n"
    "prefer exploring unexplored nodes; end with done when the task is finished.";

  // an LLM decision (parsed action)
struct Decision {
    std::string action;         // step | travel | set_target | insert | fork | done
    std::string node;  // node for step / set_target
    std::vector<Node> nodes;  // new nodes for insert
    Edge edge;  // optional edge for insert
    bool has_edge;
    Decision() : has_edge(false) {}
};

  // apply an action, return feedback text (input of the next prompt) and the forked session id
struct ApplyResult {
    std::string feedback;
    std::string new_session;  // new session id produced by fork (empty = unchanged)
    bool finished;             // done
    ApplyResult() : finished(false) {}
};

std::string join_next(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += "\n";
        out += lines[i];
    }
    return out;
}

  // observation assembly: current node / successors / recent history / unexplored / visit stats / fork chain
std::string build_observation(const Session& s, const SessionStore& store) {
    std::stringstream ss;
    ss << "session: " << s.id << "\n";
    ss << "parent : " << (s.parent.empty() ? "(root)" : s.parent) << "\n";
    ss << "target : " << (s.target.empty() ? "(none)" : s.target) << "\n";
    ss << "state  : " << s.state;
    if (!s.started()) {
        ss << " (NOT STARTED: first action must be step to entry '"
           << s.graph.entry << "')";
    }
    ss << "\n";

    std::string cur = s.started() ? s.node : s.graph.entry;
    const Node* n = s.graph.find_node(cur);
    ss << "current: " << cur << " | "
       << (n ? n->desc : "(missing node)") << "\n";

    ss << "outgoing:\n";
    std::vector<const Edge*> es = s.graph.edges_from(cur);
    if (es.empty()) ss << "  (none - dead end)\n";
    for (size_t i = 0; i < es.size(); ++i) {
        const Node* t = s.graph.find_node(es[i]->to);
        std::string label = es[i]->label.empty()
                                ? es[i]->to
                                : es[i]->label + " -> " + es[i]->to;
        ss << "  [" << (i + 1) << "] " << label
           << (es[i]->fallback ? " (fallback)" : "") << " - "
           << (t ? t->desc : "(missing)") << "\n";
    }

  // recent history (at most 8)
    ss << "recent history (last " << (s.history.size() < 8 ? s.history.size() : 8)
       << "):\n";
    size_t start = s.history.size() > 8 ? s.history.size() - 8 : 0;
    for (size_t i = start; i < s.history.size(); ++i) {
        std::string out = s.history[i].output;
        if (out.size() > 200) out = out.substr(0, 200) + "...";
        ss << "  " << s.history[i].node << " [" << s.history[i].kind
           << "] -> \"" << out << "\"\n";
    }

  // unexplored nodes
    std::vector<std::string> un = s.graph.unexplored_reachable();
    ss << "unexplored nodes (reachable, never visited): ";
    if (un.empty()) {
        ss << "(none)\n";
    } else {
        ss << un[0];
        for (size_t i = 1; i < un.size(); ++i) ss << ", " << un[i];
        ss << "\n";
    }

  // meta.visits summary (topo accumulation: explored distribution, guides whether to deepen or switch)
    ss << "visit counts: ";
    if (s.graph.meta.is_object() && s.graph.meta.contains("visits") &&
        s.graph.meta["visits"].is_object() && !s.graph.meta["visits"].empty()) {
        bool first = true;
        for (nlohmann::json::const_iterator it = s.graph.meta["visits"].begin();
             it != s.graph.meta["visits"].end(); ++it) {
            if (!first) ss << ", ";
            ss << it.key() << ":" << it.value();
            first = false;
        }
        ss << "\n";
    } else {
        ss << "(none)\n";
    }

  // fork tree summary
    if (!s.parent.empty()) {
        try {
            Session p = store.load(s.parent);
            ss << "fork info: child of " << s.parent
               << " (parent ran " << p.history.size() << " steps, "
               << p.graph.nodes.size() << " nodes)\n";
        } catch (const std::exception&) {
            ss << "fork info: child of " << s.parent << " (parent gone)\n";
        }
    }
    return ss.str();
}

std::string decision_prompt(const std::string& obs) {
    return
        "Observe the session graph below, then reply with your next action as a JSON object.\n"
        "\n"
        "=== Observation ===\n" + obs + "\n"
        "=== Decision (JSON only) ===";
}

  // parse LLM decision JSON; throw std::runtime_error on invalid input
Decision parse_decision(const std::string& text) {
    nlohmann::json j = nlohmann::json::parse(text);
    if (!j.is_object() || !j.contains("action") || !j["action"].is_string()) {
        throw std::runtime_error("decision must be a JSON object with an \"action\" field");
    }
    Decision d;
    d.action = j["action"].get<std::string>();
    if (d.action == "step" || d.action == "set_target") {
        if (!j.contains("node") || !j["node"].is_string()) {
            throw std::runtime_error("action '" + d.action + "' requires a \"node\" field");
        }
        d.node = j["node"].get<std::string>();
    } else if (d.action == "insert") {
        if (!j.contains("nodes") || !j["nodes"].is_array() || j["nodes"].empty()) {
            throw std::runtime_error("action 'insert' requires a \"nodes\" array");
        }
        d.nodes = j["nodes"].get<std::vector<Node> >();
        if (j.contains("edge") && j["edge"].is_object()) {
            d.edge = j["edge"].get<Edge>();
            d.has_edge = true;
        }
    } else if (d.action != "travel" && d.action != "fork" && d.action != "done") {
        throw std::runtime_error("unknown action '" + d.action + "'");
    }
    return d;
}

  // feedback line for a single outcome
std::string outcome_line(const StepOutcome& o) {
    std::string out = o.output;
    if (out.size() > 500) out = out.substr(0, 500) + "...";
    std::stringstream ss;
    ss << "== node " << o.node << " [" << o.kind << "]: \"" << o.desc << "\"\n";
    ss << "   output: \"" << out << "\"\n";
    if (!o.next_desc.empty()) {
        ss << "   next: " << join_next(o.next_desc) << "\n";
    } else {
        ss << "   next: (none - dead end)\n";
    }
    return ss.str();
}

ApplyResult apply_decision(SessionStore& store, const std::string& sid,
                           const Decision& d) {
    ApplyResult r;
    Session s = store.load(sid);

    if (d.action == "step") {
        StepOutcome o = session_step(s, d.node, &store);
        store.save(s);
        r.feedback = "stepped to " + o.node + "\n" + outcome_line(o);
        if (s.state == "done") r.feedback += "\nsession concluded (state=done)";
  // stepped to a fork node: switch context to the new session
        if (!o.forked_session.empty()) r.new_session = o.forked_session;
    } else if (d.action == "travel") {
        std::vector<StepOutcome> outs = session_travel(s, "", "", &store);
        store.save(s);
        std::stringstream ss;
        ss << "travel executed " << outs.size() << " node(s):\n";
        for (size_t i = 0; i < outs.size(); ++i) {
            ss << outcome_line(outs[i]);
            if (!outs[i].forked_session.empty()) {
                r.new_session = outs[i].forked_session;
            }
        }
        if (s.state == "done") ss << "\nsession concluded (state=done)";
        r.feedback = ss.str();
    } else if (d.action == "set_target") {
        session_set_target(s, d.node);
        store.save(s);
        std::vector<StepOutcome> outs = session_travel(s, "", d.node, &store);
        store.save(s);
        std::stringstream ss;
        ss << "target set to " << d.node << ", travel executed " << outs.size()
           << " node(s):\n";
        for (size_t i = 0; i < outs.size(); ++i) {
            ss << outcome_line(outs[i]);
            if (!outs[i].forked_session.empty()) {
                r.new_session = outs[i].forked_session;
            }
        }
        if (s.state == "done") ss << "\nsession concluded (state=done)";
        r.feedback = ss.str();
    } else if (d.action == "insert") {
        std::stringstream ss;
  // exploration stub: when the LLM gives no edge, auto-link the first new node from the current node
        std::string cur = s.started() ? s.node : s.graph.entry;
        Edge auto_edge;
        bool auto_edge_used = false;
        for (size_t i = 0; i < d.nodes.size(); ++i) {
            const Edge* e = NULL;
            if (i == 0) {
                if (d.has_edge) {
                    e = &d.edge;
                } else {
                    auto_edge.from = cur;
                    auto_edge.to = d.nodes[i].id;
                    auto_edge.fallback = false;
                    e = &auto_edge;
                    auto_edge_used = true;
                }
            }
            session_insert(s, d.nodes[i], e);
            ss << "inserted node " << d.nodes[i].id << " ["
               << d.nodes[i].kind << "]\n";
        }
        if (auto_edge_used) {
            ss << "auto edge " << auto_edge.from << "->" << auto_edge.to
               << " (from current node)\n";
        }
        store.save(s);
        ss << "graph now v" << s.graph.version << " | "
           << s.graph.nodes.size() << " nodes / " << s.graph.edges.size()
           << " edges";
        r.feedback = ss.str();
    } else if (d.action == "fork") {
        Session child = store.fork(sid);
        r.new_session = child.id;
        r.feedback = "forked session " + sid + " -> " + child.id +
                     " (graph cloned, will continue on the fork)";
    } else if (d.action == "done") {
        r.finished = true;
        r.feedback = "driving finished by LLM decision";
    } else {
        throw std::runtime_error("unknown action '" + d.action + "'");
    }
    return r;
}

}  // namespace

int drive_session(SessionStore& store, const std::string& session_id, int max_steps) {
    LlmConfig cfg;
    if (!llm_config_from_env(cfg)) {
        throw std::runtime_error(
            "LLM not configured: set OPENAI_API_KEY (optional OPENAI_BASE_URL / OPENAI_MODEL), "
            "or drive manually via REPL / one-shot subcommands");
    }
    if (max_steps <= 0) max_steps = 20;

    std::string current = session_id;
    int rounds = 0;
    for (; rounds < max_steps; ++rounds) {
        Session s = store.load(current);
        if (s.state == "done") {
            std::cout << "session " << current << " finished (state=done), drive stops\n";
            break;
        }
        if (s.state == "error") {
            throw std::runtime_error("session " + current + " in error state, drive stops");
        }

        std::string obs = build_observation(s, store);
        std::cout << "\n===== drive round " << (rounds + 1) << " / " << max_steps
                  << " (" << current << ") =====\n" << obs;

  // LLM decision; on parse/validation failure feed the error back and retry (<=3 per round)
        std::string prompt = decision_prompt(obs);
        bool decided = false;
        for (int retry = 0; retry < 3 && !decided; ++retry) {
            std::string content = llm_chat(cfg, SYSTEM_PROMPT, prompt);
            std::string raw;
            try {
                raw = extract_json(content);
                Decision d = parse_decision(raw);
                std::cout << "decision: "
                          << nlohmann::json{{"action", d.action},
                                            {"node", d.node}}.dump() << "\n";
                ApplyResult r = apply_decision(store, current, d);
                std::cout << r.feedback << "\n";
  // any action that forks (fork decision / step-travel into a fork node) switches context
                if (!r.new_session.empty()) {
                    current = r.new_session;
                    std::cout << "drive switched to fork session " << current << "\n";
                }
                if (r.finished) return rounds + 1;
                decided = true;
            } catch (const std::exception& e) {
                std::cout << "invalid decision: " << e.what() << "\n";
                prompt += "\n\nlast round was invalid (" + std::string(e.what()) +
                          "). Please output exactly one valid action JSON.";
            }
        }
        if (!decided) {
            throw std::runtime_error("LLM gave 3 consecutive invalid decisions, drive aborts");
        }
    }
    std::cout << "drive hit max-steps(" << max_steps << ") budget\n";
    return rounds;
}
