#ifndef GRASP_CPP_CLI_H
#define GRASP_CPP_CLI_H

  // Subcommand dispatch (shared by one-shot CLI and REPL).
  // When current_session is set: commands without an explicit session id use it;
  // open/fork update it (REPL mode). When NULL (one-shot CLI), every command must pass an explicit session id.

#include <string>
#include <vector>

  // return exit code (0 = ok); exceptions are caught and printed internally
int cmd_dispatch(const std::vector<std::string>& args, std::string* current_session);

  // default session store directory (overridable via env GRASP_CPP_SESSIONS)
std::string sessions_dir();

#endif // GRASP_CPP_CLI_H
