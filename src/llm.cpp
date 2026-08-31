#include "llm.h"

#include <cstdlib>
#include <stdexcept>

#include "json.hpp"
#include "os.h"

bool llm_config_from_env(LlmConfig& cfg) {
    const char* key = getenv("OPENAI_API_KEY");
    if (key == NULL || std::string(key).empty()) return false;
    cfg.api_key = key;
    const char* base = getenv("OPENAI_BASE_URL");
    cfg.base_url = (base != NULL && std::string(base).size() > 0) ? base
                                                                  : "https://api.openai.com/v1";
    const char* model = getenv("OPENAI_MODEL");
    cfg.model = (model != NULL && std::string(model).size() > 0) ? model
                                                                 : "gpt-4o-mini";
    return true;
}

std::string llm_chat(const LlmConfig& cfg, const std::string& system,
                     const std::string& user) {
    if (cfg.api_key.empty()) {
        throw std::runtime_error("OPENAI_API_KEY not set");
    }
    nlohmann::json payload;
    payload["model"] = cfg.model;
    payload["messages"] = nlohmann::json::array();
    payload["messages"].push_back(
        nlohmann::json{{"role", "system"}, {"content", system}});
    payload["messages"].push_back(
        nlohmann::json{{"role", "user"}, {"content", user}});
    payload["temperature"] = 0.2;
  // reasoning models (e.g. deepseek-v4-flash) have low default output caps; give room for the decision JSON
    payload["max_tokens"] = 2048;

    os::TempFile tmp;
    tmp.write_all(payload.dump());

  // API key expanded by the shell via %VAR%/$VAR (double quotes guarantee expansion),
  // never lands in argv: ps sees the literal reference, the shell substitutes the real key
    std::string cmd = "curl -sS --max-time 120 -X POST " +
                      os::shell_quote(cfg.base_url + "/chat/completions") +
                      " -H " + os::shell_quote("Content-Type: application/json") +
                      " -H \"Authorization: Bearer " +
                      os::env_var_ref("OPENAI_API_KEY") + "\"" +
                      " --data-binary @" + os::shell_quote(tmp.path());

    std::string out = os::run_shell(cmd, 150);
    if (out.compare(0, 6, "ERROR:") == 0) {
        throw std::runtime_error("LLM call failed: " + out);
    }
  // parse the response; HTTP-level errors (401/400 etc.) carry an error field in the body
    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(out);
    } catch (const std::exception& e) {
        throw std::runtime_error("LLM response parse failed: " + std::string(e.what()) +
                                 " | raw: " + out);
    }
    if (resp.contains("error")) {
        throw std::runtime_error("LLM API error: " + resp["error"].dump());
    }
    if (!resp.contains("choices") || resp["choices"].empty() ||
        !resp["choices"][0].contains("message") ||
        !resp["choices"][0]["message"].contains("content")) {
        throw std::runtime_error("LLM response missing choices[0].message.content: " + out);
    }
    return resp["choices"][0]["message"]["content"].get<std::string>();
}

std::string extract_json(const std::string& text) {
  // prefer an object inside a ```json ... ``` fence
    size_t fence = text.find("```");
    if (fence != std::string::npos) {
        size_t start = text.find('{', fence);
        size_t end = text.rfind('}');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            return text.substr(start, end - start + 1);
        }
    }
    size_t start = text.find('{');
    size_t end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        return text.substr(start, end - start + 1);
    }
    return text;
}
