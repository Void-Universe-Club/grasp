#include "store.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "engine.h"

  // ---------- JSON serialization ----------

void to_json(nlohmann::json& j, const StepRecord& s) {
    j = nlohmann::json::object();
    j["node"] = s.node;
    j["kind"] = s.kind;
    j["output"] = s.output;
    j["at_ms"] = s.at_ms;
}

void from_json(const nlohmann::json& j, StepRecord& s) {
    s.node = j.value("node", "");
    s.kind = j.value("kind", "");
    s.output = j.value("output", "");
    s.at_ms = j.value("at_ms", 0L);
}

void to_json(nlohmann::json& j, const Session& s) {
    j = nlohmann::json::object();
    j["id"] = s.id;
    j["parent"] = s.parent;
    j["graph"] = s.graph;
    j["node"] = s.node;
    j["target"] = s.target;
    j["state"] = s.state;
    j["last_output"] = s.last_output;
    j["history"] = s.history;
}

void from_json(const nlohmann::json& j, Session& s) {
    s.id = j.value("id", "");
    s.parent = j.value("parent", "");
    s.graph = j.value("graph", Graph());
    s.node = j.value("node", "");
    s.target = j.value("target", "");
    s.state = j.value("state", "running");
    s.last_output = j.value("last_output", "");
    s.history = j.value("history", std::vector<StepRecord>());
}

  // ---------- utilities ----------

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void ensure_dir(const std::string& dir) {
    if (mkdir(dir.c_str(), 0755) != 0 && !file_exists(dir)) {
        throw std::runtime_error("cannot create directory '" + dir + "'");
    }
}

// ---------- SessionStore ----------

SessionStore::SessionStore(const std::string& dir) : dir_(dir) {
    ensure_dir(dir_);
}

std::string SessionStore::make_id() {
    static long seq = 0;
    ++seq;
    return "s-" + std::to_string(now_ms()) + "-" + std::to_string(seq);
}

std::string SessionStore::path_of(const std::string& id) const {
    if (id.empty()) throw std::runtime_error("empty session id");
  // allow only conventional id characters, preventing path traversal
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            throw std::runtime_error("invalid session id '" + id + "'");
        }
    }
    return dir_ + "/" + id + ".json";
}

Session SessionStore::create_from_graph(const std::string& graph_file,
                                        const std::string& id) {
    std::ifstream in(graph_file.c_str());
    if (!in.is_open()) {
        throw std::runtime_error("cannot open graph file '" + graph_file + "'");
    }
    std::stringstream buf;
    buf << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(buf.str());
    Graph g = j.get<Graph>();
    std::string err = validate_graph(g);
    if (!err.empty()) {
        throw std::runtime_error("graph invalid: " + err);
    }
    Session s;
    s.id = id.empty() ? make_id() : id;
    s.parent = "";
    s.graph = g;
    s.node = "";
    s.target = "";
    s.state = "running";
    s.last_output = "";
    save(s);
    return s;
}

Session SessionStore::fork(const std::string& id) {
    Session src = load(id);
    Session child;
    child.id = make_id();
    child.parent = src.id;
    child.graph = src.graph;  // deep-copy the topology (including meta visit accumulation)
    child.node = "";  // runtime position restarts from entry
    child.target = "";
    child.state = "running";
    child.last_output = "";
    child.history = src.history;  // inherit the parent conversation history (context survives the fork)
    save(child);
    return child;
}

Session SessionStore::load(const std::string& id) const {
    std::string path = path_of(id);
    std::ifstream in(path.c_str());
    if (!in.is_open()) {
        throw std::runtime_error("session '" + id + "' not found (looked at " + path + ")");
    }
    std::stringstream buf;
    buf << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(buf.str());
    return j.get<Session>();
}

void SessionStore::save(const Session& s) const {
    std::string path = path_of(s.id);
    std::string tmp = path + ".tmp";
    nlohmann::json j = s;
    std::ofstream out(tmp.c_str());
    if (!out.is_open()) {
        throw std::runtime_error("cannot write session file '" + tmp + "'");
    }
    out << j.dump(2);
    out.close();
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        throw std::runtime_error("cannot commit session file '" + path + "'");
    }
}

void SessionStore::remove(const std::string& id) const {
    std::string path = path_of(id);
    if (::remove(path.c_str()) != 0) {
        throw std::runtime_error("cannot delete session '" + id + "'");
    }
}

std::vector<Session> SessionStore::list() const {
    std::vector<Session> out;
    DIR* d = opendir(dir_.c_str());
    if (d == NULL) return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        std::string name = ent->d_name;
  // collect only *.json and skip .tmp
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
        if (name.size() > 4 && name.substr(name.size() - 9) == ".json.tmp") continue;
        std::string id = name.substr(0, name.size() - 5);
        try {
            out.push_back(load(id));
        } catch (const std::exception&) {
  // corrupted session files are skipped without affecting the rest
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const Session& a, const Session& b) { return a.id < b.id; });
    return out;
}
