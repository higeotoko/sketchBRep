#pragma once

// docs/308: 基礎・地盤説明書. The ground survey's results as the person gives them (method, date, soil, the
// long-term allowable bearing, improvement, frost depth, the source) and the foundation the model has
// (the generated foundation plans: kind, stem, slab or footing, how deep under GL), set side by side with
// the figures of 平成12年建設省告示第1347号 (基礎の構造方法). The comparison is with the notice's figures
// only: it is not a structural calculation, and what the model does not hold (rebar) is 未評価.

#include <optional>
#include <string>
#include <vector>

#include "sbr/draw/Drawing.h"

namespace sbr::doc {
class Document;

struct FoundationGround {
  std::string surveyMethod;  // e.g. スクリューウエイト貫入試験
  std::string surveyDate;    // as written, e.g. 2026-10-01
  std::string soil;          // 土質・地盤の種類
  std::optional<double> allowableKNm2;  // 地盤の長期許容応力度 (kN/m²), improved ground: after improvement
  std::string basis;         // how it was found, e.g. 平成13年国交告第1113号第2 (3) 式
  std::string improvement;   // 地盤改良: empty = not given, "なし" or the method
  std::optional<double> frostDepthMm;
  std::string source;        // the survey report
  bool empty() const {
    return surveyMethod.empty() && surveyDate.empty() && soil.empty() && !allowableKNm2 && basis.empty() &&
           improvement.empty() && !frostDepthMm && source.empty();
  }
};

FoundationGround foundationGroundOf(const Document& document);

inline constexpr char kFoundationNoticeSource[] = "https://www.mlit.go.jp/notice/noticedata/pdf/201703/00006441.pdf";
inline constexpr char kFoundationNoticeCheckedOn[] = "2026-11-27";

// One row of the comparison: what, what the model / input says, the notice's figure, and the result:
// 規定内 / 規定外 / 未入力 / 未評価 / 対象外 / "" (no figure to compare with).
struct FoundationRow {
  std::string part, item, value, rule, result;
};
struct FoundationGroundStudy {
  FoundationGround ground;
  std::vector<FoundationRow> rows;
  bool hasFoundation = false;
};
FoundationGroundStudy foundationGroundStudy(const Document& document);
draw::Table foundationGroundTable(const FoundationGroundStudy& study);

}  // namespace sbr::doc
