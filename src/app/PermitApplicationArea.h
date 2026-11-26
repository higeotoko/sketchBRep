#pragma once

#include "sbr/doc/PermitIntake.h"
#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <QString>

namespace sbr::app {

// Shared by the legal forms and the city's fee worksheet, which references the
// application portion of the gross floor area in Form 2, third face, item 11(a).
inline bool hasApplicationFloorArea(const doc::AreaTable& area) { return doc::applicationFloorAreaKnown(area); }

inline QString permitSquareMetres(double mm2) {
  return QString::number(mm2 / 1'000'000.0, 'f', 2) + QStringLiteral(" ㎡");
}

}  // namespace sbr::app
