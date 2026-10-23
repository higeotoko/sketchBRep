#pragma once
// Mouse wheel zoom (docs/17_wheel_zoom.md).
//
// The wheel moves the camera towards (or away from) an anchor: the point of
// the model under the cursor. The anchor stays exactly under the cursor,
// and one notch changes the distance to it by a fixed ratio, so the zoom
// is fast far away and fine close up, and the same total wheel input gives
// the same camera however the device splits it into events:
//
//   notches q = angleDelta.y / 120 (fractions kept; pixelDelta only when a
//               device sends no angleDelta)
//   ratio   r = perNotch ^ q      (= exp(k q), products add up exactly)
//
// Perspective: the eye slides along the line to the anchor (field of view
// unchanged), distance / r. Orthographic: the view scale / r, panned so the
// anchor keeps its screen position. Both are exactly undone by -q.

#include <QPoint>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

namespace sbr::app::wheel {

// Tuning values (provisional; to be matched against SketchUp on the user's
// machine, docs/17_wheel_zoom.md).
inline constexpr double kDefaultRatioPerNotch = 1.2;  // distance to anchor / 1.2 per notch
inline constexpr double kAngleUnitsPerNotch = 120.0;  // Qt: 15 degrees = one notch
inline constexpr double kPixelsPerNotch = 50.0;       // pixel-only devices (touchpads)
inline constexpr double kMinDistance = 10.0;          // mm: closest approach to an anchor
inline constexpr double kMaxDistance = 1.0e8;         // mm: 100 km
inline constexpr double kMinVirtualDepth = 500.0;     // mm: an anchor in empty space stays this far ahead
inline constexpr double kMinOrthoScale = 10.0;        // mm: view height
inline constexpr double kMaxOrthoScale = 1.0e9;
inline constexpr int kGestureGapMs = 400;             // events closer than this are one gesture...
inline constexpr int kGestureMovePx = 3;              // ...while the cursor moves no more (logical px)

// Wheel input in notches (positive: wheel turned away from the user, zoom
// in). Uses angleDelta (fractions kept); pixelDelta only when there is no
// angleDelta, never both.
double notches(const QPoint& angleDelta, const QPoint& pixelDelta);

struct Pose {
  gp_Pnt eye;
  gp_Pnt center;
  double scale = 1.0;  // orthographic view height
  double applied = 1.0;  // ratio actually applied (< asked at a limit)
};

// Perspective: the eye moves along the line to `anchor`, the distance
// divided by `ratio` (clamped to kMinDistance..kMaxDistance); the centre
// moves with it.
Pose zoomPerspective(const gp_Pnt& eye, const gp_Pnt& center, const gp_Pnt& anchor, double ratio);

// Orthographic: the scale divided by `ratio` (clamped), eye and centre
// shifted across the view direction so `anchor` keeps its screen position.
Pose zoomOrthographic(const gp_Pnt& eye, const gp_Pnt& center, const gp_Dir& direction, double scale,
                      const gp_Pnt& anchor, double ratio);

}  // namespace sbr::app::wheel
