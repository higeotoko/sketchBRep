#include "DrawingInternal.h"

#include <cmath>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

namespace sbr::draw::detail {
namespace {
TopoDS_Wire polygon(const std::vector<gp_Pnt2d>& loop) {
  BRepBuilderAPI_MakePolygon p;
  gp_Pnt last;
  bool any = false;
  for (const gp_Pnt2d& q : loop) {
    const gp_Pnt pt(q.X(), q.Y(), 0);
    if (any && pt.Distance(last) < 1e-7) continue;
    p.Add(pt);
    last = pt;
    any = true;
  }
  p.Close();
  return p.IsDone() ? p.Wire() : TopoDS_Wire();
}
}  // namespace

std::vector<std::array<gp_Pnt2d, 3>> triangulate(const Item& fill) {
  std::vector<std::array<gp_Pnt2d, 3>> out;
  try {
    const TopoDS_Wire outer = polygon(fill.points);
    if (outer.IsNull()) return out;
    BRepBuilderAPI_MakeFace mk(gp_Pln(gp::Origin(), gp::DZ()), outer, Standard_True);
    for (const auto& hole : fill.holes) {
      TopoDS_Wire h = polygon(hole);
      if (h.IsNull()) continue;
      mk.Add(TopoDS::Wire(h.Reversed()));
    }
    if (!mk.IsDone()) return out;
    const TopoDS_Face face = mk.Face();
    BRepMesh_IncrementalMesh mesh(face, 1.0);
    for (TopExp_Explorer f(face, TopAbs_FACE); f.More(); f.Next()) {
      TopLoc_Location loc;
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
      if (t.IsNull()) continue;
      for (int i = 1; i <= t->NbTriangles(); ++i) {
        int a, b, c;
        t->Triangle(i).Get(a, b, c);
        const gp_Pnt pa = t->Node(a).Transformed(loc.Transformation()), pb = t->Node(b).Transformed(loc.Transformation()),
                     pc = t->Node(c).Transformed(loc.Transformation());
        out.push_back({gp_Pnt2d(pa.X(), pa.Y()), gp_Pnt2d(pb.X(), pb.Y()), gp_Pnt2d(pc.X(), pc.Y())});
      }
    }
  } catch (const Standard_Failure&) {
  }
  return out;
}

gp_Pnt2d arcPoint(const Item& arc, double a) {
  return gp_Pnt2d(arc.centre.X() + arc.radius * std::cos(a), arc.centre.Y() + arc.radius * std::sin(a));
}

void textBaseline(const Item& text, double scale, gp_Pnt2d& start, gp_Pnt2d& end) {
  double units = 0;  // in heights
  for (std::size_t i = 0; i < text.text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text.text[i]);
    if ((c & 0xC0) == 0x80) continue;
    units += c >= 0xE0 ? 1.0 : 0.5;
  }
  const double h = text.height * scale, w = units * h;
  const double ca = std::cos(text.angle), sa = std::sin(text.angle);
  // From the anchor to the base line's start.
  const double dx = -w * text.hAlign / 2.0;
  const double dy = text.vAlign == 1 ? -h / 2 : text.vAlign == 2 ? -h : 0.0;
  const gp_Pnt2d p = text.points.front();
  start = gp_Pnt2d(p.X() + dx * ca - dy * sa, p.Y() + dx * sa + dy * ca);
  end = gp_Pnt2d(start.X() + w * ca, start.Y() + w * sa);
}

}  // namespace sbr::draw::detail
