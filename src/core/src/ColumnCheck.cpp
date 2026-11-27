#include "sbr/doc/ColumnCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
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
  double x = 0, y = 0;  // docs/329: where it stands
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
    const double px = part.at("position")[0].get<double>(), py = part.at("position")[1].get<double>();
    for (std::size_t i = 0; i + 1 < stops.size(); ++i) out.push_back({role, width, depth, stops[i], stops[i + 1], px, py});
  }
  return out;
}
}  // namespace

CornerColumns cornerColumns(const Document& document) {
  CornerColumns out;
  std::set<long long> storeys;
  for (const FloorLevel& l : document.settings().levels)
    if (isFloorLevelName(l.name)) storeys.insert(std::llround(l.elevation));
  const auto parts = columnParts(document);
  if (storeys.size() < 2 || parts.empty()) return out;
  out.applies = true;
  // The distinct plan positions (to 1 mm) and whether a through column stands at each.
  std::map<std::pair<long long, long long>, bool> at;
  for (const ColumnPart& p : parts) {
    auto& through = at[{std::llround(p.x), std::llround(p.y)}];
    through = through || p.role == "through_column";
  }
  std::vector<std::pair<long long, long long>> points;
  for (const auto& [xy, through] : at) points.push_back(xy);
  // Andrew's monotone chain: the hull's corners (collinear points left out).
  std::sort(points.begin(), points.end());
  const auto cross = [](const auto& o, const auto& a, const auto& b) {
    return static_cast<double>(a.first - o.first) * static_cast<double>(b.second - o.second) -
           static_cast<double>(a.second - o.second) * static_cast<double>(b.first - o.first);
  };
  std::vector<std::pair<long long, long long>> hull;
  if (points.size() < 3) {
    hull = points;
  } else {
    for (int pass = 0; pass < 2; ++pass) {
      const std::size_t start = hull.size();
      for (const auto& p : points) {
        while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0) hull.pop_back();
        hull.push_back(p);
      }
      hull.pop_back();
      std::reverse(points.begin(), points.end());
    }
  }
  out.corners = static_cast<int>(hull.size());
  for (const auto& corner : hull) out.through += at[corner] ? 1 : 0;
  return out;
}

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
  std::vector<double> floors;
  for (const FloorLevel& l : document.settings().levels)
    if (isFloorLevelName(l.name)) floors.push_back(l.elevation);
  std::sort(floors.begin(), floors.end());
  floors.erase(std::unique(floors.begin(), floors.end(), [](double a, double b) { return std::abs(a - b) < 1.0; }), floors.end());
  // docs/323: each storey's Wd (N/m²) from the wall calculation: its cumulative weight over its area.
  std::vector<std::optional<double>> wd;
  const auto wall = nlohmann::json::parse(document.settings().wallCalculation, nullptr, false);
  if (wall.is_object() && wall.contains("floors") && wall.at("floors").is_array())
    for (const auto& f : wall.at("floors")) {
      std::optional<double> value;
      if (f.contains("weight_kn") && f.at("weight_kn").is_number() && f.contains("area_m2") && f.at("area_m2").is_number() &&
          f.at("area_m2").get<double>() > 0)
        value = f.at("weight_kn").get<double>() * 1000.0 / f.at("area_m2").get<double>();
      wd.push_back(value);
    }
  std::map<std::tuple<int, std::string, long long, long long, long long>, ColumnRow> groups;
  for (const ColumnPart& p : columnParts(document)) {
    const double distance = p.to - p.from, mid = (p.from + p.to) / 2;
    int storey = 0;
    for (double f : floors)
      if (f <= mid) ++storey;
    if (floors.empty()) storey = 0;
    else storey = std::max(storey, 1);
    const auto key = std::make_tuple(storey, p.role, std::llround(p.width), std::llround(p.depth), std::llround(distance));
    ColumnRow& row = groups[key];
    row.storey = storey;
    row.role = p.role == "through_column" ? "通し柱" : "柱";
    row.widthMm = p.width;
    row.depthMm = p.depth;
    row.distanceMm = distance;
    row.slenderness = distance / (std::min(p.width, p.depth) / std::sqrt(12.0));
    if (storey >= 1 && static_cast<std::size_t>(storey) <= wd.size() && wd[storey - 1]) {
      row.wdNm2 = wd[storey - 1];
      row.requiredMm = 0.027 * distance + 22.5 * *row.wdNm2 / distance;
    }
    ++row.count;
  }
  for (auto& [key, row] : groups) out.push_back(row);
  std::sort(out.begin(), out.end(), [](const ColumnRow& a, const ColumnRow& b) { return a.slenderness > b.slenderness; });
  return out;
}

draw::Table columnTable(const std::vector<ColumnRow>& rows) {
  draw::Table t;
  t.title = "構造詳細図（柱の小径・有効細長比）";
  t.headers = {"階", "部位", "断面 (mm)", "本数", "横架材間の垂直距離 (mm)", "小径／距離", "有効細長比", "令43条6項（150以下）",
               "Wd (N/m²)", "必要な小径 (mm)", "令43条1項"};
  for (const ColumnRow& r : rows) {
    char section[32], distance[32], ratio[32], lambda[32], wd[32] = "", need[32] = "";
    std::snprintf(section, sizeof section, "%.0f×%.0f", r.widthMm, r.depthMm);
    std::snprintf(distance, sizeof distance, "%.0f", r.distanceMm);
    std::snprintf(ratio, sizeof ratio, "1/%.1f", r.distanceMm / std::min(r.widthMm, r.depthMm));
    std::snprintf(lambda, sizeof lambda, "%.1f", r.slenderness);
    if (r.wdNm2) std::snprintf(wd, sizeof wd, "%.0f", *r.wdNm2);
    if (r.requiredMm) std::snprintf(need, sizeof need, "%.1f", *r.requiredMm);
    const std::string first = !r.requiredMm ? "未入力" : std::min(r.widthMm, r.depthMm) + 1e-9 >= *r.requiredMm ? "規定内" : "規定外";
    t.rows.push_back({r.storey > 0 ? std::to_string(r.storey) + "階" : "", r.role, section, std::to_string(r.count), distance, ratio,
                      lambda, r.slenderness <= 150.0 + 1e-9 ? "規定内" : "規定外", wd, need, first});
  }
  t.notes = {
      "木造架構の柱（take_off の column・through_column）を、断面と横架材間の垂直距離ごとにまとめました。",
      "横架材間の垂直距離は柱の長さ（通し柱は各階の床レベルの間の最も長い部分）とし、座屈長さに同じとしました。",
      "有効細長比は 座屈長さ ÷（小さい辺 ÷ √12）。令第43条第6項は 150 以下。",
      "令第43条第1項：平成12年建設省告示第1349号（令和7年4月施行の改正後）の de/l = 0.027 + 22.5·Wd/l²（積雪荷重は含まない）。"
      "式は国住指第147号（令和6年6月27日）の記載による（告示の本文そのものは未確認）。",
      "Wd は壁量計算の入力（その階が負担する重さ weight_kn ÷ その階の床面積 area_m2）から。入力が無い階は未入力。",
      "座屈を止める壁が付く向き（面内方向）は確認不要とされるが、その壁の条件は示されていないため、ここでは小さい辺で照合しています。",
      std::string("国住指第147号：") + kColumnNoticeSource,
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
  // docs/329: the corner columns of a building of two storeys or more.
  if (const auto corners = cornerColumns(document); corners.applies) {
    const std::string text = "外側の隅 " + std::to_string(corners.corners) + " か所のうち通し柱 " + std::to_string(corners.through) +
                             " か所（入隅などの「これに準ずる柱」は別途）";
    t.rows.push_back({"隅柱の通し柱", text, "令第43条第5項（接合部を通し柱と同等以上に補強した場合を除く）",
                      corners.through == corners.corners ? "規定内" : "要確認"});
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
