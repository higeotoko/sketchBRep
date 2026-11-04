#pragma once
// Geometry of annotations (dimensions, text leaders). Annotations are real
// edges so selection, moving, erasing and snapping work like for curves;
// their text is drawn by the viewport at annotationLabel().

#include <optional>

#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// Linear dimension between start and end, its line moved by 'offset'
// (perpendicular to the measured segment): dimension line, extension lines
// and architectural ticks.
Result<TopoDS_Shape> makeDimensionShape(const gp_Pnt& start, const gp_Pnt& end, const gp_Vec& offset);
// Where the dimension's text goes (middle of the dimension line).
gp_Pnt dimensionTextPoint(const gp_Pnt& start, const gp_Pnt& end, const gp_Vec& offset);

// Text note at 'position', with a leader line from 'target' if given.
Result<TopoDS_Shape> makeTextNoteShape(const gp_Pnt& position, const std::optional<gp_Pnt>& target);

}  // namespace sbr::geom
