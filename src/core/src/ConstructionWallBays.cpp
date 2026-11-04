#include "sbr/ConstructionWall.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/Codec.h"
#include "sbr/doc/Modeler.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <BRep_Tool.hxx>
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
}

Result<std::string> wallBayCandidates(const doc::Modeler& modeler, const std::string& options) {
  try {
    const auto opt = json::parse(options);
    if (!opt.is_object()) throw std::runtime_error("Options must be an object");
    for (const auto& [key, unused] : opt.items())
      if (key != "ids" && key != "elevation") throw std::runtime_error("Unknown wall bay option: " + key);
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
      double a0 = 1e300, a1 = -1e300, z0 = 1e300, z1 = -1e300;
      for (TopExp_Explorer v(document.worldShape(o), TopAbs_VERTEX); v.More(); v.Next()) {
        const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
        const double a = gp_Vec(origin, p).Dot(gp_Vec(along));
        a0 = std::min(a0, a); a1 = std::max(a1, a);
        z0 = std::min(z0, p.Z()); z1 = std::max(z1, p.Z());
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
      const auto at = [&](double a) { auto p = origin.Translated(gp_Vec(along) * a); p.SetZ(z0); return p; };
      const std::string signature = codec::sha256Hex(json{{"recipe", frameFingerprint(o)},
          {"start", point(at(a0))}, {"end", point(at(a1))}, {"top", z1}, {"gaps", gaps}}.dump());
      int index = 0;
      const auto append = [&](double from, double to) {
        if (to - from <= tolerance::kMinDimension) return;
        const auto level = o.attributes.find("level");
        const std::string direction = std::abs(along.Y()) < 1e-6 ? "X" : std::abs(along.X()) < 1e-6 ? "Y" : "diagonal";
        bays.push_back({{"id", o.id + ":" + std::to_string(++index)}, {"host", o.id}, {"host_name", o.name},
            {"level", level == o.attributes.end() ? json() : json(level->second)},
            {"start", point(at(from))}, {"end", point(at(to))}, {"length_mm", to - from},
            {"t0", (from - a0) / (a1 - a0)}, {"t1", (to - a0) / (a1 - a0)},
            {"geometry_height_mm", z1 - z0}, {"direction", direction},
            {"source_signature", signature}, {"status", "candidate_only"}, {"verified", false}});
      };
      double from = a0;
      for (const auto& [left, right] : gaps) {
        append(from, left);
        from = std::max(from, right);
      }
      append(from, a1);
    }
    return json{{"version", "wall-bays-1"}, {"sequence", document.sequence()}, {"bays", bays},
                {"excluded", excluded}, {"hidden_walls_omitted", hidden},
                {"notice", "開口を除いた幾何的な候補です。柱・横架材・筋かい・面材・接合条件と壁倍率は未確認。壁量へ自動計上しません。候補IDはこのスナップショット内だけで有効です。"}}.dump();
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}
}  // namespace sbr::construction
