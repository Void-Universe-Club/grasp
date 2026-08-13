#include <iostream>
#include <string>
#include <vector>

#include "cli.h"
#include "repl.h"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    if (args.empty()) {
        std::cout << "grasp: lightweight LLM-based agent framework (graph topo accumulation + meta-learning)\n\n";
        return cmd_dispatch(args, NULL);
    }
    if (args[0] == "repl") {
        std::string initial;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--session" && i + 1 < args.size()) {
                initial = args[i + 1];
            }
        }
        return run_repl(initial);
    }
    return cmd_dispatch(args, NULL);
}
