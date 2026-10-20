#pragma once
// Serialization of B-rep shapes (native BRep text, STEP).

#include <string>

#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// OCCT native BRep text without triangulation data (geometry only).
std::string shapeToBRepString(const TopoDS_Shape& shape);
Result<TopoDS_Shape> shapeFromBRepString(const std::string& text);

// STEP AP214 in millimetres.
Status writeStep(const TopoDS_Shape& shape, const std::string& path);
Result<TopoDS_Shape> readStep(const std::string& path);

}  // namespace sbr::geom
