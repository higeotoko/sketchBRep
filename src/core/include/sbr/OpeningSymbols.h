#pragma once
#include <set>
#include <string>
#include <vector>

#include <TopoDS_Edge.hxx>

#include "sbr/doc/Document.h"

namespace sbr::draw {
// Door / window fittings of wall openings, drawn as plan symbols (view and drawings).
// A fitting is {"type": T, ...}:
//   none            plain opening, no symbol (removes a fitting)
//   window          glass lines inside the wall
//   swing_single    hinged door: "hinge" left|right, "swing" in|out
//   swing_double    pair of hinged leaves meeting in the middle: "swing" in|out
//   sliding_single  片引き: one leaf, "slide" left|right (the side it opens to)
//   sliding_double  引違い: two leaves on two tracks
//   sliding_bi      両引き: two leaves opening to both sides
// "out" is the side the opening was cut from (its face normal), "in" the other side.
// left / right are as seen from the side the door opens to (swing), or from the cut side
// (sliding). Symbols are drawn to the opening's real width and wall thickness; their
// leaf thickness (30 mm), track gap (20 mm), overlaps (25 / 50 mm) and glass gap (30 mm)
// are drawing conventions, not product dimensions.
Result<std::string> normalizeFitting(const std::string& fitting);

struct SymbolOptions {
  double z = 0.0;                       // plan height (the cut): openings it passes through
  const std::set<std::string>* only = nullptr;  // host ids to draw (all when null)
};
// World edges (lines and arcs at height z) for every fitted opening the plane z passes through.
std::vector<TopoDS_Edge> openingSymbols(const doc::Document& document, const SymbolOptions& options);
}  // namespace sbr::draw
