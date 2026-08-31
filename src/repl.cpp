#include "repl.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli.h"
#include "store.h"

namespace {

// split a command line: support double/single-quoted args (insert JSON often contains spaces)
std::vector<std::string> split_args(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            in_token = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (in_token) {
                out.push_back(cur);
                cur.clear();
                in_token = false;
            }
            continue;
        }
        cur += c;
        in_token = true;
    }
    if (in_token || quote != 0) out.push_back(cur);
    return out;
}

void print_help() {
    std::cout << "REPL commands (session id may be omitted after open):\n"
                 "  open <sid> | status [--json] | list | list-next [--node ID]\n"
                 "  step <node-id> | travel [--from ID] [--target ID] | set-target <node-id>\n"
                 "  insert '<node-json>' [--edge from,to] | remove <node-id>\n"
                 "  fork | delete <sid> | drive [--max-steps N]\n"
                 "  history [N] | help | exit\n";
}

void print_history(const Session& s, int n) {
    size_t start = n > 0 && (size_t)n < s.history.size() ? s.history.size() - (size_t)n : 0;
    for (size_t i = start; i < s.history.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << s.history[i].node << " ["
                  << s.history[i].kind << "] -> \"" << s.history[i].output
                  << "\"\n";
    }
}

}  // namespace

// REPL convention: after open, operation commands omit the session id; REPL injects the current one;
// fork/delete without an id act on the current session. Use the one-shot CLI for other ids.
// commands needing sid injection (sid is always the first positional arg)
static bool cmd_takes_sid(const std::string& cmd) {
    return cmd == "status" || cmd == "list-next" || cmd == "walk" ||
           cmd == "step" || cmd == "travel" || cmd == "set-target" ||
           cmd == "insert" || cmd == "remove" || cmd == "drive";
}
// commands whose only arg is sid
static bool cmd_is_sid_only(const std::string& cmd) {
    return cmd == "fork" || cmd == "delete";
}

// inject the current session id into the args
static void inject_session(std::vector<std::string>& args,
                           const std::string& current) {
    if (current.empty()) return;
    if (cmd_takes_sid(args[0])) {
        args.insert(args.begin() + 1, current);
    } else if (cmd_is_sid_only(args[0]) && args.size() < 2) {
        args.insert(args.begin() + 1, current);
    }
}

int run_repl(const std::string& initial_session) {
    std::string current = initial_session;
    if (!current.empty()) {
        try {
            SessionStore sstore(sessions_dir());
            Session s = sstore.load(current);
            std::cout << "opened session " << current << " (" << s.graph.id
                      << " v" << s.graph.version << ", state=" << s.state << "）\n";
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            current.clear();
        }
    }
    std::cout << "grasp REPL (help for commands, exit to leave)\n";
    std::string line;
    for (;;) {
        std::cout << (current.empty() ? "grasp> " : "grasp[" + current + "]> ")
                  << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }
        std::vector<std::string> args = split_args(line);
        if (args.empty()) continue;
        std::string& cmd = args[0];

        if (cmd == "exit" || cmd == "quit") break;
        if (cmd == "help") {
            print_help();
            continue;
        }
        if (cmd == "history") {
            if (current.empty()) {
                std::cerr << "error: open <sid> first\n";
                continue;
            }
            int n = args.size() > 1 ? atoi(args[1].c_str()) : 20;
            try {
                SessionStore sstore(sessions_dir());
                print_history(sstore.load(current), n);
            } catch (const std::exception& e) {
                std::cerr << "error: " << e.what() << "\n";
            }
            continue;
        }
        inject_session(args, current);
        cmd_dispatch(args, &current);
    }
    return 0;
}
