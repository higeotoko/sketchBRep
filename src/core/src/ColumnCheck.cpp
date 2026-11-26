#include "sbr/doc/ColumnCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include <nlohmann/json.hpp>

#include "sbr/ConstructionQuantity.h"
#include "sbr/doc/Document.h"
#include "sbr/doc/Schedules.h"

namespace sbr::doc {

namespace {
// Each column part: role, sides, from and to (Z, mm); a through column split at the floor levels.
struct ColumnPart {
  std::string role;
  double width, depth, from, to;
};
std::vector<ColumnPart> columnParts(const Document& document) {
  std::vector<ColumnPart> out;
  const auto made = construction::takeOff(document);
  if (!made) return out;
  const auto report = nlohmann::json::parse(*made, nullptr, false);
  if (!report.is_object() || !report.contains("parts")) return out;
  std::vector<double> floors;
  for (const FloorLevel& l : document.settings().levels)
    if (isFloorLevelName(l.name)) floors.push_back(l.elevation);
  std::sort(floors.begin(), floors.end());
  for (const auto& part : report.at("parts")) {
    const std::string role = part.value("role", std::string());
    if (role != "column" && role != "through_column") continue;
    if (!part.at("length_mm").is_number() || !part.at("width_mm").is_number() || !part.at("depth_mm").is_number()) continue;
    if (!part.contains("position") || !part.at("position").is_array()) continue;
    const double length = part.at("length_mm").get<double>();
    const double width = part.at("width_mm").get<double>(), depth = part.at("depth_mm").get<double>();
    if (!(length > 0 && width > 0 && depth > 0)) continue;
    const double mid = part.at("position")[2].get<double>();
    std::vector<double> stops{mid - length / 2};
    if (role == "through_column")
      for (double f : floors)
        if (f > mid - length / 2 + 1 && f < mid + length / 2 - 1) stops.push_back(f);
    stops.push_back(mid + length / 2);
    for (std::size_t i = 0; i + 1 < stops.size(); ++i) out.push_back({role, width, depth, stops[i], stops[i + 1]});
  }
  return out;
}
}  // namespace

std::optional<double> clearDistanceIn(const Document& document, double z0, double z1) {
  std::optional<double> out;
  for (const ColumnPart& p : columnParts(document)) {
    const double mid = (p.from + p.to) / 2;
    if (mid < z0 || mid >= z1) continue;
    out = std::max(out.value_or(0.0), p.to - p.from);
  }
  return out;
}

std::vector<ColumnRow> columnRows(const Document& document) {
  std::vector<ColumnRow> out;
  const auto made = construction::takeOff(document);
  if (!made) return out;
  const auto report = nlohmann::json::parse(*made, nullptr, false);
  if (!report.is_object() || !report.contains("parts")) return out;
  // The storeys' floor levels: a through column is held at each (its parts between them).
  std::vector<double> floors;
  for (const FloorLevel& l : document.settings().levels)
    if (isFloorLevelName(l.name)) floors.push_back(l.elevation);
  std::sort(floors.begin(), floors.end());
  std::map<std::tuple<std::string, long long, long long, long long>, ColumnRow> groups;
  for (const auto& part : report.at("parts")) {
    const std::string role = part.value("role", std::string());
    if (role != "column" && role != "through_column") continue;
    if (!part.at("length_mm").is_number() || !part.at("width_mm").is_number() || !part.at("depth_mm").is_number()) continue;
    const double length = part.at("length_mm").get<double>();
    const double width = part.at("width_mm").get<double>(), depth = part.at("depth_mm").get<double>();
    if (!(length > 0 && width > 0 && depth > 0)) continue;
    double distance = length;
    if (role == "through_column" && part.contains("position") && part.at("position").is_array()) {
      const double mid = part.at("position")[2].get<double>();
      std::vector<double> stops{mid - length / 2};
      for (double f : floors)
        if (f > mid - length / 2 + 1 && f < mid + length / 2 - 1) stops.push_back(f);
      stops.push_back(mid + length / 2);
      distance = 0;
      for (std::size_t i = 0; i + 1 < stops.size(); ++i) distance = std::max(distance, stops[i + 1] - stops[i]);
    }
    const auto key = std::make_tuple(role, std::llround(width), std::llround(depth), std::llround(distance));
    ColumnRow& row = groups[key];
    row.role = role == "through_column" ? "通し柱" : "柱";
    row.widthMm = width;
    row.depthMm = depth;
    row.distanceMm = distance;
    row.slenderness = distance / (std::min(width, depth) / std::sqrt(12.0));
    ++row.count;
  }
  for (auto& [key, row] : groups) out.push_back(row);
  std::sort(out.begin(), out.end(), [](const ColumnRow& a, const ColumnRow& b) { return a.slenderness > b.slenderness; });
  return out;
}

draw::Table columnTable(const std::vector<ColumnRow>& rows) {
  draw::Table t;
  t.title = "構造詳細図（柱の小径・有効細長比）";
  t.headers = {"部位", "断面 (mm)", "本数", "横架材間の垂直距離 (mm)", "小径／距離", "有効細長比", "令43条6項（150以下）",
               "令43条1項（告示の割合）"};
  for (const ColumnRow& r : rows) {
    char section[32], distance[32], ratio[32], lambda[32];
    std::snprintf(section, sizeof section, "%.0f×%.0f", r.widthMm, r.depthMm);
    std::snprintf(distance, sizeof distance, "%.0f", r.distanceMm);
    std::snprintf(ratio, sizeof ratio, "1/%.1f", r.distanceMm / std::min(r.widthMm, r.depthMm));
    std::snprintf(lambda, sizeof lambda, "%.1f", r.slenderness);
    t.rows.push_back({r.role, section, std::to_string(r.count), distance, ratio, lambda,
                      r.slenderness <= 150.0 + 1e-9 ? "規定内" : "規定外", "未評価"});
  }
  t.notes = {
      "木造架構の柱（take_off の column・through_column）を、断面と横架材間の垂直距離ごとにまとめました。",
      "横架材間の垂直距離は柱の長さ（通し柱は各階の床レベルの間の最も長い部分）とし、座屈長さに同じとしました。",
      "有効細長比は 座屈長さ ÷（小さい辺 ÷ √12）。令第43条第6項は 150 以下。",
      "令第43条第1項の小径の割合は国土交通大臣が定める割合（告示）によるため、ここでは照合していません（未評価）。",
      std::string("令第43条の本文：") + kColumnArticleSource + "（" + kColumnArticleCheckedOn + " 確認）",
  };
  return t;
}

StructuralSpecs structuralSpecsOf(const Document& document) {
  StructuralSpecs s;
  const auto j = nlohmann::json::parse(document.settings().structuralSpecs, nullptr, false);
  if (!j.is_object()) return s;
  s.roofing = j.value("roofing", std::string());
  s.frameMethod = j.value("frame_method", std::string());
  s.joints = j.value("joints", std::string());
  s.wallBase = j.value("wall_base", std::string());
  s.decay = j.value("decay", std::string());
  s.source = j.value("source", std::string());
  return s;
}

draw::Table structuralSpecTable(const Document& document) {
  const StructuralSpecs s = structuralSpecsOf(document);
  draw::Table t;
  t.title = "構造詳細図（仕様）";
  t.headers = {"事項（規則第1条の3 表2）", "内容", "条文", "状態"};
  const auto given = [&](const char* item, const std::string& value, const char* article) {
    t.rows.push_back({item, value.empty() ? "未入力" : value, article, value.empty() ? "未入力" : "入力済み"});
  };
  given("屋根ふき材の種別", s.roofing, "令第39条");
  const auto columns = columnRows(document);
  if (columns.empty()) {
    t.rows.push_back({"柱の有効細長比", "モデルに木造の柱がありません", "令第43条第6項", "未入力"});
  } else {
    double worst = 0;
    for (const auto& c : columns) worst = std::max(worst, c.slenderness);
    char text[96];
    std::snprintf(text, sizeof text, "最大 %.1f（構造詳細図（柱の小径・有効細長比）による）", worst);
    t.rows.push_back({"柱の有効細長比", text, "令第43条第6項（150以下）", worst <= 150.0 + 1e-9 ? "規定内" : "規定外"});
  }
  given("構造耐力上主要な部分である軸組等の構造方法", s.frameMethod, "令第46条");
  given("構造耐力上主要な部分である継手又は仕口の構造方法", s.joints, "令第47条");
  given("外壁のうち、軸組が腐りやすい構造である部分の下地", s.wallBase, "令第49条第1項");
  given("地面から1m以内の部分の防腐又は防蟻措置", s.decay, "令第49条第2項");
  t.notes = {
      "規則第1条の3 表2（令第3章第3節が適用される建築物）が構造詳細図に求める事項です。モデルに無い事項は入力された内容です。",
      std::string("資料：") + (s.source.empty() ? "未入力" : s.source),
      std::string("条文：") + kColumnArticleSource + "（" + kColumnArticleCheckedOn + " 確認）",
  };
  return t;
}

}  // namespace sbr::doc
