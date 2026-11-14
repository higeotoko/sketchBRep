// A stair from a walking line in plan, a width and the height it climbs (docs/90 G-09,
// docs/103). Straight flights, right-angled turns as landings or winders (回り段: the corner
// square cut into triangular treads from its inner corner), U-turns as two turns.
#pragma once

#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

struct StairSpec {
  // The walking line in plan (z ignored): the middle of the width, from the foot of the first
  // riser to the edge of the upper floor. Its turns are right angles.
  std::vector<gp_Pnt> path;
  double base = 0.0;     // z of the floor it starts from
  double rise = 2800.0;  // floor to floor
  double width = 800.0;
  // Number of risers; 0 = as few as keep each riser within riserLimit (so it changes with the
  // rise).
  int risers = 0;
  double riserLimit = 200.0;
  // Per turn, in order: the treads its corner square is cut into. 1 = a landing (flat), 2 = two
  // triangles (45 degrees each), 3 = 30 degrees each (triangle, kite, triangle), up to 6.
  // Fewer entries than turns: the last one repeats (none: 3).
  std::vector<int> turns;
  std::string style = "block";  // block: one stepped solid | boards: tread boards only
  double thickness = 30.0;      // of a tread board; block: of the solid under each riser line
};

struct StairTread {
  std::vector<gp_Pnt> outline;  // in plan, at the height of its top
  std::string kind;             // tread | winder | landing
  double going = 0.0;           // tread: along the walking line; winder: 300 from its narrow end (0: not measurable)
};

// One line of the comparison with the sizes for a house's stair (建築基準法施行令 第 23 条・
// 第 24 条; see docs/103 for the basis and its date). state: ok | ng | not_evaluated.
struct StairCheck {
  std::string item;  // riser | going | winder_going | width | landing | handrail
  double value = 0.0, limit = 0.0;
  std::string state;
};

struct StairShape {
  TopoDS_Shape solid;  // block: one solid; boards: a compound of boards
  int risers = 0;
  double riser = 0.0;
  std::vector<StairTread> treads;  // bottom to top; the upper floor is the last riser's tread
  double goingMin = 0.0, goingMax = 0.0;  // of straight treads (0: none)
  double winderGoing = 0.0;               // smallest of the winders (0: none, or not measurable)
  double walkLength = 0.0;                // of the walking line
  int flights = 0, landings = 0, winders = 0;
  std::vector<StairCheck> checks;
};

// Refused with the reason when a turn is not a right angle, a part of the line is shorter
// than the width allows, or the risers do not fit the line.
Result<StairShape> makeStair(const StairSpec& spec);

// What the stair needs open in a floor whose underside is at `underside` and top at `top`:
// the plan of every tread with less than `headroom` above it, as one solid through the floor.
// Refused when no tread needs it.
Result<TopoDS_Shape> stairOpening(const StairSpec& spec, double underside, double top, double headroom);

}  // namespace sbr::geom
