#ifndef GRASP_CPP_REPL_H
#define GRASP_CPP_REPL_H

// interactive REPL: remembers the session after open; later commands may omit the id.
// all commands reuse the cli handlers (same logic).

#include <string>

int run_repl(const std::string& initial_session);

#endif // GRASP_CPP_REPL_H
