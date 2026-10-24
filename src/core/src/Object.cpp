#include "sbr/doc/Object.h"

#include <cstdio>
#include <random>

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

namespace sbr::doc {

ObjectId generateObjectId() {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  const std::uint64_t hi = engine();
  const std::uint64_t lo = engine();
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(hi >> 32), static_cast<unsigned>((hi >> 16) & 0xffff),
                static_cast<unsigned>((hi & 0x0fff) | 0x4000),
                static_cast<unsigned>(((lo >> 48) & 0x3fff) | 0x8000),
                static_cast<unsigned long long>(lo & 0xffffffffffffULL));
  return buffer;
}

const char* toString(ObjectKind kind) {
  switch (kind) {
    case ObjectKind::Empty: return "Empty";
    case ObjectKind::Curve: return "Curve";
    case ObjectKind::Face: return "Face";
    case ObjectKind::Solid: return "Solid";
    case ObjectKind::Shell: return "Shell";
    case ObjectKind::Group: return "Group";
    case ObjectKind::Instance: return "Instance";
    case ObjectKind::Annotation: return "Annotation";
    case ObjectKind::Guide: return "Guide";
    case ObjectKind::Image: return "Image";
    case ObjectKind::Other: return "Other";
  }
  return "Other";
}

ObjectKind Object::kind() const {
  if (std::holds_alternative<GroupNode>(recipe.base)) return ObjectKind::Group;
  if (std::holds_alternative<ComponentInstance>(recipe.base)) return ObjectKind::Instance;
  if (isAnnotation(recipe)) return ObjectKind::Annotation;
  if (std::holds_alternative<GuideLine>(recipe.base)) return ObjectKind::Guide;
  if (std::holds_alternative<ImageUnderlay>(recipe.base)) return ObjectKind::Image;
  if (shape.IsNull()) return ObjectKind::Empty;
  // A bare shape tells its kind by its type (exploring it allocates, and
  // kind() is asked of every object in many loops).
  switch (shape.ShapeType()) {
    case TopAbs_SOLID: return ObjectKind::Solid;
    case TopAbs_FACE: return ObjectKind::Face;
    case TopAbs_EDGE:
    case TopAbs_WIRE: return ObjectKind::Curve;
    default: break;
  }
  if (TopExp_Explorer(shape, TopAbs_SOLID).More()) return ObjectKind::Solid;
  if (TopExp_Explorer(shape, TopAbs_SHELL).More()) {
    int faces = 0;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More() && faces < 2; f.Next()) ++faces;
    if (faces >= 2) return ObjectKind::Shell;
  }
  if (TopExp_Explorer(shape, TopAbs_FACE).More()) return ObjectKind::Face;
  if (TopExp_Explorer(shape, TopAbs_EDGE).More()) return ObjectKind::Curve;
  return ObjectKind::Other;
}

Status rebuild(Object& object, const RegenContext& context) {
  auto result = regenerate(object.recipe, context);
  if (!result) return result.status();
  object.shape = result.value();
  return Status::success();
}

std::optional<EdgeMark> edgeMark(const TopoDS_Edge& localEdge) {
  if (localEdge.IsNull()) return std::nullopt;
  BRepAdaptor_Curve curve(localEdge);
  gp_Pnt p;
  gp_Vec d;
  curve.D1((curve.FirstParameter() + curve.LastParameter()) / 2.0, p, d);
  if (d.Magnitude() < 1e-12) return std::nullopt;
  return EdgeMark{p, gp_Dir(d)};
}

std::vector<int> hiddenEdgeIndices(const Object& object) {
  std::vector<int> out;
  if (object.hiddenEdges.empty() || object.shape.IsNull()) return out;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(object.shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge edge = TopoDS::Edge(edges(i));
    const auto own = edgeMark(edge);
    if (!own) continue;
    BRepAdaptor_Curve curve(edge);
    const bool straight = curve.GetType() == GeomAbs_Line;
    for (const EdgeMark& mark : object.hiddenEdges) {
      if (!own->direction.IsParallel(mark.direction, 1e-6)) continue;
      bool same = own->point.Distance(mark.point) < 1e-3;
      if (!same && straight) {
        // The mark lies on this segment (the edge grew or shrank along its line).
        const gp_Pnt a = curve.Value(curve.FirstParameter());
        const gp_Pnt b = curve.Value(curve.LastParameter());
        const gp_Vec ab(a, b);
        const double t = gp_Vec(a, mark.point).Dot(ab) / ab.SquareMagnitude();
        same = t > 0 && t < 1 && a.Translated(ab * t).Distance(mark.point) < 1e-3;
      }
      if (same) {
        out.push_back(i);
        break;
      }
    }
  }
  return out;
}

}  // namespace sbr::doc
