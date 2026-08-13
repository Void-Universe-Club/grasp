#ifndef GRASP_CPP_LLM_H
#define GRASP_CPP_LLM_H

  // LLM client: curl subprocess calling an OpenAI Chat Completions compatible endpoint (zero deps).
  // config from env: OPENAI_API_KEY / OPENAI_BASE_URL / OPENAI_MODEL.
  // the API key never lands in argv (expanded by sh from $VAR in the curl command line), avoiding ps leaks.

#include <string>

struct LlmConfig {
    std::string api_key;
    std::string base_url;  // default https://api.openai.com/v1
    std::string model;  // default gpt-4o-mini
};

  // read config from env; return false when OPENAI_API_KEY is missing
bool llm_config_from_env(LlmConfig& cfg);

  // call chat completions and return the assistant content text.
  // throw std::runtime_error on network failure / HTTP error / parse failure.
std::string llm_chat(const LlmConfig& cfg, const std::string& system,
                     const std::string& user);

  // extract the JSON body from a reply (strip ```json fences and surrounding prose)
std::string extract_json(const std::string& text);

#endif // GRASP_CPP_LLM_H
