#include "sbr/geom/Operations.h"

#include <TopoDS_Shell.hxx>

#include <TopoDS_Compound.hxx>

#include <BRepGProp.hxx>

#include <GProp_GProps.hxx>

#include <Precision.hxx>

#include <BRepBuilderAPI_Sewing.hxx>

#include <ShapeAnalysis_FreeBounds.hxx>

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
#include <BRepFeat_SplitShape.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <ShapeFix_Solid.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <TopoDS_Wire.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopoDS_Edge.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Progress.h"
#include "sbr/geom/Validation.h"

namespace sbr::geom {

bool isPlanarFace(const TopoDS_Face& face, gp_Pln* planeOut) {
  if (face.IsNull()) return false;
  TopLoc_Location location;
  const Handle(Geom_Surface) surface = BRep_Tool::Surface(face, location);
  if (surface.IsNull()) return false;
  GeomLib_IsPlanarSurface test(surface, tolerance::kLinear);
  if (!test.IsPlanar()) return false;
  if (planeOut) {
    // The surface is stored without the face's placement: apply it.
    gp_Pln plane = test.Plan();
    if (!location.IsIdentity()) plane.Transform(location.Transformation());
    *planeOut = plane;
  }
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

namespace {

// Edges lying on one of the segments (both ends on its line, within it).
void keepEdgesOn(ShapeUpgrade_UnifySameDomain& unify, const TopoDS_Shape& shape,
                 const std::vector<Segment>& lines) {
  if (lines.empty()) return;
  auto onSegment = [](const gp_Pnt& p, const Segment& s) {
    const gp_Vec d(s.first, s.second);
    const double len = d.Magnitude();
    if (len < 1e-9) return false;
    const gp_Vec v(s.first, p);
    const double t = v.Dot(d) / (len * len);
    if (t < -1e-6 || t > 1 + 1e-6) return false;
    return v.Crossed(d).Magnitude() / len < 1e-3;
  };
  for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
    if (curve.GetType() != GeomAbs_Line) continue;
    const gp_Pnt a = curve.Value(curve.FirstParameter());
    const gp_Pnt b = curve.Value(curve.LastParameter());
    for (const Segment& s : lines) {
      if (onSegment(a, s) && onSegment(b, s)) {
        unify.KeepShape(e.Current());
        break;
      }
    }
  }
}

}  // namespace

Result<TopoDS_Shape> pushPullFace(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance,
                                  const std::vector<Segment>& keepLines) {
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
      BRepAlgoAPI_Fuse fuse(solid, prism.Shape(), progressRange());
      if (cancelled()) return Result<TopoDS_Shape>::failure(kCancelledMessage);
      if (!fuse.IsDone() || fuse.HasErrors()) {
        return Result<TopoDS_Shape>::failure("Push/pull: boolean union failed");
      }
      combined = fuse.Shape();
    } else {
      BRepAlgoAPI_Cut cut(solid, prism.Shape(), progressRange());
      if (cancelled()) return Result<TopoDS_Shape>::failure(kCancelledMessage);
      if (!cut.IsDone() || cut.HasErrors()) {
        return Result<TopoDS_Shape>::failure("Push/pull: boolean subtraction failed");
      }
      combined = cut.Shape();
    }

    ShapeUpgrade_UnifySameDomain unify(combined, Standard_True, Standard_True, Standard_False);
    keepEdgesOn(unify, combined, keepLines);
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
        case BooleanOp::Union: algo = std::make_unique<BRepAlgoAPI_Fuse>(current, shapes[i], progressRange()); break;
        case BooleanOp::Subtract: algo = std::make_unique<BRepAlgoAPI_Cut>(current, shapes[i], progressRange()); break;
        case BooleanOp::Intersect:
          algo = std::make_unique<BRepAlgoAPI_Common>(current, shapes[i], progressRange());
          break;
      }
      if (cancelled()) return Result<TopoDS_Shape>::failure(kCancelledMessage);
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

bool pointOnFace(const TopoDS_Face& face, const gp_Pnt& p, double tolerance) {
  gp_Pln plane;
  if (!isPlanarFace(face, &plane) || plane.Distance(p) > tolerance) return false;
  BRepClass_FaceClassifier classifier(face, p, tolerance);
  return classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON;
}

bool pointOnFaceBoundary(const TopoDS_Face& face, const gp_Pnt& p, double tolerance) {
  if (!pointOnFace(face, p, tolerance)) return false;
  const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(p);
  for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
    BRepExtrema_DistShapeShape dist(v, e.Current());
    if (dist.IsDone() && dist.Value() <= tolerance) return true;
  }
  return false;
}

namespace {

Result<TopoDS_Wire> splitWire(const TopoDS_Face& face, const std::vector<gp_Pnt>& polyline) {
  if (polyline.size() < 2) return Result<TopoDS_Wire>::failure("Split needs a line");
  for (const gp_Pnt& p : polyline) {
    if (!pointOnFace(face, p)) return Result<TopoDS_Wire>::failure("The line leaves the face");
  }
  if (!pointOnFaceBoundary(face, polyline.front()) || !pointOnFaceBoundary(face, polyline.back())) {
    return Result<TopoDS_Wire>::failure("The line must run from edge to edge of the face");
  }
  BRepBuilderAPI_MakeWire wire;
  for (std::size_t i = 1; i < polyline.size(); ++i) {
    if (polyline[i - 1].Distance(polyline[i]) < tolerance::kMinDimension) continue;
    wire.Add(BRepBuilderAPI_MakeEdge(polyline[i - 1], polyline[i]).Edge());
  }
  if (!wire.IsDone()) return Result<TopoDS_Wire>::failure("Invalid split line");
  return wire.Wire();
}

}  // namespace

Result<std::vector<TopoDS_Face>> splitPlanarFace(const TopoDS_Face& face, const std::vector<gp_Pnt>& polyline) {
  if (!isPlanarFace(face)) return Result<std::vector<TopoDS_Face>>::failure("Only planar faces can be split");
  auto wire = splitWire(face, polyline);
  if (!wire) return Result<std::vector<TopoDS_Face>>::failure(wire.error());
  try {
    BRepFeat_SplitShape split(face);
    split.Add(wire.value(), face);
    split.Build();
    if (!split.IsDone()) return Result<std::vector<TopoDS_Face>>::failure("The face could not be split");
    std::vector<TopoDS_Face> parts;
    for (TopExp_Explorer e(split.Shape(), TopAbs_FACE); e.More(); e.Next()) {
      TopoDS_Face part = TopoDS::Face(e.Current());
      if (BRepCheck_Analyzer(part).IsValid()) parts.push_back(part);
    }
    if (parts.size() < 2) return Result<std::vector<TopoDS_Face>>::failure("The line does not divide the face");
    return parts;
  } catch (const Standard_Failure& e) {
    return Result<std::vector<TopoDS_Face>>::failure(std::string("Split failed: ") + e.GetMessageString());
  }
}

Result<std::vector<TopoDS_Face>> splitPlanarFaceExtended(const TopoDS_Face& face,
                                                         const std::vector<gp_Pnt>& polyline) {
  if (polyline.size() < 2) return Result<std::vector<TopoDS_Face>>::failure("Split needs a line");
  const Bnd_Box box = exactBounds(face);
  const double reach = 2.0 * std::sqrt(box.SquareExtent()) + 1.0;
  std::vector<gp_Pnt> line = polyline;
  const gp_Vec first(line[1], line[0]);
  const gp_Vec last(line[line.size() - 2], line.back());
  if (first.Magnitude() < 1e-9 || last.Magnitude() < 1e-9) {
    return Result<std::vector<TopoDS_Face>>::failure("Invalid split line");
  }
  line.front().Translate(first.Normalized() * reach);
  line.back().Translate(last.Normalized() * reach);
  try {
    TopTools_ListOfShape arguments, tools;
    arguments.Append(face);
    for (std::size_t i = 1; i < line.size(); ++i) tools.Append(BRepBuilderAPI_MakeEdge(line[i - 1], line[i]).Edge());
    BRepAlgoAPI_Splitter splitter;
    splitter.SetArguments(arguments);
    splitter.SetTools(tools);
    splitter.Build();
    if (!splitter.IsDone()) return Result<std::vector<TopoDS_Face>>::failure("The face could not be split");
    std::vector<TopoDS_Face> parts;
    for (TopExp_Explorer e(splitter.Shape(), TopAbs_FACE); e.More(); e.Next()) parts.push_back(TopoDS::Face(e.Current()));
    if (parts.size() < 2) return Result<std::vector<TopoDS_Face>>::failure("The line does not divide the face");
    return parts;
  } catch (const Standard_Failure& e) {
    return Result<std::vector<TopoDS_Face>>::failure(std::string("Split failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> splitFace(const TopoDS_Shape& solid, const TopoDS_Face& face,
                               const std::vector<gp_Pnt>& polyline) {
  auto wire = splitWire(face, polyline);
  if (!wire) return Result<TopoDS_Shape>::failure(wire.error());
  try {
    BRepFeat_SplitShape split(solid);
    split.Add(wire.value(), face);
    split.Build();
    if (!split.IsDone()) return Result<TopoDS_Shape>::failure("The face could not be split");
    const TopoDS_Shape result = split.Shape();
    int before = 0, after = 0;
    for (TopExp_Explorer e(solid, TopAbs_FACE); e.More(); e.Next()) ++before;
    for (TopExp_Explorer e(result, TopAbs_FACE); e.More(); e.Next()) ++after;
    if (after <= before) return Result<TopoDS_Shape>::failure("The line does not divide the face");
    const ValidationReport report = validateShape(result, true);
    if (!report.valid) return Result<TopoDS_Shape>::failure("Split produced an invalid solid");
    return result;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Split failed: ") + e.GetMessageString());
  }
}

std::optional<gp_Dir> faceNormalAt(const TopoDS_Face& face, const gp_Pnt& p) {
  try {
    const Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
    if (surface.IsNull()) return std::nullopt;
    GeomAPI_ProjectPointOnSurf project(p, surface);
    if (project.NbPoints() < 1) return std::nullopt;
    double u = 0, v = 0;
    project.LowerDistanceParameters(u, v);
    BRepGProp_Face prop(face);  // takes the face orientation into account
    gp_Pnt at;
    gp_Vec n;
    prop.Normal(u, v, at, n);
    if (n.Magnitude() < 1e-12) return std::nullopt;
    return gp_Dir(n);
  } catch (const Standard_Failure&) {
    return std::nullopt;
  }
}

Result<TopoDS_Shape> offsetSolidFace(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance) {
  if (std::abs(distance) < tolerance::kMinDimension) return Result<TopoDS_Shape>::failure("Distance must be non-zero");
  try {
    BRepOffset_MakeOffset maker;
    maker.Initialize(solid, 0.0, 1e-4, BRepOffset_Skin, Standard_False, Standard_False, GeomAbs_Intersection,
                     Standard_False);
    maker.SetOffsetOnFace(face, distance);
    maker.MakeOffsetShape(progressRange());
    if (cancelled()) return Result<TopoDS_Shape>::failure(kCancelledMessage);
    if (!maker.IsDone()) return Result<TopoDS_Shape>::failure("The face could not be moved");
    TopoDS_Shape result = maker.Shape();
    if (result.ShapeType() == TopAbs_SHELL) {
      BRepBuilderAPI_MakeSolid makeSolid(TopoDS::Shell(result));
      if (!makeSolid.IsDone()) return Result<TopoDS_Shape>::failure("The result is not closed");
      result = makeSolid.Solid();
      ShapeFix_Solid fix(TopoDS::Solid(result));
      fix.Perform();
      result = fix.Solid();
    } else if (result.ShapeType() == TopAbs_COMPOUND) {
      TopExp_Explorer only(result, TopAbs_SOLID);
      if (!only.More()) return Result<TopoDS_Shape>::failure("The result is not a solid");
      result = only.Current();
    }
    const ValidationReport report = validateShape(result, true);
    if (!report.valid) return Result<TopoDS_Shape>::failure("Moving the face gives an invalid solid");
    return result;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Offset failed: ") + e.GetMessageString());
  }
}

bool faceInsideFace(const TopoDS_Face& inner, const TopoDS_Face& outer) {
  gp_Pln a, b;
  if (!isPlanarFace(inner, &a) || !isPlanarFace(outer, &b)) return false;
  if (!a.Axis().Direction().IsParallel(b.Axis().Direction(), 1e-9) || b.Distance(a.Location()) > 1e-3) {
    return false;
  }
  // Every vertex and edge midpoint strictly inside (not touching the boundary).
  for (TopExp_Explorer e(inner, TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
      const gp_Pnt p = curve.Value(curve.FirstParameter() + t * (curve.LastParameter() - curve.FirstParameter()));
      BRepClass_FaceClassifier classifier(outer, p, 1e-4);
      if (classifier.State() != TopAbs_IN || pointOnFaceBoundary(outer, p)) return false;
    }
  }
  return true;
}

Result<TopoDS_Face> cutHole(const TopoDS_Face& face, const TopoDS_Face& hole) {
  try {
    BRepAlgoAPI_Cut cut(face, hole);
    if (!cut.IsDone()) return Result<TopoDS_Face>::failure("The hole could not be cut");
    TopExp_Explorer only(cut.Shape(), TopAbs_FACE);
    if (!only.More()) return Result<TopoDS_Face>::failure("The hole removes the whole face");
    TopoDS_Face result = TopoDS::Face(only.Current());
    only.Next();
    if (only.More()) return Result<TopoDS_Face>::failure("The hole splits the face");
    if (!BRepCheck_Analyzer(result).IsValid()) return Result<TopoDS_Face>::failure("Invalid face with hole");
    // Keep the profile's orientation.
    const auto n0 = planarFaceNormal(face);
    const auto n1 = planarFaceNormal(result);
    if (n0 && n1 && n0->Dot(*n1) < 0) result.Reverse();
    return result;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Face>::failure(std::string("Hole failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> capHoles(const TopoDS_Shape& shell) {
  using R = Result<TopoDS_Shape>;
  ShapeAnalysis_FreeBounds bounds(shell, Precision::Confusion() * 10, Standard_False, Standard_False);
  const TopoDS_Compound wires = bounds.GetClosedWires();
  BRepBuilderAPI_Sewing sewing(1e-6);
  for (TopExp_Explorer f(shell, TopAbs_FACE); f.More(); f.Next()) sewing.Add(f.Current());
  int holes = 0;
  for (TopExp_Explorer w(wires, TopAbs_WIRE); w.More(); w.Next()) {
    BRepBuilderAPI_MakeFace cap(TopoDS::Wire(w.Current()), Standard_True);
    if (!cap.IsDone()) return R::failure("A hole is not flat; it cannot be filled with one face");
    sewing.Add(cap.Face());
    ++holes;
  }
  if (holes == 0) return R::failure("There are no holes to fill");
  sewing.Perform();
  const TopoDS_Shape sewn = sewing.SewedShape();
  for (TopExp_Explorer s(sewn, TopAbs_SHELL); s.More(); s.Next()) {
    const TopoDS_Shell closed = TopoDS::Shell(s.Current());
    if (!BRep_Tool::IsClosed(closed)) continue;
    BRepBuilderAPI_MakeSolid make(closed);
    if (!make.IsDone()) continue;
    ShapeFix_Solid fix(make.Solid());
    fix.Perform();
    TopoDS_Shape solid = fix.Solid();
    // Outward: a negative volume means the shell came out inside out.
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    if (props.Mass() < 0) solid.Reverse();
    return solid;
  }
  return sewn;  // still open somewhere
}

}  // namespace sbr::geom
