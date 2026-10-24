#include "sbr/geom/Annotation.h"

#include <algorithm>
#include <cmath>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

namespace sbr::geom {

gp_Pnt dimensionTextPoint(const gp_Pnt& start, const gp_Pnt& end, const gp_Vec& offset) {
  return gp_Pnt((start.XYZ() + end.XYZ()) / 2.0).Translated(offset);
}

Result<TopoDS_Shape> makeDimensionShape(const gp_Pnt& start, const gp_Pnt& end, const gp_Vec& offset) {
  const gp_Vec along(start, end);
  const double length = along.Magnitude();
  if (length < tolerance::kMinDimension) return Result<TopoDS_Shape>::failure("Dimension points coincide");
  const gp_Dir dir(along);
  // Only the perpendicular part of the offset moves the line.
  const gp_Vec perp = offset - gp_Vec(dir) * offset.Dot(gp_Vec(dir));

  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  auto add = [&](const gp_Pnt& a, const gp_Pnt& b) {
    if (a.Distance(b) > tolerance::kMinDimension) builder.Add(compound, BRepBuilderAPI_MakeEdge(a, b).Edge());
  };
  const gp_Pnt a = start.Translated(perp);
  const gp_Pnt b = end.Translated(perp);
  const double tick = std::clamp(length * 0.03, 40.0, 250.0);
  add(a.Translated(-gp_Vec(dir) * tick), b.Translated(gp_Vec(dir) * tick));  // dimension line, slightly past
  if (perp.Magnitude() > tolerance::kMinDimension) {
    // Extension lines: small gap at the measured points, a little past the line.
    const gp_Dir out(perp);
    const double gap = std::min(tick * 0.5, perp.Magnitude() * 0.5);
    for (const gp_Pnt& p : {start, end}) {
      add(p.Translated(gp_Vec(out) * gap), p.Translated(perp + gp_Vec(out) * tick));
    }
    // 45° ticks (architectural style) in the plane of line and offset.
    const gp_Vec slash = (gp_Vec(dir) + gp_Vec(out)).Normalized() * tick * 0.7;
    for (const gp_Pnt& p : {a, b}) add(p.Translated(-slash), p.Translated(slash));
  } else {
    // No offset: mark the ends with short perpendicular bars instead.
    gp_Vec bar = gp_Vec(dir).Crossed(gp_Vec(0, 0, 1));
    if (bar.Magnitude() < 1e-9) bar = gp_Vec(1, 0, 0);
    bar.Normalize();
    for (const gp_Pnt& p : {a, b}) add(p.Translated(-bar * tick), p.Translated(bar * tick));
  }
  return TopoDS_Shape(compound);
}

Result<TopoDS_Shape> makeTextNoteShape(const gp_Pnt& position, const std::optional<gp_Pnt>& target) {
  if (target && target->Distance(position) > tolerance::kMinDimension) {
    return TopoDS_Shape(BRepBuilderAPI_MakeEdge(*target, position).Edge());
  }
  return TopoDS_Shape(BRepBuilderAPI_MakeVertex(position).Vertex());
}

}  // namespace sbr::geom
