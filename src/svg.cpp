#include "svg.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <sstream>
#include <vector>

namespace {

  // XML-escape text for embedding into the SVG document
std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        switch (s[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += s[i];
        }
    }
    return out;
}

  // wrap text into lines of at most max_chars UTF-8 characters (break at spaces when possible).
  // operates on character boundaries so multi-byte text (CJK) is never cut mid-character.
std::vector<std::string> wrap(const std::string& text, size_t max_chars) {
    // split the input into UTF-8 characters (ASCII 1 byte; leading byte 0xC0+ determines the rest)
    std::vector<std::string> chars;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t n = 1;
        if (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        chars.push_back(text.substr(i, n));
        i += n;
    }
    std::vector<std::string> lines;
    std::vector<std::string> cur;
    size_t last_space = std::string::npos;  // index into cur (char positions)
    for (size_t i = 0; i < chars.size(); ++i) {
        const std::string& ch = chars[i];
        bool space = (ch == " " || ch == "\n");
        if (space) last_space = cur.size();
        if (ch == "\n" || cur.size() >= max_chars) {
            if (ch != "\n" && last_space != std::string::npos && last_space > 0 &&
                last_space < cur.size()) {
                std::string line;
                for (size_t k = 0; k < last_space; ++k) line += cur[k];
                lines.push_back(line);
                std::vector<std::string> rest(cur.begin() + last_space + 1, cur.end());
                cur = rest;
            } else {
                std::string line;
                for (size_t k = 0; k < cur.size(); ++k) line += cur[k];
                lines.push_back(line);
                cur.clear();
            }
            last_space = std::string::npos;
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) {
        std::string line;
        for (size_t k = 0; k < cur.size(); ++k) line += cur[k];
        lines.push_back(line);
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

  // kind -> fill color (human-readable: color tells the node role at a glance)
const char* kind_color(const std::string& kind) {
    if (kind == "ask") return "#d29922";       // orange: waits for input
    if (kind == "conclude") return "#3fb950";  // green: terminal
    if (kind == "fork") return "#bc8cff";      // purple: branches a session
    return "#2a7de1";                          // blue: executes a command
}

}  // namespace

std::string session_to_svg(const Session& s, int width, int height) {
    const Graph& g = s.graph;

  // ---------- layered layout: BFS from entry ----------
    std::map<std::string, int> layer;
    std::vector<std::string> order;  // nodes in discovery order (stable x per layer)
    std::map<std::string, size_t> idx;
    std::queue<std::string> q;
    const Node* entry = g.find_node(g.entry);
    std::string start = entry ? g.entry : (g.nodes.empty() ? "" : g.nodes[0].id);
    if (!start.empty()) {
        layer[start] = 0;
        q.push(start);
        order.push_back(start);
        idx[start] = 0;
    }
    size_t ni = 1;
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        std::vector<const Edge*> es = g.edges_from(cur);
        std::sort(es.begin(), es.end(),
                  [](const Edge* a, const Edge* b) { return a->to < b->to; });
        for (size_t i = 0; i < es.size(); ++i) {
            const std::string& to = es[i]->to;
            if (layer.count(to) > 0) continue;  // already placed
            layer[to] = layer[cur] + 1;
            idx[to] = ni++;
            order.push_back(to);
            q.push(to);
        }
    }
  // nodes not reachable from entry: append as their own layer (orphans)
    int max_layer = 0;
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const std::string& id = g.nodes[i].id;
        if (layer.count(id) == 0) {
            layer[id] = 0;
            if (idx.count(id) == 0) idx[id] = ni++;
            order.push_back(id);
        }
        if (layer[id] > max_layer) max_layer = layer[id];
    }
    int layers = max_layer + 1;

  // per-layer x positions: same layer shares one row, x spreads evenly
    std::map<std::string, double> px, py;
    const double margin = 70.0;
    const double node_w = 190.0;
    const double node_h = 78.0;
    const double row_h = 150.0;
    const double usable_w = width - 2 * margin;
    std::map<int, int> layer_count;
    std::map<int, int> layer_pos;
    for (size_t i = 0; i < order.size(); ++i) layer_count[layer[order[i]]]++;
    for (size_t i = 0; i < order.size(); ++i) {
        int l = layer[order[i]];
        int c = layer_count[l];
        int p = layer_pos[l]++;
        px[order[i]] = margin + (c == 1 ? usable_w / 2
                                        : (p + 0.5) * usable_w / c);
        py[order[i]] = margin + l * row_h;
    }
    double content_h = margin * 2 + (layers - 1) * row_h + node_h;
    if (content_h > height) height = static_cast<int>(content_h) + 10;

  // ---------- assemble the document ----------
    std::stringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
        << "\" height=\"" << height << "\" viewBox=\"0 0 " << width << " "
        << height << "\" font-family=\"-apple-system,Segoe UI,Noto Sans,sans-serif\">\n";
    svg << "  <rect width=\"100%\" height=\"100%\" fill=\"#0d1117\"/>\n";
    svg << "  <text x=\"" << margin << "\" y=\"30\" fill=\"#e6edf3\" font-size=\"15\" "
        << "font-weight=\"600\">session: " << esc(s.id)
        << "  ·  graph: " << esc(g.id) << " v" << g.version
        << "  ·  " << g.nodes.size() << " nodes / " << g.edges.size()
        << " edges  ·  state: " << esc(s.state) << "</text>\n";

  // edges first (under the nodes)
    for (size_t i = 0; i < g.edges.size(); ++i) {
        const Edge& e = g.edges[i];
        if (px.count(e.from) == 0 || px.count(e.to) == 0) continue;
        double x1 = px[e.from] + node_w / 2;
        double y1 = py[e.from] + node_h;
        double x2 = px[e.to] + node_w / 2;
        double y2 = py[e.to];
        double mx = (x1 + x2) / 2;
        double my = (y1 + y2) / 2;
        double ctrl = std::max(20.0, std::abs(x2 - x1) * 0.25);
        svg << "  <path d=\"M " << x1 << " " << y1 << " C " << x1 << " "
            << (y1 + ctrl) << ", " << x2 << " " << (y2 - ctrl) << ", " << x2
            << " " << y2 << "\" fill=\"none\" stroke=\"#8b949e\" stroke-width=\"1.6\""
            << (e.fallback ? " stroke-dasharray=\"5,4\"" : "")
            << " opacity=\"0.75\"/>\n";
        if (!e.label.empty()) {
            svg << "  <text x=\"" << mx << "\" y=\"" << (my - 4)
                << "\" fill=\"#8b949e\" font-size=\"11\" text-anchor=\"middle\""
                << " font-style=\"italic\">" << esc(e.label) << "</text>\n";
        }
    }

  // nodes
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const Node& n = g.nodes[i];
        if (px.count(n.id) == 0) continue;
        double x = px[n.id];
        double y = py[n.id];
        bool current = s.started() && s.node == n.id;
        std::string fill = kind_color(n.kind);
        std::string head = n.id;
        if (n.id == g.entry) head += " (entry)";
        if (current) head += "  ◀ current";
        std::vector<std::string> body = wrap(n.desc, 26);
        if (body.size() > 3) {
            body.resize(3);
            body[2] += "…";
        }
        svg << "  <rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << node_w
            << "\" height=\"" << node_h << "\" rx=\"10\" fill=\"#161b22\""
            << " stroke=\"" << fill << "\" stroke-width=\"" << (current ? 3.2 : 1.8)
            << "\"/>\n";
        svg << "  <text x=\"" << (x + 10) << "\" y=\"" << (y + 18)
            << "\" fill=\"" << fill << "\" font-size=\"12.5\" font-weight=\"700\">"
            << esc(head) << "</text>\n";
        double ty = y + 38;
        for (size_t k = 0; k < body.size(); ++k) {
            svg << "  <text x=\"" << (x + 10) << "\" y=\"" << ty
                << "\" fill=\"#c9d1d9\" font-size=\"10.5\">" << esc(body[k])
                << "</text>\n";
            ty += 13;
        }
        int visits = g.visit_count(n.id);
        if (visits > 0) {
            svg << "  <circle cx=\"" << (x + node_w - 14) << "\" cy=\"" << (y + 14)
                << "\" r=\"9\" fill=\"" << fill << "\"/>\n";
            svg << "  <text x=\"" << (x + node_w - 14) << "\" y=\"" << (y + 17.5)
                << "\" fill=\"#0d1117\" font-size=\"10\" font-weight=\"700\""
                << " text-anchor=\"middle\">" << visits << "</text>\n";
        }
    }

  // legend
    double lx = margin;
    double ly = height - 26;
    svg << "  <rect x=\"" << (lx - 10) << "\" y=\"" << (ly - 16)
        << "\" width=\"" << (width - 2 * margin + 20) << "\" height=\"30\" rx=\"8\""
        << " fill=\"#161b22\" stroke=\"#2d333b\"/>\n";
    struct Leg { const char* kind; const char* color; };
    Leg legs[] = {{"exec", "#2a7de1"}, {"ask", "#d29922"},
                  {"conclude", "#3fb950"}, {"fork", "#bc8cff"}};
    double ox = lx;
    for (size_t i = 0; i < 4; ++i) {
        svg << "  <rect x=\"" << ox << "\" y=\"" << (ly - 10) << "\" width=\"16\""
            << " height=\"10\" rx=\"3\" fill=\"" << legs[i].color << "\"/>\n";
        svg << "  <text x=\"" << (ox + 22) << "\" y=\"" << ly << "\" fill=\"#8b949e\""
            << " font-size=\"12\">" << legs[i].kind << "</text>\n";
        ox += 22 + 11 * 1 + 56;
    }
    svg << "  <text x=\"" << (ox + 4) << "\" y=\"" << ly << "\" fill=\"#8b949e\""
        << " font-size=\"12\">— dashed edge: fallback · badge: visit count</text>\n";

    svg << "</svg>\n";
    return svg.str();
}
