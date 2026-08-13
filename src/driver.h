#ifndef GRASP_CPP_DRIVER_H
#define GRASP_CPP_DRIVER_H

  // drive meta-learning loop: LLM reads the session topo (current/successors/history/unexplored)
  // -> outputs a JSON decision (step/travel/set_target/insert/fork/done) -> framework executes -> feedback,
  // looping until the LLM decides done or the step budget runs out. Outcomes and visit stats accumulate in the session graph.

#include <string>
#include "store.h"

  // run the drive loop on a session; max_steps is the LLM decision-round budget.
  // return the rounds executed; throw std::runtime_error when the LLM is unconfigured or the loop fails.
int drive_session(SessionStore& store, const std::string& session_id, int max_steps);

#endif // GRASP_CPP_DRIVER_H
