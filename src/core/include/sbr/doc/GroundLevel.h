#pragma once

// docs/288: 地盤面 (Building Standards Act Enforcement Order art. 2 (2), read on e-Gov 2026-11-26): the
// horizontal plane at the average height where the building meets the surrounding ground; where
// that height differs by more than 3 m, the average within each 3 m. Worked out from the measured
// contact perimeter (set_site_ground), each edge's height taken as a straight line between its points.

#include "sbr/doc/Document.h"
#include "sbr/draw/Drawing.h"

#include <optional>
#include <string>

namespace sbr::doc {

inline constexpr const char* kGroundLevelLaw = "建築基準法施行令 第2条第2項（e-Gov で 2026-11-26 に確認）";

struct GroundLevelStudy {
  // 未入力: no contact perimeter; 要確認: the perimeter, its points or the building's placement do
  // not hold; 算出: one ground level (height difference 3 m or less); 区分が必要: more than 3 m, the
  // division into parts is the designer's and each part's level is not worked out here.
  std::string state = "未入力";
  std::string problem;
  std::optional<SiteGroundReport> report;
  std::string datum, source;
  double lowMm = 0.0, highMm = 0.0;  // lowest and highest point, from the datum
  double levelMm = 0.0;              // the average (the 地盤面 when state is 算出; a reference otherwise)
  // docs/293: in the model's heights, when the datum's Z is given; and the level named GL, when there is one.
  std::optional<double> levelZ;
  std::optional<double> glZ;
  double rangeMm() const { return highMm - lowMm; }
};

GroundLevelStudy groundLevelStudy(const Document& document);

// 地盤面算出表: per edge the horizontal length, the heights at its ends, their average and length x
// average; the total, the result and the basis in the notes.
draw::Table groundLevelTable(const GroundLevelStudy& study);

}  // namespace sbr::doc
