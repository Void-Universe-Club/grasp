#ifndef GRASP_CPP_STORE_H
#define GRASP_CPP_STORE_H

  // SessionStore: persistence management over the sessions/ directory (RAII).
  // Each session is one JSON file (full graph snapshot + runtime state);
  // writes use tmp + rename atomic commit, so a crash never leaves a half-written file.

#include <string>
#include <vector>
#include "model.h"

  // a record of one executed step (raw material of meta-learning feedback and topo accumulation)
struct StepRecord {
    std::string node;
    std::string kind;
    std::string output;
    long at_ms;
};

  // Session: one run over a graph.
struct Session {
    std::string id;
    std::string parent;  // source session id of a fork (empty = root session)
    Graph graph;  // full topology (mutated at runtime, persisted with the session = topo accumulation)
    std::string node;  // current node (empty = not started)
    std::string target;  // target set by set-target (empty = none)
    std::string state;       // running | waiting | done | error
    std::string last_output;
    std::vector<StepRecord> history;

    bool started() const { return !node.empty(); }
};

void to_json(nlohmann::json& j, const StepRecord& s);
void from_json(const nlohmann::json& j, StepRecord& s);
void to_json(nlohmann::json& j, const Session& s);
void from_json(const nlohmann::json& j, Session& s);

  // directory management: creates the dir on construction; load/save/remove/list all target that dir.
class SessionStore {
public:
    explicit SessionStore(const std::string& dir);

  // create a session from a graph file; auto-generate id when id is empty
    Session create_from_graph(const std::string& graph_file, const std::string& id);
  // fork: deep-copy the graph, parent = original id, new session starts at entry (runtime state cleared)
    Session fork(const std::string& id);
    Session load(const std::string& id) const;  // throw std::runtime_error when missing
    void save(const Session& s) const;  // atomic write
    void remove(const std::string& id) const;
    std::vector<Session> list() const;  // all sessions (sorted by id)

    std::string dir() const { return dir_; }
    static std::string make_id();

private:
    std::string dir_;
    std::string path_of(const std::string& id) const;
};

#endif // GRASP_CPP_STORE_H
