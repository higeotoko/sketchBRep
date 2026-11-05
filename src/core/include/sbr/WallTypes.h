#pragma once
#include <string>
#include <vector>

#include "sbr/Types.h"

namespace sbr::construction {
// Wall build-ups for timber walls drawn on the column centre line (柱心): the layers outside to
// inside around one core (the column / stud width), so the wall's faces sit where the finishes
// are, not centred on the line. The thicknesses are usual values to draw with (目安), not a
// product's or a specification's; the core width follows the frame's columns (105 or 120).
struct WallLayer {
  std::string name;
  double thickness = 0.0;
  std::string role;  // core | finish | other
};

struct WallType {
  std::string id;    // e.g. "exterior_siding"
  std::string name;  // shown to the user (Japanese)
  bool exterior = false;
  std::vector<WallLayer> layers;  // outside -> inside, exactly one core
  double core() const;
  double outside() const;  // from the core's centre (the drawn line) to the outer face
  double inside() const;   // from the core's centre to the inner face
  double thickness() const { return outside() + inside(); }
  std::string layersJson() const;  // [{"name","thickness","role"}, ...] outside -> inside
};

// The presets, with a 105 core.
const std::vector<WallType>& wallTypes();
// One preset with the given core width (column size, mm).
Result<WallType> wallType(const std::string& id, double core = 105.0);
}  // namespace sbr::construction
