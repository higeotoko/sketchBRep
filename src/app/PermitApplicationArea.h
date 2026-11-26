#pragma once

#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <QString>

namespace sbr::app {

// Shared by the legal forms and the city's fee worksheet, which references the
// application portion of the gross floor area in Form 2, third face, item 11(a).
inline bool hasApplicationFloorArea(const doc::AreaTable& area) {
  return area.legalTotal > 0 &&
      std::all_of(area.rows.begin(), area.rows.end(), [](const doc::AreaRow& floor) {
        return floor.legalKnown && floor.unreadableWalls == 0;
      });
}

inline QString permitSquareMetres(double mm2) {
  return QString::number(mm2 / 1'000'000.0, 'f', 2) + QStringLiteral(" ㎡");
}

}  // namespace sbr::app
