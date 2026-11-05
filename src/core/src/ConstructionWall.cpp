#include "sbr/ConstructionWall.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sbr::embedded {
extern const char* const kTimber_wall_tables[];
extern const unsigned kTimber_wall_tables_parts;
}  // namespace sbr::embedded
namespace sbr::construction {
namespace {
using json = nlohmann::json;
constexpr const char* kVersion = "wall-quantity-1";
void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error("壁量: " + message);
}
void keys(const json& value, const std::set<std::string>& allowed) {
  require(value.is_object(), "objectが必要です");
  for (const auto& [key, unused] : value.items()) {
    (void)unused;
    require(allowed.count(key) != 0, "不明な項目: " + key);
  }
}
void text(const json& value, const char* key) {
  require(value.contains(key) && value.at(key).is_string() && !value.at(key).get<std::string>().empty(),
          std::string(key) + "が必要です");
}
bool number(const json& value, const char* key, bool zero = false) {
  if (!value.contains(key) || value.at(key).is_null()) return false;
  const auto& n = value.at(key);
  require(n.is_number() && std::isfinite(n.get<double>()) && n.get<double>() <= 1e12 &&
              (zero ? n.get<double>() >= 0 : n.get<double>() >= 1e-9),
          std::string(key) + "は有限の" + (zero ? "非負" : "正") + "の値が必要です");
  return true;
}
bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
long long day(const std::string& s) {
  require(s.size() == 10 && s[4] == '-' && s[7] == '-', "日付はYYYY-MM-DDで指定してください");
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) require(s[i] >= '0' && s[i] <= '9', "日付が不正です");
  const int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  require(y >= 1900 && m >= 1 && m <= 12 && d >= 1 && d <= days[m - 1] + (m == 2 && leap(y)),
          "日付が不正です");
  long long result = 0;
  for (int a = 1900; a < y; ++a) result += leap(a) ? 366 : 365;
  for (int a = 1; a < m; ++a) result += days[a - 1] + (a == 2 && leap(y));
  return result + d;
}
std::string today() {
  const auto now = std::time(nullptr);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%d");
  return out.str();
}
void validateTable(const json& t) {
  if (t.is_null()) return;
  require(t.is_object(), "係数表が不正です");
  for (const auto* k : {"version", "method", "basis", "effective_from", "checked_on", "errata", "normative"})
    text(t, k);
  require(t.at("method") == "jp_post_beam_2025", "未対応の計算方法です");
  require(t.at("normative") == "law" || t.at("normative") == "user", "出典の種別が不正です");
  day(t.at("effective_from"));
  day(t.at("checked_on"));
  const auto& source = t.at("source");
  for (const auto* k : {"name", "version", "date", "url"}) text(source, k);
  day(source.at("date"));
  if (t.contains("earthquake") && !t.at("earthquake").is_null())
    for (const auto* k : {"denominator_cm", "period_per_height_m", "co_normal", "co_designated"})
      require(number(t.at("earthquake"), k), "必要壁量の地震係数が不足しています");
  if (t.contains("wind") && !t.at("wind").is_null()) {
    require(number(t.at("wind"), "normal_cm_m2") && number(t.at("wind"), "special_max_cm_m2"),
            "風の係数が不足しています");
    require(t.at("wind").at("special_max_cm_m2") >= t.at("wind").at("normal_cm_m2"),
            "風の係数の範囲が不正です");
  }
  require(t.contains("entries") && t.at("entries").is_object(), "壁倍率の表が必要です");
  for (const auto& [id, entry] : t.at("entries").items()) {
    require(!id.empty(), "壁仕様IDが必要です");
    text(entry, "name");
    text(entry, "conditions");
    require(number(entry, "ratio") && entry.at("ratio").get<double>() <= 7, "壁倍率は0超7以下です");
    require(entry.contains("brace") && entry.at("brace").is_boolean(), "筋かいの種別が必要です");
  }
}
void point(const json& j) {
  require(j.is_array() && j.size() == 2, "区間はXY座標2値（mm）で指定してください");
  for (const auto& n : j)
    require(n.is_number() && std::isfinite(n.get<double>()) && std::abs(n.get<double>()) <= 1e9,
            "座標が不正です");
}
std::string quote(const std::string& s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"') r += '"';
    r += c;
  }
  return r + '"';
}
struct Wall {
  json result;
  std::string floor, direction;
  double a = 0, b = 0, line = 0;
  bool geometric = false;
};
// 四分割法 (wall balance), after 平成12年建設省告示第1352号 (MLIT consolidated text, rev. 2007),
// moved into 昭和56年建設省告示第1100号 第四 from 2025-04-01. Per storey and direction: the two side
// parts (outer quarters of the plan, measured across the direction), existing = sum of wall length x
// reduced ratio (alpha-h applies, MLIT material), required = side-part floor area x the storey's
// earthquake requirement per floor area (Lw, which replaced 令46条4項 表2). Storeys are counted per
// side part: a part without (or with only part of) the upper floor over it needs a one-storey Lw that
// the inputs do not give, so it is not evaluated. Pass: both 壁量充足率 > 1, or 壁率比 >= 0.5.
json quarterBalance(const json& in, const json& rows, const std::vector<Wall>& walls) {
  json out = json::array();
  const auto& floors = in.at("floors");
  auto rect = [](const json& f) -> std::optional<std::array<double, 4>> {
    if (!f.contains("plan")) return std::nullopt;
    const auto& p = f.at("plan");
    return std::array<double, 4>{p.at("x")[0].get<double>(), p.at("x")[1].get<double>(), p.at("y")[0].get<double>(),
                                 p.at("y")[1].get<double>()};
  };
  for (std::size_t i = 0; i < floors.size(); ++i) {
    const auto& f = floors[i];
    for (const std::string direction : {"X", "Y"}) {
      json row = {{"floor", f.at("id")}, {"name", f.value("name", f.at("id"))}, {"direction", direction},
                  {"strips", json::array()}, {"wall_ratio", nullptr}, {"status", "not_evaluated"},
                  {"reasons", json::array()}};
      auto reason = [&](const std::string& s) { row["reasons"].push_back(s); };
      const json* q = nullptr;
      for (const auto& r : rows)
        if (r.at("floor") == f.at("id") && r.at("direction") == direction) q = &r;
      const auto plan = rect(f);
      if (!plan) reason("この階の平面の範囲（plan）が未入力です");
      double lw = 0;
      if (!q || !q->contains("earthquake_cm_m2")) reason("この階の床面積あたりの必要壁量（地震）が未評価です");
      else lw = q->at("earthquake_cm_m2").get<double>();
      bool wallsKnown = f.value("walls_complete", false);
      if (!wallsKnown) reason("耐力壁の入力完了が未確認です");
      for (const auto& w : walls)
        if (w.floor == f.at("id") && w.direction == direction && w.result.at("effective_m").is_null()) {
          wallsKnown = false;
          reason("未評価の耐力壁があります（壁量の内訳参照）");
          break;
        }
      if (plan) {
        const auto [x0, x1, y0, y1] = *plan;
        // X-direction walls resist along X: their side parts are the quarters at both Y ends.
        const bool alongX = direction == "X";
        const double lo = alongX ? y0 : x0, hi = alongX ? y1 : x1, quarter = (hi - lo) / 4;
        const json* upper = i + 1 < floors.size() ? &floors[i + 1] : nullptr;
        const auto upperPlan = upper ? rect(*upper) : std::nullopt;
        std::vector<double> ratios;
        bool evaluable = row["reasons"].empty();
        for (int side = 0; side < 2; ++side) {
          const double a = side == 0 ? lo : hi - quarter, b = side == 0 ? lo + quarter : hi;
          const std::array<double, 4> strip = alongX ? std::array<double, 4>{x0, x1, a, b}
                                                     : std::array<double, 4>{a, b, y0, y1};
          json s = {{"side", side == 0 ? "low" : "high"},
                    {"rect", {{strip[0], strip[2]}, {strip[1], strip[3]}}},
                    {"area_m2", (strip[1] - strip[0]) * (strip[3] - strip[2]) / 1e6},
                    {"required_m", nullptr}, {"existing_m", nullptr}, {"ratio", nullptr}};
          if (f.contains("side_areas_m2")) s["area_m2"] = f.at("side_areas_m2").at(direction)[side];
          // Storeys counted per side part: the upper floor must cover this part entirely.
          if (upper) {
            if (!upperPlan) {
              evaluable = false;
              reason("上の階の平面の範囲（plan）が未入力です");
            } else {
              const auto& u = *upperPlan;
              const double cover = std::max(0.0, std::min(u[1], strip[1]) - std::max(u[0], strip[0])) *
                                   std::max(0.0, std::min(u[3], strip[3]) - std::max(u[2], strip[2]));
              const double whole = (strip[1] - strip[0]) * (strip[3] - strip[2]);
              s["upper_floor"] = cover >= whole - 1 ? "full" : cover <= 1 ? "none" : "partial";
              if (s["upper_floor"] != "full") {
                evaluable = false;
                reason(s["upper_floor"] == "none"
                           ? "上の階が載っていない側端部分（下屋）は、平屋としての床面積あたり必要壁量が入力にないため未評価です"
                           : "上の階が一部だけ載る側端部分は未対応です（階数の数え方を決められません）");
              }
            }
          }
          double existing = 0;
          for (const auto& w : walls)
            if (w.floor == f.at("id") && w.direction == direction && w.geometric && w.line >= a - 1 &&
                w.line <= b + 1 && !w.result.at("effective_m").is_null())
              existing += w.result.at("effective_m").get<double>();
          s["existing_m"] = existing;
          if (lw > 0) {
            const double required = s["area_m2"].get<double>() * lw / 100;
            s["required_m"] = required;
            if (required > 0) {
              s["ratio"] = existing / required;
              ratios.push_back(existing / required);
            }
          }
          row["strips"].push_back(s);
        }
        if (evaluable && ratios.size() == 2) {
          const double small = std::min(ratios[0], ratios[1]), large = std::max(ratios[0], ratios[1]);
          row["wall_ratio"] = large > 0 ? small / large : 0.0;
          const bool bothOver = ratios[0] > 1 && ratios[1] > 1;
          row["status"] = bothOver || row["wall_ratio"].get<double>() >= .5 ? "pass" : "fail";
          row["basis"] = bothOver ? "両側端の壁量充足率が1を超える" : "壁率比が0.5以上か";
        }
      }
      out.push_back(std::move(row));
    }
  }
  return out;
}
}  // namespace

std::string wallTables() {
  static const std::string value = [] {
    std::string out;
    for (unsigned i = 0; i < embedded::kTimber_wall_tables_parts; ++i)
      out += embedded::kTimber_wall_tables[i];
    validateTable(json::parse(out));
    return out;
  }();
  return value;
}

std::string normalizeWallInputs(const std::string& input) {
  json in = json::parse(input);
  keys(in, {"schema_version", "frame", "height_m", "co_designated", "wind_coefficient_cm_m2", "input_basis",
            "floors", "walls", "table_snapshot"});
  require(in.value("schema_version", 1) == 1, "未対応の入力版です");
  in["schema_version"] = 1;
  if (in.contains("frame")) require(in.at("frame").is_string(), "frameはグループIDです");
  if (in.contains("input_basis")) require(in.at("input_basis").is_string(), "入力の根拠は文字列です");
  number(in, "height_m");
  if (in.contains("co_designated") && !in.at("co_designated").is_null())
    require(in.at("co_designated").is_boolean(), "C0区域指定はtrue/falseで指定してください");
  number(in, "wind_coefficient_cm_m2");
  if (!in.contains("table_snapshot")) in["table_snapshot"] = json::parse(wallTables());
  validateTable(in.at("table_snapshot"));
  std::set<std::string> floors, walls;
  if (!in.contains("floors")) in["floors"] = json::array();
  require(in.at("floors").is_array() && in.at("floors").size() <= 2, "床の指定は1〜2階が対象です");
  for (const auto& f : in.at("floors")) {
    keys(f, {"id", "name", "area_m2", "weight_kn", "wind_x_m2", "wind_y_m2", "walls_complete", "plan", "side_areas_m2"});
    text(f, "id");
    require(floors.insert(f.at("id")).second, "階IDが重複しています");
    if (f.contains("name")) require(f.at("name").is_string(), "階名は文字列です");
    for (const auto* k : {"area_m2", "weight_kn"}) number(f, k);
    for (const auto* k : {"wind_x_m2", "wind_y_m2"}) number(f, k, true);
    if (f.contains("walls_complete"))
      require(f.at("walls_complete").is_boolean(), "耐力壁の入力完了は真偽値です");
    // Plan extent for 四分割法 (world mm rectangle) and optional side-part areas when the
    // floor is not that rectangle.
    if (f.contains("plan")) {
      const auto& p = f.at("plan");
      require(p.is_object() && p.contains("x") && p.contains("y"), "planは{x:[min,max],y:[min,max]}（mm）です");
      for (const auto* axis : {"x", "y"}) {
        const auto& r = p.at(axis);
        require(r.is_array() && r.size() == 2, "planの範囲は[min,max]です");
        point(json::array({r[0], r[1]}));
        require(r[1].get<double>() - r[0].get<double>() >= 100, "planの範囲が小さすぎるか逆順です");
      }
    }
    if (f.contains("side_areas_m2")) {
      const auto& s = f.at("side_areas_m2");
      require(s.is_object() && s.contains("X") && s.contains("Y"), "side_areas_m2は{X:[低,高],Y:[低,高]}（m2）です");
      for (const auto* axis : {"X", "Y"}) {
        const auto& r = s.at(axis);
        require(r.is_array() && r.size() == 2 && r[0].is_number() && r[1].is_number() && r[0].get<double>() > 0 &&
                    r[1].get<double>() > 0,
                "側端部分の床面積は正の数値です");
      }
    }
  }
  if (!in.contains("walls")) in["walls"] = json::array();
  require(in.at("walls").is_array() && in.at("walls").size() <= 2000, "耐力壁は2000区間以下です");
  for (const auto& w : in.at("walls")) {
    keys(w, {"id", "floor", "direction", "start", "end", "height_mm", "spec", "verified", "grid",
             "start_axes", "end_axes"});
    for (const auto* k : {"id", "floor", "direction", "spec"}) text(w, k);
    require(walls.insert(w.at("id")).second, "壁IDが重複しています");
    require(floors.count(w.at("floor")) != 0, "壁の階IDが未登録です");
    require(w.at("direction") == "X" || w.at("direction") == "Y", "方向はX/Yです");
    number(w, "height_mm");
    if (w.contains("verified")) require(w.at("verified").is_boolean(), "仕様確認は真偽値です");
    if (w.contains("start")) point(w.at("start"));
    if (w.contains("end")) point(w.at("end"));
    if (w.contains("grid")) {
      text(w, "grid");
      for (const auto* key : {"start_axes", "end_axes"}) {
        require(w.contains(key) && w.at(key).is_array() && w.at(key).size() == 2,
                "通り芯の始終点は[X通りID,Y通りID]です");
        for (const auto& a : w.at(key))
          require(a.is_string() && !a.get<std::string>().empty(), "通り芯IDが必要です");
      }
      require(!w.contains("start") && !w.contains("end"), "通り芯参照と手動座標を同時に指定できません");
    } else
      require(!w.contains("start_axes") && !w.contains("end_axes"), "通り芯グループIDが必要です");
  }
  return in.dump();
}

Result<std::string> checkWallQuantity(const doc::Document& d, const std::string& options) {
  try {
    const json opt = json::parse(options);
    keys(opt, {"inputs", "as_of", "frame", "include_csv"});
    const json in = json::parse(
        normalizeWallInputs(opt.contains("inputs") ? opt.at("inputs").dump() : d.settings().wallCalculation));
    const json& table = in.at("table_snapshot");
    const std::string date = opt.value("as_of", today());
    const auto nowDay = day(date);
    const bool stale = !table.is_null() && nowDay - day(table.at("checked_on")) > 365;
    const bool future = !table.is_null() &&
                        (day(table.at("checked_on")) > nowDay || day(table.at("effective_from")) > nowDay);
    std::string notice =
        "意匠・概略の検討用。入力した耐力壁と荷重による壁量比較で、建物全体の安全・法適合の判定ではありません"
        "。";
    if (table.is_null())
      notice += "未評価：必要壁量・壁倍率の表がありません。";
    else {
      notice += "壁量の根拠: " + table.at("basis").get<std::string>() + "／施行 " +
                table.at("effective_from").get<std::string>() + "／表の確認日 " +
                table.at("checked_on").get<std::string>() + "／版 " + table.at("version").get<std::string>() +
                "。最新の基準を確認してください。";
      if (stale) notice += "表の確認から1年以上たっています。";
      if (future) notice += "表の施行日・確認日が計算日より後です。この日付では未評価です。";
    }
    notice += "四分割法は平成12年告示1352号（2025年4月から告示1100号第四）の考え方で別欄に計算。N値・柱頭柱脚・準耐力壁等は未評価。";
    bool scoped = true;
    const std::string frame = in.value("frame", std::string());
    if (!frame.empty()) scoped = d.find(frame) && d.find(frame)->isContainer();
    if (opt.contains("frame") && opt.at("frame") != frame) scoped = false;
    std::vector<Wall> walls;
    for (const auto& w : in.at("walls")) {
      Wall wall;
      wall.floor = w.at("floor");
      wall.direction = w.at("direction");
      wall.result = {{"id", w.at("id")},       {"floor", wall.floor},       {"direction", wall.direction},
                     {"spec", w.at("spec")},   {"length_mm", nullptr},      {"ratio", nullptr},
                     {"effective_m", nullptr}, {"status", "not_evaluated"}, {"reasons", json::array()}};
      auto reason = [&](const char* s) { wall.result["reasons"].push_back(s); };
      json a = w.value("start", json()), b = w.value("end", json());
      if (w.contains("grid")) {
        const auto* object = d.find(w.at("grid"));
        const auto* group = object ? std::get_if<doc::GroupNode>(&object->recipe.base) : nullptr;
        if (!group || !group->grid)
          reason("通り芯が削除されたか見つかりません");
        else {
          const auto transform = d.worldTransform(*object);
          if (std::abs(transform.Value(3, 1)) > 1e-8 || std::abs(transform.Value(3, 2)) > 1e-8)
            reason("傾斜した通り芯は今回の壁量計算の対象外です");
          auto resolve = [&](const char* key) -> json {
            const auto& pair = w.at(key);
            const auto& grid = *group->grid;
            const auto x =
                std::find_if(grid.x.begin(), grid.x.end(), [&](const auto& v) { return v.id == pair[0]; });
            const auto y =
                std::find_if(grid.y.begin(), grid.y.end(), [&](const auto& v) { return v.id == pair[1]; });
            if (x == grid.x.end() || y == grid.y.end()) return nullptr;
            const auto p = gp_Pnt(x->offset, y->offset, 0).Transformed(d.worldTransform(*object));
            return {p.X(), p.Y()};
          };
          a = resolve("start_axes");
          b = resolve("end_axes");
          if (a.is_null() || b.is_null()) reason("参照する通り芯が削除されました");
        }
      }
      if (a.is_null() || b.is_null())
        reason("壁の区間が未指定です");
      else {
        const bool x = wall.direction == "X";
        const double alongA = a[x ? 0 : 1], alongB = b[x ? 0 : 1];
        wall.a = std::min(alongA, alongB);
        wall.b = std::max(alongA, alongB);
        wall.line = a[x ? 1 : 0];
        if (std::abs(a[x ? 1 : 0].get<double>() - b[x ? 1 : 0].get<double>()) > .001 || wall.b - wall.a < 1)
          reason("壁の区間が指定したX/Y方向に平行でないか短すぎます");
        else {
          wall.geometric = true;
          wall.result["start"] = a;
          wall.result["end"] = b;
          wall.result["length_mm"] = wall.b - wall.a;
        }
      }
      if (!w.value("verified", false)) reason("施工・断面・接合を含む壁仕様が未確認です");
      if (table.is_null() || !table.at("entries").contains(w.at("spec").get<std::string>()))
        reason("壁倍率の仕様表がありません");
      else {
        const auto& spec = table.at("entries").at(w.at("spec").get<std::string>());
        wall.result["conditions"] = spec.at("conditions");
        double factor = 1;
        if (spec.at("brace").get<bool>()) {
          if (!w.contains("height_mm") || w.at("height_mm").is_null())
            reason("筋かいの横架材上端間の高さが未指定です");
          else if (w.at("height_mm").get<double>() > 3200 && wall.geometric)
            factor = std::min(1.0, 3.5 * (wall.b - wall.a) / w.at("height_mm").get<double>());
        }
        wall.result["height_factor"] = factor;
        wall.result["ratio"] = spec.at("ratio").get<double>() * factor;
      }
      walls.push_back(std::move(wall));
    }
    // Two designations on the same physical interval cannot earn twice the
    // resistance, even with different specifications or reversed endpoints.
    for (std::size_t i = 0; i < walls.size(); ++i)
      for (std::size_t j = i + 1; j < walls.size(); ++j) {
        auto& a = walls[i];
        auto& b = walls[j];
        if (a.geometric && b.geometric && a.floor == b.floor && a.direction == b.direction &&
            std::abs(a.line - b.line) < .001 && std::min(a.b, b.b) - std::max(a.a, b.a) > .001) {
          a.result["reasons"].push_back("同じ区間に壁指定が重複しています");
          b.result["reasons"].push_back("同じ区間に壁指定が重複しています");
        }
      }
    json parts = json::array();
    for (auto& w : walls) {
      if (w.result["reasons"].empty()) {
        w.result["status"] = "evaluated";
        w.result["effective_m"] =
            w.result["length_mm"].get<double>() * w.result["ratio"].get<double>() / 1000;
      }
      parts.push_back(w.result);
    }
    json rows = json::array();
    const auto& floors = in.at("floors");
    for (std::size_t i = 0; i < floors.size(); ++i) {
      const auto& f = floors[i];
      for (const auto* direction : {"X", "Y"}) {
        json row = {{"floor", f.at("id")},       {"name", f.value("name", f.at("id"))},
                    {"direction", direction},    {"existing_m", nullptr},
                    {"known_existing_m", 0.0},   {"earthquake_m", nullptr},
                    {"wind_m", nullptr},         {"required_m", nullptr},
                    {"status", "not_evaluated"}, {"reasons", json::array()}};
        auto reason = [&](const char* s) { row["reasons"].push_back(s); };
        bool knownWalls = f.value("walls_complete", false);
        if (!knownWalls) reason("耐力壁の入力完了が未確認です");
        double existing = 0;
        for (const auto& w : walls)
          if (w.floor == f.at("id") && w.direction == direction) {
            if (w.result.at("effective_m").is_null()) {
              knownWalls = false;
              reason("未評価の耐力壁があります（内訳参照）");
            } else
              existing += w.result.at("effective_m").get<double>();
          }
        row["known_existing_m"] = existing;
        if (knownWalls) row["existing_m"] = existing;
        if (table.is_null())
          reason("必要壁量の表がありません");
        else {
          const auto eq = table.value("earthquake", json());
          const auto wind = table.value("wind", json());
          const bool eqInputs = number(f, "area_m2") && number(f, "weight_kn") && !floors.empty() &&
                                number(floors[0], "weight_kn") && number(in, "height_m") &&
                                in.contains("co_designated") && in.at("co_designated").is_boolean();
          bool loadOrder = true;
          for (std::size_t k = 1; k < floors.size(); ++k)
            if (!number(floors[k - 1], "weight_kn") || !number(floors[k], "weight_kn") ||
                floors[k - 1].at("weight_kn").get<double>() < floors[k].at("weight_kn").get<double>())
              loadOrder = false;
          if (eq.is_null())
            reason("必要壁量の地震係数表がありません");
          else if (!eqInputs || !loadOrder)
            reason("床面積・累積荷重・高さ・C0区域指定が不足または荷重の大小関係が不正です");
          else {
            const double alpha = f.at("weight_kn").get<double>() / floors[0].at("weight_kn").get<double>();
            const double period =
                eq.at("period_per_height_m").get<double>() * in.at("height_m").get<double>();
            const double ai = 1 + (1 / std::sqrt(alpha) - alpha) * 2 * period / (1 + 3 * period);
            const double co = eq.at(in.at("co_designated").get<bool>() ? "co_designated" : "co_normal");
            const double coefficient =
                ai * co * f.at("weight_kn").get<double>() /
                (eq.at("denominator_cm").get<double>() * f.at("area_m2").get<double>());
            row["ai"] = ai;
            row["co"] = co;
            row["earthquake_cm_m2"] = coefficient;
            row["earthquake_m"] = coefficient * f.at("area_m2").get<double>() / 100;
          }
          const char* windKey = direction == std::string("X") ? "wind_x_m2" : "wind_y_m2";
          if (wind.is_null())
            reason("必要壁量の風係数表がありません");
          else if (!number(f, windKey, true) || !number(in, "wind_coefficient_cm_m2"))
            reason("控除後の見付面積・地域の風係数が未指定です");
          else {
            const double c = in.at("wind_coefficient_cm_m2");
            if (c < wind.at("normal_cm_m2").get<double>() || c > wind.at("special_max_cm_m2").get<double>())
              reason("風係数が表の適用範囲外です");
            else
              row["wind_m"] = f.at(windKey).get<double>() * c / 100;
          }
        }
        if (future) reason("表の施行日・確認日が計算日より後です");
        if (!scoped) reason("計算対象の架構が異なるか削除されました");
        if (in.value("input_basis", std::string()).empty()) reason("荷重・面積・区域の入力根拠が未記入です");
        if (!row["earthquake_m"].is_null() && !row["wind_m"].is_null())
          row["required_m"] = std::max(row["earthquake_m"].get<double>(), row["wind_m"].get<double>());
        if (row["reasons"].empty() && !row["required_m"].is_null() && !row["existing_m"].is_null())
          row["status"] = existing + 1e-10 >= row["required_m"].get<double>() ? "pass" : "fail";
        row["notice"] = notice;
        rows.push_back(std::move(row));
      }
    }
    if (rows.empty())
      rows.push_back({{"floor", ""},
                      {"direction", ""},
                      {"existing_m", nullptr},
                      {"required_m", nullptr},
                      {"status", "not_evaluated"},
                      {"reasons", {"計算する階が未登録です"}},
                      {"notice", notice}});
    bool complete = true;
    for (const auto& row : rows)
      if (row.at("status") == "not_evaluated") complete = false;
    json out = {{"system", "timber"},
                {"checker_version", kVersion},
                {"as_of", date},
                {"table_snapshot", table},
                {"inputs", in},
                {"rows", rows},
                {"walls", parts},
                {"quarter_balance", quarterBalance(in, rows, walls)},
                {"quarter_balance_basis",
                 {{"source", "平成12年建設省告示第1352号（国交省掲載の改正平成19年告示1227号までの版）"},
                  {"url", "https://www.mlit.go.jp/notice/noticedata/pdf/201703/00006444.pdf"},
                  {"note", "2025年4月1日から昭和56年建設省告示第1100号第四へ移行（移行後の条文は未照合）。必要壁量は側端部分の床面積×その階の地震のLw（令46条4項の表二を置き換えた式）。αhの低減は四分割法にも適用（国交省資料001746169 p.10）。準耐力壁等は算入しない。"}}},
                {"notice", notice},
                {"stale_table", stale},
                {"complete", complete},
                {"comparison_tolerance_m", 1e-10},
                {"unassessed", {"N値", "柱頭柱脚", "準耐力壁等", "四分割法の下屋・一部だけ上階がある側端部分", "断面", "基礎"}}};
    return out.dump();
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}

std::string wallQuantityCsv(const std::string& report) {
  const json r = json::parse(report);
  std::ostringstream out;
  out << "\xef\xbb\xbf"
      << "種別,ID,階,方向,区間長mm,補正後壁倍率,存在壁量m,地震必要m,風必要m,必要壁量m,評価,理由,但し書き,"
         "入力,根拠表\r\n";
  auto write = [&](const json& row, bool wall) {
    json values = {wall ? "耐力壁" : "階方向",
                   row.value("id", json()),
                   row.at("floor"),
                   row.at("direction"),
                   row.value("length_mm", json()),
                   row.value("ratio", json()),
                   row.value(wall ? "effective_m" : "existing_m", json()),
                   row.value("earthquake_m", json()),
                   row.value("wind_m", json()),
                   row.value("required_m", json()),
                   row.at("status"),
                   row.at("reasons"),
                   r.at("notice"),
                   r.at("inputs"),
                   r.at("table_snapshot")};
    bool first = true;
    for (const auto& v : values) {
      if (!first) out << ',';
      first = false;
      out << quote(v.is_null() ? "" : v.is_string() ? v.get<std::string>() : v.dump());
    }
    out << "\r\n";
  };
  for (const auto& row : r.at("rows")) write(row, false);
  for (const auto& row : r.at("walls")) write(row, true);
  return out.str();
}
}  // namespace sbr::construction
