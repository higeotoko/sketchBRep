#include "sbr/doc/FoundationGround.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include <Bnd_Box.hxx>
#include <nlohmann/json.hpp>

#include "sbr/doc/Document.h"
#include "sbr/doc/GroundLevel.h"
#include "sbr/doc/Schedules.h"

namespace sbr::doc {

namespace {
using nlohmann::json;

std::string mm(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f mm", v);
  return b;
}
std::string number(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}
std::string atLeast(bool ok) { return ok ? "規定内" : "規定外"; }
}  // namespace

FoundationGround foundationGroundOf(const Document& document) {
  FoundationGround g;
  const json j = json::parse(document.settings().foundationGround, nullptr, false);
  if (!j.is_object()) return g;
  g.surveyMethod = j.value("survey_method", std::string());
  g.surveyDate = j.value("survey_date", std::string());
  g.soil = j.value("soil", std::string());
  if (j.contains("allowable_kn_m2") && j.at("allowable_kn_m2").is_number()) g.allowableKNm2 = j.at("allowable_kn_m2").get<double>();
  g.basis = j.value("basis", std::string());
  g.improvement = j.value("improvement", std::string());
  if (j.contains("frost_depth_mm") && j.at("frost_depth_mm").is_number()) g.frostDepthMm = j.at("frost_depth_mm").get<double>();
  g.source = j.value("source", std::string());
  return g;
}

FoundationGroundStudy foundationGroundStudy(const Document& document) {
  FoundationGroundStudy study;
  study.ground = foundationGroundOf(document);
  const FoundationGround& g = study.ground;
  auto& rows = study.rows;
  const auto given = [](const std::string& s) { return s.empty() ? std::string("未入力") : s; };
  rows.push_back({"地盤", "調査の方法", given(g.surveyMethod), "", g.surveyMethod.empty() ? "未入力" : ""});
  rows.push_back({"地盤", "調査日", given(g.surveyDate), "", g.surveyDate.empty() ? "未入力" : ""});
  rows.push_back({"地盤", "土質・地盤の種類", given(g.soil), "", g.soil.empty() ? "未入力" : ""});
  rows.push_back({"地盤", "長期許容応力度",
                  g.allowableKNm2 ? number(*g.allowableKNm2) + " kN/m²" + (g.basis.empty() ? "" : "（" + g.basis + "）")
                                  : std::string("未入力"),
                  "", g.allowableKNm2 ? "" : "未入力"});
  rows.push_back({"地盤", "地盤改良", given(g.improvement), "", g.improvement.empty() ? "未入力" : ""});
  rows.push_back({"地盤", "凍結深度", g.frostDepthMm ? mm(*g.frostDepthMm) : std::string("未入力"), "",
                  g.frostDepthMm ? "" : "未入力"});
  rows.push_back({"地盤", "調査の資料", given(g.source), "", g.source.empty() ? "未入力" : ""});

  // The ground line (GL): the study's, else the level named GL (not the average ground).
  std::optional<double> gl = groundLevelStudy(document).glZ;
  if (!gl)
    for (const FloorLevel& l : document.settings().levels)
      if (isGroundLevelName(l.name) && l.name.find("平均") == std::string::npos) gl = l.elevation;
  // Storeys (for the strip footing's width) and whether the building is timber.
  std::set<long long> storeyHeights;
  for (const FloorLevel& l : document.settings().levels)
    if (levelIsStorey(document.settings().levels, l)) storeyHeights.insert(std::llround(l.elevation));
  bool timber = false;
  for (const Object& o : document.objects()) {
    const auto c = o.attributes.find("material_class");
    timber |= c != o.attributes.end() && c->second == "timber";
  }

  for (const Object& root : document.objects()) {
    const auto stored = root.attributes.find("foundation_plan");
    if (stored == root.attributes.end()) continue;
    const json manifest = json::parse(stored->second, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("inputs")) continue;
    const json& in = manifest.at("inputs");
    const std::string type = in.value("type", std::string("strip"));
    const std::string part = root.name.empty() ? std::string("基礎") : root.name;
    if (type == "pads") {
      rows.push_back({part, "種類", "束石・独立基礎（床束の下）", "", "対象外"});
      continue;
    }
    study.hasFoundation = true;
    const bool mat = type == "mat";
    double bottom = 1e300;
    if (manifest.contains("parts") && manifest.at("parts").is_object())
      for (const auto& [id, stamp] : manifest.at("parts").items()) {
        (void)stamp;
        const Object* p = document.find(id);
        if (!p) continue;
        const Bnd_Box b = document.worldBounds(*p);
        if (b.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        bottom = std::min(bottom, z0);
      }
    const double top = manifest.value("top_z", 0.0);
    rows.push_back({part, "種類", mat ? "べた基礎（鉄筋コンクリート造）" : "布基礎（鉄筋コンクリート造）",
                    "第1：30 kN/m² 以上は布基礎も可、20 以上 30 未満は基礎ぐい又はべた基礎、20 未満は基礎ぐい",
                    !g.allowableKNm2 ? "未入力"
                    : mat            ? atLeast(*g.allowableKNm2 >= 20.0)
                                     : atLeast(*g.allowableKNm2 >= 30.0)});
    const double stemWidth = in.value("stem_width", 150.0);
    rows.push_back({part, "立上りの厚さ", mm(stemWidth), "第3項三号：120 mm 以上", atLeast(stemWidth >= 120.0 - 1e-6)});
    if (gl)
      rows.push_back({part, "立上りの地上部分の高さ", mm(top - *gl) + "（GL から立上りの天端まで）", "第3項三号：300 mm 以上",
                      atLeast(top - *gl >= 300.0 - 1e-6)});
    else
      rows.push_back({part, "立上りの地上部分の高さ", "GL が未入力", "第3項三号：300 mm 以上", "未入力"});
    const double slab = mat ? in.value("slab_thickness", 150.0) : in.value("footing_thickness", 150.0);
    const double minSlab = mat ? 120.0 : 150.0;
    rows.push_back({part, "底盤の厚さ", mm(slab), mat ? "第3項三号：120 mm 以上" : "第4項一号：150 mm 以上",
                    atLeast(slab >= minSlab - 1e-6)});
    const double minDepth = mat ? 120.0 : 240.0;
    const std::string depthRule = std::string(mat ? "第3項四号" : "第4項一号") + "：" + mm(minDepth) +
                                  " 以上（底部が雨水等の影響を受けない密実で良好な地盤に達する場合を除く）";
    if (gl && bottom < 1e299) {
      const double depth = *gl - bottom;
      rows.push_back({part, "根入れの深さ", mm(depth) + "（GL から基礎の底まで）", depthRule, atLeast(depth >= minDepth - 1e-6)});
      rows.push_back({part, "凍結深度との比較", mm(depth), "第3項四号：凍結深度よりも深くする（又は凍上を防ぐ措置）",
                      g.frostDepthMm ? atLeast(depth >= *g.frostDepthMm - 1e-6) : "未入力"});
    } else {
      rows.push_back({part, "根入れの深さ", gl ? "基礎の形が読めません" : "GL が未入力", depthRule, "未入力"});
    }
    if (!mat) {
      const double width = in.value("footing_width", 450.0);
      std::string rule = "第4項二号の表（木造等）";
      std::string result = "未入力";
      const std::size_t storeys = storeyHeights.size();
      if (g.allowableKNm2 && timber && (storeys == 1 || storeys == 2)) {
        const double q = *g.allowableKNm2;
        double need = 0;
        if (q >= 70) need = storeys == 1 ? 180 : 240;
        else if (q >= 50) need = storeys == 1 ? 240 : 360;
        else if (q >= 30) need = storeys == 1 ? 300 : 450;
        if (need > 0) {
          rule += "：" + std::string(storeys == 1 ? "平家建て" : "二階建て") + "・" + number(q) + " kN/m² で " + mm(need) + " 以上";
          result = atLeast(width >= need - 1e-6);
        } else {
          rule += "：30 kN/m² 未満は布基礎にできません";
          result = "規定外";
        }
      } else if (g.allowableKNm2) {
        rule += "：木造の平家建て・二階建て以外は未評価";
        result = "未評価";
      }
      rows.push_back({part, "底盤の幅", mm(width), rule, result});
    }
    rows.push_back({part, "配筋", "モデルに配筋の入力がありません",
                    mat ? "第3項五号：立上り主筋 D12 以上上下各1本、補強筋 9 mm 以上 @300 以下、底盤 9 mm 以上縦横 @300 以下"
                        : "第4項一号（第3項五号イ・ロ・ニ）・三号",
                    "未評価"});
  }
  if (!study.hasFoundation)
    rows.push_back({"基礎", "種類", "モデルに基礎の計画がありません（generate_foundation）", "", "未入力"});
  return study;
}

draw::Table foundationGroundTable(const FoundationGroundStudy& study) {
  draw::Table t;
  t.title = "基礎・地盤説明書";
  t.headers = {"区分", "項目", "内容", "平成12年建設省告示第1347号", "照合"};
  for (const auto& r : study.rows) t.rows.push_back({r.part, r.item, r.value, r.rule, r.result});
  t.notes = {
      "地盤の欄は入力された調査結果、基礎の欄はモデルの基礎計画（generate_foundation）の寸法です。",
      "照合は告示の数値との比較だけです。構造計算・地盤の安全を確かめたものではありません。配筋はモデルに無いため未評価です。",
      std::string("告示の本文：") + kFoundationNoticeSource + "（" + kFoundationNoticeCheckedOn + " 確認。その後の改正の有無は別途確認）",
  };
  return t;
}

std::optional<draw::View> foundationDetailView(const Document& document) {
  using draw::Item;
  using draw::Pen;
  const json* chosen = nullptr;
  json manifest;
  for (const Object& root : document.objects()) {
    const auto stored = root.attributes.find("foundation_plan");
    if (stored == root.attributes.end()) continue;
    manifest = json::parse(stored->second, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("inputs")) continue;
    if (manifest.at("inputs").value("type", std::string("strip")) == "pads") continue;
    chosen = &manifest;
    break;
  }
  if (!chosen) return std::nullopt;
  const json& in = manifest.at("inputs");
  const bool mat = in.value("type", std::string("strip")) == "mat";
  const double top = manifest.value("top_z", 0.0);
  const double sw = in.value("stem_width", 150.0), sh = in.value("stem_height", mat ? 400.0 : 600.0);
  const double base = top - sh;  // the stem's bottom: the slab's top (mat) or the footing's top (strip)
  const double packing = in.value("packing_mm", 20.0);
  // The sill on the stem: its section from the first source sill, else none drawn.
  double sillW = 0, sillD = 0;
  const Object* source = nullptr;
  if (in.contains("sources") && in.at("sources").is_array() && !in.at("sources").empty() && in.at("sources")[0].is_string())
    source = document.find(in.at("sources")[0].get<std::string>());
  // docs/316: a plan made from a frame: a sill of that frame.
  if (!source && in.contains("frame") && in.at("frame").is_string()) {
    const std::string frame = in.at("frame").get<std::string>();
    for (const Object& o : document.objects()) {
      const auto kind = o.attributes.find("kind");
      if (kind != o.attributes.end() && kind->second == "sill" && document.isAncestor(frame, o.id)) {
        source = &o;
        break;
      }
    }
  }
  {
    if (const Object* sill = source) {
      const Bnd_Box b = document.worldBounds(*sill);
      if (!b.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        sillW = std::min(x1 - x0, y1 - y0);
        sillD = z1 - z0;
      }
    }
  }
  std::optional<double> gl = groundLevelStudy(document).glZ;
  if (!gl)
    for (const FloorLevel& l : document.settings().levels)
      if (isGroundLevelName(l.name) && l.name.find("平均") == std::string::npos) gl = l.elevation;

  draw::View view;
  view.spec.kind = draw::ViewKind::Section;
  // 構造詳細図 by name: the intake check sheet (docs/286) looks for the drawing by it.
  view.spec.title = std::string("構造詳細図（") + (mat ? "べた基礎" : "布基礎") + "の断面）";
  const auto P = [](double u, double v) { return gp_Pnt2d(u, v); };
  const auto concrete = [&](std::vector<gp_Pnt2d> loop) {
    Item fill;
    fill.kind = Item::Kind::Fill;
    fill.pen = Pen::Fill;
    fill.points = loop;
    view.items.push_back(fill);
    Item outline;
    outline.kind = Item::Kind::Polyline;
    outline.pen = Pen::Cut;
    outline.closed = true;
    outline.points = std::move(loop);
    view.items.push_back(std::move(outline));
  };
  const auto line = [&](Pen pen, gp_Pnt2d a, gp_Pnt2d b) {
    Item l;
    l.kind = Item::Kind::Line;
    l.pen = pen;
    l.points = {a, b};
    view.items.push_back(std::move(l));
  };
  const auto note = [&](gp_Pnt2d at, const std::string& text, int hAlign = 0) {
    Item t;
    t.kind = Item::Kind::Text;
    t.pen = Pen::Text;
    t.text = text;
    t.height = 2.5;
    t.hAlign = hAlign;
    t.vAlign = 1;
    t.points = {at};
    view.items.push_back(std::move(t));
  };
  const auto dim = [&](gp_Pnt2d a, gp_Pnt2d b, gp_Vec2d offset) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f", a.Distance(b));
    view.dimensions.push_back({a, b, offset, text});
  };
  // The outer stem centred on u = 0; the inside (the slab, the floor) to the right; outside to the left.
  const double inside = 900.0;  // how much of the slab / footing is shown inwards, ended with a break line
  double bottom = base;
  if (mat) {
    const double slab = in.value("slab_thickness", 150.0);
    const double ew = in.value("edge_width", 300.0), ed = in.value("edge_depth", 150.0);
    std::vector<gp_Pnt2d> loop{P(-sw / 2, top), P(sw / 2, top), P(sw / 2, base), P(inside, base), P(inside, base - slab)};
    if (ed > 0) {
      loop.push_back(P(ew / 2, base - slab));
      loop.push_back(P(ew / 2, base - slab - ed));
      loop.push_back(P(-ew / 2, base - slab - ed));
      loop.push_back(P(-ew / 2, base - slab));
    }
    loop.push_back(P(-sw / 2, base - slab));
    concrete(loop);
    bottom = base - slab - std::max(0.0, ed);
    dim(P(inside, base), P(inside, base - slab), gp_Vec2d(250, 0));
    if (ed > 0) {
      dim(P(-ew / 2, bottom), P(ew / 2, bottom), gp_Vec2d(0, -250));
      dim(P(-ew / 2, base - slab), P(-ew / 2, bottom), gp_Vec2d(-350, 0));
    }
    note(P(inside / 2 + sw / 4, base - slab / 2), "耐圧盤（鉄筋コンクリート造）", 1);
  } else {
    const double fw = in.value("footing_width", 450.0), ft = in.value("footing_thickness", 150.0);
    concrete({P(-sw / 2, top), P(sw / 2, top), P(sw / 2, base), P(fw / 2, base), P(fw / 2, base - ft), P(-fw / 2, base - ft),
              P(-fw / 2, base), P(-sw / 2, base)});
    bottom = base - ft;
    dim(P(-fw / 2, bottom), P(fw / 2, bottom), gp_Vec2d(0, -250));
    dim(P(fw / 2, base), P(fw / 2, bottom), gp_Vec2d(250, 0));
    note(P(fw / 2 + 80, base - ft / 2), "底盤（鉄筋コンクリート造）");
  }
  dim(P(-sw / 2, top), P(sw / 2, top), gp_Vec2d(0, 250));
  dim(P(sw / 2, top), P(sw / 2, base), gp_Vec2d(mat ? 1200 : 700, 0));
  note(P(sw / 2 + 80, (top + base) / 2), "立上り（鉄筋コンクリート造）");
  // Packing and the sill above it.
  if (packing > 0) {
    concrete({P(-sw / 2 + 10, top), P(sw / 2 - 10, top), P(sw / 2 - 10, top + packing), P(-sw / 2 + 10, top + packing)});
    // Above the sill, clear of the dimensions' extension lines at the stem's top.
    note(P(-sw / 2 - 80, top + packing + std::max(sillD, 0.0) + 90), "基礎パッキン " + mm(packing), 2);
  }
  if (sillW > 0 && sillD > 0) {
    Item sill;
    sill.kind = Item::Kind::Polyline;
    sill.pen = Pen::Cut;
    sill.closed = true;
    sill.points = {P(-sillW / 2, top + packing), P(sillW / 2, top + packing), P(sillW / 2, top + packing + sillD),
                   P(-sillW / 2, top + packing + sillD)};
    view.items.push_back(sill);
    line(Pen::Visible, P(-sillW / 2, top + packing), P(sillW / 2, top + packing + sillD));  // 構造材の断面 (×)
    line(Pen::Visible, P(sillW / 2, top + packing), P(-sillW / 2, top + packing + sillD));
    char text[64];
    std::snprintf(text, sizeof text, "土台 %.0f×%.0f", sillW, sillD);
    note(P(-sillW / 2 - 80, top + packing + sillD + 200), text, 2);
  }
  // The ground line and the depths from it.
  if (gl) {
    const double u0 = -sw / 2 - 900, u1 = inside + 150;
    line(Pen::Ground, P(u0, *gl), P(u1, *gl));
    for (double u = u0 + 60; u + 80 < -sw / 2 - 100; u += 160) line(Pen::Visible, P(u + 80, *gl), P(u, *gl - 80));
    note(P(u0, *gl + 60), "GL", 0);
    dim(P(-sw / 2, *gl), P(-sw / 2, top), gp_Vec2d(-600, 0));       // 立上りの地上部分
    dim(P(-sw / 2, *gl), P(-sw / 2, bottom), gp_Vec2d(-600, 0));    // 根入れ
  }
  view.calculationNotes.push_back("基礎計画（generate_foundation）の寸法から描いた断面です。配筋・かぶり・定着はモデルに無く、別途とします。");
  view.calculationNotes.push_back("各寸法と平成12年建設省告示第1347号の数値との照合は、基礎・地盤説明書に示します。");
  return view;
}

}  // namespace sbr::doc
