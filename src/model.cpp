#include "model.h"

#include <algorithm>
#include <queue>
#include <set>

  // ---------- node kinds ----------

bool node_kind_valid(const std::string& kind) {
    return kind == "exec" || kind == "ask" || kind == "conclude" || kind == "fork";
}

  // ---------- Graph queries and topo accumulation ----------

const Node* Graph::find_node(const std::string& id) const {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) return &nodes[i];
    }
    return NULL;
}

std::vector<const Edge*> Graph::edges_from(const std::string& id) const {
    std::vector<const Edge*> out;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (edges[i].from == id) out.push_back(&edges[i]);
    }
    return out;
}

std::vector<const Edge*> Graph::edges_to(const std::string& id) const {
    std::vector<const Edge*> out;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (edges[i].to == id) out.push_back(&edges[i]);
    }
    return out;
}

void Graph::mark_visited(const std::string& id) {
    if (!meta.is_object()) meta = nlohmann::json::object();
    nlohmann::json& visits = meta["visits"];
    if (!visits.is_object()) visits = nlohmann::json::object();
    int n = visits.value(id, 0);
    visits[id] = n + 1;
    nlohmann::json& explored = meta["explored"];
    if (!explored.is_array()) explored = nlohmann::json::array();
    bool seen = false;
    for (size_t i = 0; i < explored.size(); ++i) {
        if (explored[i] == id) { seen = true; break; }
    }
    if (!seen) explored.push_back(id);
}

int Graph::visit_count(const std::string& id) const {
    if (!meta.is_object() || !meta.contains("visits") ||
        !meta["visits"].is_object()) {
        return 0;
    }
    return meta["visits"].value(id, 0);
}

bool Graph::is_explored(const std::string& id) const {
    if (!meta.is_object() || !meta.contains("explored") ||
        !meta["explored"].is_array()) {
        return false;
    }
    const nlohmann::json& explored = meta["explored"];
    for (size_t i = 0; i < explored.size(); ++i) {
        if (explored[i] == id) return true;
    }
    return false;
}

std::vector<std::string> Graph::reachable_ids() const {
  // BFS from entry, collect all reachable nodes
    std::set<std::string> seen;
    std::queue<std::string> q;
    if (find_node(entry) != NULL) {
        seen.insert(entry);
        q.push(entry);
    }
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        std::vector<const Edge*> es = edges_from(cur);
        for (size_t i = 0; i < es.size(); ++i) {
            if (seen.count(es[i]->to) == 0 && find_node(es[i]->to) != NULL) {
                seen.insert(es[i]->to);
                q.push(es[i]->to);
            }
        }
    }
    return std::vector<std::string>(seen.begin(), seen.end());
}

std::vector<std::string> Graph::unexplored_reachable() const {
    std::vector<std::string> reach = reachable_ids();
    std::vector<std::string> out;
    for (size_t i = 0; i < reach.size(); ++i) {
        if (visit_count(reach[i]) == 0) out.push_back(reach[i]);
    }
    return out;
}

  // ---------- edge-visit accumulation (evidence for not retracing paths) ----------

void Graph::mark_edge_visited(const std::string& from, const std::string& to) {
    if (!meta.is_object()) meta = nlohmann::json::object();
    nlohmann::json& ev = meta["edge_visits"];
    if (!ev.is_object()) ev = nlohmann::json::object();
    std::string key = from + "->" + to;
    ev[key] = ev.value(key, 0) + 1;
}

int Graph::edge_visit_count(const std::string& from, const std::string& to) const {
    if (!meta.is_object() || !meta.contains("edge_visits") ||
        !meta["edge_visits"].is_object()) {
        return 0;
    }
    return meta["edge_visits"].value(from + "->" + to, 0);
}

std::vector<std::string> Graph::unexplored_edges() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (edge_visit_count(edges[i].from, edges[i].to) == 0) {
            out.push_back(edges[i].from + "->" + edges[i].to);
        }
    }
    return out;
}

  // ---------- validation ----------

std::string validate_graph(const Graph& g) {
    if (g.find_node(g.entry) == NULL) {
        return "entry node '" + g.entry + "' does not exist";
    }
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        for (size_t j = i + 1; j < g.nodes.size(); ++j) {
            if (g.nodes[i].id == g.nodes[j].id) {
                return "duplicate node id '" + g.nodes[i].id + "'";
            }
        }
    }
    for (size_t i = 0; i < g.edges.size(); ++i) {
        const Edge& e = g.edges[i];
        if (g.find_node(e.from) == NULL) {
            return "edge from '" + e.from + "' references missing node";
        }
        if (g.find_node(e.to) == NULL) {
            return "edge to '" + e.to + "' references missing node";
        }
    }
    return "";
}

std::string validate_new_node(const Graph& g, const Node& n) {
    if (n.id.empty()) return "node id is empty";
    if (!node_kind_valid(n.kind)) {
        return "invalid node kind '" + n.kind + "' (expected exec|ask|conclude|fork)";
    }
    if (g.find_node(n.id) != NULL) {
        return "node id '" + n.id + "' already exists";
    }
    return "";
}

std::string validate_new_edge(const Graph& g, const Edge& e) {
    if (g.find_node(e.from) == NULL) {
        return "edge from '" + e.from + "' references missing node";
    }
    if (g.find_node(e.to) == NULL) {
        return "edge to '" + e.to + "' references missing node";
    }
    return "";
}

  // ---------- JSON serialization ----------

void to_json(nlohmann::json& j, const Node& n) {
    j = nlohmann::json::object();
    j["id"] = n.id;
    j["desc"] = n.desc;
    j["kind"] = n.kind;
    if (!n.cmd.empty()) j["cmd"] = n.cmd;
    if (!n.prompt.empty()) j["prompt"] = n.prompt;
    if (!n.message.empty()) j["message"] = n.message;
    if (n.timeout_secs > 0) j["timeout_secs"] = n.timeout_secs;
}

void from_json(const nlohmann::json& j, Node& n) {
    n.id = j.value("id", "");
    n.desc = j.value("desc", "");
    n.kind = j.value("kind", "exec");
    n.cmd = j.value("cmd", "");
    n.prompt = j.value("prompt", "");
    n.message = j.value("message", "");
    n.timeout_secs = j.value("timeout_secs", 0L);
}

void to_json(nlohmann::json& j, const Edge& e) {
    j = nlohmann::json::object();
    j["from"] = e.from;
    j["to"] = e.to;
    if (!e.label.empty()) j["label"] = e.label;
    if (e.fallback) j["fallback"] = true;
}

void from_json(const nlohmann::json& j, Edge& e) {
    e.from = j.value("from", "");
    e.to = j.value("to", "");
    e.label = j.value("label", "");
    e.fallback = j.value("fallback", false);
}

void to_json(nlohmann::json& j, const Graph& g) {
    j = nlohmann::json::object();
    j["id"] = g.id;
    j["version"] = g.version;
    j["entry"] = g.entry;
    j["nodes"] = g.nodes;
    j["edges"] = g.edges;
    if (g.meta.is_object()) {
        j["meta"] = g.meta;
    }
}

void from_json(const nlohmann::json& j, Graph& g) {
    g.id = j.value("id", "");
    g.version = j.value("version", 1L);
    g.entry = j.value("entry", "");
    g.nodes = j.value("nodes", std::vector<Node>());
    g.edges = j.value("edges", std::vector<Edge>());
    if (j.contains("meta") && j["meta"].is_object()) {
        g.meta = j["meta"];
    } else {
        g.meta = nlohmann::json::object();
    }
}
