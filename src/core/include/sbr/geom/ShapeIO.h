#pragma once
// Serialization of B-rep shapes (native BRep text, STEP).

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// OCCT native BRep text without triangulation data (geometry only).
std::string shapeToBRepString(const TopoDS_Shape& shape);
Result<TopoDS_Shape> shapeFromBRepString(const std::string& text);

// STEP AP214 in millimetres.
Status writeStep(const TopoDS_Shape& shape, const std::string& path);
Result<TopoDS_Shape> readStep(const std::string& path);

// A named, optionally coloured part for STEP exchange.
struct NamedShape {
  TopoDS_Shape shape;
  std::string name;
  std::optional<std::array<double, 3>> color;  // RGB 0..1
  std::string layer;                           // tag / Rhino layer (3DM)
};

// STEP with names and colours (XCAF). Each part becomes a product.
Status writeStepParts(const std::vector<NamedShape>& parts, const std::string& path);
// Reads assemblies flattened to world-placed solids, keeping product names
// and surface colours where present.
Result<std::vector<NamedShape>> readStepParts(const std::string& path);

}  // namespace sbr::geom
