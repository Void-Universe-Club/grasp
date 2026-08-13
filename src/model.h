#ifndef GRASP_CPP_MODEL_H
#define GRASP_CPP_MODEL_H

  // Data model: graph (knowledge topology) + nodes + edges.
  // Core philosophy mirrors grasp: the graph itself is persistable, modifiable knowledge;
  // the meta field accumulates visit stats (visits / explored), the carrier of meta-learning (topo accumulation).

#include <string>
#include <vector>
#include "json.hpp"

  // a node: one step in the graph. desc is the description visible to the LLM.
struct Node {
    std::string id;
    std::string desc;
    std::string kind;       // exec | ask | conclude | fork
    std::string cmd;  // shell command run when kind == exec
    std::string prompt;  // question printed to the human when kind == ask
    std::string message;  // closing message when kind == conclude
    long timeout_secs;  // timeout for kind == exec (0 = default 60s)
};

// edge: a from -> to transition. label is the event description (e.g. "find food"/"sleep"),
// the option language an agent strolls with; fallback is the default exit when the LLM makes no choice (last in list).
struct Edge {
    std::string from;
    std::string to;
    std::string label;   // event description (optional; used as option text during walk)
    bool fallback;
};

// Graph: the topology itself. meta accumulates visit stats（{"visits": {...}, "explored": [...]}）。
struct Graph {
    std::string id;
    long version;
    std::string entry;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    nlohmann::json meta;

    const Node* find_node(const std::string& id) const;
    std::vector<const Edge*> edges_from(const std::string& id) const;
    std::vector<const Edge*> edges_to(const std::string& id) const;
    // visit accumulation: visits++ and record into explored
    void mark_visited(const std::string& id);
    int visit_count(const std::string& id) const;
    bool is_explored(const std::string& id) const;
    // edge-visit accumulation: walked edges go into meta["edge_visits"] (evidence for not retracing)
    void mark_edge_visited(const std::string& from, const std::string& to);
    int edge_visit_count(const std::string& from, const std::string& to) const;
    // edges reachable from entry and never walked, as "from->to" list
    std::vector<std::string> unexplored_edges() const;
    // nodes reachable from entry and never visited (LLM exploration targets)
    std::vector<std::string> unexplored_reachable() const;
    std::vector<std::string> reachable_ids() const;
};

// whether a node kind is legal
bool node_kind_valid(const std::string& kind);

// validation (3 rules): entry exists / node ids unique / edge references valid.
// return empty string on success, else the reason.
std::string validate_graph(const Graph& g);

// insert validation: new node id not duplicate, kind legal
std::string validate_new_node(const Graph& g, const Node& n);
// insert --edge validation: from / to must exist
std::string validate_new_edge(const Graph& g, const Edge& e);

// JSON serialization (shared by graph file and session snapshot)
void to_json(nlohmann::json& j, const Node& n);
void from_json(const nlohmann::json& j, Node& n);
void to_json(nlohmann::json& j, const Edge& e);
void from_json(const nlohmann::json& j, Edge& e);
void to_json(nlohmann::json& j, const Graph& g);
void from_json(const nlohmann::json& j, Graph& g);

#endif // GRASP_CPP_MODEL_H
