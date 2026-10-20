#include "sbr/geom/Operations.h"

#include <cmath>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <Geom_Surface.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Validation.h"

namespace sbr::geom {

bool isPlanarFace(const TopoDS_Face& face, gp_Pln* planeOut) {
  if (face.IsNull()) return false;
  const Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
  if (surface.IsNull()) return false;
  GeomLib_IsPlanarSurface test(surface, tolerance::kLinear);
  if (!test.IsPlanar()) return false;
  if (planeOut) *planeOut = test.Plan();
  return true;
}

std::optional<gp_Dir> planarFaceNormal(const TopoDS_Face& face) {
  if (!isPlanarFace(face)) return std::nullopt;
  // BRepGProp_Face accounts for the face orientation.
  BRepGProp_Face prop(face);
  double u0, u1, v0, v1;
  prop.Bounds(u0, u1, v0, v1);
  gp_Pnt p;
  gp_Vec n;
  prop.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, n);
  if (n.Magnitude() < 1e-12) return std::nullopt;
  return gp_Dir(n);
}

namespace {

// Pulls a single solid out of a boolean / healing result.
Result<TopoDS_Shape> singleSolid(const TopoDS_Shape& shape) {
  TopoDS_Shape solid;
  int count = 0;
  for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
    solid = it.Current();
    ++count;
  }
  if (count == 0) return Result<TopoDS_Shape>::failure("Operation produced no solid");
  if (count > 1) return Result<TopoDS_Shape>::failure("Operation would split the solid");
  return solid;
}

Result<TopoDS_Shape> validatedSolid(const TopoDS_Shape& shape, const char* what) {
  const ValidationReport report = validateShape(shape, true);
  if (!report.valid) {
    return Result<TopoDS_Shape>::failure(std::string(what) + " produced an invalid solid: " +
                                         report.summary());
  }
  return shape;
}

}  // namespace

Result<TopoDS_Shape> extrudeFace(const TopoDS_Face& face, double distance) {
  if (!std::isfinite(distance) || std::abs(distance) < tolerance::kMinDimension) {
    return Result<TopoDS_Shape>::failure("Extrusion distance must be non-zero");
  }
  const auto normal = planarFaceNormal(face);
  if (!normal) return Result<TopoDS_Shape>::failure("Only planar faces can be extruded");
  try {
    BRepPrimAPI_MakePrism prism(face, gp_Vec(*normal) * distance, Standard_True, Standard_True);
    if (!prism.IsDone()) return Result<TopoDS_Shape>::failure("Extrusion failed");
    TopoDS_Shape solid = prism.Shape();
    if (volume(solid) < 0) solid.Reverse();
    return validatedSolid(solid, "Extrusion");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Extrusion failed: ") +
                                         e.GetMessageString());
  }
}

Result<TopoDS_Shape> pushPullFace(const TopoDS_Shape& solid, const TopoDS_Face& face,
                                  double distance) {
  if (!std::isfinite(distance) || std::abs(distance) < tolerance::kMinDimension) {
    return Result<TopoDS_Shape>::failure("Push/pull distance must be non-zero");
  }
  auto normalOpt = planarFaceNormal(face);
  if (!normalOpt) return Result<TopoDS_Shape>::failure("Only planar faces can be pushed/pulled");
  gp_Dir normal = *normalOpt;

  try {
    // Make sure the normal points out of the solid even if the input
    // orientation is unusual (e.g. a face taken from a copy).
    BRepGProp_Face prop(face);
    double u0, u1, v0, v1;
    prop.Bounds(u0, u1, v0, v1);
    gp_Pnt mid;
    gp_Vec dummy;
    prop.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), mid, dummy);
    BRepClass3d_SolidClassifier classifier(solid, mid.Translated(gp_Vec(normal) * 1e-2),
                                           tolerance::kLinear);
    if (classifier.State() == TopAbs_IN) normal.Reverse();

    TopoDS_Face base = face;
    if (planarFaceNormal(base)->Dot(normal) < 0) base.Reverse();
    BRepPrimAPI_MakePrism prism(base, gp_Vec(normal) * distance, Standard_True, Standard_True);
    if (!prism.IsDone()) return Result<TopoDS_Shape>::failure("Push/pull prism failed");

    TopoDS_Shape combined;
    if (distance > 0) {
      BRepAlgoAPI_Fuse fuse(solid, prism.Shape());
      if (!fuse.IsDone() || fuse.HasErrors()) {
        return Result<TopoDS_Shape>::failure("Push/pull: boolean union failed");
      }
      combined = fuse.Shape();
    } else {
      BRepAlgoAPI_Cut cut(solid, prism.Shape());
      if (!cut.IsDone() || cut.HasErrors()) {
        return Result<TopoDS_Shape>::failure("Push/pull: boolean subtraction failed");
      }
      combined = cut.Shape();
    }

    ShapeUpgrade_UnifySameDomain unify(combined, Standard_True, Standard_True, Standard_False);
    unify.Build();
    const auto extracted = singleSolid(unify.Shape());
    if (!extracted) return extracted;
    return validatedSolid(extracted.value(), "Push/pull");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Push/pull failed: ") +
                                         e.GetMessageString());
  }
}

}  // namespace sbr::geom
