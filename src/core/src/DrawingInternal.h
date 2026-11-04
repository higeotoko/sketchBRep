#pragma once
// Shared by the drawing writers (not public).

#include <array>
#include <vector>

#include "sbr/draw/Drawing.h"

namespace sbr::draw::detail {

// A fill (outer loop and holes) as triangles.
std::vector<std::array<gp_Pnt2d, 3>> triangulate(const Item& fill);
// An arc's point at angle a.
gp_Pnt2d arcPoint(const Item& arc, double a);
// The points a text's base line starts and ends at (left to right, on its
// angle), its width taken from its characters (full width = height).
void textBaseline(const Item& text, double scale, gp_Pnt2d& start, gp_Pnt2d& end);

}  // namespace sbr::draw::detail
