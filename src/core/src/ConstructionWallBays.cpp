#include "sbr/ConstructionWall.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/Codec.h"
#include "sbr/doc/Modeler.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <map>
#include <tuple>
#include <stdexcept>
#include <BRep_Tool.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

namespace sbr::construction {
namespace {
using json = nlohmann::json;
// Every subtraction must be a known rectangular extrusion. An unrecognised
// cut must not silently become a full-length, apparently intact wall bay.
bool supportedCuts(const doc::Object& wall, const doc::Document& document, std::size_t& cuts) {
  const auto* recipe = &wall.recipe;
  gp_Trsf placement = document.worldTransform(wall);
  for (int depth = 0; depth < 64; ++depth) {
    const auto* boolean = std::get_if<doc::BooleanFeature>(&recipe->base);
    if (!boolean) {
      const auto* rect = std::get_if<doc::RectangleProfile>(&recipe->base);
      return rect && rect->plane.normal.Transformed(placement).IsParallel(gp::DZ(), 1e-6);
    }
    if (boolean->kind != doc::BooleanKind::Subtract || !recipe->modifiers.empty() ||
        boolean->operands.size() < 2 || !boolean->operands[0].recipe) return false;
    for (std::size_t i = 1; i < boolean->operands.size(); ++i) {
      const auto& operand = boolean->operands[i];
      if (!operand.recipe || operand.recipe->modifiers.size() != 1 ||
          !std::holds_alternative<doc::ExtrudeOp>(operand.recipe->modifiers[0])) return false;
      const auto* rect = std::get_if<doc::RectangleProfile>(&operand.recipe->base);
      if (!rect) return false;
      const auto world = placement * operand.placement;
      if (std::abs(rect->plane.normal.Transformed(world).Z()) > 1e-6 ||
          std::abs(rect->plane.xDir.Transformed(world).Z()) > 1e-6) return false;
      ++cuts;
    }
    placement = placement * boolean->operands[0].placement;
    recipe = boolean->operands[0].recipe.get();
  }
  return false;
}
json point(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

struct BayMember {
  std::string id, name, role, fingerprint;
  gp_Pnt a, b;
  TopoDS_Shape shape;
  double low, high;
  bool column;
};
double rounded(double v) { return std::round(v * 1e6) / 1e6; }
json roundedPoint(const gp_Pnt& p) { return {rounded(p.X()), rounded(p.Y()), rounded(p.Z())}; }
bool contains(const TopoDS_Shape& shape, const gp_Pnt& p, double tolerance) {
  for (TopExp_Explorer solid(shape, TopAbs_SOLID); solid.More(); solid.Next()) {
    BRepClass3d_SolidClassifier classifier(solid.Current(), p, tolerance);
    if (classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON) return true;
  }
  BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(p).Vertex(), shape);
  return distance.IsDone() && distance.Value() <= tolerance;
}
struct SupportScan {
  std::vector<BayMember> members;
  json unassessed = json::array();
  double tolerance;
  std::map<std::pair<std::size_t, std::size_t>, double> distances;
  explicit SupportScan(const doc::Document& d) {
    const auto patch = d.settings().constructionOverrides.find("timber");
    const auto guide = json::parse(guideData("timber", patch == d.settings().constructionOverrides.end() ? "{}" : patch->second));
    tolerance = guide.at("checks").at("tolerance_mm").get<double>();
    if (!std::isfinite(tolerance) || tolerance <= 0) throw std::runtime_error("Member detection tolerance must be positive");
    const std::set<std::string> horizontals{"sill", "beam", "girder", "rim_beam", "floor_beam", "floor_girder", "eave_beam", "roof_beam"};
    for (const auto& o : d.objects()) {
      const auto attribute = [&](const std::string& key) -> std::string {
        const auto own = o.attributes.find(key);
        if (own != o.attributes.end()) return own->second;
        const auto* instance = std::get_if<doc::ComponentInstance>(&o.recipe.base);
        const auto* definition = instance ? d.findDefinition(instance->definitionId) : nullptr;
        if (definition && definition->objects.size() == 1) {
          const auto inherited = definition->objects[0].attributes.find(key);
          if (inherited != definition->objects[0].attributes.end()) return inherited->second;
        }
        return {};
      };
      const auto role = attribute("kind");
      if (attribute("material_class") != "timber" || attribute("display_only") == "true") continue;
      const bool column = role == "column" || role == "through_column";
      if (!column && !horizontals.count(role)) continue;
      const auto expanded = frameMemberObject(d, o);
      const auto& own = expanded ? *expanded : o;
      const auto* member = std::get_if<doc::LinearMember>(&own.recipe.base);
      if (!member || !own.recipe.modifiers.empty()) {
        unassessed.push_back({{"id", o.id}, {"name", o.name}, {"reason", "edited_or_unknown_member"}});
        continue;
      }
      const auto frame = geom::memberFrame(member->start, member->end, member->up, member->roll);
      if (!frame) continue;
      const auto world = d.parentTransform(o) * own.transform;
      const gp_Vec centre = -gp_Vec(frame->XDirection()) * (member->anchor.across * member->section.width / 2) -
                           gp_Vec(frame->YDirection()) * (member->anchor.up * member->section.depth / 2);
      const auto a = member->start.Translated(centre).Transformed(world);
      const auto b = member->end.Translated(centre).Transformed(world);
      const gp_Vec delta(a, b);
      if (delta.Magnitude() <= tolerance || (column ? !gp_Dir(delta).IsParallel(gp::DZ(), 1e-6) : std::abs(gp_Dir(delta).Z()) > 1e-6)) {
        unassessed.push_back({{"id", o.id}, {"name", o.name}, {"reason", "unsupported_member_axis"}});
        continue;
      }
      auto shape = d.worldShape(o);
      if (shape.IsNull()) { unassessed.push_back({{"id", o.id}, {"name", o.name}, {"reason", "missing_shape"}}); continue; }
      double low = 1e300, high = -1e300;
      for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next()) {
        const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
        low = std::min(low, p.Z()); high = std::max(high, p.Z());
      }
      const auto fingerprint = codec::sha256Hex(json{{"recipe", frameFingerprint(own)}, {"role", role},
          {"a", roundedPoint(a)}, {"b", roundedPoint(b)}}.dump());
      members.push_back({o.id, o.name, role, fingerprint, a, b, shape, low, high, column});
    }
  }
  std::vector<std::pair<double, std::size_t>> posts(const gp_Pnt& origin, const gp_Dir& along,
                                                 double low, double high, double n0, double n1) const {
    std::vector<std::pair<double, std::size_t>> result;
    const gp_Dir normal = gp::DZ().Crossed(along);
    for (std::size_t i = 0; i < members.size(); ++i) {
      const auto& m = members[i];
      if (!m.column || std::min(high, m.high) - std::max(low, m.low) <= tolerance) continue;
      const double station = gp_Vec(origin, m.a).Dot(gp_Vec(along));
      const double across = gp_Vec(origin, m.a).Dot(gp_Vec(normal));
      auto onWall = origin.Translated(gp_Vec(along) * station + gp_Vec(normal) * std::clamp(across, n0, n1));
      onWall.SetZ((std::max(low, m.low) + std::min(high, m.high)) / 2);
      if (contains(m.shape, onWall, tolerance)) result.emplace_back(station, i);
    }
    return result;
  }
  json support(const std::vector<std::size_t>& indices, const std::vector<std::size_t>& partial = {}) const {
    json ids = json::array(), names = json::array(), partialIds = json::array(), partialNames = json::array();
    for (auto i : indices) { ids.push_back(members[i].id); names.push_back(members[i].name); }
    for (auto i : partial) { partialIds.push_back(members[i].id); partialNames.push_back(members[i].name); }
    return {{"ids", ids}, {"names", names}, {"partial_ids", partialIds}, {"partial_names", partialNames},
            {"status", indices.size() > 1 ? "ambiguous" : indices.size() == 1 ? "detected" : partial.empty() ? "missing" : "partial"}};
  }
  double distance(std::size_t a, std::size_t b) {
    const auto key = std::minmax(a, b);
    const auto found = distances.find(key);
    if (found != distances.end()) return found->second;
    BRepExtrema_DistShapeShape check(members[a].shape, members[b].shape);
    const double value = check.IsDone() ? check.Value() : -1;
    distances[key] = value;
    return value;
  }
  json frame(const gp_Pnt& start, const gp_Pnt& end, double high, double n0, double n1,
             const std::vector<std::pair<double, std::size_t>>& posts, double from, double to) {
    std::vector<std::size_t> left, right, bottom, top, partialBottom, partialTop;
    for (const auto& [at, i] : posts) {
      if (std::abs(at - from) <= tolerance) left.push_back(i);
      if (std::abs(at - to) <= tolerance) right.push_back(i);
    }
    const gp_Dir along(gp_Vec(start, end));
    const gp_Dir normal = gp::DZ().Crossed(along);
    for (std::size_t i = 0; i < members.size(); ++i) {
      const auto& m = members[i];
      if (m.column || !gp_Dir(gp_Vec(m.a, m.b)).IsParallel(along, 1e-6)) continue;
      const double a = gp_Vec(start, m.a).Dot(gp_Vec(along)), b = gp_Vec(start, m.b).Dot(gp_Vec(along));
      const double lo = std::min(a, b), hi = std::max(a, b), length = start.Distance(end);
      if (std::min(hi, length) - std::max(lo, 0.0) <= tolerance) continue;
      auto mid = start.Translated(gp_Vec(along) * ((std::max(lo, 0.0) + std::min(hi, length)) / 2));
      mid.Translate(gp_Vec(normal) * std::clamp(gp_Vec(start, m.a).Dot(gp_Vec(normal)), n0, n1));
      const bool covers = lo <= tolerance && hi >= length - tolerance;
      for (bool upper : {false, true}) {
        mid.SetZ(upper ? high : start.Z());
        if (mid.Z() < m.low - tolerance || mid.Z() > m.high + tolerance || !contains(m.shape, mid, tolerance)) continue;
        auto& list = upper ? (covers ? top : partialTop) : (covers ? bottom : partialBottom);
        list.push_back(i);
      }
    }
    json supports = {{"start_post", support(left)}, {"end_post", support(right)},
                     {"bottom", support(bottom, partialBottom)}, {"top", support(top, partialTop)}};
    bool unique = left.size() == 1 && right.size() == 1 && bottom.size() == 1 && top.size() == 1;
    std::string geometry = "incomplete";
    json contacts = json::object(), sources = json::array();
    for (const auto& list : {left, right, bottom, top, partialBottom, partialTop})
      for (auto i : list) sources.push_back({{"id", members[i].id}, {"state", members[i].fingerprint}});
    if (left.size() > 1 || right.size() > 1 || bottom.size() > 1 || top.size() > 1) geometry = "ambiguous";
    if (unique) {
      geometry = "touching";
      for (const auto& pair : std::vector<std::tuple<const char*, std::size_t, std::size_t>>{
               {"start_bottom", left[0], bottom[0]}, {"start_top", left[0], top[0]},
               {"end_bottom", right[0], bottom[0]}, {"end_top", right[0], top[0]}}) {
        const double gap = distance(std::get<1>(pair), std::get<2>(pair));
        contacts[std::get<0>(pair)] = gap < 0 ? json() : json(gap);
        if (gap < 0) geometry = "unassessed";
        else if (gap > tolerance && geometry != "unassessed") geometry = "gaps";
      }
    }
    return {{"supports", supports}, {"frame_geometry", geometry}, {"joint_distances_mm", contacts},
            {"support_signature", codec::sha256Hex(json{{"members", sources}, {"tolerance", tolerance}}.dump())}};
  }
};
}

Result<std::string> wallBayCandidates(const doc::Modeler& modeler, const std::string& options) {
  try {
    const auto opt = json::parse(options);
    if (!opt.is_object()) throw std::runtime_error("Options must be an object");
    for (const auto& [key, unused] : opt.items())
      if (key != "ids" && key != "elevation" && key != "split_at_posts") throw std::runtime_error("Unknown wall bay option: " + key);
    const bool splitAtPosts = opt.value("split_at_posts", true);
    std::set<std::string> ids;
    if (opt.contains("ids")) {
      if (!opt["ids"].is_array()) throw std::runtime_error("ids must be an array");
      for (const auto& id : opt["ids"]) ids.insert(id.get<std::string>());
    }
    std::optional<double> elevation;
    if (opt.contains("elevation")) {
      elevation = opt["elevation"].get<double>();
      if (!std::isfinite(*elevation)) throw std::runtime_error("elevation must be finite");
    }
    const auto& document = modeler.document();
    SupportScan scan(document);
    for (const auto& id : ids) if (!document.find(id)) throw std::runtime_error("Wall not found: " + id);
    json bays = json::array(), excluded = json::array();
    int hidden = 0;
    for (const auto& o : document.objects()) {
      if (opt.contains("ids") && !ids.count(o.id)) continue;
      const auto kind = o.attributes.find("kind");
      if (kind == o.attributes.end() || kind->second != "wall") continue;
      if (!document.isVisible(o.id)) { ++hidden; continue; }
      const auto reference = wallReferenceLine(o, document);
      std::size_t cuts = 0;
      const auto openings = modeler.openings(o.id);
      if (!reference || !supportedCuts(o, document, cuts) || !openings || cuts != openings->size()) {
        excluded.push_back({{"host", o.id}, {"name", o.name}, {"reason", "unsupported_wall_or_cut"}});
        continue;
      }
      const auto origin = reference->Location();
      const auto along = reference->Direction();
      const gp_Dir normal = gp::DZ().Crossed(along);
      double a0 = 1e300, a1 = -1e300, z0 = 1e300, z1 = -1e300, n0 = 1e300, n1 = -1e300;
      for (TopExp_Explorer v(document.worldShape(o), TopAbs_VERTEX); v.More(); v.Next()) {
        const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
        const double a = gp_Vec(origin, p).Dot(gp_Vec(along));
        const double n = gp_Vec(origin, p).Dot(gp_Vec(normal));
        a0 = std::min(a0, a); a1 = std::max(a1, a);
        z0 = std::min(z0, p.Z()); z1 = std::max(z1, p.Z());
        n0 = std::min(n0, n); n1 = std::max(n1, n);
      }
      if (!(a1 - a0 > tolerance::kMinDimension) || !(z1 - z0 > tolerance::kMinDimension)) continue;
      if (elevation && (*elevation < z0 - tolerance::kLinear || *elevation >= z1 - tolerance::kLinear)) continue;
      std::vector<std::pair<double, double>> gaps;
      bool known = true;
      for (const auto& opening : *openings) {
        if (std::abs(opening.normal.Z()) > 1e-6 || std::abs(opening.normal.Dot(along)) > 1e-6) {
          known = false; break;
        }
        const double c = gp_Vec(origin, opening.center).Dot(gp_Vec(along));
        gaps.emplace_back(std::clamp(c - opening.along / 2, a0, a1),
                          std::clamp(c + opening.along / 2, a0, a1));
      }
      if (!known) {
        excluded.push_back({{"host", o.id}, {"name", o.name}, {"reason", "unsupported_opening_direction"}});
        continue;
      }
      std::sort(gaps.begin(), gaps.end());
      const auto posts = scan.posts(origin, along, z0, z1, n0, n1);
      const auto at = [&](double a) { auto p = origin.Translated(gp_Vec(along) * a); p.SetZ(z0); return p; };
      const std::string signature = codec::sha256Hex(json{{"recipe", frameFingerprint(o)},
          {"start", point(at(a0))}, {"end", point(at(a1))}, {"top", z1},
          {"normal_band", {rounded(n0), rounded(n1)}}, {"gaps", gaps}}.dump());
      int index = 0;
      const auto appendBay = [&](double from, double to) {
        if (to - from <= tolerance::kMinDimension) return;
        const auto level = o.attributes.find("level");
        const std::string direction = std::abs(along.Y()) < 1e-6 ? "X" : std::abs(along.X()) < 1e-6 ? "Y" : "diagonal";
        auto frame = scan.frame(at(from), at(to), z1, n0, n1, posts, from, to);
        json bay = {{"id", o.id + ":" + std::to_string(++index)}, {"host", o.id}, {"host_name", o.name},
            {"level", level == o.attributes.end() ? json() : json(level->second)},
            {"start", point(at(from))}, {"end", point(at(to))}, {"length_mm", to - from},
            {"t0", (from - a0) / (a1 - a0)}, {"t1", (to - a0) / (a1 - a0)},
            {"geometry_height_mm", z1 - z0}, {"direction", direction},
            {"source_signature", codec::sha256Hex(signature + frame["support_signature"].get<std::string>())},
            {"status", "candidate_only"}, {"verified", false}};
        bay.update(frame);
        bays.push_back(std::move(bay));
      };
      const auto append = [&](double from, double to) {
        if (to - from <= tolerance::kMinDimension) return;
        std::vector<double> stations{from, to};
        if (splitAtPosts) for (const auto& [a, unused] : posts)
          if (a > from + scan.tolerance && a < to - scan.tolerance) stations.push_back(a);
        std::sort(stations.begin(), stations.end());
        double previous = stations.front();
        for (std::size_t i = 1; i < stations.size(); ++i) {
          if (stations[i] - previous <= scan.tolerance) continue;
          appendBay(previous, stations[i]);
          previous = stations[i];
        }
      };
      double from = a0;
      for (const auto& [left, right] : gaps) {
        append(from, left);
        from = std::max(from, right);
      }
      append(from, a1);
    }
    return json{{"version", "wall-bays-2"}, {"sequence", document.sequence()}, {"bays", bays},
                {"tolerance_mm", scan.tolerance}, {"split_at_posts", splitAtPosts}, {"unassessed_members", scan.unassessed},
                {"excluded", excluded}, {"hidden_walls_omitted", hidden},
                {"notice", "実在する柱の中心位置と開口で区切った候補です。四辺の部材・実形状の接触は幾何診断で、接合耐力・施工仕様・壁倍率は未確認。壁量へ自動計上しません。候補IDはこのスナップショット内だけで有効です。"}}.dump();
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}
}  // namespace sbr::construction
