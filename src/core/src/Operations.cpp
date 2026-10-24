#include "sbr/geom/Operations.h"

#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <ShapeFix_Face.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

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
#include <BRepBuilderAPI_GTransform.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <gp_Lin.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Geom_Line.hxx>
#include <Geom_Plane.hxx>
#include <BRepLib.hxx>
#include <BRepTools_Modifier.hxx>
#include <BRepTools_GTrsfModification.hxx>
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

gp_GTrsf stretchTransform(const gp_Pnt& center, const gp_Dir& xDir, const gp_Dir& yDir, double fx, double fy,
                          double fz) {
  const gp_Dir zDir = xDir.Crossed(yDir);
  const gp_Dir y = zDir.Crossed(xDir);  // made exactly perpendicular
  // M = R diag(f) R^T with R's columns the frame's axes.
  const gp_Mat r(xDir.XYZ(), y.XYZ(), zDir.XYZ());
  gp_Mat d;
  d.SetDiagonal(fx, fy, fz);
  const gp_Mat m = r * d * r.Transposed();
  gp_GTrsf t;
  t.SetVectorialPart(m);
  t.SetTranslationPart(center.XYZ() - m * center.XYZ());
  return t;
}

namespace {

// OCCT's general transform works on NURBS only: the shape is converted to
// NURBS first, transformed, and what an affine map keeps flat or straight
// comes back as an exact plane / line (flat faces stay flat for push/pull,
// snapping and reshaping); the rest (circles -> ellipses, cylinders,
// splines) stays exact NURBS. Lines on curved faces stay NURBS too, so that
// their parameters keep matching those faces' curves.
class StretchModification : public BRepTools_GTrsfModification {
 public:
  StretchModification(const gp_GTrsf& g, const TopoDS_Shape& nurbs) : BRepTools_GTrsfModification(g), g_(g) {
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(nurbs, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
      for (const TopoDS_Shape& f : edgeFaces(i)) {
        if (!planeFrame(TopoDS::Face(f))) onCurved_.Add(edgeFaces.FindKey(i));
      }
    }
  }

  Standard_Boolean NewSurface(const TopoDS_Face& F, Handle(Geom_Surface)& S, TopLoc_Location& L, Standard_Real& Tol,
                              Standard_Boolean& RevWires, Standard_Boolean& RevFace) override {
    const auto frame = planeFrame(F);
    if (!frame) return BRepTools_GTrsfModification::NewSurface(F, S, L, Tol, RevWires, RevFace);
    S = new Geom_Plane(mapped(*frame));
    L = TopLoc_Location();
    Tol = BRep_Tool::Tolerance(F);
    RevWires = Standard_False;
    RevFace = Standard_False;
    return Standard_True;
  }

  Standard_Boolean NewCurve(const TopoDS_Edge& E, Handle(Geom_Curve)& C, TopLoc_Location& L,
                            Standard_Real& Tol) override {
    const auto line = lineOf(E);
    if (!line) return BRepTools_GTrsfModification::NewCurve(E, C, L, Tol);
    gp_XYZ origin = line->first.XYZ();
    g_.Transforms(origin);
    C = new Geom_Line(gp_Pnt(origin), gp_Dir(g_.VectorialPart() * line->second));
    L = TopLoc_Location();
    Tol = BRep_Tool::Tolerance(E);
    return Standard_True;
  }

  Standard_Boolean NewParameter(const TopoDS_Vertex& V, const TopoDS_Edge& E, Standard_Real& P,
                                Standard_Real& Tol) override {
    const auto line = lineOf(E);
    if (!line) return BRepTools_GTrsfModification::NewParameter(V, E, P, Tol);
    // p(t) = a + t d becomes G(a) + t M d: unit speed again on the new line.
    P = BRep_Tool::Parameter(V, E) * (g_.VectorialPart() * line->second).Modulus();
    Tol = BRep_Tool::Tolerance(V);
    return Standard_True;
  }

  Standard_Boolean NewCurve2d(const TopoDS_Edge& E, const TopoDS_Face& F, const TopoDS_Edge& NewE,
                              const TopoDS_Face& NewF, Handle(Geom2d_Curve)& C, Standard_Real& Tol) override {
    const auto frame = planeFrame(F);
    if (!frame) return BRepTools_GTrsfModification::NewCurve2d(E, F, NewE, NewF, C, Tol);
    const gp_Ax3 to = mapped(*frame);
    // Plane coordinates map linearly: (u, v) -> A (u, v).
    const gp_Mat& m = g_.VectorialPart();
    const gp_XYZ mx = m * frame->xu, my = m * frame->yv;
    const gp_XYZ X = to.XDirection().XYZ(), Y = to.YDirection().XYZ();
    const double a11 = mx.Dot(X), a12 = my.Dot(X), a21 = mx.Dot(Y), a22 = my.Dot(Y);
    auto map = [&](const gp_XY& uv) { return gp_XY(a11 * uv.X() + a12 * uv.Y(), a21 * uv.X() + a22 * uv.Y()); };
    double f = 0, l = 0;
    const Handle(Geom2d_Curve) old = BRep_Tool::CurveOnSurface(E, F, f, l);
    if (old.IsNull()) return Standard_False;
    Handle(Geom2d_BSplineCurve) spline = Handle(Geom2d_BSplineCurve)::DownCast(old);
    if (spline.IsNull()) spline = Geom2dConvert::CurveToBSplineCurve(old);
    if (lineOf(E) && spline->Degree() == 1 && spline->NbPoles() == 2) {
      // The edge became a line (unit speed): so does its pcurve.
      const double k0 = spline->Knot(1), k1 = spline->Knot(spline->NbKnots());
      const gp_XY d = (spline->Pole(2).XY() - spline->Pole(1).XY()) / (k1 - k0);
      const gp_XY a = spline->Pole(1).XY() - k0 * d;
      C = new Geom2d_Line(gp_Pnt2d(map(a)), gp_Dir2d(map(d)));
    } else {
      spline = Handle(Geom2d_BSplineCurve)::DownCast(spline->Copy());
      for (int i = 1; i <= spline->NbPoles(); ++i) spline->SetPole(i, gp_Pnt2d(map(spline->Pole(i).XY())));
      C = spline;
    }
    Tol = BRep_Tool::Tolerance(E);
    return Standard_True;
  }

 private:
  // A flat face's surface as o + u xu + v yv (NURBS planes are bilinear).
  struct Frame {
    gp_XYZ o, xu, yv;
  };
  static std::optional<Frame> planeFrame(const TopoDS_Face& F) {
    TopLoc_Location loc;
    const Handle(Geom_Surface) s = BRep_Tool::Surface(F, loc);
    if (s.IsNull()) return std::nullopt;
    const Handle(Geom_BSplineSurface) b = Handle(Geom_BSplineSurface)::DownCast(s);
    if (b.IsNull() || b->UDegree() != 1 || b->VDegree() != 1 || b->NbUPoles() != 2 || b->NbVPoles() != 2 ||
        b->IsURational() || b->IsVRational()) {
      return std::nullopt;
    }
    if (!GeomLib_IsPlanarSurface(b, 1e-9).IsPlanar()) return std::nullopt;
    double u0, u1, v0, v1;
    b->Bounds(u0, u1, v0, v1);
    gp_Pnt p;
    gp_Vec du, dv;
    b->D1(u0, v0, p, du, dv);
    if (!loc.IsIdentity()) {
      p.Transform(loc.Transformation());
      du.Transform(loc.Transformation());
      dv.Transform(loc.Transformation());
    }
    return Frame{p.XYZ() - u0 * du.XYZ() - v0 * dv.XYZ(), du.XYZ(), dv.XYZ()};
  }

  gp_Ax3 mapped(const Frame& f) const {
    gp_XYZ o = f.o;
    g_.Transforms(o);
    const gp_Mat& m = g_.VectorialPart();
    const gp_XYZ x = m * f.xu, y = m * f.yv;
    return gp_Ax3(gp_Pnt(o), gp_Dir(x.Crossed(y)), gp_Dir(x));
  }

  // A straight edge on flat faces only: p(t) = a + t d.
  std::optional<std::pair<gp_Pnt, gp_XYZ>> lineOf(const TopoDS_Edge& E) const {
    if (BRep_Tool::Degenerated(E) || onCurved_.Contains(E)) return std::nullopt;
    TopLoc_Location loc;
    double f = 0, l = 0;
    const Handle(Geom_BSplineCurve) b = Handle(Geom_BSplineCurve)::DownCast(BRep_Tool::Curve(E, loc, f, l));
    if (b.IsNull() || b->Degree() != 1 || b->NbPoles() != 2 || b->IsRational()) return std::nullopt;
    const double k0 = b->Knot(1), k1 = b->Knot(b->NbKnots());
    gp_Pnt p0 = b->Pole(1), p1 = b->Pole(2);
    if (!loc.IsIdentity()) {
      p0.Transform(loc.Transformation());
      p1.Transform(loc.Transformation());
    }
    const gp_XYZ d = (p1.XYZ() - p0.XYZ()) / (k1 - k0);
    return std::make_pair(gp_Pnt(p0.XYZ() - k0 * d), d);
  }

  gp_GTrsf g_;
  TopTools_MapOfShape onCurved_;
};

}  // namespace

Result<TopoDS_Shape> stretchShape(const TopoDS_Shape& shape, const gp_GTrsf& transform) {
  try {
    BRepBuilderAPI_NurbsConvert nurbs(shape, Standard_True);
    if (!nurbs.IsDone()) return Result<TopoDS_Shape>::failure("The shape could not be stretched");
    const TopoDS_Shape converted = nurbs.Shape();
    Handle(StretchModification) modification = new StretchModification(transform, converted);
    BRepTools_Modifier modifier(converted, modification);
    if (!modifier.IsDone()) return Result<TopoDS_Shape>::failure("The shape could not be stretched");
    TopoDS_Shape result = modifier.ModifiedShape(converted);
    BRepLib::SameParameter(result, 1e-6, Standard_True);
    return result;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Stretch failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> imprintOffset(const TopoDS_Shape& solid, const TopoDS_Face& face, double distance) {
  if (!(distance < 0.0)) {
    return Result<TopoDS_Shape>::failure("On a face of a solid the offset goes inwards (outwards it would leave the face)");
  }
  auto inner = offsetFace(face, distance);
  if (!inner) return Result<TopoDS_Shape>::failure(inner.error());
  try {
    const TopoDS_Wire loop = BRepTools::OuterWire(inner.value());
    BRepFeat_SplitShape split(solid);
    split.Add(loop, face);
    split.Build();
    if (!split.IsDone()) return Result<TopoDS_Shape>::failure("The offset could not be drawn on the face");
    const TopoDS_Shape result = split.Shape();
    int before = 0, after = 0;
    for (TopExp_Explorer e(solid, TopAbs_FACE); e.More(); e.Next()) ++before;
    for (TopExp_Explorer e(result, TopAbs_FACE); e.More(); e.Next()) ++after;
    if (after != before + 1) return Result<TopoDS_Shape>::failure("The offset does not divide the face");
    const ValidationReport report = validateShape(result, true);
    if (!report.valid) return Result<TopoDS_Shape>::failure("The offset made an invalid solid");
    return result;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Offset failed: ") + e.GetMessageString());
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

Result<TopoDS_Shape> sewIntoSolid(const std::vector<TopoDS_Shape>& pieces, double tolerance) {
  using R = Result<TopoDS_Shape>;
  try {
    BRepBuilderAPI_Sewing sewing(tolerance);
    sewing.SetNonManifoldMode(Standard_True);  // three faces on an edge stay visible as such
    int faces = 0;
    for (const TopoDS_Shape& piece : pieces) {
      for (TopExp_Explorer f(piece, TopAbs_FACE); f.More(); f.Next(), ++faces) sewing.Add(f.Current());
    }
    if (faces < 4) return R::failure("Too few faces to close a volume");
    sewing.Perform();
    const TopoDS_Shape sewn = sewing.SewedShape();
    TopoDS_Shell shell;
    int shells = 0;
    for (TopExp_Explorer s(sewn, TopAbs_SHELL); s.More(); s.Next(), ++shells) shell = TopoDS::Shell(s.Current());
    if (shells != 1) return R::failure("The faces do not join into one piece");
    TopTools_IndexedMapOfShape inShell;
    TopExp::MapShapes(shell, TopAbs_FACE, inShell);
    if (inShell.Extent() != faces) return R::failure("Some faces do not join the others");
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(shell, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
      if (BRep_Tool::Degenerated(TopoDS::Edge(edgeFaces.FindKey(i)))) continue;
      const int n = edgeFaces(i).Extent();
      if (n < 2) return R::failure("There is a gap: an edge has a face on one side only");
      if (n > 2) return R::failure("An edge is shared by more than two faces");
    }
    BRepBuilderAPI_MakeSolid make(shell);
    if (!make.IsDone()) return R::failure("The faces do not close a volume");
    ShapeFix_Solid fix(make.Solid());
    fix.Perform();
    TopoDS_Shape solid = fix.Solid();
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid, props);
    if (std::abs(props.Mass()) < 1e-6) return R::failure("The faces close no volume");
    if (props.Mass() < 0) solid.Reverse();
    if (!BRepCheck_Analyzer(solid).IsValid()) return R::failure("The faces do not make a valid solid");
    BOPAlgo_ArgumentAnalyzer analyzer;
    analyzer.SetShape1(solid);
    analyzer.SelfInterMode() = Standard_True;
    analyzer.Perform();
    if (analyzer.HasFaulty()) return R::failure("Faces pass through each other");
    return solid;
  } catch (const Standard_Failure& e) {
    return R::failure(std::string("Joining the faces failed: ") + e.GetMessageString());
  }
}

namespace {

// Newell's normal of a closed polygon (not normalised; zero when degenerate).
gp_Vec newellNormal(const std::vector<gp_Pnt>& loop) {
  gp_Vec n;
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const gp_Pnt& a = loop[i];
    const gp_Pnt& b = loop[(i + 1) % loop.size()];
    n += gp_Vec((a.Y() - b.Y()) * (a.Z() + b.Z()), (a.Z() - b.Z()) * (a.X() + b.X()),
                (a.X() - b.X()) * (a.Y() + b.Y()));
  }
  return n * 0.5;
}

}  // namespace

Result<TopoDS_Shape> moveVertices(const TopoDS_Shape& shape, const std::vector<gp_Pnt>& points, const gp_Vec& delta,
                                  bool autofold) {
  using R = Result<TopoDS_Shape>;
  if (shape.IsNull()) return R::failure("Nothing to reshape");
  if (points.empty()) return R::failure("No points to move");
  if (delta.Magnitude() < Precision::Confusion()) return R::failure("Distance must be non-zero");
  constexpr double kMatch = 1e-3;  // mm: points come snapped from the shape itself
  constexpr double kFlat = 1e-3;   // mm: how far a corner may sit off its face's plane

  TopTools_IndexedMapOfShape vertices;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
  std::vector<bool> moved(vertices.Extent() + 1, false);
  for (const gp_Pnt& p : points) {
    bool found = false;
    for (int i = 1; i <= vertices.Extent(); ++i) {
      if (BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))).Distance(p) < kMatch) {
        moved[i] = true;
        found = true;
      }
    }
    if (!found) return R::failure("The point to move is not a corner of the shape (any more)");
  }
  auto position = [&](const TopoDS_Vertex& v) {
    const gp_Pnt p = BRep_Tool::Pnt(v);
    return moved[vertices.FindIndex(v)] ? p.Translated(delta) : p;
  };

  bool solid = false;
  for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next()) solid = true;
  bool loneFace = shape.ShapeType() == TopAbs_FACE;

  std::vector<TopoDS_Face> rebuilt;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face face = TopoDS::Face(e.Current());
    if (!isPlanarFace(face)) return R::failure("Only shapes with flat faces can be reshaped by moving corners");
    for (TopExp_Explorer ed(face, TopAbs_EDGE); ed.More(); ed.Next()) {
      if (BRepAdaptor_Curve(TopoDS::Edge(ed.Current())).GetType() != GeomAbs_Line) {
        return R::failure("Only shapes with straight edges can be reshaped by moving corners");
      }
    }
    // Loops in order: the outer one first.
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    std::vector<TopoDS_Wire> wires{outer};
    for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
      if (!w.Current().IsSame(outer)) wires.push_back(TopoDS::Wire(w.Current()));
    }
    std::vector<std::vector<gp_Pnt>> before, after;
    for (const TopoDS_Wire& wire : wires) {
      std::vector<gp_Pnt> b, a;
      for (BRepTools_WireExplorer x(wire, face); x.More(); x.Next()) {
        b.push_back(BRep_Tool::Pnt(x.CurrentVertex()));
        a.push_back(position(x.CurrentVertex()));
      }
      if (a.size() < 3) return R::failure("Only shapes with straight edges can be reshaped by moving corners");
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].Distance(a[(i + 1) % a.size()]) < kMatch) return R::failure("Two corners would meet; an edge would vanish");
      }
      before.push_back(std::move(b));
      after.push_back(std::move(a));
    }
    const gp_Vec n0 = newellNormal(before.front());
    const gp_Vec n1 = newellNormal(after.front());
    auto polygon = [](const std::vector<gp_Pnt>& loop) {
      BRepBuilderAPI_MakePolygon make;
      for (const gp_Pnt& p : loop) make.Add(p);
      make.Close();
      return make.Wire();
    };
    auto flat = [&](const std::vector<gp_Pnt>& loop, gp_Pln* out) {
      const gp_Vec n = newellNormal(loop);
      if (n.Magnitude() < 1e-9) return false;
      gp_XYZ c;
      for (const gp_Pnt& p : loop) c += p.XYZ();
      c /= static_cast<double>(loop.size());
      const gp_Pln pln{gp_Pnt(c), gp_Dir(n)};
      for (const gp_Pnt& p : loop) {
        if (pln.Distance(p) > kFlat) return false;
      }
      if (out) *out = pln;
      return true;
    };
    gp_Pln plane;
    bool isFlat = true;
    if (n1.Magnitude() < 1e-9) {
      isFlat = false;  // collapsed as a whole: only folding could still make sense of it
    } else {
      gp_XYZ c;
      for (const gp_Pnt& p : after.front()) c += p.XYZ();
      c /= static_cast<double>(after.front().size());
      plane = gp_Pln{gp_Pnt(c), gp_Dir(n1)};
      for (const auto& loop : after) {
        for (const gp_Pnt& p : loop) isFlat = isFlat && plane.Distance(p) <= kFlat;
      }
    }
    if (isFlat && n0.Dot(n1) <= 0) return R::failure("A face would fold over (turn inside out)");
    if (!isFlat && !autofold && n1.Magnitude() < 1e-9) return R::failure("A face would collapse");
    if (!isFlat) {
      if (!autofold) {
        return R::failure("A face would no longer be flat; move a whole edge or face, or split the face first");
      }
      // Autofold: divide this face into flat pieces.
      if (after.size() > 1) return R::failure("A face with holes would have to be folded; split it first");
      const std::vector<gp_Pnt>& b = before.front();
      const std::vector<gp_Pnt>& a = after.front();
      const std::size_t n = a.size();
      for (std::size_t i = 0; i < n; ++i) {
        const gp_Vec turn = gp_Vec(b[i], b[(i + 1) % n]).Crossed(gp_Vec(b[(i + 1) % n], b[(i + 2) % n]));
        if (turn.Magnitude() > 1e-9 && turn.Dot(n0) < 0) {
          return R::failure("A concave face would have to be folded; split it first");
        }
      }
      std::vector<std::size_t> still;
      for (std::size_t i = 0; i < n; ++i) {
        if (a[i].Distance(b[i]) < kMatch) still.push_back(i);
      }
      std::vector<std::vector<gp_Pnt>> pieces;
      // Triangles of a fan from `loop.front()`, joined while they stay flat.
      auto fold = [&](const std::vector<gp_Pnt>& loop) {
        std::vector<gp_Pnt> piece{loop[0], loop[1]};
        for (std::size_t k = 2; k < loop.size(); ++k) {
          std::vector<gp_Pnt> grown = piece;
          grown.push_back(loop[k]);
          if (piece.size() < 3 || flat(grown, nullptr)) {
            piece = std::move(grown);
            continue;
          }
          pieces.push_back(piece);
          piece = {loop[0], loop[k - 1], loop[k]};
        }
        pieces.push_back(piece);
      };
      std::vector<gp_Pnt> core;
      for (std::size_t i : still) core.push_back(a[i]);
      if (still.size() >= 3 && newellNormal(core).Magnitude() > 1e-9) {
        // The corners that stay put keep their face on the old plane; each
        // run of moved corners between two of them becomes its own pieces.
        pieces.push_back(core);
        for (std::size_t k = 0; k < still.size(); ++k) {
          const std::size_t from = still[k], to = still[(k + 1) % still.size()];
          if ((from + 1) % n == to) continue;
          std::vector<gp_Pnt> run;
          for (std::size_t i = from;; i = (i + 1) % n) {
            run.push_back(a[i]);
            if (i == to) break;
          }
          fold(run);
        }
      } else {
        fold(a);
      }
      for (const auto& piece : pieces) {
        gp_Pln pln;
        if (piece.size() < 3 || !flat(piece, &pln)) return R::failure("A face could not be folded into flat pieces");
        // A piece may lean past upright (an overhang); turned right round
        // it would lie folded back onto the face. Solids are checked as a
        // whole below (valid, not crossing itself, with volume).
        if (gp_Dir(newellNormal(piece)).Dot(gp_Dir(n0)) < -0.99) {
          return R::failure("A face would fold over (turn inside out)");
        }
        BRepBuilderAPI_MakeFace make(pln, polygon(piece), Standard_True);
        if (!make.IsDone()) return R::failure("A folded face could not be built");
        ShapeFix_Face fix(make.Face());
        fix.FixOrientation();
        fix.Perform();
        rebuilt.push_back(TopoDS::Face(fix.Face()));
      }
      loneFace = false;  // a folded lone face is an open shell
      continue;
    }
    BRepBuilderAPI_MakeFace make(plane, polygon(after.front()), Standard_True);
    for (std::size_t i = 1; i < after.size(); ++i) make.Add(polygon(after[i]));
    if (!make.IsDone()) return R::failure("A face could not be rebuilt");
    ShapeFix_Face fix(make.Face());
    fix.FixOrientation();
    fix.Perform();
    rebuilt.push_back(TopoDS::Face(fix.Face()));
  }
  if (loneFace) return TopoDS_Shape(rebuilt.front());

  BRepBuilderAPI_Sewing sewing(1e-6);
  for (const TopoDS_Face& f : rebuilt) sewing.Add(f);
  sewing.Perform();
  const TopoDS_Shape sewn = sewing.SewedShape();
  if (!solid) return sewn;
  TopoDS_Shell shell;
  int shells = 0;
  for (TopExp_Explorer s(sewn, TopAbs_SHELL); s.More(); s.Next(), ++shells) shell = TopoDS::Shell(s.Current());
  if (shells != 1 || !BRep_Tool::IsClosed(shell)) return R::failure("The solid would come apart");
  BRepBuilderAPI_MakeSolid makeSolid(shell);
  if (!makeSolid.IsDone()) return R::failure("The solid would come apart");
  ShapeFix_Solid fixSolid(makeSolid.Solid());
  fixSolid.Perform();
  TopoDS_Shape result = fixSolid.Solid();
  GProp_GProps props;
  BRepGProp::VolumeProperties(result, props);
  if (props.Mass() < 0) result.Reverse();
  if (std::abs(props.Mass()) < 1e-6) return R::failure("The solid would have no volume");
  if (!BRepCheck_Analyzer(result).IsValid()) return R::failure("The moved shape would not be a valid solid");
  // Faces passing through each other are not caught by the checker.
  BOPAlgo_ArgumentAnalyzer analyzer;
  analyzer.SetShape1(result);
  analyzer.SelfInterMode() = Standard_True;
  analyzer.Perform();
  if (analyzer.HasFaulty()) return R::failure("The solid would cross itself");
  return result;
}

}  // namespace sbr::geom
