#include "cli.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "driver.h"
#include "model.h"
#include "session.h"
#include "store.h"
#include "svg.h"

std::string sessions_dir() {
    const char* d = getenv("GRASP_CPP_SESSIONS");
    return (d != NULL && std::string(d).size() > 0) ? d : "sessions";
}

namespace {

SessionStore& store() {
    static SessionStore s(sessions_dir());
    return s;
}

  // session id from args: explicit wins, else current_session (REPL)
std::string session_id_of(const std::vector<std::string>& args, size_t* pos,
                          std::string* current_session) {
    if (*pos < args.size() && args[*pos] != "--max-steps" &&
        args[*pos] != "--from" && args[*pos] != "--target" &&
        args[*pos] != "--node" && args[*pos] != "--json" &&
        args[*pos] != "--edge") {
        return args[(*pos)++];
    }
    if (current_session != NULL && !current_session->empty()) {
        return *current_session;
    }
    throw std::runtime_error("missing session id (open <id> first in REPL)");
}

  // parse --key value / --flag options, return positional remainder
std::vector<std::string> parse_options(const std::vector<std::string>& args,
                                       size_t from,
                                       std::map<std::string, std::string>& opts,
                                       std::vector<std::string>* flags) {
    std::vector<std::string> rest;
    for (size_t i = from; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() > 2 && a.compare(0, 2, "--") == 0) {
            std::string name = a.substr(2);
            if (i + 1 < args.size() && args[i + 1].compare(0, 2, "--") != 0) {
                opts[name] = args[++i];
            } else {
                flags->push_back(name);
            }
        } else {
            rest.push_back(a);
        }
    }
    return rest;
}

int option_int(const std::map<std::string, std::string>& opts,
               const std::string& name, int def) {
    std::map<std::string, std::string>::const_iterator it = opts.find(name);
    if (it == opts.end()) return def;
    return atoi(it->second.c_str());
}

  // print execution outcomes of step/travel
void print_outcomes(const std::vector<StepOutcome>& outs) {
    for (size_t i = 0; i < outs.size(); ++i) {
        const StepOutcome& o = outs[i];
        std::cout << "== node " << o.node << " [" << o.kind << "]: \""
                  << o.desc << "\"\n";
        std::cout << "   output: \"" << o.output << "\"\n";
        if (!o.next_desc.empty()) {
            std::cout << "   next: ";
            for (size_t j = 0; j < o.next_desc.size(); ++j) {
                if (j > 0) std::cout << " | ";
                std::cout << o.next_desc[j];
            }
            std::cout << "\n";
        } else {
            std::cout << "   next: (none - dead end)\n";
        }
        if (o.reached_target) std::cout << "   [reached target]\n";
        if (!o.forked_session.empty()) {
            std::cout << "   [forked] new session " << o.forked_session << "\n";
        }
    }
}

  // ---------- per-command handlers ----------

int cmd_new(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    if (rest.empty()) {
        throw std::runtime_error("usage: new <graph.json> [--id NAME]");
    }
    std::string id = opts.count("id") > 0 ? opts["id"] : "";
    Session s = store().create_from_graph(rest[0], id);
    std::cout << "session " << s.id << " created from " << rest[0]
              << " (graph " << s.graph.id << " v" << s.graph.version << ")\n";
    if (current != NULL) *current = s.id;
    return 0;
}

int cmd_open(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    if (rest.empty()) {
        throw std::runtime_error("usage: open <session-id>");
    }
    Session s = store().load(rest[0]);
    if (current != NULL) *current = s.id;
    std::cout << session_status(s, false);
    return 0;
}

int cmd_fork(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    if (rest.empty()) {
        throw std::runtime_error("usage: fork <session-id>");
    }
    Session child = store().fork(rest[0]);
    std::cout << "forked " << rest[0] << " -> " << child.id << "\n";
    if (current != NULL) *current = child.id;
    return 0;
}

int cmd_delete(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    if (rest.empty()) {
        throw std::runtime_error("usage: delete <session-id>");
    }
    store().remove(rest[0]);
    std::cout << "deleted session " << rest[0] << "\n";
    if (current != NULL && *current == rest[0]) current->clear();
    return 0;
}

int cmd_list(const std::vector<std::string>&, std::string*) {
    std::vector<Session> all = store().list();
    std::cout << "SESSION ID          PARENT   STATE    GRAPH      V  NODES  EDGES  UNEXPL\n";
    for (size_t i = 0; i < all.size(); ++i) {
        const Session& s = all[i];
        std::vector<std::string> un = s.graph.unexplored_reachable();
        std::cout << s.id
                  << "  " << (s.parent.empty() ? "-" : s.parent)
                  << "  " << s.state
                  << "  " << s.graph.id
                  << "  " << s.graph.version
                  << "  " << s.graph.nodes.size()
                  << "  " << s.graph.edges.size()
                  << "  " << un.size() << "\n";
    }
    std::cout << all.size() << " session(s)\n";
    return 0;
}

int cmd_status(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    bool as_json = flags.size() > 0 && flags[0] == "json";
    Session s = store().load(sid);
    std::cout << session_status(s, as_json);
    return 0;
}

int cmd_list_next(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    Session s = store().load(sid);
    std::string node = opts.count("node") > 0 ? opts["node"] : "";
    std::vector<std::string> lines = session_list_next(s, node);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << lines[i] << "\n";
    }
    return 0;
}

int cmd_walk(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    Session s = store().load(sid);
    std::string from = opts.count("from") > 0 ? opts["from"] : "";
    int choose = option_int(opts, "choose", 0);
    // --auto: smart auto-walk (-1 = prefer unexplored edges, else fallback, else first)
    int auto_choose = 0;
    if (std::find(flags.begin(), flags.end(), "auto") != flags.end()) {
        auto_choose = -1;
    } else if (opts.count("auto") > 0) {
        auto_choose = option_int(opts, "auto", -1);  // --auto N: fixed option index
    }
    int steps = option_int(opts, "steps", 50);
    std::string stmt = session_walk(s, from, choose, auto_choose, steps, &store());
    store().save(s);
    std::cout << stmt << "\n";
    return 0;
}

int cmd_step(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    if (pos >= rest.size()) {
        throw std::runtime_error("usage: step <session-id> <node-id>");
    }
    Session s = store().load(sid);
    std::vector<StepOutcome> outs;
    outs.push_back(session_step(s, rest[pos], &store()));
    store().save(s);
    print_outcomes(outs);
    return 0;
}

int cmd_travel(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    Session s = store().load(sid);
    std::string from = opts.count("from") > 0 ? opts["from"] : "";
    std::string target = opts.count("target") > 0 ? opts["target"] : "";
    std::vector<StepOutcome> outs = session_travel(s, from, target, &store());
    store().save(s);
    print_outcomes(outs);
    std::cout << "travel done: " << outs.size() << " node(s), state=" << s.state
              << "\n";
    return 0;
}

int cmd_set_target(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    if (pos >= rest.size()) {
        throw std::runtime_error("usage: set-target <session-id> <node-id>");
    }
    Session s = store().load(sid);
    session_set_target(s, rest[pos]);
    store().save(s);
    std::cout << "target set to " << rest[pos] << "\n";
    return 0;
}

int cmd_show(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    Session s = store().load(sid);
    std::string node = pos < rest.size() ? rest[pos] : (s.started() ? s.node : s.graph.entry);
    const Node* n = s.graph.find_node(node);
    if (n == NULL) {
        throw std::runtime_error("node '" + node + "' does not exist");
    }
    std::cout << "node    : " << n->id << " [" << n->kind << "]"
              << (n->id == s.graph.entry ? " (entry)" : "")
              << (s.started() && s.node == n->id ? " (current)" : "") << "\n";
    std::cout << "desc    : " << n->desc << "\n";
    if (!n->cmd.empty()) std::cout << "cmd     : " << n->cmd << "\n";
    if (!n->prompt.empty()) std::cout << "prompt  : " << n->prompt << "\n";
    if (!n->message.empty()) std::cout << "message : " << n->message << "\n";
    if (n->timeout_secs > 0) std::cout << "timeout : " << n->timeout_secs << "s\n";
    std::cout << "visits  : " << s.graph.visit_count(n->id) << "\n";
    std::vector<const Edge*> out = s.graph.edges_from(n->id);
    std::cout << "outgoing: " << out.size() << "\n";
    for (size_t i = 0; i < out.size(); ++i) {
        std::cout << "  -> " << out[i]->to
                  << (out[i]->label.empty() ? "" : " (" + out[i]->label + ")")
                  << (out[i]->fallback ? " [fallback]" : "")
                  << (s.graph.edge_visit_count(n->id, out[i]->to) == 0 ? " [unexplored]" : "")
                  << "\n";
    }
    std::vector<const Edge*> in = s.graph.edges_to(n->id);
    std::cout << "incoming: " << in.size() << "\n";
    for (size_t i = 0; i < in.size(); ++i) {
        std::cout << "  <- " << in[i]->from
                  << (in[i]->label.empty() ? "" : " (" + in[i]->label + ")")
                  << "\n";
    }
    return 0;
}

int cmd_insert(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    nlohmann::json j;
    if (opts.count("file") > 0) {  // read the node JSON from a file (long descs stay readable)
        std::ifstream f(opts["file"].c_str());
        if (!f) throw std::runtime_error("cannot read " + opts["file"]);
        f >> j;
    } else if (pos >= rest.size()) {
        throw std::runtime_error("usage: insert <session-id> '<node-json>' [--edge from,to]");
    } else if (rest[pos][0] == '{') {  // inline JSON form (original)
        j = nlohmann::json::parse(rest[pos]);
    } else {  // flag form: insert <sid> <id> --desc '...' [--cmd '...'] [--kind exec] [--edge from,to]
        if (pos >= rest.size()) {
            throw std::runtime_error(
                "usage: insert <session-id> '<node-json>' | <node-id> --desc '...' [--cmd '...'] [--kind kind] [--edge from,to]");
        }
        if (opts.count("desc") == 0) {
            throw std::runtime_error("flag form requires --desc '...'");
        }
        j = nlohmann::json::object();
        j["id"] = rest[pos];
        j["desc"] = opts["desc"];
        j["kind"] = opts.count("kind") > 0 ? opts["kind"] : "exec";
        if (opts.count("cmd") > 0) j["cmd"] = opts["cmd"];
        if (opts.count("prompt") > 0) j["prompt"] = opts["prompt"];
        if (opts.count("message") > 0) j["message"] = opts["message"];
    }
  // batch form: --file may hold a JSON array of nodes; each element may carry
  // "edge_from"/"edge_to" to auto-link (batch insert + edges in one save)
    if (j.is_array()) {
        Session s = store().load(sid);
        for (size_t bi = 0; bi < j.size(); ++bi) {
            Node n = j[bi].get<Node>();
            Edge e;
            bool has_edge = false;
            if (j[bi].contains("edge_from") && j[bi].contains("edge_to")) {
                e.from = j[bi]["edge_from"].get<std::string>();
                e.to = j[bi]["edge_to"].get<std::string>();
                e.fallback = j[bi].value("edge_fallback", false);
                has_edge = true;
            }
            session_insert(s, n, has_edge ? &e : NULL);
            std::cout << "inserted node " << n.id << " [" << n.kind << "]"
                      << (has_edge ? " + edge " + e.from + "->" + e.to : "")
                      << "\n";
        }
        store().save(s);
        std::cout << "batch done | graph v" << s.graph.version << " | "
                  << s.graph.nodes.size() << " nodes / " << s.graph.edges.size()
                  << " edges\n";
        return 0;
    }
    Node n = j.get<Node>();
    Edge e;
    bool has_edge = false;
    if (opts.count("edge") > 0) {
        std::string spec = opts["edge"];
        size_t comma = spec.find(',');
        if (comma == std::string::npos) {
            throw std::runtime_error("--edge must be from,to");
        }
        e.from = spec.substr(0, comma);
        e.to = spec.substr(comma + 1);
        e.fallback = false;
        has_edge = true;
    }
    Session s = store().load(sid);
    session_insert(s, n, has_edge ? &e : NULL);
    store().save(s);
    std::cout << "inserted node " << n.id << " [" << n.kind << "]"
              << (has_edge ? " + edge " + e.from + "->" + e.to : "")
              << " | graph v" << s.graph.version << "\n";
    return 0;
}

int cmd_add_edge(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    if (pos + 1 >= rest.size()) {
        throw std::runtime_error("usage: add-edge <session-id> <from> <to> [--label '...'] [--fallback]");
    }
    Edge e;
    e.from = rest[pos];
    e.to = rest[pos + 1];
    e.label = opts.count("label") > 0 ? opts["label"] : "";
    e.fallback = false;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i] == "fallback") e.fallback = true;
    }
    Session s = store().load(sid);
    session_add_edge(s, e);
    store().save(s);
    std::cout << "added edge " << e.from << " -> " << e.to
              << (e.label.empty() ? "" : " (" + e.label + ")")
              << " | graph v" << s.graph.version << "\n";
    return 0;
}

int cmd_remove_edge(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    if (pos + 1 >= rest.size()) {
        throw std::runtime_error("usage: remove-edge <session-id> <from> <to>");
    }
    Session s = store().load(sid);
    session_remove_edge(s, rest[pos], rest[pos + 1]);
    store().save(s);
    std::cout << "removed edge " << rest[pos] << " -> " << rest[pos + 1]
              << " | graph v" << s.graph.version << "\n";
    return 0;
}

int cmd_remove(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    if (pos >= rest.size()) {
        throw std::runtime_error("usage: remove <session-id> <node-id>");
    }
    Session s = store().load(sid);
    session_remove(s, rest[pos]);
    store().save(s);
    std::cout << "removed node " << rest[pos] << " | graph v"
              << s.graph.version << "\n";
    return 0;
}

int cmd_dump_svg(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    Session s = store().load(sid);
    int w = option_int(opts, "width", 960);
    int h = option_int(opts, "height", 640);
    std::string svg = session_to_svg(s, w, h);
    if (opts.count("out") > 0) {
        std::string path = opts["out"];
        FILE* f = fopen(path.c_str(), "w");
        if (f == NULL) {
            throw std::runtime_error("cannot write " + path);
        }
        fwrite(svg.data(), 1, svg.size(), f);
        fclose(f);
        std::cout << "wrote " << path << " (" << svg.size() << " bytes)\n";
    } else {
        std::cout << svg;
    }
    return 0;
}

int cmd_merge(const std::vector<std::string>& args, std::string* current) {
    (void)current;
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    if (rest.size() < 2) {
        throw std::runtime_error("usage: merge <dst-session> <src-session>");
    }
    Session dst = store().load(rest[0]);
    Session src = store().load(rest[1]);
    std::string summary = session_merge(dst, src);
    store().save(dst);
    std::cout << summary << "\n";
    return 0;
}

int cmd_rebase(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    Session child = store().load(sid);
    if (child.parent.empty()) {
        throw std::runtime_error("session '" + sid + "' is a root session (no parent to rebase from)");
    }
    Session parent = store().load(child.parent);
    std::string summary = session_merge(child, parent);  // sync parent's newest topology into the child
    store().save(child);
    std::cout << "rebased " << sid << " from parent " << child.parent << ": " << summary << "\n";
    return 0;
}

int cmd_drive(const std::vector<std::string>& args, std::string* current) {
    std::map<std::string, std::string> opts;
    std::vector<std::string> flags;
    std::vector<std::string> rest = parse_options(args, 1, opts, &flags);
    size_t pos = 0;
    std::string sid = session_id_of(rest, &pos, current);
    int max_steps = option_int(opts, "max-steps", 20);
    drive_session(store(), sid, max_steps);
    return 0;
}

}  // namespace

int cmd_dispatch(const std::vector<std::string>& args, std::string* current_session) {
    if (args.empty()) {
        std::cout << "usage: grasp <command> [args...]\n"
                     "commands: new open fork delete list status list-next step travel\n"
                     "          set-target insert remove drive repl\n"
                     "help: grasp help\n";
        return 1;
    }
    const std::string& cmd = args[0];
    try {
        if (cmd == "new") return cmd_new(args, current_session);
        if (cmd == "open") return cmd_open(args, current_session);
        if (cmd == "fork") return cmd_fork(args, current_session);
        if (cmd == "delete") return cmd_delete(args, current_session);
        if (cmd == "list") return cmd_list(args, current_session);
        if (cmd == "status") return cmd_status(args, current_session);
        if (cmd == "list-next") return cmd_list_next(args, current_session);
        if (cmd == "walk") return cmd_walk(args, current_session);
        if (cmd == "step") return cmd_step(args, current_session);
        if (cmd == "travel") return cmd_travel(args, current_session);
        if (cmd == "set-target") return cmd_set_target(args, current_session);
        if (cmd == "insert") return cmd_insert(args, current_session);
        if (cmd == "add-edge") return cmd_add_edge(args, current_session);
        if (cmd == "remove-edge") return cmd_remove_edge(args, current_session);
        if (cmd == "remove") return cmd_remove(args, current_session);
        if (cmd == "show") return cmd_show(args, current_session);
        if (cmd == "merge") return cmd_merge(args, current_session);
        if (cmd == "rebase") return cmd_rebase(args, current_session);
        if (cmd == "dump-svg") return cmd_dump_svg(args, current_session);
        if (cmd == "drive") return cmd_drive(args, current_session);
        if (cmd == "help") {
            std::cout <<
                "grasp: a deterministic external thinking tool for AI agents (an 'external lucid friend').\n"
                "\n"
                "[What this is]\n"
                "  A graph topology IS accumulate-able knowledge: nodes are natural-language state\n"
                "  descriptions (e.g. 'at home'), edges are event language (label, e.g. 'find food').\n"
                "  You stroll the topology to think; grasp joins the path descriptions into a\n"
                "  sentence and returns it. At a multi-edge node it stops and asks: 'There are N\n"
                "  options: ..., which do you choose? Or would you like to add more options?'\n"
                "  Every move (walk/step/travel) accumulates node visits and edge visits;\n"
                "  unexplored nodes (unexpl) and edges (unexplE) stay visible -- you never retrace\n"
                "  and you always know what remains unexplored. Sessions persist in sessions/\n"
                "  (atomic writes); the graph evolves with the session = your experience trail.\n"
                "\n"
                "[Build your meta session (5 steps)]\n"
                "  1. Design a thinking topology: see graphs/meta.json -- a ground-truth node (e.g.\n"
                "     'AI agent memory by weights is unreliable; thinking must derive from meta and\n"
                "     sub sessions') then orient -> analyze -> plan -> act -> reflect -> learn -> back.\n"
                "  2. Create: grasp new graphs/meta.json --id my-meta\n"
                "  3. Stroll to think: grasp walk my-meta (it asks you at multi-edge nodes)\n"
                "  4. Choose an edge: grasp walk my-meta --choose N\n"
                "     or execute a node (act): grasp step my-meta <node-id>\n"
                "  5. Persist new knowledge (the core!):\n"
                "     grasp insert my-meta '{\"id\":\"m_lesson\",\"desc\":\"lesson: ...\",\"kind\":\"exec\",\"cmd\":\"echo x\"}' --edge m_reflect,m_lesson\n"
                "     grasp add-edge my-meta m_lesson m_ground --label 'lesson persisted'\n"
                "     On the next walk the lesson resurfaces as a node/option, and you grow wiser.\n"
                "\n"
                "[Commands]\n"
                "  new <graph.json> [--id NAME]     create a session from a graph file\n"
                "  open <sid>                       open / view session status\n"
                "  fork <sid>                       fork a child session (deep-copies graph + inherits history, parent recorded)\n"
                "  delete <sid>                     delete a session\n"
                "  list                             list all sessions\n"
                "  status <sid> [--json]            detailed status: node/target/visits/unexpl(explored nodes)/\n"
                "                                   unexplE(unexplored edges)/fork chain\n"
                "  list-next <sid> [--node ID]      outgoing edges of the current (or given) node (label + target desc)\n"
                "  walk <sid> [--from ID] [--choose N] [--steps N]\n"
                "                                   topology stroll (thinking mode, executes nothing): single edges\n"
                "                                   advance automatically; multi-edge nodes stop and ask; unwalked\n"
                "                                   edges are marked [unexplored]; --choose N picks the Nth edge at start\n"
                "  step <sid> <node-id>             single step: jump to a successor of the current node and execute\n"
                "  travel <sid> [--from ID] [--target ID]\n"
                "                                   traverse along edges (BFS pathfinding toward target)\n"
                "  set-target <sid> <node-id>       set the target node (travel heads toward it)\n"
                "  insert <sid> '<node-json>' [--edge from,to]\n"
                "                                   insert a node (+optional edge), validated, version++ -- persist knowledge\n"
                "  add-edge <sid> <from> <to> [--label '...'] [--fallback]\n"
                "                                   append an edge (topology evolution)\n"
                "  remove-edge <sid> <from> <to>    remove an edge (topology cleanup)\n"
                "  remove <sid> <node-id>           remove a node and its edges (entry/current node protected)\n"
                "  show <sid> [<node-id>]            node detail: desc/cmd/kind/visits/outgoing+incoming edges\n"
                "  merge <dst-sid> <src-sid>          merge src's topology into dst (append-only union; conflicts keep dst)\n"
                "  rebase <sid>                       sync the parent session's newest topology into this session\n"
                "  dump-svg <sid> [--out FILE] [--width W] [--height H]\n"
                "                                   render the session graph as a standalone SVG\n"
                "  drive <sid> [--max-steps N]      optional: built-in LLM decision loop (or drive it yourself)\n"
                "  repl [--session sid]             interactive REPL (session id omitted after open)\n"
                "\n"
                "[Node JSON]\n"
                "  {\"id\":\"n1\",\"desc\":\"natural-language description (the text walk stitches)\",\"kind\":\"exec\",\"cmd\":\"echo hi\"}\n"
                "  kind: exec(run command) | ask(read stdin) | conclude(end session) | fork(fork a child session)\n"
                "  desc is the state description for the agent; cmd runs on execution ($ENV_VAR expanded)\n"
                "\n"
                "[Environment]\n"
                "  OPENAI_API_KEY / OPENAI_BASE_URL / OPENAI_MODEL   only needed by drive\n"
                "  GRASP_CPP_SESSIONS  session directory (default sessions/)\n"
                "\n"
                "[Example: a day of meta-session work]\n"
                "  grasp new graphs/meta.json --id my-meta\n"
                "  grasp walk my-meta                          # stroll from the ground truth\n"
                "  grasp walk my-meta --choose 2               # take the 'analyze' branch\n"
                "  grasp step my-meta m_act                    # act\n"
                "  grasp insert my-meta '{\"id\":\"m_today_lesson\",\"desc\":\"...\",\"kind\":\"exec\",\"cmd\":\"echo x\"}' --edge m_reflect,m_today_lesson\n"
                "  grasp add-edge my-meta m_today_lesson m_ground --label 'lesson persisted'\n"
                "  grasp status my-meta                        # see unexplored nodes/edges, plan next\n";
            return 0;
        }
        throw std::runtime_error("unknown command '" + cmd + "' (see help)");
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
