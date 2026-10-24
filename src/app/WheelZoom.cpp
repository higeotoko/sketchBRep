#include "WheelZoom.h"

#include <algorithm>

#include <gp_Vec.hxx>

namespace sbr::app::wheel {

double notches(const QPoint& angleDelta, const QPoint& pixelDelta) {
  if (angleDelta.y() != 0) return angleDelta.y() / kAngleUnitsPerNotch;
  if (angleDelta.x() == 0 && pixelDelta.y() != 0) return pixelDelta.y() / kPixelsPerNotch;
  return 0.0;  // horizontal wheel, or an empty begin / end event
}

Pose zoomPerspective(const gp_Pnt& eye, const gp_Pnt& center, const gp_Pnt& anchor, double ratio) {
  Pose pose{eye, center, 1.0, 1.0};
  const double distance = eye.Distance(anchor);
  if (distance < 1e-9 || !(ratio > 0.0)) return pose;
  const double wanted = std::clamp(distance / ratio, std::min(kMinDistance, distance),
                                   std::max(kMaxDistance, distance));
  const gp_Vec move = gp_Vec(eye, anchor) * (1.0 - wanted / distance);
  pose.eye = eye.Translated(move);
  pose.center = center.Translated(move);
  pose.applied = distance / wanted;
  return pose;
}

Pose zoomOrthographic(const gp_Pnt& eye, const gp_Pnt& center, const gp_Dir& direction, double scale,
                      const gp_Pnt& anchor, double ratio) {
  Pose pose{eye, center, scale, 1.0};
  if (!(scale > 0.0) || !(ratio > 0.0)) return pose;
  const double wanted = std::clamp(scale / ratio, std::min(kMinOrthoScale, scale), std::max(kMaxOrthoScale, scale));
  const double applied = scale / wanted;
  // Screen position of the anchor ~ its offset across the view / scale.
  gp_Vec offset(center, anchor);
  offset -= gp_Vec(direction) * offset.Dot(gp_Vec(direction));
  const gp_Vec shift = offset * (1.0 - 1.0 / applied);
  pose.eye = eye.Translated(shift);
  pose.center = center.Translated(shift);
  pose.scale = wanted;
  pose.applied = applied;
  return pose;
}

}  // namespace sbr::app::wheel
