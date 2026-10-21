#include "sbr/geom/FaceRef.h"

#include <cmath>
#include <limits>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt2d.hxx>
#include <optional>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/geom/Operations.h"

namespace sbr::geom {
namespace {

bool faceCentroid(const TopoDS_Face& face, gp_Pnt& centroid, double& area) {
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  area = props.Mass();
  if (area <= 0) return false;
  centroid = props.CentreOfMass();
  return true;
}

}  // namespace

Result<FaceRef> FaceRef::fromFace(const TopoDS_Face& face) {
  const auto normal = planarFaceNormal(face);
  if (!normal) return Result<FaceRef>::failure("Face is not planar");
  FaceRef ref;
  ref.normal = *normal;
  if (!faceCentroid(face, ref.centroid, ref.area)) {
    return Result<FaceRef>::failure("Face has no area");
  }
  return ref;
}

Result<TopoDS_Face> FaceRef::resolve(const TopoDS_Shape& shape) const {
  // Planes are compared with a tolerance that scales mildly with model size.
  const double planeTolerance = 1e-3;
  const double angleTolerance = 1e-6;

  TopoDS_Face best;
  double bestDistance = std::numeric_limits<double>::max();
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face face = TopoDS::Face(it.Current());
    const auto n = planarFaceNormal(face);
    if (!n || n->Angle(normal) > angleTolerance) continue;
    gp_Pnt c;
    double a = 0;
    if (!faceCentroid(face, c, a)) continue;
    // Same supporting plane: centroid offset along the normal is zero.
    const double planeOffset = std::abs(gp_Vec(centroid, c).Dot(gp_Vec(normal)));
    if (planeOffset > planeTolerance) continue;
    const double d = centroid.Distance(c);
    if (d < bestDistance) {
      bestDistance = d;
      best = face;
    }
  }
  if (best.IsNull()) {
    return Result<TopoDS_Face>::failure(
        "Referenced face no longer exists (topology changed by an earlier edit)");
  }
  return best;
}

FaceRef FaceRef::transformed(const gp_Trsf& trsf) const {
  FaceRef out = *this;
  out.normal = normal.Transformed(trsf);
  out.centroid = centroid.Transformed(trsf);
  const double s = std::abs(trsf.ScaleFactor());
  out.area = area * s * s;
  return out;
}

namespace {

// A point strictly inside the face (UV centre or a nearby grid point).
std::optional<std::pair<gp_Pnt, gp_Dir>> interiorPoint(const TopoDS_Face& face) {
  double u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  BRepGProp_Face prop(face);
  for (double fu : {0.5, 0.25, 0.75}) {
    for (double fv : {0.5, 0.25, 0.75}) {
      const gp_Pnt2d uv(u0 + (u1 - u0) * fu, v0 + (v1 - v0) * fv);
      BRepClass_FaceClassifier classifier(face, uv, 1e-7);
      if (classifier.State() != TopAbs_IN) continue;
      gp_Pnt p;
      gp_Vec n;
      prop.Normal(uv.X(), uv.Y(), p, n);
      if (n.Magnitude() < 1e-12) continue;
      return std::make_pair(p, gp_Dir(n));
    }
  }
  return std::nullopt;
}

}  // namespace

Result<SurfaceRef> SurfaceRef::fromFace(const TopoDS_Face& face) {
  const auto inside = interiorPoint(face);
  if (!inside) return Result<SurfaceRef>::failure("Face has no interior point");
  SurfaceRef ref;
  ref.point = inside->first;
  ref.normal = inside->second;
  ref.surfaceType = static_cast<int>(BRepAdaptor_Surface(face).GetType());
  return ref;
}

Result<TopoDS_Face> SurfaceRef::resolve(const TopoDS_Shape& shape) const {
  TopoDS_Face best;
  double bestDistance = std::numeric_limits<double>::max();
  const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(point);
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face face = TopoDS::Face(e.Current());
    if (static_cast<int>(BRepAdaptor_Surface(face).GetType()) != surfaceType) continue;
    BRepExtrema_DistShapeShape dist(v, face);
    if (!dist.IsDone() || dist.NbSolution() < 1) continue;
    const auto n = faceNormalAt(face, dist.PointOnShape2(1));
    if (!n || n->Angle(normal) > M_PI / 4) continue;  // must face the same way
    if (dist.Value() < bestDistance) {
      bestDistance = dist.Value();
      best = face;
    }
  }
  if (best.IsNull()) return Result<TopoDS_Face>::failure("Referenced curved face no longer exists");
  return best;
}

}  // namespace sbr::geom
