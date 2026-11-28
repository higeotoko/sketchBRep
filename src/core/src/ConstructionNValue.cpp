// docs/358: 柱頭・柱脚の接合方法の N 値計算法 (令第47条第1項, 平12建告第1460号), as the MLIT manual
// 「改正建築基準法 ２階建ての木造一戸建て住宅（軸組構法）等の確認申請・審査マニュアル」(2024年11月 第3版) gives
// it on pp.100-102 (図3-19, 表3-14), with its 正誤表 (2024-11-27: the formulas on p.101).
#include "sbr/ConstructionWall.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <Bnd_Box.hxx>

namespace sbr::construction {
namespace {
using json = nlohmann::json;

// 表3-14 (manual p.102), by brace class: 0 木材15×90以上又は鉄筋φ9以上, 1 木材30×90以上, 2 木材45×90以上,
// 3 木材90×90以上.
constexpr double kOneHead[4] = {0, 0.5, 0.5, 2.0};    // 柱の片側のみ, the brace's top end on this column
constexpr double kOneFoot[4] = {0, -0.5, -0.5, -2.0};  // 柱の片側のみ, its bottom end on this column
// 柱の両側, both braces' top ends on this column (rows and columns: the two braces' classes).
constexpr double kBothHead[4][4] = {{0, 0.5, 0.5, 2.0}, {0.5, 1.0, 1.0, 2.5}, {0.5, 1.0, 1.0, 2.5}, {2.0, 2.5, 2.5, 4.0}};
// 柱の両側, one brace's top end and the other's bottom end on this column (rows: the one at the head; columns:
// the one at the foot), as printed.
constexpr double kHeadFoot[4][4] = {{0, -0.5, -0.5, 2}, {0.5, 0.5, 0, 1.5}, {0.5, 0.5, 0.5, 1.5}, {2.0, 1.5, 1.5, 2.0}};
// 柱の両側, たすきがけ on one side and a brace whose top end is on this column on the other (by that brace's class;
// the same for every たすきがけ).
constexpr double kCrossHead[4] = {0, 0.5, 0.5, 2.0};
// Both bottom ends on the column, たすきがけ with a bottom end, たすきがけ on both sides, たすきがけ on one side only: 0.

// 告示1460号 表三 (い)～(ぬ) by the N value, as the manual's 使用金物一覧 gives them.
struct Joint {
  double upTo;
  const char* mark;
  const char* name;
};
constexpr Joint kJoints[] = {
    {0.0, "(い)", "短ほぞ差し及びかすがい打ち"},
    {0.65, "(ろ)", "長ほぞ差し込み栓又はかど金物 CP-L"},
    {1.0, "(は)", "山形プレート VP 又はかど金物 CP-T"},
    {1.4, "(に)", "羽子板ボルト又は短冊金物（スクリュー釘なし）"},
    {1.6, "(ほ)", "羽子板ボルト又は短冊金物（スクリュー釘あり）"},
    {1.8, "(へ)", "10kN 引き寄せ金物"},
    {2.8, "(と)", "15kN 引き寄せ金物"},
    {3.7, "(ち)", "20kN 引き寄せ金物"},
    {4.7, "(り)", "25kN 引き寄せ金物"},
    {5.6, "(ぬ)", "15kN 引き寄せ金物×2"},
};

int braceClass(const geom::Section& s) {
  const double t = std::min(s.width, s.depth), b = std::max(s.width, s.depth);
  if (b < 90 - 0.5) return -1;
  if (t >= 90 - 0.5) return 3;
  if (t >= 45 - 0.5) return 2;
  if (t >= 30 - 0.5) return 1;
  if (t >= 15 - 0.5) return 0;
  return -1;
}

struct Brace {
  int floor = 0;
  gp_Pnt low, high;  // its ends, world
  int cls = -1;
};
struct WallPart {
  int floor = 0;
  std::string direction;
  double line = 0, a = 0, b = 0;  // along: a < b
  double ratio = 0;               // 壁倍率 used for N
  double heightMm = 0;
  bool brace = false;
};
struct Side {
  enum Kind { None, Head, Foot, Cross } kind = None;
  int cls = -1;
};
const char* sideName(Side::Kind k) {
  switch (k) {
    case Side::Head: return "柱頭に筋かい";
    case Side::Foot: return "柱脚に筋かい";
    case Side::Cross: return "たすき掛け";
    default: return "筋かいなし";
  }
}

std::optional<double> correction(const Side& l, const Side& r, std::string& why) {
  const auto need = [&](const Side& s) {
    if (s.cls < 0) why = "筋かいの断面が表3-14の区分（15×90・30×90・45×90・90×90以上）に当たりません";
    return s.cls >= 0;
  };
  if (l.kind == Side::None && r.kind == Side::None) return 0.0;
  if (l.kind == Side::None || r.kind == Side::None) {
    const Side& s = l.kind == Side::None ? r : l;
    if (s.kind == Side::Cross) return 0.0;
    if (!need(s)) return std::nullopt;
    return s.kind == Side::Head ? kOneHead[s.cls] : kOneFoot[s.cls];
  }
  if (l.kind == Side::Cross || r.kind == Side::Cross) {
    const Side& other = l.kind == Side::Cross ? r : l;
    if (other.kind != Side::Head) return 0.0;
    if (!need(other)) return std::nullopt;
    return kCrossHead[other.cls];
  }
  if (l.kind == Side::Foot && r.kind == Side::Foot) return 0.0;
  if (!need(l) || !need(r)) return std::nullopt;
  if (l.kind == Side::Head && r.kind == Side::Head) return kBothHead[l.cls][r.cls];
  const Side& head = l.kind == Side::Head ? l : r;
  const Side& foot = l.kind == Side::Head ? r : l;
  return kHeadFoot[head.cls][foot.cls];
}

double heightFactor(double mm) { return (mm <= 3200 + 1e-6 ? 2700.0 : mm) / 2700.0; }
}  // namespace

Result<std::string> columnNValues(const doc::Document& d) {
  using R = Result<std::string>;
  try {
    json out{{"basis",
              "柱頭・柱脚の接合方法：N値計算法（令第47条第1項、平12建告第1460号）。式・係数・補正値は国土交通省「改正建築基準法 "
              "２階建ての木造一戸建て住宅（軸組構法）等の確認申請・審査マニュアル」2024年11月 第3版 pp.100-102（図3-19、表3-14）"
              "と同正誤表（2024年11月27日）、接合方法は同マニュアル（経過措置対応版）の使用金物一覧（告示1460号 表三）による。"},
             {"columns", json::array()},
             {"reasons", json::array()}};
    const auto fail = [&](const std::string& why) {
      out["status"] = "not_evaluated";
      out["reasons"].push_back(why);
      return R(out.dump());
    };
    if (d.settings().wallCalculation.empty() || d.settings().wallCalculation == "{}")
      return fail("壁量の計算の入力がありません（N値は耐力壁の倍率と配置から求めます）");
    const json in = json::parse(normalizeWallInputs(d.settings().wallCalculation));
    const auto checked = checkWallQuantity(d);
    if (!checked) return fail("壁量の計算を読めません：" + checked.error());
    const json report = json::parse(*checked);
    const json floors = in.value("floors", json::array());
    if (floors.empty() || floors.size() > 2) return fail("階が1〜2の壁量の計算に限ります");
    std::map<std::string, int> floorIndex;  // id -> 1, 2 (in the order given, the lowest first)
    std::map<int, std::array<double, 4>> plan;  // x0, x1, y0, y1
    std::map<int, std::string> floorName;
    for (std::size_t k = 0; k < floors.size(); ++k) {
      const int f = static_cast<int>(k) + 1;
      floorIndex[floors[k].at("id").get<std::string>()] = f;
      floorName[f] = floors[k].value("name", floors[k].at("id").get<std::string>());
      if (floors[k].contains("plan")) {
        const auto& p = floors[k].at("plan");
        plan[f] = {p.at("x").at(0).get<double>(), p.at("x").at(1).get<double>(), p.at("y").at(0).get<double>(),
                   p.at("y").at(1).get<double>()};
      }
    }
    const int storeys = static_cast<int>(floors.size());
    // The walls: their intervals and 壁倍率 (準耐力壁等 at 0 unless over 1.5, manual p.101 ※).
    std::map<std::string, double> heights;
    std::map<std::string, bool> braced;
    const json tableEntries = in.at("table_snapshot").value("entries", json::object());
    for (const auto& w : in.value("walls", json::array())) {
      if (w.contains("height_mm") && w.at("height_mm").is_number()) heights[w.at("id")] = w.at("height_mm").get<double>();
      const std::string spec = w.value("spec", std::string());
      braced[w.at("id")] = tableEntries.contains(spec) && tableEntries.at(spec).value("brace", false);
    }
    std::vector<WallPart> walls;
    int unevaluated = 0;
    for (const auto& w : report.value("walls", json::array())) {
      if (w.value("status", std::string()) != "evaluated" || !w.contains("start") || !w.contains("end")) {
        ++unevaluated;
        continue;
      }
      WallPart p;
      p.floor = floorIndex.count(w.at("floor").get<std::string>()) ? floorIndex.at(w.at("floor").get<std::string>()) : 0;
      p.direction = w.at("direction");
      const double ax = w.at("start").at(0).get<double>(), ay = w.at("start").at(1).get<double>(),
                   bx = w.at("end").at(0).get<double>(), by = w.at("end").at(1).get<double>();
      const bool x = p.direction == "X";
      p.line = x ? ay : ax;
      p.a = std::min(x ? ax : ay, x ? bx : by);
      p.b = std::max(x ? ax : ay, x ? bx : by);
      const double ratio = w.value("ratio", 0.0), factor = w.value("height_factor", 1.0);
      const bool semi = w.value("kind", std::string()) == "semi";
      p.ratio = semi ? (ratio > 1.5 + 1e-9 ? ratio : 0.0) : (factor > 0 ? ratio / factor : ratio);
      p.heightMm = heights.count(w.at("id")) ? heights.at(w.at("id")) : 0;
      p.brace = braced.count(w.at("id")) && braced.at(w.at("id"));
      walls.push_back(p);
    }
    if (unevaluated > 0)
      out["reasons"].push_back("壁量の計算で未評価の壁が " + std::to_string(unevaluated) + " 枚あります（N値には入れていません）");
    // The storeys: the frame's stage of each member (1階軸組 / 2階軸組); a through column (通し柱, made with the first
    // storey) stands in both.
    const auto floorOfLayer = [](const std::string& layer) { return layer == "2階軸組" ? 2 : layer == "1階軸組" ? 1 : 0; };
    // The frame's columns (plan points per storey) and braces.
    std::map<int, std::vector<std::pair<gp_Pnt2d, std::string>>> columns;
    std::vector<Brace> braces;
    for (const doc::Object& o : d.objects()) {
      const auto kind = o.attributes.find("kind");
      if (kind == o.attributes.end() || o.shape.IsNull()) continue;
      if (kind->second == "column" || kind->second == "through_column") {
        const Bnd_Box b = d.worldBounds(o);
        if (b.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        const gp_Pnt2d p((x0 + x1) / 2, (y0 + y1) / 2);
        const std::string key = o.attributes.count("frame_key") ? o.attributes.at("frame_key") : o.name;
        const bool through = kind->second == "through_column";
        const int f = floorOfLayer(o.layer);
        if (f == 0) continue;  // not a frame column
        columns[f].push_back({p, key + (through ? "（通し柱）" : "")});
        if (through && storeys == 2) columns[2].push_back({p, key + "（通し柱）"});
      } else if (kind->second == "brace") {
        const auto member = frameMemberObject(d, o);
        const doc::Object& part = member ? *member : o;
        const auto* linear = std::get_if<doc::LinearMember>(&part.recipe.base);
        if (!linear) continue;
        const gp_Trsf w = d.worldTransform(part);
        gp_Pnt a = linear->start.Transformed(w), c = linear->end.Transformed(w);
        if (a.Z() > c.Z()) std::swap(a, c);
        const int f = floorOfLayer(o.layer);
        if (f == 0) continue;
        braces.push_back({f, a, c, braceClass(linear->section)});
      }
    }
    if (columns.empty()) return fail("架構の柱がありません（generate_frame）");
    // Per storey, per column, per direction: the difference of the wall ratios on its two sides and its brace
    // correction.
    struct Dir {
      bool walls = false;
      double left = 0, right = 0, corr = 0, a = 0, heightMm = 0;
      Side l, r;
      std::string why;
    };
    const auto evaluate = [&](int f, const gp_Pnt2d& p, const std::string& dir) {
      Dir r;
      const bool x = dir == "X";
      const double t = x ? p.X() : p.Y(), line = x ? p.Y() : p.X();
      bool braceWallLeft = false, braceWallRight = false;
      for (const WallPart& w : walls) {
        if (w.floor != f || w.direction != dir || std::abs(w.line - line) > 1.0 || t < w.a - 1.0 || t > w.b + 1.0) continue;
        r.walls = true;
        r.heightMm = std::max(r.heightMm, w.heightMm);
        if (w.a < t - 1.0) r.left += w.ratio, braceWallLeft = braceWallLeft || w.brace;
        if (w.b > t + 1.0) r.right += w.ratio, braceWallRight = braceWallRight || w.brace;
      }
      if (!r.walls) return r;
      for (const Brace& b : braces) {
        if (b.floor != f) continue;
        const double la = x ? b.low.Y() : b.low.X(), lb = x ? b.high.Y() : b.high.X();
        if (std::abs(la - line) > 1.0 || std::abs(lb - line) > 1.0) continue;
        const double ta = x ? b.low.X() : b.low.Y(), tb = x ? b.high.X() : b.high.Y();
        const bool footHere = std::abs(ta - t) < 1.0, headHere = std::abs(tb - t) < 1.0;
        if (!footHere && !headHere) continue;
        const double other = footHere ? tb : ta;
        Side& s = other < t ? r.l : r.r;
        const Side::Kind k = headHere ? Side::Head : Side::Foot;
        if (s.kind == Side::None) s.kind = k, s.cls = b.cls;
        else if (s.kind != k) s.kind = Side::Cross, s.cls = std::min(s.cls, b.cls);
      }
      if ((braceWallLeft && r.l.kind == Side::None) || (braceWallRight && r.r.kind == Side::None))
        r.why = "壁量の計算の筋かいの壁に、架構の筋かいが見つかりません（補正値を決められません）";
      else if ((r.l.kind != Side::None && r.left <= 0) || (r.r.kind != Side::None && r.right <= 0))
        r.why = "架構の筋かいが、壁量の計算の壁に入っていません";
      else if (r.heightMm <= 0)
        r.why = "壁の横架材の上端間の高さ（height_mm）がありません";
      else if (const auto c = correction(r.l, r.r, r.why)) {
        r.corr = *c;
        r.a = std::abs(r.left - r.right) + r.corr;
      }
      return r;
    };
    const auto corner = [&](int f, const gp_Pnt2d& p) -> std::optional<bool> {
      if (!plan.count(f)) return std::nullopt;
      const auto& q = plan.at(f);
      const bool onX = std::abs(p.X() - q[0]) < 1.0 || std::abs(p.X() - q[1]) < 1.0;
      const bool onY = std::abs(p.Y() - q[2]) < 1.0 || std::abs(p.Y() - q[3]) < 1.0;
      return onX && onY;
    };
    const auto inside = [&](int f, const gp_Pnt2d& p) {
      if (!plan.count(f)) return false;
      const auto& q = plan.at(f);
      return p.X() > q[0] - 1.0 && p.X() < q[1] + 1.0 && p.Y() > q[2] - 1.0 && p.Y() < q[3] + 1.0;
    };
    const auto jointOf = [](double n) -> std::optional<Joint> {
      for (const Joint& j : kJoints)
        if (n <= j.upTo + 1e-9) return j;
      return std::nullopt;
    };
    std::map<std::string, int> counts;
    int evaluatedColumns = 0, notEvaluated = 0;
    for (int f = storeys; f >= 1; --f) {
      // docs/359: the columns numbered as the bracing plan of the storey numbers them (docs/333: from the top row down,
      // left to right in each row), so the table and the plan name a column alike (the manual's 10-2).
      std::vector<std::pair<gp_Pnt2d, std::string>> unique;
      {
        std::set<std::pair<long long, long long>> seen;
        for (const auto& c : columns[f])
          if (seen.insert({std::llround(c.first.X()), std::llround(c.first.Y())}).second) unique.push_back(c);
      }
      std::sort(unique.begin(), unique.end(), [](const auto& l, const auto& r) {
        const long long ly = std::llround(l.first.Y()), ry = std::llround(r.first.Y());
        return ly != ry ? ly > ry : l.first.X() < r.first.X();
      });
      for (std::size_t index = 0; index < unique.size(); ++index) {
        const gp_Pnt2d& p = unique[index].first;
        const std::string& key = unique[index].second;
        json row{{"floor", floorName[f]}, {"number", index + 1}, {"at", {p.X(), p.Y()}}, {"column", key},
                 {"directions", json::object()}, {"reasons", json::array()}};
        const bool upper = f == 1 && storeys == 2 && inside(2, p);
        const auto isCorner = corner(f, p);
        const auto upperCorner = upper ? corner(2, p) : std::optional<bool>(false);
        if (!isCorner || !upperCorner) row["reasons"].push_back("出隅かどうかを決める階の平面の範囲（plan）がありません");
        bool any = false;
        double best = -1e300;
        for (const std::string dir : {"X", "Y"}) {
          const Dir one = evaluate(f, p, dir);
          const Dir two = upper ? evaluate(2, p, dir) : Dir{};
          if (!one.walls && !two.walls) continue;
          any = true;
          json dj{{"wall_left", one.left}, {"wall_right", one.right}, {"brace_left", sideName(one.l.kind)},
                  {"brace_right", sideName(one.r.kind)}, {"correction", one.corr}, {"A1", one.a}};
          std::string why = one.why.empty() ? two.why : one.why;
          if (!why.empty()) {
            row["reasons"].push_back(dir + "方向：" + why);
            row["directions"][dir] = dj;
            continue;
          }
          if (!isCorner || !upperCorner) {
            row["directions"][dir] = dj;
            continue;
          }
          const double b1 = *isCorner ? 0.8 : 0.5, h1 = heightFactor(one.walls ? one.heightMm : two.heightMm);
          double n = 0, l = 0;
          if (upper) {
            const double b2 = *upperCorner ? 0.8 : 0.5, h2 = two.walls ? heightFactor(two.heightMm) : 1.0;
            l = *isCorner ? 1.0 : 1.6;
            n = (one.a * b1) * h1 + (two.a * b2) * h2 - l;
            dj["A2"] = two.a;
            dj["B2"] = b2;
            dj["H2/2.7"] = h2;
          } else {
            l = *isCorner ? 0.4 : 0.6;
            n = (one.a * b1) * h1 - l;
          }
          dj["B1"] = b1;
          dj["H1/2.7"] = h1;
          dj["L"] = l;
          dj["N"] = n;
          row["directions"][dir] = dj;
          best = std::max(best, n);
        }
        if (!any) continue;  // no bearing wall at this column: not one the N value is asked of
        row["corner"] = isCorner ? json(*isCorner) : json(nullptr);
        row["second_floor_above"] = upper;
        if (!row["reasons"].empty() || best < -1e299) {
          row["status"] = "not_evaluated";
          ++notEvaluated;
        } else if (const auto j = jointOf(best)) {
          row["status"] = "evaluated";
          row["N"] = best;
          row["joint"] = j->mark;
          row["joint_name"] = j->name;
          const bool through = key.find("通し柱") != std::string::npos;
          row["head"] = through && f == 1 && storeys == 2 ? "通し柱（接合なし）" : j->mark;
          row["foot"] = through && f == 2 ? "通し柱（接合なし）" : j->mark;
          ++counts[j->mark];
          ++evaluatedColumns;
        } else {
          row["status"] = "not_evaluated";
          row["N"] = best;
          row["reasons"].push_back("N値が5.6を超えます（告示1460号 表三の範囲外。構造計算等で確かめます）");
          ++notEvaluated;
        }
        out["columns"].push_back(row);
      }
    }
    out["status"] = notEvaluated == 0 && evaluatedColumns > 0 ? "evaluated" : evaluatedColumns > 0 ? "partial" : "not_evaluated";
    out["evaluated"] = evaluatedColumns;
    out["not_evaluated"] = notEvaluated;
    out["joints"] = counts;
    out["notes"] = {"一つの柱で X・Y 両方向の N値を求め、大きい方で接合方法を選ぶ（マニュアル図3-19）。",
                    "壁倍率は壁量の計算の仕様の倍率（高さによる低減の前）。準耐力壁等は倍率 1.5 以下を 0 とする（同 p.101 ※）。",
                    "H は壁量の計算の壁の横架材の上端間の高さ（3.2 m 以下は 2.7 m）。",
                    "出隅は壁量の計算の各階の平面の範囲（長方形）の角。長方形でない平面の出隅は判定していない。",
                    "金物の製品は選んでいない。告示1460号 表三の接合方法と同等以上の金物（Zマーク金物・同等認定金物等）を使う。"};
    return R(out.dump());
  } catch (const std::exception& e) {
    return R::failure(std::string("N値計算：") + e.what());
  }
}
}  // namespace sbr::construction
