#include "WheelZoom.h"

#include <algorithm>
#include <cmath>

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

Pose zoomThroughSurface(const gp_Pnt& eye, const gp_Pnt& center, const gp_Pnt& anchor,
                        const gp_Dir& approach, double ratio) {
  Pose pose{eye, center, 1.0, 1.0};
  if (!(ratio > 0.0) || !std::isfinite(ratio)) return pose;
  // Signed distance: after crossing, the old anchor lies behind the eye.
  // Using eye->anchor as the direction there would pull the camera back.
  const double distance = gp_Vec(eye, anchor).Dot(gp_Vec(approach));
  const auto coordinate = [](double d) {
    // Once past the surface, cap speed at the existing empty-space depth
    // scale. Continued scrolling inside must not shoot through the house.
    if (d < -kMinVirtualDepth)
      return -(1.0 + std::log(kMinVirtualDepth / kMinDistance) + (-d - kMinVirtualDepth) / kMinVirtualDepth);
    const double magnitude = std::abs(d) / kMinDistance;
    return magnitude <= 1.0 ? d / kMinDistance : std::copysign(1.0 + std::log(magnitude), d);
  };
  const double before = coordinate(distance);
  const double limit = std::max(kMaxDistance, std::abs(distance));
  const double after = std::clamp(before - std::log(ratio), coordinate(-limit), coordinate(limit));
  const double magnitude = std::abs(after);
  const double exitZone = 1.0 + std::log(kMinVirtualDepth / kMinDistance);
  const double wanted = after < -exitZone ? -(kMinVirtualDepth + (-after - exitZone) * kMinVirtualDepth)
      : magnitude <= 1.0 ? after * kMinDistance
      : std::copysign(kMinDistance * std::exp(magnitude - 1.0), after);
  const gp_Vec move = gp_Vec(approach) * (distance - wanted);
  pose.eye = eye.Translated(move);
  pose.center = center.Translated(move);
  pose.applied = std::exp(before - after);
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
