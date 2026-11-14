// Rooms from walls in plan (docs/90 G-07, docs/98): the closed regions that wall lines
// enclose. Plain 2D geometry, no document: the caller gives the walls of one storey.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace sbr::geom {

struct PlanPoint {
  double x = 0.0, y = 0.0;
};

// A straight wall in plan: the line it was drawn on (as long as the wall is) and how far the
// wall reaches to the right and to the left of it, looking from `a` to `b`.
struct PlanWall {
  PlanPoint a, b;
  double right = 0.0, left = 0.0;
};

// One closed region.
struct RoomRegion {
  // The outline on the wall lines (壁芯), counter-clockwise, and the outlines of walls standing
  // free inside it (clockwise).
  std::vector<PlanPoint> centre;
  std::vector<std::vector<PlanPoint>> centreHoles;
  double centreArea = 0.0;  // mm2, holes taken out
  // The outline on the wall faces (内法) and its area. Not given (`innerKnown` false) when the
  // faces do not make a simple outline (walls thicker than the room is wide, ...): never a guess.
  bool innerKnown = false;
  std::vector<PlanPoint> inner;
  std::vector<std::vector<PlanPoint>> innerHoles;
  double innerArea = 0.0;
  double innerPerimeter = 0.0;  // of the outer inner outline
  std::vector<std::size_t> walls;  // indices of the walls around it (and of those inside it)
};

struct RoomRegions {
  std::vector<RoomRegion> regions;
  // Wall ends that meet no other wall (where an outline may be open).
  std::vector<PlanPoint> looseEnds;
};

// Every closed region of `walls`. A wall end counts as meeting another wall when it reaches
// that wall's band (butt joints, corners, T joints); crossing walls divide each other.
RoomRegions roomRegions(const std::vector<PlanWall>& walls);

// The region `p` is in (on its wall lines' outline, not in a hole), if any.
std::optional<std::size_t> regionAt(const RoomRegions& regions, const PlanPoint& p);

}  // namespace sbr::geom
