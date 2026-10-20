#include "sbr/geom/FaceRef.h"

#include <cmath>
#include <limits>

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

}  // namespace sbr::geom
