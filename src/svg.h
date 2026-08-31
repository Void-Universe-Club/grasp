#ifndef GRASP_CPP_SVG_H
#define GRASP_CPP_SVG_H

  // Topology visualization: render a session graph as a standalone SVG
  // (zero deps, pure string generation). Layered BFS layout from the entry
  // node: same layer = same row; nodes colored by kind; edges labeled,
  // fallback edges dashed; visit counts as badges; current node highlighted.

#include <string>
#include "model.h"
#include "store.h"

  // render the session's graph to an SVG document. width/height in px.
std::string session_to_svg(const Session& s, int width = 960, int height = 640);

#endif // GRASP_CPP_SVG_H
