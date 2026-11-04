#include "sbr/display/DisplayMesh.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

namespace sbr::display {

TopoDS_Shape makeDisplayCopy(const TopoDS_Shape& canonical) {
  if (canonical.IsNull()) return canonical;
  BRepBuilderAPI_Copy copier(canonical, Standard_True, Standard_False);
  return copier.Shape();
}

DisplayMesh buildDisplayMesh(const TopoDS_Shape& canonical, const DisplayQuality& quality) {
  DisplayMesh mesh;
  if (canonical.IsNull()) return mesh;

  const TopoDS_Shape shape = makeDisplayCopy(canonical);
  BRepMesh_IncrementalMesh mesher(shape, quality.linearDeflection, Standard_False,
                                  quality.angularDeflection, Standard_True);

  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face face = TopoDS::Face(it.Current());
    TopLoc_Location location;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
    if (tri.IsNull()) continue;
    const gp_Trsf trsf = location.Transformation();
    const bool reversed = face.Orientation() == TopAbs_REVERSED;

    const auto base = static_cast<std::uint32_t>(mesh.vertexCount());
    std::vector<gp_Vec> accum(tri->NbNodes(), gp_Vec(0, 0, 0));
    for (int i = 1; i <= tri->NbNodes(); ++i) {
      const gp_Pnt p = tri->Node(i).Transformed(trsf);
      mesh.positions.insert(mesh.positions.end(), {static_cast<float>(p.X()),
                                                   static_cast<float>(p.Y()),
                                                   static_cast<float>(p.Z())});
    }
    for (int t = 1; t <= tri->NbTriangles(); ++t) {
      int a, b, c;
      tri->Triangle(t).Get(a, b, c);
      if (reversed) std::swap(b, c);
      const gp_Pnt pa = tri->Node(a).Transformed(trsf);
      const gp_Pnt pb = tri->Node(b).Transformed(trsf);
      const gp_Pnt pc = tri->Node(c).Transformed(trsf);
      const gp_Vec n = gp_Vec(pa, pb).Crossed(gp_Vec(pa, pc));
      accum[a - 1] += n;
      accum[b - 1] += n;
      accum[c - 1] += n;
      mesh.triangles.insert(mesh.triangles.end(),
                            {base + static_cast<std::uint32_t>(a - 1),
                             base + static_cast<std::uint32_t>(b - 1),
                             base + static_cast<std::uint32_t>(c - 1)});
    }
    for (gp_Vec n : accum) {
      if (n.Magnitude() > 1e-12) n.Normalize();
      mesh.normals.insert(mesh.normals.end(), {static_cast<float>(n.X()),
                                               static_cast<float>(n.Y()),
                                               static_cast<float>(n.Z())});
    }
  }

  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge edge = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(edge)) continue;
    BRepAdaptor_Curve curve(edge);
    GCPnts_TangentialDeflection sampler(curve, quality.angularDeflection,
                                        quality.linearDeflection);
    std::vector<float> polyline;
    for (int k = 1; k <= sampler.NbPoints(); ++k) {
      const gp_Pnt p = sampler.Value(k);
      polyline.insert(polyline.end(), {static_cast<float>(p.X()), static_cast<float>(p.Y()),
                                       static_cast<float>(p.Z())});
    }
    mesh.edgePolylines.push_back(std::move(polyline));
  }
  return mesh;
}

}  // namespace sbr::display
