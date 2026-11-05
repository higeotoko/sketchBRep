#include "sbr/OpeningSymbols.h"

#include <cmath>
#include <set>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <gp_Vec.hxx>
#include <nlohmann/json.hpp>

#include "sbr/doc/Modeler.h"

namespace sbr::draw {
namespace {
using json = nlohmann::json;
constexpr double kLeaf = 30;     // drawn leaf thickness of sliding doors
constexpr double kTrack = 20;    // half the gap between the two tracks of 引違い
constexpr double kOverlap = 25;  // a sliding leaf runs past the jamb / the other leaf
constexpr double kGlass = 15;    // half the gap between the two glass lines of a window

const std::set<std::string> kTypes = {"none", "window", "swing_single", "swing_double",
                                      "sliding_single", "sliding_double", "sliding_bi"};

TopoDS_Edge line(const gp_Pnt& a, const gp_Pnt& b) { return BRepBuilderAPI_MakeEdge(a, b).Edge(); }
}  // namespace

Result<std::string> normalizeFitting(const std::string& text) {
  const json in = json::parse(text, nullptr, false);
  if (!in.is_object() || !in.contains("type") || !in.at("type").is_string())
    return Result<std::string>::failure("fitting needs {\"type\": ...}");
  const std::string type = in.at("type");
  if (!kTypes.count(type)) return Result<std::string>::failure("Unknown fitting type: " + type);
  json out = {{"type", type}};
  auto choose = [&](const char* key, const char* a, const char* b, const char* fallback) -> bool {
    const std::string v = in.value(key, std::string(fallback));
    if (v != a && v != b) return false;
    out[key] = v;
    return true;
  };
  for (const auto& [key, unused] : in.items())
    if (key != "type" && key != "hinge" && key != "swing" && key != "slide")
      return Result<std::string>::failure("Unknown fitting field: " + key);
  if (type == "swing_single" && !(choose("hinge", "left", "right", "left") && choose("swing", "in", "out", "in")))
    return Result<std::string>::failure("hinge is left|right and swing in|out");
  if (type == "swing_double" && !choose("swing", "in", "out", "in"))
    return Result<std::string>::failure("swing is in|out");
  if (type == "sliding_single" && !choose("slide", "left", "right", "left"))
    return Result<std::string>::failure("slide is left|right");
  return out.dump();
}

std::vector<TopoDS_Edge> openingSymbols(const doc::Document& d, const SymbolOptions& options) {
  std::vector<TopoDS_Edge> out;
  for (const auto& host : d.objects()) {
    const auto stored = host.attributes.find("opening_fittings");
    if (stored == host.attributes.end()) continue;
    if (options.only ? !options.only->count(host.id) : !d.isVisible(host.id)) continue;
    const json fittings = json::parse(stored->second, nullptr, false);
    if (!fittings.is_object()) continue;
    const auto list = doc::Modeler::openingsIn(d, host.id);
    if (!list) continue;
    for (const auto& o : *list) {
      const auto it = fittings.find(std::to_string(o.index));
      if (it == fittings.end() || !it->is_object()) continue;
      const std::string type = it->value("type", std::string("none"));
      // Only openings the plan cut passes through (a high window above the cut is not drawn).
      if (!(o.bottom < options.z && options.z < o.bottom + o.height)) continue;
      if (std::abs(o.normal.Z()) > 1e-6 || o.depth <= 0) continue;
      const gp_Vec n(o.normal.X(), o.normal.Y(), 0), u = gp_Vec(0, 0, 1).Crossed(n);
      const double w = o.width, t = o.depth;
      const gp_Pnt face(o.center.X(), o.center.Y(), options.z);  // middle of the opening on its face
      // A point at `s` along the wall from the middle, `depth` into the wall from the face.
      auto at = [&](double s, double depth) { return face.Translated(u * s - n * depth); };
      auto box = [&](double s0, double s1, double depth, double thick) {
        const gp_Pnt a = at(s0, depth - thick / 2), b = at(s1, depth - thick / 2), c = at(s1, depth + thick / 2),
                     e = at(s0, depth + thick / 2);
        out.push_back(line(a, b)); out.push_back(line(b, c)); out.push_back(line(c, e)); out.push_back(line(e, a));
      };
      // A small arrow along the wall (the way a sliding leaf opens).
      auto arrow = [&](double s, double depth, double sign) {
        const double len = std::min(w / 4, 300.0), head = std::min(len / 2, 80.0);
        const gp_Pnt tail = at(s - sign * len / 2, depth), tip = at(s + sign * len / 2, depth);
        out.push_back(line(tail, tip));
        out.push_back(line(tip, at(s + sign * (len / 2 - head), depth - head * 0.5)));
        out.push_back(line(tip, at(s + sign * (len / 2 - head), depth + head * 0.5)));
      };
      // A hinged leaf: from the hinge, square to the wall, with its quarter arc to the closing jamb.
      auto leaf = [&](double hingeS, double closeS, double radius, bool outward) {
        const double faceDepth = outward ? 0.0 : t;
        const gp_Pnt hinge = at(hingeS, faceDepth), close = at(closeS, faceDepth);
        const gp_Vec swing = outward ? n : -n;
        const gp_Pnt open = hinge.Translated(swing * radius);
        out.push_back(line(hinge, open));
        gp_Vec toClose(hinge, close);
        toClose.Normalize();
        gp_Vec mid = toClose + swing.Normalized();
        mid.Normalize();
        const Handle(Geom_TrimmedCurve) arc = GC_MakeArcOfCircle(open, hinge.Translated(mid * radius), close).Value();
        out.push_back(BRepBuilderAPI_MakeEdge(arc).Edge());
      };
      if (type == "window") {
        out.push_back(line(at(-w / 2, t / 2 - kGlass), at(w / 2, t / 2 - kGlass)));
        out.push_back(line(at(-w / 2, t / 2 + kGlass), at(w / 2, t / 2 + kGlass)));
      } else if (type == "swing_single") {
        const bool outward = it->value("swing", std::string("in")) == "out";
        // Seen from the side it opens to: from outside, right is +u; from inside, right is -u.
        const bool hingeHigh = (it->value("hinge", std::string("left")) == "right") == outward;
        leaf(hingeHigh ? w / 2 : -w / 2, hingeHigh ? -w / 2 : w / 2, w, outward);
      } else if (type == "swing_double") {
        const bool outward = it->value("swing", std::string("in")) == "out";
        leaf(-w / 2, 0, w / 2, outward);
        leaf(w / 2, 0, w / 2, outward);
      } else if (type == "sliding_single") {
        // Seen from the cut side (+n): right is +u.
        const double sign = it->value("slide", std::string("left")) == "right" ? 1 : -1;
        box(-w / 2 - kOverlap, w / 2 + kOverlap, t / 2, kLeaf);
        arrow(0, t / 2, sign);
      } else if (type == "sliding_double") {
        box(-w / 2 - kOverlap, kOverlap, t / 2 - kTrack, kLeaf);
        box(-kOverlap, w / 2 + kOverlap, t / 2 + kTrack, kLeaf);
        arrow(-w / 4, t / 2 - kTrack, 1);
        arrow(w / 4, t / 2 + kTrack, -1);
      } else if (type == "sliding_bi") {
        box(-w / 2 - kOverlap, 0, t / 2, kLeaf);
        box(0, w / 2 + kOverlap, t / 2, kLeaf);
        arrow(-w / 4, t / 2, -1);
        arrow(w / 4, t / 2, 1);
      }
    }
  }
  return out;
}
}  // namespace sbr::draw
