#ifndef GRASP_CPP_JEV_H
#define GRASP_CPP_JEV_H

  // System-One edge picker: posts a NanoJev "choice" question to a local
  // /api/evaluate server (curl subprocess, zero deps, no API key — localhost only).
  // config from env: GRASP_JEV_URL (e.g. http://127.0.0.1:8765),
  //                  GRASP_JEV_MIN_P (drive trust threshold, default 0.50).

#include <string>
#include <utility>
#include <vector>

  // the ONE canonical instruction; training data must use the exact same text
#define JEV_INSTRUCTION \
  "You are the grasp agent standing at the graph node described in the state above. " \
  "To advance the current task most efficiently, which edge should you follow next?"

struct JevOption {
    std::string id;    // option key = target node id
    std::string text;  // option semantics = edge label + target desc
};

struct JevAnswer {
    std::string choice;  // chosen option id
    std::vector<std::pair<std::string, double> > probs;  // sorted desc
    double top_prob() const { return probs.empty() ? 0.0 : probs[0].second; }
};

  // canonical option text: edge label + "。目标节点: " + target desc(80B); if label empty,
  // desc alone; if both empty, target id. Training-data builders must reuse this exact rule.
std::string jev_option_text(const std::string& label, const std::string& target_id,
                            const std::string& target_desc);

  // true when GRASP_JEV_URL is set and non-empty
bool jev_available();

  // drive trust threshold from GRASP_JEV_MIN_P (default 0.50)
double jev_min_p();

  // POST one choice question (state + options) and return the probability ranking.
  // throws std::runtime_error on transport / schema / parse failure (never silent).
JevAnswer jev_ask_choice(const std::string& state,
                         const std::vector<JevOption>& options);

#endif // GRASP_CPP_JEV_H
