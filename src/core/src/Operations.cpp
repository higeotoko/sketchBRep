#include "sbr/geom/Operations.h"

#include <cmath>
#include <memory>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepTools.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
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

Result<TopoDS_Face> offsetFace(const TopoDS_Face& face, double distance) {
  if (!std::isfinite(distance) || std::abs(distance) < tolerance::kMinDimension) {
    return Result<TopoDS_Face>::failure("Offset distance must be non-zero");
  }
  gp_Pln plane;
  const auto normal = planarFaceNormal(face);
  if (!normal || !isPlanarFace(face, &plane)) {
    return Result<TopoDS_Face>::failure("Only planar faces can be offset");
  }
  int wires = 0;
  for (TopExp_Explorer it(face, TopAbs_WIRE); it.More(); it.Next()) ++wires;
  if (wires != 1) return Result<TopoDS_Face>::failure("Faces with holes cannot be offset yet");

  try {
    // Work on a forward copy so that "outwards" is independent of orientation.
    TopoDS_Face forward = face;
    forward.Orientation(TopAbs_FORWARD);
    BRepOffsetAPI_MakeOffset offset(forward, GeomAbs_Intersection);
    offset.Perform(distance);
    if (!offset.IsDone()) return Result<TopoDS_Face>::failure("Offset failed");

    std::vector<TopoDS_Wire> result;
    for (TopExp_Explorer it(offset.Shape(), TopAbs_WIRE); it.More(); it.Next()) {
      result.push_back(TopoDS::Wire(it.Current()));
    }
    if (result.size() != 1) {
      return Result<TopoDS_Face>::failure(result.empty() ? "Offset collapses the outline"
                                                         : "Offset splits the outline");
    }
    BRepBuilderAPI_MakeFace makeFace(plane, result.front(), Standard_True);
    if (!makeFace.IsDone()) return Result<TopoDS_Face>::failure("Offset face construction failed");
    TopoDS_Face out = makeFace.Face();
    // MakeFace may treat a clockwise wire as a hole; fix the orientation.
    BRepCheck_Analyzer check(out);
    if (!check.IsValid() || area(out) <= 0) {
      result.front().Reverse();
      out = BRepBuilderAPI_MakeFace(plane, result.front(), Standard_True).Face();
    }
    const auto n = planarFaceNormal(out);
    if (n && n->Dot(*normal) < 0) out.Reverse();
    if (!BRepCheck_Analyzer(out).IsValid() || area(out) < tolerance::kMinDimension) {
      return Result<TopoDS_Face>::failure("Offset produced an invalid face");
    }
    return out;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Face>::failure(std::string("Offset failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> booleanCombine(BooleanOp op, const std::vector<TopoDS_Shape>& shapes) {
  if (shapes.size() < 2) return Result<TopoDS_Shape>::failure("A Boolean needs at least two solids");
  for (const TopoDS_Shape& s : shapes) {
    if (s.IsNull() || !TopExp_Explorer(s, TopAbs_SOLID).More()) {
      return Result<TopoDS_Shape>::failure("Boolean operands must be solids");
    }
  }
  try {
    TopoDS_Shape current = shapes.front();
    for (std::size_t i = 1; i < shapes.size(); ++i) {
      std::unique_ptr<BRepAlgoAPI_BooleanOperation> algo;
      switch (op) {
        case BooleanOp::Union: algo = std::make_unique<BRepAlgoAPI_Fuse>(current, shapes[i]); break;
        case BooleanOp::Subtract: algo = std::make_unique<BRepAlgoAPI_Cut>(current, shapes[i]); break;
        case BooleanOp::Intersect:
          algo = std::make_unique<BRepAlgoAPI_Common>(current, shapes[i]);
          break;
      }
      if (!algo->IsDone() || algo->HasErrors()) {
        return Result<TopoDS_Shape>::failure("Boolean operation failed");
      }
      current = algo->Shape();
    }
    ShapeUpgrade_UnifySameDomain unify(current, Standard_True, Standard_True, Standard_False);
    unify.Build();
    TopoDS_Shape result = unify.Shape();
    if (!TopExp_Explorer(result, TopAbs_SOLID).More()) {
      return Result<TopoDS_Shape>::failure("Boolean result is empty");
    }
    // A single solid is returned bare so that push/pull keeps working on it.
    const auto single = singleSolid(result);
    if (single) result = single.value();
    const ValidationReport report = validateShape(result, false);
    if (!report.valid || report.volume <= 0) {
      return Result<TopoDS_Shape>::failure("Boolean produced an invalid shape: " + report.summary());
    }
    return result;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Boolean failed: ") + e.GetMessageString());
  }
}

}  // namespace sbr::geom
