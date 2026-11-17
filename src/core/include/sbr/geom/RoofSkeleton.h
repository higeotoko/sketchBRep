// The faces of a roof over a plan outline with inward corners, by moving every side inwards at
// its own speed (a weighted straight skeleton, docs/164). Plain 2D arithmetic: no kernel.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace sbr::geom {

struct SkeletonPoint {
  double x = 0.0, y = 0.0;
};

// One side of the outline (counter-clockwise: the inside on its left). Its line stands `out`
// outside the side until the height `start`; from there on it moves inwards, 1 / slope for every
// unit of height. A side that is not `moving` stands there for good (a gable, a wall).
struct SkeletonSide {
  SkeletonPoint a, b;
  bool moving = true;
  double slope = 0.4;
  double out = 0.0;
  double start = 0.0;
};

// For every side, the part of the plan its plane roofs (a polygon, counter-clockwise; empty for
// sides that stand still or roof nothing). nullopt, with the reason in `why`, when the outline
// is not one this can work out (the caller then builds the roof another way): the faces do not
// add up to the outline grown by the overhangs, two sides in one line move differently, ...
std::optional<std::vector<std::vector<SkeletonPoint>>> roofSkeletonFaces(const std::vector<SkeletonSide>& sides, std::string* why = nullptr);

}  // namespace sbr::geom
