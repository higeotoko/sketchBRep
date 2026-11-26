#include "sbr/OpeningSymbols.h"

#include <cmath>
#include <set>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
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

// docs/188: 親子開き (swing_unequal) and 折れ戸 (folding), as the owner's SketchUp extension has them.
const std::set<std::string> kTypes = {"none", "window", "swing_single", "swing_double", "swing_unequal", "folding",
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
  // Sizes of the fitting's shape (docs/157), each optional: the frame's face width, a door
  // leaf's thickness, the glass.
  struct Size {
    const char* key;
    double low, high;
  };
  static const Size sizes[] = {{"frame", 10.0, 200.0}, {"leaf", 10.0, 100.0}, {"glass", 3.0, 30.0}, {"sash", 10.0, 100.0}};
  for (const auto& [key, unused] : in.items()) {
    if (key == "type" || key == "hinge" || key == "swing" || key == "slide" || key == "fold" || key == "child") continue;
    if (key == "type_name") {
      // The registered fitting type it was made from (docs/171): a label, one line.
      const json& v = in.at(key);
      if (!v.is_string() || v.get<std::string>().empty() || v.get<std::string>().size() > 120 ||
          v.get<std::string>().find_first_of("\r\n") != std::string::npos)
        return Result<std::string>::failure("A fitting type's name is one line of at most 40 characters");
      if (type != "none") out[key] = v;
      continue;
    }
    const Size* size = nullptr;
    for (const Size& s : sizes)
      if (key == s.key) size = &s;
    if (!size) return Result<std::string>::failure("Unknown fitting field: " + key);
    const json& v = in.at(key);
    if (!v.is_number() || !(v.get<double>() >= size->low && v.get<double>() <= size->high))
      return Result<std::string>::failure(std::string("A fitting's ") + key + " is " + std::to_string(static_cast<int>(size->low)) +
                                          " to " + std::to_string(static_cast<int>(size->high)));
    if (type != "none") out[key] = v.get<double>();
  }
  if (type == "swing_single" && !(choose("hinge", "left", "right", "left") && choose("swing", "in", "out", "in")))
    return Result<std::string>::failure("hinge is left|right and swing in|out");
  if (type == "swing_double" && !choose("swing", "in", "out", "in"))
    return Result<std::string>::failure("swing is in|out");
  if (type == "sliding_single" && !choose("slide", "left", "right", "left"))
    return Result<std::string>::failure("slide is left|right");
  if (type == "swing_unequal") {
    // The parent leaf's hinge, the way both open, and the child leaf's width (docs/188).
    if (!(choose("hinge", "left", "right", "left") && choose("swing", "in", "out", "in")))
      return Result<std::string>::failure("hinge is left|right and swing in|out");
    const json child = in.value("child", json(300.0));
    if (!child.is_number() || !(child.get<double>() >= 150 && child.get<double>() <= 900))
      return Result<std::string>::failure("A child leaf (child) is 150 to 900 wide");
    out["child"] = child.get<double>();
  }
  if (type == "folding" && !(choose("fold", "left", "right", "left") && choose("swing", "in", "out", "in")))
    return Result<std::string>::failure("fold is left|right and swing in|out");
  return out.dump();
}

Result<std::string> fittingTowards(const std::string& current, const gp_Pnt& center, const gp_Dir& normal, double depth,
                                   const gp_Pnt& point) {
  const json in = json::parse(current, nullptr, false);
  if (!in.is_object()) return Result<std::string>::failure("No fitting");
  json out = in;
  const std::string type = in.value("type", std::string("none"));
  // The opening's face is on its `normal` side; the wall's middle `depth / 2` into it.
  const gp_Vec n(normal.X(), normal.Y(), 0), u = gp_Vec(0, 0, 1).Crossed(n);
  const gp_Vec to(gp_Pnt(center.X(), center.Y(), 0), gp_Pnt(point.X(), point.Y(), 0));
  const double across = to.Dot(n) + depth / 2;  // > 0: on the face side (outward)
  const double along = to.Dot(u);               // > 0: the +u half
  const bool outward = across > 0;
  // Seen from the side it opens to: from outside, right is +u; from inside, right is -u (as drawn).
  const bool rightHalf = outward ? along > 0 : along < 0;
  if (type == "swing_single" || type == "swing_unequal") {
    out["swing"] = outward ? "out" : "in";
    out["hinge"] = rightHalf ? "right" : "left";
  } else if (type == "swing_double") {
    out["swing"] = outward ? "out" : "in";
  } else if (type == "folding") {
    out["swing"] = outward ? "out" : "in";
    out["fold"] = along > 0 ? "right" : "left";  // gathers at the jamb on the side pointed at
  } else if (type == "sliding_single") {
    out["slide"] = along > 0 ? "right" : "left";  // opens towards the side pointed at
  } else {
    return Result<std::string>::failure("This fitting has no way to open to choose");
  }
  return normalizeFitting(out.dump());
}

const std::vector<FittingChoice>& fittingKinds() {
  static const std::vector<FittingChoice> kinds = {
      {"なし（開口だけ）", R"({"type":"none"})"},     {"窓", R"({"type":"window"})"},
      {"片開き", R"({"type":"swing_single"})"},       {"両開き", R"({"type":"swing_double"})"},
      {"親子開き", R"({"type":"swing_unequal"})"},    {"折れ戸", R"({"type":"folding"})"},
      {"片引き", R"({"type":"sliding_single"})"},     {"引違い", R"({"type":"sliding_double"})"},
      {"両引き", R"({"type":"sliding_bi"})"},
  };
  return kinds;
}

std::vector<FittingChoice> fittingWays(const std::string& type) {
  if (type == "swing_single" || type == "swing_unequal")
    return {{"左吊元・内開き", R"({"hinge":"left","swing":"in"})"}, {"右吊元・内開き", R"({"hinge":"right","swing":"in"})"},
            {"左吊元・外開き", R"({"hinge":"left","swing":"out"})"}, {"右吊元・外開き", R"({"hinge":"right","swing":"out"})"}};
  if (type == "swing_double") return {{"内開き", R"({"swing":"in"})"}, {"外開き", R"({"swing":"out"})"}};
  if (type == "folding")
    return {{"左へたたむ・内開き", R"({"fold":"left","swing":"in"})"}, {"右へたたむ・内開き", R"({"fold":"right","swing":"in"})"},
            {"左へたたむ・外開き", R"({"fold":"left","swing":"out"})"}, {"右へたたむ・外開き", R"({"fold":"right","swing":"out"})"}};
  if (type == "sliding_single") return {{"左へ引く", R"({"slide":"left"})"}, {"右へ引く", R"({"slide":"right"})"}};
  return {};
}

const std::vector<FittingChoice>& fittingChoices() {
  static const std::vector<FittingChoice> choices = {
      {"なし（開口だけ）", R"({"type":"none"})"},
      {"窓", R"({"type":"window"})"},
      {"片開き 左吊元・内開き", R"({"type":"swing_single","hinge":"left","swing":"in"})"},
      {"片開き 右吊元・内開き", R"({"type":"swing_single","hinge":"right","swing":"in"})"},
      {"片開き 左吊元・外開き", R"({"type":"swing_single","hinge":"left","swing":"out"})"},
      {"片開き 右吊元・外開き", R"({"type":"swing_single","hinge":"right","swing":"out"})"},
      {"両開き 内開き", R"({"type":"swing_double","swing":"in"})"},
      {"両開き 外開き", R"({"type":"swing_double","swing":"out"})"},
      {"親子開き 内開き", R"({"type":"swing_unequal","hinge":"left","swing":"in","child":300})"},
      {"親子開き 外開き", R"({"type":"swing_unequal","hinge":"left","swing":"out","child":300})"},
      {"折れ戸 内開き", R"({"type":"folding","fold":"left","swing":"in"})"},
      {"折れ戸 外開き", R"({"type":"folding","fold":"left","swing":"out"})"},
      {"片引き 左へ引く", R"({"type":"sliding_single","slide":"left"})"},
      {"片引き 右へ引く", R"({"type":"sliding_single","slide":"right"})"},
      {"引違い", R"({"type":"sliding_double"})"},
      {"両引き", R"({"type":"sliding_bi"})"},
  };
  return choices;
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
      if (!options.onlyOpening.empty() && o.id != options.onlyOpening) continue;
      if (!options.skipOpening.empty() && o.id == options.skipOpening) continue;  // docs/299
      const auto stored = fittings.find(o.id);  // the opening's id (its number for walls from before ids)
      json overridden;
      if (!options.fittingOverride.empty()) overridden = json::parse(options.fittingOverride, nullptr, false);
      const json* fit = overridden.is_object() ? &overridden : stored == fittings.end() ? nullptr : &*stored;
      if (!fit || !fit->is_object()) continue;
      const json* it = fit;
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
      } else if (type == "swing_unequal") {
        // 親子開き (docs/188): the parent leaf from its hinge, the child from the other jamb, meeting
        // where the child ends; both arcs on the side they open to.
        const bool outward = it->value("swing", std::string("in")) == "out";
        const bool hingeHigh = (it->value("hinge", std::string("left")) == "right") == outward;
        const double child = std::min(it->value("child", 300.0), w / 2 - 1.0);
        const double h = hingeHigh ? w / 2 : -w / 2, meet = hingeHigh ? -w / 2 + child : w / 2 - child;
        leaf(h, meet, w - child, outward);
        leaf(-h, meet, child, outward);
      } else if (type == "folding") {
        // 折れ戸 (docs/188, as doors.rb draws it): the closed line across, and the leaves folded to
        // the jamb they gather at, a zigzag on the side they open to.
        const bool outward = it->value("swing", std::string("in")) == "out";
        const double side = it->value("fold", std::string("left")) == "right" ? 1 : -1;  // seen from the cut side
        const double first = side * w / 2, last = -side * w / 2;
        out.push_back(line(at(-w / 2, t / 2), at(w / 2, t / 2)));
        const double faceDepth = outward ? 0.0 : t, swing = outward ? -1.0 : 1.0;  // depth grows into the wall
        const gp_Pnt a = at(first, faceDepth), b = at(first + (last - first) * 0.32, faceDepth + swing * w * 0.26),
                     c = at(first + (last - first) * 0.64, faceDepth);
        out.push_back(line(a, b));
        out.push_back(line(b, c));
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
std::vector<FittingSolid> fittingSolids(const doc::Document& d, const std::set<std::string>* only) {
  std::vector<FittingSolid> out;
  for (const auto& host : d.objects()) {
    const auto stored = host.attributes.find("opening_fittings");
    if (stored == host.attributes.end()) continue;
    if (only ? !only->count(host.id) : !d.isVisible(host.id)) continue;
    const json fittings = json::parse(stored->second, nullptr, false);
    if (!fittings.is_object()) continue;
    const auto list = doc::Modeler::openingsIn(d, host.id);
    if (!list) continue;
    for (const auto& o : *list) {
      const auto it = fittings.find(o.id);
      if (it == fittings.end() || !it->is_object()) continue;
      const std::string type = it->value("type", std::string("none"));
      if (type == "none" || std::abs(o.normal.Z()) > 1e-6 || o.depth <= 0) continue;
      // The drawing sizes (docs/146), or the fitting's own (docs/157).
      const double kFrame = it->value("frame", 30.0), kPane = it->value("glass", 6.0), kLeaf = it->value("leaf", 40.0);
      const double kSash = it->value("sash", 35.0);  // docs/174: a sliding sash's stiles and rails, on the face
      const gp_Vec n(o.normal.X(), o.normal.Y(), 0), u = gp_Vec(0, 0, 1).Crossed(n);
      const double w = o.width, h = o.height, t = o.depth, z0 = o.bottom;
      if (w <= 2 * kFrame + 10 || h <= 2 * kFrame + 10) continue;
      const bool door = type != "window" && o.sill < 1.0;
      const gp_Pnt face(o.center.X(), o.center.Y(), 0.0);
      // A box: along the wall s0..s1 from the middle, up z0..z1, into the wall d0..d1 from the face.
      auto box = [&](double s0, double s1, double za, double zb, double d0, double d1) -> TopoDS_Shape {
        BRepBuilderAPI_MakePolygon polygon;
        for (const auto& [s, dd] : {std::pair{s0, d0}, std::pair{s1, d0}, std::pair{s1, d1}, std::pair{s0, d1}}) {
          const gp_Pnt p = face.Translated(u * s - n * dd);
          polygon.Add(gp_Pnt(p.X(), p.Y(), za));
        }
        polygon.Close();
        if (!polygon.IsDone()) return TopoDS_Shape();
        BRepBuilderAPI_MakeFace f(polygon.Wire(), Standard_True);
        if (!f.IsDone()) return TopoDS_Shape();
        BRepPrimAPI_MakePrism prism(f.Face(), gp_Vec(0, 0, zb - za));
        return prism.IsDone() ? prism.Shape() : TopoDS_Shape();
      };
      auto add = [&](const TopoDS_Shape& shape, const char* part) {
        if (!shape.IsNull()) out.push_back({shape, part, host.id, o.id});
      };
      // The frame: the opening's lining, less what is inside it.
      const double iw = w - 2 * kFrame, bottom = door ? 0.0 : kFrame, ih = h - kFrame - bottom;
      const TopoDS_Shape outer = box(-w / 2, w / 2, z0, z0 + h, 0, t);
      const TopoDS_Shape inner = box(-iw / 2, iw / 2, z0 + bottom - (door ? 1.0 : 0.0), z0 + bottom + ih, -1, t + 1);
      if (!outer.IsNull() && !inner.IsNull()) {
        BRepAlgoAPI_Cut frame(outer, inner);
        if (frame.IsDone()) add(frame.Shape(), "frame");
      }
      const double zi0 = z0 + bottom, zi1 = z0 + bottom + ih, mid = t / 2;
      if (!door) {
        if (type == "sliding_double") {
          // 引違い: two sashes on two tracks, overlapping in the middle (docs/174): each a frame
          // (stiles and rails kSash wide, 30 deep) with its pane inside.
          for (const auto& [s0, s1, track] : {std::tuple{-iw / 2, 25.0, mid - 15}, std::tuple{-25.0, iw / 2, mid + 15}}) {
            if (s1 - s0 > 2 * kSash + 10 && zi1 - zi0 > 2 * kSash + 10) {
              const TopoDS_Shape sashOuter = box(s0, s1, zi0, zi1, track - 15, track + 15);
              const TopoDS_Shape sashInner = box(s0 + kSash, s1 - kSash, zi0 + kSash, zi1 - kSash, track - 16, track + 16);
              if (!sashOuter.IsNull() && !sashInner.IsNull()) {
                BRepAlgoAPI_Cut sash(sashOuter, sashInner);
                if (sash.IsDone()) add(sash.Shape(), "sash");
              }
              add(box(s0 + kSash, s1 - kSash, zi0 + kSash, zi1 - kSash, track - kPane / 2, track + kPane / 2), "glass");
            } else {
              add(box(s0, s1, zi0, zi1, track - kPane / 2, track + kPane / 2), "glass");
            }
          }
        } else {
          add(box(-iw / 2, iw / 2, zi0, zi1, mid - kPane / 2, mid + kPane / 2), "glass");
        }
        continue;
      }
      const bool outward = it->value("swing", std::string("in")) == "out";
      // docs/174: a lever handle on both faces of a hinged leaf (its face d0..d0 + kLeaf): a rose
      // 50 square and a lever 120 long pointing to the hinge, 900 above the floor, 60 from the
      // closing edge `edge` (the lever runs the way `toHinge` says).
      auto handle = [&](double edge, double toHinge, double d0) {
        const double c = edge + toHinge * 60, hz = z0 + 900;
        if (hz + 30 > zi1) return;
        for (const double side : {-1.0, 1.0}) {
          const double face = side < 0 ? d0 : d0 + kLeaf;  // the leaf's face this side
          const double out0 = face + side * 0, out1 = face + side * 50;
          add(box(c - 25, c + 25, hz - 25, hz + 25, std::min(out0, out1), std::max(out0, out1)), "handle");
          const double l0 = face + side * 40, l1 = face + side * 60;
          add(box(std::min(c, c + toHinge * 120), std::max(c, c + toHinge * 120), hz - 10, hz + 10, std::min(l0, l1), std::max(l0, l1)), "handle");
        }
      };
      if (type == "swing_single") {
        const double d0 = outward ? 0.0 : t - kLeaf;
        add(box(-iw / 2, iw / 2, zi0, zi1, d0, d0 + kLeaf), "leaf");
        // The hinge side as the plan symbol has it (seen from the side it opens to).
        const bool hingeHigh = (it->value("hinge", std::string("left")) == "right") == outward;
        handle(hingeHigh ? -iw / 2 : iw / 2, hingeHigh ? 1.0 : -1.0, d0);
      } else if (type == "swing_double") {
        const double d0 = outward ? 0.0 : t - kLeaf;
        add(box(-iw / 2, 0, zi0, zi1, d0, d0 + kLeaf), "leaf");
        add(box(0, iw / 2, zi0, zi1, d0, d0 + kLeaf), "leaf");
        handle(0, -1.0, d0);  // the left leaf's, at the meeting edge
        handle(0, 1.0, d0);   // the right leaf's
      } else if (type == "swing_unequal") {
        // 親子開き (docs/188): the parent leaf and the child leaf side by side, closed.
        const double d0 = outward ? 0.0 : t - kLeaf;
        const bool hingeHigh = (it->value("hinge", std::string("left")) == "right") == outward;
        const double child = std::min(it->value("child", 300.0), iw / 2 - 1.0);
        const double meet = hingeHigh ? -iw / 2 + child : iw / 2 - child;
        add(box(std::min(meet, hingeHigh ? iw / 2 : -iw / 2), std::max(meet, hingeHigh ? iw / 2 : -iw / 2), zi0, zi1, d0, d0 + kLeaf), "leaf");
        add(box(std::min(meet, hingeHigh ? -iw / 2 : iw / 2), std::max(meet, hingeHigh ? -iw / 2 : iw / 2), zi0, zi1, d0, d0 + kLeaf), "leaf");
        handle(meet, hingeHigh ? 1.0 : -1.0, d0);  // the parent's, at the meeting edge
      } else if (type == "folding") {
        // 折れ戸 (docs/188): four panels across, closed, in the middle of the wall.
        const double panel = iw / 4;
        for (int k = 0; k < 4; ++k) add(box(-iw / 2 + k * panel, -iw / 2 + (k + 1) * panel, zi0, zi1, mid - kLeaf / 2, mid + kLeaf / 2), "leaf");
      } else if (type == "sliding_double") {
        add(box(-iw / 2, 25, zi0, zi1, mid - kLeaf, mid), "leaf");  // two tracks, side by side
        add(box(-25, iw / 2, zi0, zi1, mid, mid + kLeaf), "leaf");
      } else if (type == "sliding_bi") {
        add(box(-iw / 2, 0, zi0, zi1, mid - kLeaf / 2, mid + kLeaf / 2), "leaf");
        add(box(0, iw / 2, zi0, zi1, mid - kLeaf / 2, mid + kLeaf / 2), "leaf");
      } else {
        add(box(-iw / 2, iw / 2, zi0, zi1, mid - kLeaf / 2, mid + kLeaf / 2), "leaf");  // 片引き and others: one leaf
      }
    }
  }
  return out;
}

}  // namespace sbr::draw
