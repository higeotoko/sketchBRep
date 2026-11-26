#pragma once

// docs/310: 構造詳細図（柱の小径・有効細長比）. The timber columns of the model (take_off roles column and
// through_column), grouped by section and the vertical distance between the horizontal members they meet
// (横架材の相互間の垂直距離: a column's length; a through column's longest part between two storeys' floor
// levels), with the ratio of the smaller side to that distance and the effective slenderness ratio
// (座屈長さ / 最小二次率半径, the distance taken as the buckling length) against 令第43条第6項 (150 以下).
// The ratio 令第43条第1項 asks for is set by a notice of the minister and is not compared here (未評価).

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
draw::Table columnTable(const std::vector<ColumnRow>& rows);

inline constexpr char kColumnArticleSource[] = "https://laws.e-gov.go.jp/law/325CO0000000338";
inline constexpr char kColumnArticleCheckedOn[] = "2026-11-27";

}  // namespace sbr::doc
