#include "sbr/ConstructionQuantity.h"

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

#include "sbr/ConstructionFrame.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/KernelError.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"

namespace sbr::construction {
namespace {
using namespace doc;
using json = nlohmann::json;
std::string attr(const Object& o, const std::string& key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? "" : it->second;
}
double rounded(double v) { return std::round(v * 1000) / 1000; }
json xyz(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
std::string prefix(const std::string& role) {
  if (role == "through_column") return "TC";
  if (role == "column" || role == "roof_post" || role == "floor_post") return "C";
  if (role == "joist") return "J";
  if (role == "rafter") return "R";
  if (role == "floor_sheathing") return "P";
  if (role == "stud") return "S";
  if (role == "brace" || role == "horizontal_brace") return "D";
  return "B";
}
std::string csv(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    out += c;
    if (c == '"') out += '"';
  }
  return out + '"';
}
}  // namespace
Result<std::string> takeOff(const doc::Document& d, const std::string& text) {
  try {
    const auto options = json::parse(text);
    if (!options.is_object()) throw std::runtime_error("take_off options must be an object");
    if (options.value("system", std::string("timber")) != "timber")
      throw std::runtime_error("Only timber take-off is implemented");
    const auto patch = d.settings().constructionOverrides.find("timber");
    const auto data = json::parse(
        guideData("timber", patch == d.settings().constructionOverrides.end() ? "{}" : patch->second));
    const auto& roles = data.at("roles").at("entries");
    const std::string within = options.value("within", std::string());
    if (!within.empty() && (!d.find(within) || !d.find(within)->isContainer()))
      throw std::runtime_error("within must name a group");
    std::set<std::string> tags;
    if (options.contains("tags")) {
      if (!options.at("tags").is_array()) throw std::runtime_error("tags must be an array");
      for (const auto& t : options.at("tags")) tags.insert(t.get<std::string>());
    }
    if (options.contains("stock_length")) {
      const double value = options.at("stock_length").get<double>();
      if (!std::isfinite(value) || value <= 0)
        throw std::runtime_error("stock_length must be finite and positive");
    }
    const bool visibleOnly = options.value("visible_only", false);
    json parts = json::array(), excluded = json::array();
    std::map<std::string, json> grouped;
    std::map<std::string, double> stockByFrame;
    for (const auto& o : d.objects()) {
      const auto* g = std::get_if<GroupNode>(&o.recipe.base);
      if (!g || g->constructionFrame.empty()) continue;
      const auto manifest = json::parse(g->constructionFrame, nullptr, false);
      if (manifest.is_object() && manifest.contains("inputs") &&
          manifest.at("inputs").contains("stock_length"))
        stockByFrame[o.id] = manifest.at("inputs").at("stock_length").get<double>();
    }
    int count = 0, unmeasured = 0;
    double designTotal = 0, netTotal = 0;
    for (const auto& o : d.objects()) {
      if (o.isContainer() || (!within.empty() && !d.isAncestor(within, o.id)) ||
          (!tags.empty() && !tags.count(o.layer)) || (visibleOnly && !d.isVisible(o.id)))
        continue;
      Object semantic = o;
      bool assembly = false;
      if (const auto* ref = std::get_if<ComponentInstance>(&o.recipe.base)) {
        const auto* definition = d.findDefinition(ref->definitionId);
        if (definition) {
          if (definition->objects.size() == 1 &&
              std::holds_alternative<LinearMember>(definition->objects[0].recipe.base)) {
            for (const auto* key : {"kind", "material_class", "display_only"})
              if (!semantic.attributes.count(key) && definition->objects[0].attributes.count(key))
                semantic.attributes[key] = definition->objects[0].attributes.at(key);
          } else {
            const bool containsTimber =
                std::any_of(definition->objects.begin(), definition->objects.end(),
                            [](const Object& child) { return attr(child, "material_class") == "timber"; });
            assembly = containsTimber || attr(o, "material_class") == "timber";
            if (assembly) {
              semantic.attributes["material_class"] = "timber";
              semantic.attributes["kind"] = "unclassified";
            }
          }
        }
      }
      if (attr(semantic, "display_only") == "true") {
        excluded.push_back({{"id", o.id}, {"reason_code", "display_only"}, {"role", attr(o, "kind")}});
        continue;
      }
      if (attr(semantic, "material_class") != "timber") continue;
      ++count;
      auto own = frameMemberObject(d, semantic);
      // A mirrored simple placement still has the same dimensions. Express
      // its design axis in that placement's reflected coordinates.
      gp_Trsf mirror;
      if (const auto* ref = std::get_if<ComponentInstance>(&o.recipe.base);
          ref && ref->mirrored && ref->stretches.empty()) {
        Object plain = semantic;
        std::get<ComponentInstance>(plain.recipe.base).mirrored = false;
        own = frameMemberObject(d, plain);
        mirror = componentMirror();
      }
      const Object& expanded = own ? *own : semantic;
      const auto* member = std::get_if<LinearMember>(&expanded.recipe.base);
      const bool measured = member && expanded.recipe.modifiers.empty();
      const std::string role = attr(semantic, "kind").empty() ? "unclassified" : attr(semantic, "kind");
      const std::string roleName = roles.contains(role) ? roles.at(role).at("name").get<std::string>()
                                   : role == "unclassified"
                                       ? (assembly ? "複合部品・内訳未評価" : "用途未分類")
                                       : role;
      double stock = data.at("presets").at("entries").at("stock_length").at("value");
      std::string stockBasis = "project_preset";
      for (auto parent = o.parent; !parent.empty();) {
        if (stockByFrame.count(parent)) {
          stock = stockByFrame.at(parent);
          stockBasis = "frame_input";
          break;
        }
        const auto* p = d.find(parent);
        if (!p) break;
        parent = p->parent;
      }
      if (options.contains("stock_length")) {
        stock = options.at("stock_length").get<double>();
        stockBasis = "take_off_input";
      }
      if (!std::isfinite(stock) || stock <= 0)
        throw std::runtime_error("stock_length must be finite and positive");
      json part = {{"id", o.id},
                   {"role", role},
                   {"role_name", roleName},
                   {"tag", o.layer},
                   {"count", 1},
                   {"status", measured ? "measured" : "not_evaluated"},
                   {"length_mm", nullptr},
                   {"width_mm", nullptr},
                   {"depth_mm", nullptr},
                   {"design_volume_m3", nullptr},
                   {"net_volume_m3", nullptr},
                   {"stock_length_mm", nullptr},
                   {"stock_basis", stockBasis},
                   {"section", "未評価"},
                   {"unit", assembly || role == "unclassified" ? "個"
                            : role == "floor_sheathing"        ? "枚"
                                                               : "本"}};
      const auto world = d.worldTransform(o);
      gp_Pnt at = gp::Origin();
      const auto bounds = d.worldBounds(o);
      if (!bounds.IsVoid()) {
        double x, y, z, X, Y, Z;
        bounds.Get(x, y, z, X, Y, Z);
        at = gp_Pnt((x + X) / 2, (y + Y) / 2, (z + Z) / 2);
      }
      if (!assembly && TopExp_Explorer(o.shape, TopAbs_SOLID).More()) {
        TopTools_IndexedMapOfShape solids;
        TopExp::MapShapes(d.worldShape(o), TopAbs_SOLID, solids);
        double volume = 0;
        for (int i = 1; i <= solids.Extent(); ++i) volume += std::abs(geom::volume(solids(i))) / 1e9;
        if (std::isfinite(volume) && volume > 0) {
          part["net_volume_m3"] = volume;
          netTotal += volume;
        }
      }
      std::string key = role + "/unknown/" + o.id;
      if (measured) {
        gp_Trsf pose = d.parentTransform(o) * expanded.transform;
        if (const auto* ref = std::get_if<ComponentInstance>(&o.recipe.base); ref && ref->mirrored)
          pose = world * mirror * o.transform.Inverted() * expanded.transform;
        const auto a = member->start.Transformed(pose), b = member->end.Transformed(pose);
        at = gp_Pnt((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2, (a.Z() + b.Z()) / 2);
        const double scale = std::abs(pose.ScaleFactor());
        const double length = a.Distance(b), width = member->section.width * scale,
                     depth = member->section.depth * scale;
        const auto face = geom::makeSectionFace(member->section, gp_Ax3());
        if (!face) throw std::runtime_error(face.error());
        const double volume = geom::area(*face) * scale * scale * length / 1e9;
        part["length_mm"] = length;
        part["width_mm"] = width;
        part["depth_mm"] = depth;
        part["design_volume_m3"] = volume;
        designTotal += volume;
        part["axis_start"] = xyz(a);
        part["axis_end"] = xyz(b);
        std::ostringstream section;
        section << std::setprecision(10) << width << "×" << depth;
        part["section"] = section.str();
        part["catalog"] = member->section.catalog;
        if (role != "floor_sheathing") part["stock_length_mm"] = stock;
        auto sectionKey = json::parse(doc::objectStateJson(expanded)).at("recipe").at("base").at("section");
        sectionKey["width"] = rounded(width);
        sectionKey["depth"] = rounded(depth);
        for (const auto* field : {"web", "flange", "radius"})
          if (sectionKey.contains(field))
            sectionKey[field] = rounded(sectionKey.at(field).get<double>() * scale);
        if (sectionKey.contains("loops"))
          for (auto& loop : sectionKey["loops"])
            for (auto& at : loop) {
              at[0] = rounded(at[0].get<double>() * scale);
              at[1] = rounded(at[1].get<double>() * scale);
            }
        sectionKey.erase("label");
        if (member->section.kind != geom::Section::Kind::Rectangle)
          part["section"] = geom::sectionName(member->section) + " (" + section.str() + ")";
        key = json{{"section_shape", sectionKey},
                   {"role", role},
                   {"kind", geom::toString(member->section.kind)},
                   {"catalog", member->section.catalog},
                   {"width", rounded(width)},
                   {"depth", rounded(depth)},
                   {"length", rounded(length)},
                   {"stock", role == "floor_sheathing" ? json(nullptr) : json(stock)}}
                  .dump();
      } else {
        ++unmeasured;
        part["reason_code"] =
            assembly ? "component_member_breakdown_unavailable" : "member_dimensions_unavailable_or_edited";
      }
      if (part["net_volume_m3"].is_null()) {
        part["net_reason_code"] = "closed_solid_unavailable";
      }
      part["position"] = xyz(at);
      part["group_key"] = key;
      auto& row = grouped[key];
      if (row.is_null()) {
        row = part;
        row["count"] = 0;
        row["ids"] = json::array();
        row["tags"] = json::array();
        row["total_length_mm"] = measured ? json(0.0) : json(nullptr);
        row["net_volume_m3"] = 0.0;
        row["net_volume_complete"] = true;
        row["design_volume_m3"] = measured ? json(0.0) : json(nullptr);
      }
      row["count"] = row.at("count").get<int>() + 1;
      row["ids"].push_back(o.id);
      if (std::find(row["tags"].begin(), row["tags"].end(), o.layer) == row["tags"].end())
        row["tags"].push_back(o.layer);
      if (measured)
        row["total_length_mm"] = row.at("total_length_mm").get<double>() + part.at("length_mm").get<double>();
      if (measured)
        row["design_volume_m3"] =
            row.at("design_volume_m3").get<double>() + part.at("design_volume_m3").get<double>();
      if (!part["net_volume_m3"].is_null())
        row["net_volume_m3"] = row.at("net_volume_m3").get<double>() + part.at("net_volume_m3").get<double>();
      else
        row["net_volume_complete"] = false;
      parts.push_back(std::move(part));
    }
    json rows = json::array();
    std::map<std::string, int> counters;
    std::map<std::string, std::string> marks;
    for (auto& [key, row] : grouped) {
      const auto code = prefix(row.at("role"));
      std::ostringstream mark;
      mark << code << std::setfill('0') << std::setw(2) << ++counters[code];
      row["mark"] = mark.str();
      marks[key] = mark.str();
      row["stock_equivalent"] = nullptr;
      row["stock_lower_bound"] = nullptr;
      if (!row["stock_length_mm"].is_null()) {
        const double equivalent =
            row.at("total_length_mm").get<double>() / row.at("stock_length_mm").get<double>();
        row["stock_equivalent"] = equivalent;
        row["stock_lower_bound"] = std::ceil(equivalent - 1e-9);
        row["joint_required"] =
            row.at("length_mm").get<double>() > row.at("stock_length_mm").get<double>() + tolerance::kLinear;
      }
      if (!row.at("net_volume_complete").get<bool>()) row["net_volume_m3"] = nullptr;
      row.erase("position");
      row.erase("axis_start");
      row.erase("axis_end");
      row.erase("id");
      row.erase("tag");
      row.erase("group_key");
      rows.push_back(row);
    }
    for (auto& part : parts) {
      part["mark"] = marks.at(part.at("group_key"));
      part.erase("group_key");
    }
    return json{{"system", "timber"},
                {"version", "take-off-1"},
                {"rows", rows},
                {"parts", parts},
                {"excluded", excluded},
                {"totals",
                 {{"count", count},
                  {"measured_count", count - unmeasured},
                  {"not_evaluated_count", unmeasured},
                  {"known_design_volume_m3", designTotal},
                  {"known_net_volume_m3", netTotal}}},
                {"complete", unmeasured == 0 && std::all_of(rows.begin(), rows.end(),
                                                            [](const json& r) {
                                                              return r.at("net_volume_complete").get<bool>();
                                                            })},
                {"scope",
                 {{"name", within.empty() ? "全体" : d.find(within)->name},
                  {"within", within},
                  {"tags", tags},
                  {"visible_only", visibleOnly}}},
                {"options", options},
                {"source", data.at("outputs").at("source")},
                {"normative", data.at("outputs").at("normative")},
                {"notice", data.at("notice")},
                {"quantity_notes", data.at("outputs").at("quantity_notes")}}
        .dump();
  } catch (const Standard_Failure& e) {
    return Result<std::string>::failure(kernelMessage(e));
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}
std::string takeOffCsv(const std::string& text) {
  const auto data = json::parse(text);
  std::ostringstream out;
  out << "\xef\xbb\xbf"
      << "符号,役割,断面,長さ(mm),数量,単位,材積(断面×長さ "
         "m3),実形状体積(m3),定尺(mm),定尺換算,切上げ参考本数,継手要確認,評価,タグ,対象条件,出典,版,"
         "但し書き\r\n";
  for (const auto& row : data.at("rows")) {
    std::vector<std::string> values;
    for (const auto* key :
         {"mark", "role_name", "section", "length_mm", "count", "unit", "design_volume_m3", "net_volume_m3",
          "stock_length_mm", "stock_equivalent", "stock_lower_bound", "joint_required", "status", "tags"}) {
      const auto v = row.value(key, json(nullptr));
      values.push_back(v.is_null() ? "" : v.is_string() ? v.get<std::string>() : v.dump());
    }
    values.push_back(data.at("scope").dump());
    values.push_back(data.at("source").dump());
    values.push_back(data.at("version"));
    values.push_back(data.at("notice").get<std::string>() + " " + data.at("quantity_notes").dump());
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i) out << ',';
      out << csv(values[i]);
    }
    out << "\r\n";
  }
  return out.str();
}
}  // namespace sbr::construction
