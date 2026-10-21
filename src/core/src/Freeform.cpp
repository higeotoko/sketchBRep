#include "sbr/geom/Freeform.h"

#include <cmath>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>

#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

namespace sbr::geom {
namespace {
Result<TopoDS_Shape> checked(const TopoDS_Shape& shape, const char* what) {
  const bool hasSolid = TopExp_Explorer(shape, TopAbs_SOLID).More();
  const ValidationReport r = validateShape(shape, false);
  if (!r.valid || (hasSolid && r.volume <= 0)) {
    return Result<TopoDS_Shape>::failure(std::string(what) + " produced an invalid shape: " + r.summary());
  }
  return shape;
}
}  // namespace

gp_Pnt arcApex(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge) {
  const gp_Vec chord(start, end);
  const gp_Pnt mid((start.XYZ() + end.XYZ()) / 2.0);
  const gp_Vec side = gp_Vec(normal).Crossed(chord);
  if (side.Magnitude() < 1e-12) return mid;
  return mid.Translated(side.Normalized() * bulge);
}

Result<TopoDS_Shape> makeArc(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge,
                             bool closed) {
  const gp_Vec chord(start, end);
  if (chord.Magnitude() < tolerance::kMinDimension) return Result<TopoDS_Shape>::failure("Arc ends coincide");
  if (std::abs(gp_Vec(normal).Dot(chord.Normalized())) > 1e-6) {
    return Result<TopoDS_Shape>::failure("Arc chord must lie in the plane");
  }
  if (std::abs(bulge) < tolerance::kMinDimension) return Result<TopoDS_Shape>::failure("Arc is flat");
  try {
    GC_MakeArcOfCircle arc(start, arcApex(start, end, normal, bulge), end);
    if (!arc.IsDone()) return Result<TopoDS_Shape>::failure("Arc could not be built");
    const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(arc.Value());
    if (!closed) return TopoDS_Shape(BRepBuilderAPI_MakeWire(edge).Wire());
    BRepBuilderAPI_MakeWire wire(edge, BRepBuilderAPI_MakeEdge(end, start));
    BRepBuilderAPI_MakeFace face(wire.Wire(), true);
    if (!face.IsDone()) return Result<TopoDS_Shape>::failure("Arc segment face could not be built");
    TopoDS_Face f = face.Face();
    // Profiles face along their plane normal, so extrusion goes that way.
    if (const auto n = planarFaceNormal(f); n && n->Dot(normal) < 0) f.Reverse();
    return TopoDS_Shape(f);
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Arc failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Wire> makeInterpolatedCurve(const std::vector<gp_Pnt>& points, bool closed) {
  if (points.size() < 2) return Result<TopoDS_Wire>::failure("A curve needs at least 2 points");
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    if (points[i].Distance(points[i + 1]) < tolerance::kMinDimension) {
      return Result<TopoDS_Wire>::failure("Curve has coincident consecutive points");
    }
  }
  try {
    Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, static_cast<int>(points.size()));
    for (std::size_t i = 0; i < points.size(); ++i) pts->SetValue(static_cast<int>(i) + 1, points[i]);
    GeomAPI_Interpolate interpolate(pts, closed ? Standard_True : Standard_False, tolerance::kLinear);
    interpolate.Perform();
    if (!interpolate.IsDone()) return Result<TopoDS_Wire>::failure("Curve interpolation failed");
    const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(interpolate.Curve()).Edge();
    return BRepBuilderAPI_MakeWire(edge).Wire();
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Wire>::failure(std::string("Curve failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> loft(const std::vector<TopoDS_Wire>& sections, bool solid) {
  if (sections.size() < 2) return Result<TopoDS_Shape>::failure("Loft needs at least 2 sections");
  try {
    BRepOffsetAPI_ThruSections maker(solid ? Standard_True : Standard_False, Standard_False);
    for (const TopoDS_Wire& w : sections) maker.AddWire(w);
    maker.Build();
    if (!maker.IsDone()) return Result<TopoDS_Shape>::failure("Loft failed");
    return checked(maker.Shape(), "Loft");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Loft failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> revolve(const TopoDS_Shape& profile, const gp_Ax1& axis, double angle) {
  if (!std::isfinite(angle) || std::abs(angle) < 1e-9) {
    return Result<TopoDS_Shape>::failure("Revolve angle must be non-zero");
  }
  try {
    BRepPrimAPI_MakeRevol maker(profile, axis, std::min(std::abs(angle), 2 * M_PI) * (angle < 0 ? -1 : 1));
    if (!maker.IsDone()) return Result<TopoDS_Shape>::failure("Revolve failed");
    return checked(maker.Shape(), "Revolve");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Revolve failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> sweep(const TopoDS_Shape& profile, const TopoDS_Wire& spine) {
  try {
    BRepOffsetAPI_MakePipe maker(spine, profile);
    maker.Build();
    if (!maker.IsDone()) return Result<TopoDS_Shape>::failure("Sweep failed");
    return checked(maker.Shape(), "Sweep");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Sweep failed: ") + e.GetMessageString());
  }
}

}  // namespace sbr::geom
