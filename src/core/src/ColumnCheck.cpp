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

}  // namespace sbr::doc
