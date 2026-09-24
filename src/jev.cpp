#include "jev.h"

#include <cstdlib>
#include <algorithm>
#include <stdexcept>

#include "json.hpp"
#include "os.h"

namespace {

const char* kQid = "next-edge";
// same state cap as jev-lab/build_grasp_data.py (STATE_MAX_BYTES); keeps tokens within run_config max_length
const size_t kStateMaxBytes = 480;

std::string jev_url() {
    const char* url = getenv("GRASP_JEV_URL");
    if (url == NULL || std::string(url).empty()) {
        throw std::runtime_error("GRASP_JEV_URL not set");
    }
    return url;
}

}  // namespace

bool jev_available() {
    const char* url = getenv("GRASP_JEV_URL");
    return url != NULL && std::string(url).size() > 0;
}

std::string jev_option_text(const std::string& label, const std::string& target_id,
                            const std::string& target_desc) {
    // duel experiment 2026-09-23: options carrying ONLY the edge label make System One
    // blind to the target node's warnings (it walked straight into known traps 15x).
    // The target summary is part of the option, not optional decoration.
    std::string desc80 = os::trunc_utf8(target_desc, 80);
    if (label.empty()) {
        return desc80.empty() ? target_id : desc80;
    }
    return desc80.empty() ? label : label + "。目标节点: " + desc80;
}

double jev_min_p() {
    const char* v = getenv("GRASP_JEV_MIN_P");
    if (v == NULL || std::string(v).empty()) return 0.50;
    double p = atof(v);
    if (!(p >= 0.0 && p <= 1.0)) {
        throw std::runtime_error(std::string("GRASP_JEV_MIN_P must be 0..1, got '") + v + "'");
    }
    return p;
}

JevAnswer jev_ask_choice(const std::string& state, const std::vector<JevOption>& options) {
    if (options.size() < 2) {
        throw std::runtime_error("jev choice needs >=2 options");
    }
    nlohmann::json criteria = nlohmann::json::object();
    for (size_t i = 0; i < options.size(); ++i) {
        criteria[options[i].id] = options[i].text;
    }
    nlohmann::json question;
    question["type"] = "choice";
    question["instructions"] = JEV_INSTRUCTION;
    question["criteria"] = criteria;

    nlohmann::json state_item;
    state_item["id"] = "grasp-fork";
    state_item["state"] = os::trunc_utf8(state, kStateMaxBytes);
    state_item["questions"] = nlohmann::json{{kQid, question}};

    nlohmann::json payload;
    payload["states"] = nlohmann::json::array({state_item});

    os::TempFile tmp;
    tmp.write_all(payload.dump());

    std::string cmd = "curl -sS --max-time 30 -X POST " +
                      os::shell_quote(jev_url() + "/api/evaluate") +
                      " -H " + os::shell_quote("Content-Type: application/json") +
                      " --data-binary @" + os::shell_quote(tmp.path());
    std::string out = os::run_shell(cmd, 45);
    if (out.compare(0, 6, "ERROR:") == 0) {
        throw std::runtime_error("Jev call failed: " + out);
    }

    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(out);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Jev response parse failed: ") +
                                 e.what() + " | raw: " + out);
    }
    if (resp.contains("error")) {
        throw std::runtime_error("Jev server error: " + resp["error"].dump());
    }
    if (!resp.contains("states") || !resp["states"].is_array() ||
        resp["states"].empty() || !resp["states"][0].contains("answers") ||
        !resp["states"][0]["answers"].contains(kQid)) {
        throw std::runtime_error("Jev response missing states[0].answers['" +
                                 std::string(kQid) + "']: " + out);
    }
    const nlohmann::json& answer = resp["states"][0]["answers"][kQid];
    if (!answer.contains("choice") || !answer["choice"].is_string() ||
        !answer.contains("probabilities") || !answer["probabilities"].is_object()) {
        throw std::runtime_error("Jev answer missing choice/probabilities: " + answer.dump());
    }

    JevAnswer ans;
    ans.choice = answer["choice"].get<std::string>();
    for (nlohmann::json::const_iterator it = answer["probabilities"].begin();
         it != answer["probabilities"].end(); ++it) {
        if (!it.value().is_number()) {
            throw std::runtime_error("Jev probability not a number: " + it.value().dump());
        }
        ans.probs.push_back(std::make_pair(it.key(), it.value().get<double>()));
    }
    std::sort(ans.probs.begin(), ans.probs.end(),
              [](const std::pair<std::string, double>& a,
                 const std::pair<std::string, double>& b) {
                  return a.second > b.second;
              });
    return ans;
}
