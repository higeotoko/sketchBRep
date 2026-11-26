#include "sbr/doc/GroundLevel.h"

#include <algorithm>
#include <cstdio>

namespace sbr::doc {
namespace {

std::string metres3(double mm) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", mm / 1000.0);
  return buf;
}

std::string signedMetres(double mm) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%+.3f m", mm / 1000.0);
  return buf;
}

}  // namespace

GroundLevelStudy groundLevelStudy(const Document& document) {
  GroundLevelStudy out;
  const auto& boundary = document.settings().siteBoundary;
  if (!boundary || !boundary->ground) {
    out.problem = "建物の接地外周と測点の高さが未入力です（敷地・配置図 → 建物の接地外周・高さ）";
    return out;
  }
  const SiteGround& ground = *boundary->ground;
  out.datum = ground.datum;
  out.source = ground.source;
  if (const auto placement = document.checkSiteGroundPlacement(ground); !placement) {
    out.state = "要確認";
    out.problem = placement.message();
    return out;
  }
  const auto report = document.siteGroundReport(ground);
  if (!report || report->lengthMm <= 0 || report->heightsMm.empty()) {
    out.state = "要確認";
    out.problem = report ? std::string("接地外周の長さが 0 です") : report.error();
    return out;
  }
  out.report = *report;
  const auto [low, high] = std::minmax_element(report->heightsMm.begin(), report->heightsMm.end());
  out.lowMm = *low;
  out.highMm = *high;
  out.levelMm = report->averageMm();
  out.state = out.rangeMm() > 3000.0 ? "区分が必要" : "算出";
  if (ground.datumZ) out.levelZ = *ground.datumZ + out.levelMm;
  for (const FloorLevel& l : document.settings().levels)
    if (isGroundLevelName(l.name)) out.glZ = l.elevation;
  if (out.state == "区分が必要")
    out.problem = "接地する位置の高低差が 3 m を超えます。3 m 以内ごとの区分（建物の部分の分け方）は設計者が決め、"
                  "区分ごとの地盤面はここでは算出していません";
  return out;
}

draw::Table groundLevelTable(const GroundLevelStudy& study) {
  draw::Table t;
  t.title = "地盤面算出表";
  t.headers = {"辺", "水平長さ L (m)", "始点高 (m)", "終点高 (m)", "平均高 h (m)", "L×h (m²)"};
  if (!study.report) {
    t.notes = {"地盤面は算出していません：" + study.problem, kGroundLevelLaw};
    return t;
  }
  const SiteGroundReport& r = *study.report;
  const auto label = [](std::size_t i) { return "P" + std::to_string(i + 1); };
  const std::size_t n = r.vertices.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = (i + 1) % n;
    const double a = r.heightsMm[i], b = r.heightsMm[j], length = r.lengthsMm[i];
    const double average = (a + b) / 2.0;
    char area[64];
    std::snprintf(area, sizeof area, "%.3f", length * average / 1e6);
    t.rows.push_back({label(i) + "–" + label(j), metres3(length), metres3(a), metres3(b), metres3(average), area});
  }
  char sum[64];
  std::snprintf(sum, sizeof sum, "%.3f", r.weightedSumMm2 / 1e6);
  t.rows.push_back({"合計", metres3(r.lengthMm), "", "", "", sum});
  const std::string level = signedMetres(study.levelMm);
  char formula[256];
  std::snprintf(formula, sizeof formula, "Σ(L×h) ÷ ΣL ＝ %.3f ÷ %.3f ＝ ", r.weightedSumMm2 / 1e6, r.lengthMm / 1000.0);
  if (study.state == "算出") {
    t.notes.push_back("地盤面 ＝ " + std::string(formula) + level + "（基準点からの高さ）");
  } else {
    t.notes.push_back("全体の平均（参考） ＝ " + std::string(formula) + level);
    t.notes.push_back(study.problem);
  }
  t.notes.push_back("高低差 ＝ 最高 " + signedMetres(study.highMm) + " − 最低 " + signedMetres(study.lowMm) + " ＝ " +
                    metres3(study.rangeMm()) + " m" + (study.rangeMm() > 3000.0 ? "（3 m を超える）" : "（3 m 以下）"));
  if (study.levelZ) {
    char z[160];
    std::snprintf(z, sizeof z, "モデルの高さ：地盤面 Z %.0f mm（基準点 Z %.0f ＋ %.0f）", *study.levelZ, *study.levelZ - study.levelMm,
                  study.levelMm);
    std::string line = z;
    if (study.glZ) {
      char gl[96];
      std::snprintf(gl, sizeof gl, "。レベル GL は Z %.0f mm（差 %+.0f mm）", *study.glZ, *study.glZ - *study.levelZ);
      line += gl;
    }
    t.notes.push_back(line);
  } else {
    t.notes.push_back("基準点のモデルの高さ（Z）が未入力のため、GL・高さの検討との照合はしていません");
  }
  t.notes.push_back("各辺は両端の測点の間を直線とみなし、平均高は両端の平均。合計と地盤面は表示を丸める前の値で計算");
  t.notes.push_back("基準点 " + study.datum + "　確認資料 " + study.source);
  t.notes.push_back(std::string("根拠：") + kGroundLevelLaw + "。建築物が周囲の地面と接する位置の平均の高さにおける水平面");
  return t;
}

}  // namespace sbr::doc
