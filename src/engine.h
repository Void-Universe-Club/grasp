#ifndef GRASP_CPP_ENGINE_H
#define GRASP_CPP_ENGINE_H

  // Node execution engine: fork/exec + pipe read + timeout kill (RAII-managed fds and child).
  // exec node output = merged stdout+stderr text; a non-zero exit appends an ERROR note (no throw).

#include <string>

  // run a shell command and return the merged output; kill and throw std::runtime_error after timeout_secs.
  // default timeout 60s (when timeout_secs <= 0).
std::string run_shell(const std::string& cmd, long timeout_secs);

  // current timestamp (milliseconds)
long now_ms();

#endif // GRASP_CPP_ENGINE_H
