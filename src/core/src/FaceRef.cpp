#include "sbr/geom/FaceRef.h"

#include <cmath>
#include <limits>

#include <algorithm>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <gp_Pln.hxx>
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


Bnd_Box faceBox(const TopoDS_Face& face) {
  Bnd_Box box;
  BRepBndLib::AddOptimal(face, box, Standard_False, Standard_False);
  return box;
}

// Positive overlap of two flat boxes (touching along a split line does not
// count): every axis on which either has an extent must overlap.
bool boxesOverlap(const Bnd_Box& box, const gp_Pnt& lo, const gp_Pnt& hi, double tol) {
  if (box.IsVoid()) return false;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  const double a0[3] = {x0, y0, z0}, a1[3] = {x1, y1, z1};
  const double b0[3] = {lo.X(), lo.Y(), lo.Z()}, b1[3] = {hi.X(), hi.Y(), hi.Z()};
  for (int i = 0; i < 3; ++i) {
    const double overlap = std::min(a1[i], b1[i]) - std::max(a0[i], b0[i]);
    const bool flat = a1[i] - a0[i] < tol && b1[i] - b0[i] < tol;
    if (flat ? overlap < -tol : overlap <= tol) return false;
  }
  return true;
}

bool holds(const TopoDS_Face& face, const gp_Pnt& p) {
  BRepClass_FaceClassifier classifier(face, p, 1e-6);
  return classifier.State() == TopAbs_IN;
}

// Which side of each split line on the plane a point lies (0 = on it).
std::vector<int> sides(const std::vector<std::pair<gp_Pnt, gp_Pnt>>& lines, const gp_Dir& normal, const gp_Pnt& p) {
  std::vector<int> out;
  for (const auto& [a, b] : lines) {
    const double v = gp_Vec(a, b).Crossed(gp_Vec(a, p)).Dot(gp_Vec(normal));
    out.push_back(std::abs(v) < 1e-6 ? 0 : v > 0 ? 1 : -1);
  }
  return out;
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

Result<FaceRef> FaceRef::identify(const TopoDS_Face& face) {
  auto plain = fromFace(face);
  if (!plain) return plain;
  FaceRef ref = plain.value();
  if (const auto p = interiorPoint(face)) {
    ref.inside = p->first;
  } else if (holds(face, ref.centroid)) {
    ref.inside = ref.centroid;
  }
  const Bnd_Box box = faceBox(face);
  if (!box.IsVoid()) ref.extent = std::make_pair(box.CornerMin(), box.CornerMax());
  return ref;
}

Result<TopoDS_Face> FaceRef::resolve(const TopoDS_Shape& shape,
                                     const std::vector<std::pair<gp_Pnt, gp_Pnt>>& splitLines) const {
  // Planes are compared with a tolerance that scales mildly with model size.
  const double planeTolerance = 1e-3;
  const double angleTolerance = 1e-6;
  using R = Result<TopoDS_Face>;

  struct Candidate {
    TopoDS_Face face;
    gp_Pnt centroid;
  };
  std::vector<Candidate> candidates;
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
    candidates.push_back({face, c});
  }
  const R gone = R::failure("Referenced face no longer exists (topology changed by an earlier edit)");
  if (candidates.empty()) return gone;

  if (!inside && !extent) {  // older reference: closest centroid
    const auto best = std::min_element(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
      return centroid.Distance(a.centroid) < centroid.Distance(b.centroid);
    });
    return best->face;
  }
  // 1. The face that holds the remembered point.
  if (inside) {
    std::vector<const Candidate*> holding;
    for (const Candidate& c : candidates) {
      if (holds(c.face, *inside)) holding.push_back(&c);
    }
    if (holding.size() == 1) return holding.front()->face;
  }
  // 2. The faces still covering part of where it was.
  std::vector<const Candidate*> overlapping;
  for (const Candidate& c : candidates) {
    if (!extent || boxesOverlap(faceBox(c.face), extent->first, extent->second, planeTolerance)) {
      overlapping.push_back(&c);
    }
  }
  if (overlapping.empty()) return gone;
  if (overlapping.size() == 1) return overlapping.front()->face;
  // 3. Same side of every split line drawn on this plane.
  const gp_Pln plane(centroid, normal);
  std::vector<std::pair<gp_Pnt, gp_Pnt>> onPlane;
  for (const auto& line : splitLines) {
    if (plane.Distance(line.first) < planeTolerance && plane.Distance(line.second) < planeTolerance &&
        line.first.Distance(line.second) > planeTolerance) {
      onPlane.push_back(line);
    }
  }
  if (inside && !onPlane.empty()) {
    const std::vector<int> wanted = sides(onPlane, normal, *inside);
    std::vector<const Candidate*> same;
    for (const Candidate* c : overlapping) {
      const auto p = interiorPoint(c->face);
      if (!p) continue;
      const std::vector<int> got = sides(onPlane, normal, p->first);
      bool match = true;
      for (std::size_t i = 0; i < got.size(); ++i) match &= wanted[i] == 0 || got[i] == wanted[i];
      if (match) same.push_back(c);
    }
    if (same.size() == 1) return same.front()->face;
  }
  return R::failure("Referenced face is ambiguous (" + std::to_string(overlapping.size()) +
                    " faces on that plane could be it); pick the face again");
}

std::vector<TopoDS_Face> FaceRef::continuations(const TopoDS_Shape& shape) const {
  const double planeTolerance = 1e-3;
  if (!extent) {  // no identity: the face resolve() would edit
    auto face = resolve(shape);
    return face ? std::vector<TopoDS_Face>{face.value()} : std::vector<TopoDS_Face>{};
  }
  const gp_Vec n(normal);
  const double offset = gp_Vec(centroid.XYZ()).Dot(n);
  std::vector<TopoDS_Face> onPlane, grown;
  std::vector<std::pair<TopoDS_Face, double>> facing;  // other planes: (face, its offset)
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face face = TopoDS::Face(it.Current());
    const auto fn = planarFaceNormal(face);
    if (!fn || fn->Angle(normal) > 1e-6) continue;
    gp_Pnt c;
    double a = 0;
    if (!faceCentroid(face, c, a)) continue;
    const double o = gp_Vec(c.XYZ()).Dot(n);
    if (std::abs(o - offset) <= planeTolerance) {
      // On the plane: counts when a point inside it lies in the old box.
      const auto p = interiorPoint(face);
      const gp_Pnt inner = p ? p->first : c;
      Bnd_Box box;
      box.Add(extent->first);
      box.Add(extent->second);
      box.Enlarge(planeTolerance);
      if (!box.IsOut(inner)) {
        onPlane.push_back(face);
      } else if (boxesOverlap(faceBox(face), extent->first, extent->second, planeTolerance)) {
        grown.push_back(face);  // still covers where it was, now larger
      }
    } else {
      facing.emplace_back(face, o);
    }
  }
  if (!onPlane.empty()) return onPlane;
  if (grown.size() == 1) return grown;
  if (!grown.empty()) return {};
  // Moved parallel to itself: compare boxes after bringing it onto the
  // remembered plane.
  std::vector<TopoDS_Face> moved;
  for (const auto& [face, o] : facing) {
    const gp_Vec back = n * (offset - o);
    const Bnd_Box box = faceBox(face);
    if (box.IsVoid()) continue;
    if (boxesOverlap(box, extent->first.Translated(-back), extent->second.Translated(-back), planeTolerance)) {
      moved.push_back(face);
    }
  }
  if (moved.size() == 1) return moved;
  return {};
}

FaceRef FaceRef::transformed(const gp_Trsf& trsf) const {
  FaceRef out = *this;
  out.normal = normal.Transformed(trsf);
  out.centroid = centroid.Transformed(trsf);
  const double s = std::abs(trsf.ScaleFactor());
  out.area = area * s * s;
  if (inside) out.inside = inside->Transformed(trsf);
  if (extent) {
    Bnd_Box box;
    for (int i = 0; i < 8; ++i) {
      box.Add(gp_Pnt(i & 1 ? extent->second.X() : extent->first.X(), i & 2 ? extent->second.Y() : extent->first.Y(),
                     i & 4 ? extent->second.Z() : extent->first.Z())
                  .Transformed(trsf));
    }
    box.SetGap(0.0);
    out.extent = std::make_pair(box.CornerMin(), box.CornerMax());
  }
  return out;
}

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
  double secondDistance = std::numeric_limits<double>::max();
  const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(point);
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face face = TopoDS::Face(e.Current());
    if (static_cast<int>(BRepAdaptor_Surface(face).GetType()) != surfaceType) continue;
    BRepExtrema_DistShapeShape dist(v, face);
    if (!dist.IsDone() || dist.NbSolution() < 1) continue;
    const auto n = faceNormalAt(face, dist.PointOnShape2(1));
    if (!n || n->Angle(normal) > M_PI / 4) continue;  // must face the same way
    if (dist.Value() < bestDistance - 1e-6) {
      secondDistance = bestDistance;
      bestDistance = dist.Value();
      best = face;
    } else {
      secondDistance = std::min(secondDistance, dist.Value());
    }
  }
  if (best.IsNull()) return Result<TopoDS_Face>::failure("Referenced curved face no longer exists");
  if (secondDistance - bestDistance < 1e-6) {
    return Result<TopoDS_Face>::failure("Referenced curved face is ambiguous; pick the face again");
  }
  return best;
}

}  // namespace sbr::geom
