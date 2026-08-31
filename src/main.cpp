#include <iostream>
#include <string>
#include <vector>

#include "cli.h"
#include "os.h"
#include "repl.h"

  // shared entry: argv already UTF-8 (Windows wmain converts from UTF-16)
static int run(const std::vector<std::string>& args) {
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

#ifdef _WIN32
  // Windows: MSVC main() decodes argv with the system ACP (GBK on zh-CN), so Chinese
  // args from PowerShell/Node arrive as GBK bytes and break UTF-8 JSON parsing.
  // wmain + CommandLineToArgvW gives the true UTF-16 command line; os::wargv_to_utf8
  // converts it to UTF-8.
int wmain(int argc, wchar_t** wargv) {
    return run(os::wargv_to_utf8(argc, wargv));
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    return run(args);
}
#endif
