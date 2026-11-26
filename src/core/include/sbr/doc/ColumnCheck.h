#pragma once

// docs/310: 構造詳細図（柱の小径・有効細長比）. The timber columns of the model (take_off roles column and
// through_column), grouped by section and the vertical distance between the horizontal members they meet
// (横架材の相互間の垂直距離: a column's length; a through column's longest part between two storeys' floor
// levels), with the ratio of the smaller side to that distance and the effective slenderness ratio
// (座屈長さ / 最小二次率半径, the distance taken as the buckling length) against 令第43条第6項 (150 以下).
// The ratio 令第43条第1項 asks for is set by a notice of the minister and is not compared here (未評価).

#include <optional>
#include <string>
#include <vector>

#include "sbr/draw/Drawing.h"

namespace sbr::doc {
class Document;

struct ColumnRow {
  std::string role;      // 柱 / 通し柱
  double widthMm = 0, depthMm = 0;
  double distanceMm = 0;  // 横架材間の垂直距離 (taken as the buckling length)
  int count = 0;
  double slenderness = 0;  // distance / (min side / sqrt 12)
};
std::vector<ColumnRow> columnRows(const Document& document);
// docs/315: the longest vertical distance between horizontal members of the columns standing in [z0, z1)
// (a column's part whose middle is in the range; through columns split at the storeys' floor levels), for
// 第五面 ニ (横架材間の垂直距離). Empty when no column stands there.
std::optional<double> clearDistanceIn(const Document& document, double z0, double z1);
draw::Table columnTable(const std::vector<ColumnRow>& rows);

// docs/311: 構造詳細図（仕様）: what 規則第1条の3 表2 asks the 構造詳細図 of a timber building to show, as the
// person gives it (each one line), with the article it answers; the columns' slenderness from columnRows.
struct StructuralSpecs {
  std::string roofing;     // 屋根ふき材の種別 (令39条)
  std::string frameMethod; // 軸組等の構造方法 (令46条)
  std::string joints;      // 継手又は仕口の構造方法 (令47条)
  std::string wallBase;    // 外壁のうち軸組が腐りやすい構造である部分の下地 (令49条1項)
  std::string decay;       // 地面から1m以内の防腐・防蟻措置 (令49条2項)
  std::string source;
  bool empty() const {
    return roofing.empty() && frameMethod.empty() && joints.empty() && wallBase.empty() && decay.empty() && source.empty();
  }
};
StructuralSpecs structuralSpecsOf(const Document& document);
draw::Table structuralSpecTable(const Document& document);

inline constexpr char kColumnArticleSource[] = "https://laws.e-gov.go.jp/law/325CO0000000338";
inline constexpr char kColumnArticleCheckedOn[] = "2026-11-27";

}  // namespace sbr::doc
