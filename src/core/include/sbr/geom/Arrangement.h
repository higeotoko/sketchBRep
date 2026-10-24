#pragma once

// Regions enclosed by crossing straight lines (SketchUp: lines that cross
// make faces where they close an area, not only end to end).

#include <utility>
#include <vector>

#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

namespace sbr::geom {

struct LineArrangement {
  // An input segment between two neighbouring crossings / ends on it.
  struct Piece {
    int segment = 0;  // index of the input segment
    gp_Pnt a, b;
    bool bounding = false;  // an edge of one of the regions
  };
  std::vector<Piece> pieces;
  // Enclosed areas that have at least one edge on a "fresh" segment:
  // corner points, counter-clockwise about the plane normal, no repeats.
  std::vector<std::vector<gp_Pnt>> regions;
};

// Cuts the segments (all lying in `plane`) at every crossing and touching
// end, drops dangling tails, and returns the smallest enclosed areas.
// Overlapping collinear segments count once.
LineArrangement arrangeLines(const std::vector<std::pair<gp_Pnt, gp_Pnt>>& segments, const std::vector<bool>& fresh,
                             const gp_Pln& plane);

}  // namespace sbr::geom
