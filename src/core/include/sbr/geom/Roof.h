// A roof as one solid from a plan outline, a pitch and overhangs (docs/90 G-08, docs/99).
// Gable (切妻), hip (寄棟) and shed (片流れ) over outlines whose corners are all right angles.
#pragma once

#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

struct RoofSpec {
  std::string kind = "gable";   // gable | hip | shed
  std::vector<gp_Pnt> outline;  // plan corners (z ignored), closed implicitly: the lines the roof bears on
  double height = 0.0;          // z of the roof's underside above the outline (the top of the walls / plates)
  double pitch = 4.0;           // rise per 10 of run (寸勾配)
  double eave = 600.0;          // overhang beyond the outline at eaves, in plan (軒の出)
  double gableEave = 300.0;     // overhang at gable ends, in plan (けらばの出); shed: at the two sloping sides
  double thickness = 100.0;     // across the roof, at right angles to its slope
  // gable over one rectangle: ridge along its "long" or "short" side (several rectangles: always long).
  std::string ridge = "long";
  // shed: the plan direction the roof falls towards (snapped to the outline's nearer side direction).
  double fallX = 0.0, fallY = -1.0;
};

struct RoofShape {
  TopoDS_Shape solid;
  double planArea = 0.0;     // of the roof seen from above, overhangs included (mm2)
  double slopeArea = 0.0;    // of its upper surface (mm2)
  double ridgeHeight = 0.0;  // z of its highest point
  double eaveHeight = 0.0;   // z of the lowest edge of its underside
  int parts = 0;             // rectangles the outline was covered with
};

// Refused with the reason when the outline has a corner that is not a right angle, crosses
// itself, or a size is not usable.
Result<RoofShape> makeRoof(const RoofSpec& spec);

// Everything above the roof's underside, within its plan (overhangs included), for `up` mm
// upwards: what a wall under the roof loses to stand up to it.
Result<TopoDS_Shape> roofSpaceAbove(const RoofSpec& spec, double up);

}  // namespace sbr::geom
