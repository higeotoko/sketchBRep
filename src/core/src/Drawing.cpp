#include "sbr/draw/Drawing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include "sbr/KernelError.h"
#include "sbr/doc/Document.h"

namespace sbr::draw {

const char* penLayerName(Pen pen) {
  switch (pen) {
    case Pen::Cut: return "CUT";
    case Pen::Visible: return "VISIBLE";
    case Pen::Hidden: return "HIDDEN";
    case Pen::Fill: return "FILL";
    case Pen::Grid: return "GRID";
    case Pen::GridLabel: return "GRID_LABEL";
    case Pen::Level: return "LEVEL";
    case Pen::Dimension: return "DIMENSION";
    case Pen::Text: return "TEXT";
    case Pen::Frame: return "FRAME";
    case Pen::TitleBlock: return "TITLE";
  }
  return "0";
}

double penWidth(Pen pen) {
  switch (pen) {
    case Pen::Cut: return 0.5;
    case Pen::Visible: return 0.18;
    case Pen::Hidden: return 0.13;
    case Pen::Fill: return 0.0;
    case Pen::Grid: return 0.13;
    case Pen::GridLabel: return 0.18;
    case Pen::Level: return 0.13;
    case Pen::Dimension: return 0.13;
    case Pen::Text: return 0.18;
    case Pen::Frame: return 0.7;
    case Pen::TitleBlock: return 0.25;
  }
  return 0.18;
}

std::vector<double> penDashes(Pen pen) {
  switch (pen) {
    case Pen::Hidden: return {1.5, 0.75};
    case Pen::Grid:
    case Pen::Level: return {8.0, 1.0, 0.5, 1.0};  // dash-dot
    default: return {};
  }
}

namespace {

constexpr double kOnPlane = 1e-4;  // mm: an edge / face on the cut plane

// Where the view puts world points: (u, v) on the sheet, w towards the viewer.
struct Projection {
  gp_Ax3 frame;
  gp_Trsf toLocal;
  explicit Projection(const gp_Ax3& f) : frame(f) { toLocal.SetTransformation(f); }
  gp_Pnt local(const gp_Pnt& p) const { return p.Transformed(toLocal); }
  gp_Pnt2d uv(const gp_Pnt& p) const {
    const gp_Pnt l = local(p);
    return gp_Pnt2d(l.X(), l.Y());
  }
  gp_Dir localDir(const gp_Dir& d) const { return d.Transformed(toLocal); }
};

Item line(Pen pen, const gp_Pnt2d& a, const gp_Pnt2d& b) {
  Item i;
  i.kind = Item::Kind::Line;
  i.pen = pen;
  i.points = {a, b};
  return i;
}

// An edge (already in local coordinates: x, y on the sheet) as items: lines
// and arcs exactly, other curves as polylines within `deflection`.
void addLocalEdge(const TopoDS_Edge& edge, Pen pen, double deflection, std::vector<Item>& out) {
  if (BRep_Tool::Degenerated(edge)) return;
  BRepAdaptor_Curve c(edge);
  const double f = c.FirstParameter(), l = c.LastParameter();
  if (!(l > f)) return;
  if (c.GetType() == GeomAbs_Line) {
    const gp_Pnt a = c.Value(f), b = c.Value(l);
    if (a.Distance(b) < 1e-9) return;
    out.push_back(line(pen, gp_Pnt2d(a.X(), a.Y()), gp_Pnt2d(b.X(), b.Y())));
    return;
  }
  if (c.GetType() == GeomAbs_Circle) {
    const gp_Circ circle = c.Circle();
    const gp_Dir axis = circle.Axis().Direction();
    if (std::abs(axis.Z()) > 1 - 1e-9) {
      // Seen square on: an arc. Its angles from the start and end points.
      const gp_Pnt a = c.Value(f), b = c.Value(l), centre = circle.Location();
      Item i;
      i.kind = Item::Kind::Arc;
      i.pen = pen;
      i.centre = gp_Pnt2d(centre.X(), centre.Y());
      i.radius = circle.Radius();
      double sweep = l - f;
      double s = std::atan2(a.Y() - centre.Y(), a.X() - centre.X());
      if (axis.Z() < 0) s = std::atan2(b.Y() - centre.Y(), b.X() - centre.X());  // clockwise: from the end
      if (sweep > 2 * M_PI - 1e-9) {
        sweep = 2 * M_PI;
        s = 0.0;
      }
      i.start = s;
      i.sweep = sweep;
      out.push_back(i);
      return;
    }
  }
  GCPnts_QuasiUniformDeflection sample(c, std::max(deflection, 1e-4), f, l);
  if (!sample.IsDone() || sample.NbPoints() < 2) return;
  Item i;
  i.kind = Item::Kind::Polyline;
  i.pen = pen;
  for (int k = 1; k <= sample.NbPoints(); ++k) {
    const gp_Pnt p = sample.Value(k);
    i.points.push_back(gp_Pnt2d(p.X(), p.Y()));
  }
  // Seen edge-on (e.g. a circle in an elevation): straight; keep the ends.
  if (i.points.size() > 2) {
    const gp_Vec2d d(i.points.front(), i.points.back());
    bool straight = d.Magnitude() > 1e-9;
    for (const gp_Pnt2d& q : i.points) {
      if (!straight) break;
      straight = std::abs(d.Crossed(gp_Vec2d(i.points.front(), q))) / d.Magnitude() < 1e-6;
    }
    if (straight) i.points = {i.points.front(), i.points.back()};
  }
  out.push_back(i);
}

void addLocalEdges(const TopoDS_Shape& shape, Pen pen, double deflection, std::vector<Item>& out) {
  if (shape.IsNull()) return;
  for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) addLocalEdge(TopoDS::Edge(e.Current()), pen, deflection, out);
}

// The distance from a point to an item's line (lines, arcs, polylines).
double distanceTo(const Item& item, const gp_Pnt2d& p) {
  auto toSegment = [&](const gp_Pnt2d& a, const gp_Pnt2d& b) {
    const gp_Vec2d ab(a, b), ap(a, p);
    const double len2 = ab.SquareMagnitude();
    const double t = len2 > 0 ? std::clamp(ap.Dot(ab) / len2, 0.0, 1.0) : 0.0;
    return p.Distance(gp_Pnt2d(a.XY() + ab.XY() * t));
  };
  switch (item.kind) {
    case Item::Kind::Line: return toSegment(item.points[0], item.points[1]);
    case Item::Kind::Polyline: {
      double best = 1e300;
      for (std::size_t k = 1; k < item.points.size(); ++k) best = std::min(best, toSegment(item.points[k - 1], item.points[k]));
      return best;
    }
    case Item::Kind::Arc: {
      double a = std::atan2(p.Y() - item.centre.Y(), p.X() - item.centre.X()) - item.start;
      while (a < 0) a += 2 * M_PI;
      while (a > 2 * M_PI) a -= 2 * M_PI;
      if (a <= item.sweep + 1e-9) return std::abs(p.Distance(item.centre) - item.radius);
      const gp_Pnt2d e0(item.centre.X() + item.radius * std::cos(item.start), item.centre.Y() + item.radius * std::sin(item.start));
      const gp_Pnt2d e1(item.centre.X() + item.radius * std::cos(item.start + item.sweep),
                        item.centre.Y() + item.radius * std::sin(item.start + item.sweep));
      return std::min(p.Distance(e0), p.Distance(e1));
    }
    default: return 1e300;
  }
}

std::vector<gp_Pnt2d> samples(const Item& item) {
  std::vector<gp_Pnt2d> out;
  if (item.kind == Item::Kind::Arc) {
    for (double t : {0.1, 0.5, 0.9}) {
      const double a = item.start + item.sweep * t;
      out.push_back(gp_Pnt2d(item.centre.X() + item.radius * std::cos(a), item.centre.Y() + item.radius * std::sin(a)));
    }
  } else if (item.points.size() >= 2) {
    for (double t : {0.1, 0.5, 0.9}) {
      // Along the whole line (by points: enough for the check).
      const double at = t * (item.points.size() - 1);
      const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(at), item.points.size() - 2);
      const double s = at - k;
      out.push_back(gp_Pnt2d(item.points[k].XY() * (1 - s) + item.points[k + 1].XY() * s));
    }
  }
  return out;
}

// The loops of a planar face (local coordinates) as polygons.
void addFill(const TopoDS_Face& face, double deflection, std::vector<Item>& out) {
  Item fill;
  fill.kind = Item::Kind::Fill;
  fill.pen = Pen::Fill;
  const TopoDS_Wire outer = BRepTools::OuterWire(face);
  for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
    std::vector<Item> edges;
    addLocalEdges(w.Current(), Pen::Fill, deflection, edges);
    std::vector<gp_Pnt2d> loop;
    for (const Item& e : edges) {
      std::vector<gp_Pnt2d> pts;
      if (e.kind == Item::Kind::Arc) {
        const int n = std::max(8, static_cast<int>(std::ceil(e.sweep / (M_PI / 18))));
        for (int k = 0; k <= n; ++k) {
          const double a = e.start + e.sweep * k / n;
          pts.push_back(gp_Pnt2d(e.centre.X() + e.radius * std::cos(a), e.centre.Y() + e.radius * std::sin(a)));
        }
      } else {
        pts = e.points;
      }
      if (pts.empty()) continue;
      // Chain: each edge joined to the loop at its nearer end.
      if (!loop.empty() && pts.back().Distance(loop.back()) < pts.front().Distance(loop.back())) std::reverse(pts.begin(), pts.end());
      if (loop.empty() && edges.size() > 1) {
        // Start so that the next edge joins at our end.
        const Item& next = edges[1];
        const gp_Pnt2d n0 = next.kind == Item::Kind::Arc ? gp_Pnt2d(next.centre.X() + next.radius * std::cos(next.start),
                                                                     next.centre.Y() + next.radius * std::sin(next.start))
                                                          : next.points.front();
        const gp_Pnt2d n1 = next.kind == Item::Kind::Arc
                                ? gp_Pnt2d(next.centre.X() + next.radius * std::cos(next.start + next.sweep),
                                           next.centre.Y() + next.radius * std::sin(next.start + next.sweep))
                                : next.points.back();
        const double endJoin = std::min(pts.back().Distance(n0), pts.back().Distance(n1));
        const double startJoin = std::min(pts.front().Distance(n0), pts.front().Distance(n1));
        if (startJoin < endJoin) std::reverse(pts.begin(), pts.end());
      }
      loop.insert(loop.end(), pts.begin(), pts.end());
    }
    if (loop.size() < 3) continue;
    if (w.Current().IsSame(outer)) fill.points = loop;
    else fill.holes.push_back(loop);
  }
  if (fill.points.size() >= 3) out.push_back(fill);
}

std::string formatMm(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f", std::round(v) == 0 ? 0.0 : v);
  return b;
}

}  // namespace

ViewSpec planView(double cutZ) {
  ViewSpec s;
  s.kind = ViewKind::Plan;
  s.frame = gp_Ax3(gp_Pnt(0, 0, cutZ), gp::DZ(), gp::DX());
  return s;
}

ViewSpec sectionView(const gp_Pnt& point, const gp_Dir& towardsViewer) {
  ViewSpec s;
  s.kind = ViewKind::Section;
  gp_Vec n(towardsViewer.X(), towardsViewer.Y(), 0);
  if (n.Magnitude() < 1e-9) n = gp_Vec(0, -1, 0);
  const gp_Dir dir(n);
  const gp_Dir right = gp::DZ().Crossed(dir);  // up x towards-viewer: the sheet's right
  s.frame = gp_Ax3(point, dir, right);
  return s;
}

ViewSpec elevationView(const doc::Document& document, int side) {
  Bnd_Box all;
  for (const doc::Object& o : document.objects()) {
    if (!o.shape.IsNull() && document.isVisible(o.id)) all.Add(document.worldBounds(o));
  }
  double x0 = -1000, y0 = -1000, z0 = 0, x1 = 1000, y1 = 1000, z1 = 1000;
  if (!all.IsVoid()) all.Get(x0, y0, z0, x1, y1, z1);
  const double pad = 1000.0;
  ViewSpec s;
  s.kind = ViewKind::Elevation;
  // The viewer: south (-Y) looking north, east (+X), north (+Y), west (-X).
  static const gp_Dir viewers[4] = {gp_Dir(0, -1, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(-1, 0, 0)};
  const gp_Dir n = viewers[((side % 4) + 4) % 4];
  const gp_Pnt c((x0 + x1) / 2, (y0 + y1) / 2, 0);
  const double half = std::max(x1 - x0, y1 - y0) / 2 + pad;
  s.frame = gp_Ax3(c.Translated(gp_Vec(n) * half), n, gp::DZ().Crossed(n));
  return s;
}

Result<View> makeView(const doc::Document& document, const ViewSpec& spec) {
  using R = Result<View>;
  View view;
  view.spec = spec;
  const Projection proj(spec.frame);
  const gp_Dir n = spec.frame.Direction();
  const gp_Pln plane(spec.frame.Location(), n);
  const bool cutting = spec.kind != ViewKind::Elevation;
  // The whole model's size: the clipping box and the curve deflection.
  Bnd_Box all;
  for (const doc::Object& o : document.objects()) {
    if (!o.shape.IsNull() && document.isVisible(o.id)) all.Add(document.worldBounds(o));
  }
  if (all.IsVoid()) return R::failure("Nothing to draw: the model is empty or hidden");
  const double diag = std::sqrt(all.SquareExtent());
  const double deflection = std::max(0.05, diag * 1e-5);
  const double big = diag * 4 + 10000;
  // The side kept: towards -n from the plane (plans: below the cut).
  const double keepDepth = spec.kind == ViewKind::Plan && spec.depth > 0 ? spec.depth : big;
  const gp_Pnt boxOrigin = spec.frame.Location().Translated(gp_Vec(spec.frame.XDirection()) * -big +
                                                           gp_Vec(spec.frame.YDirection()) * -big + gp_Vec(n) * -keepDepth);
  TopoDS_Shape keepBox;
  try {
    keepBox = BRepPrimAPI_MakeBox(gp_Ax2(boxOrigin, n, spec.frame.XDirection()), 2 * big, 2 * big, keepDepth).Shape();
  } catch (const Standard_Failure& e) {
    return R::failure("Could not set up the view: " + kernelMessage(e));
  }

  BRep_Builder builder;
  TopoDS_Compound seen;
  builder.MakeCompound(seen);
  bool anySeen = false;
  std::vector<Item> cut, fills, loose;
  for (const doc::Object& o : document.objects()) {
    if (o.shape.IsNull() || !document.isVisible(o.id)) continue;
    if (o.attributes.count("grid_part")) continue;  // drawn from the grid itself
    const doc::ObjectKind kind = o.kind();
    const bool body = kind == doc::ObjectKind::Solid || kind == doc::ObjectKind::Shell ||
                      kind == doc::ObjectKind::Face || kind == doc::ObjectKind::Instance;
    const bool curve = kind == doc::ObjectKind::Curve;
    if (!body && !curve) continue;
    // Where it lies: in front of the cut (gone), behind (kept whole), or across.
    Bnd_Box local;
    const TopoDS_Shape world = document.worldShape(o);
    BRepBndLib::Add(world.Moved(TopLoc_Location(proj.toLocal)), local);
    if (local.IsVoid()) continue;
    double ux0, uy0, uz0, ux1, uy1, uz1;
    local.Get(ux0, uy0, uz0, ux1, uy1, uz1);
    const bool gone = cutting && uz0 > kOnPlane;
    const bool beyondDepth = uz1 < -keepDepth;
    if (gone || beyondDepth) continue;
    const bool across = cutting && uz1 > kOnPlane;
    try {
      if (curve) {
        // Loose lines: drawn where they lie on the kept side (not hidden by
        // the bodies: lines on a floor stay visible).
        for (TopExp_Explorer e(world, TopAbs_EDGE); e.More(); e.Next()) {
          const TopoDS_Edge edge = TopoDS::Edge(e.Current());
          BRepAdaptor_Curve c(edge);
          const gp_Pnt a = proj.local(c.Value(c.FirstParameter())), b = proj.local(c.Value(c.LastParameter()));
          const gp_Pnt mid = proj.local(c.Value((c.FirstParameter() + c.LastParameter()) / 2));
          if (cutting && std::max({a.Z(), b.Z(), mid.Z()}) > kOnPlane) {
            if (c.GetType() != GeomAbs_Line || std::min(a.Z(), b.Z()) > kOnPlane) continue;
            // A straight line across the cut: the part kept.
            const double t = a.Z() / (a.Z() - b.Z());
            const gp_Pnt at(a.XYZ() + (b.XYZ() - a.XYZ()) * t);
            const gp_Pnt kept = a.Z() <= kOnPlane ? a : b;
            loose.push_back(line(Pen::Visible, gp_Pnt2d(kept.X(), kept.Y()), gp_Pnt2d(at.X(), at.Y())));
            continue;
          }
          addLocalEdge(TopoDS::Edge(edge.Moved(TopLoc_Location(proj.toLocal))), Pen::Visible, deflection, loose);
        }
        ++view.objects;
        continue;
      }
      if (!across) {
        builder.Add(seen, world);
        anySeen = true;
        ++view.objects;
        continue;
      }
      // Cut: the lines on the plane, exactly; the part kept seen beyond.
      // Non-destructive: the model's shapes are shared with the window
      // (drawings are made on another thread), never changed here.
      BRepAlgoAPI_Section section(world, plane, Standard_False);
      section.SetNonDestructive(Standard_True);
      section.Build();
      if (section.IsDone()) addLocalEdges(section.Shape().Moved(TopLoc_Location(proj.toLocal)), Pen::Cut, deflection, cut);
      BRepAlgoAPI_Common kept;
      {
        TopTools_ListOfShape arguments, tools;
        arguments.Append(world);
        tools.Append(keepBox);
        kept.SetArguments(arguments);
        kept.SetTools(tools);
        kept.SetNonDestructive(Standard_True);
        kept.Build();
      }
      if (!kept.IsDone() || kept.Shape().IsNull()) {
        view.notes.push_back("'" + o.name + "' could not be cut: drawn uncut");
        builder.Add(seen, world);
      } else {
        builder.Add(seen, kept.Shape());
        // The cut areas: faces of the kept part on the plane (solids only).
        if (spec.fill && (kind == doc::ObjectKind::Solid || kind == doc::ObjectKind::Instance)) {
          for (TopExp_Explorer f(kept.Shape(), TopAbs_FACE); f.More(); f.Next()) {
            const TopoDS_Face face = TopoDS::Face(f.Current());
            BRepAdaptor_Surface s(face);
            if (s.GetType() != GeomAbs_Plane) continue;
            const gp_Pln p = s.Plane();
            if (!p.Axis().Direction().IsParallel(n, 1e-9) || plane.Distance(p.Location()) > kOnPlane) continue;
            addFill(TopoDS::Face(face.Moved(TopLoc_Location(proj.toLocal))), deflection, fills);
          }
        }
      }
      anySeen = true;
      ++view.objects;
    } catch (const Standard_Failure& e) {
      view.notes.push_back("'" + o.name + "' left out: " + kernelMessage(e));
    }
  }

  // Seen beyond, hidden lines removed (all bodies together: each hides the others).
  std::vector<Item> visible, hidden;
  if (anySeen) {
    try {
      Handle(HLRBRep_Algo) hlr = new HLRBRep_Algo();
      hlr->Add(seen);
      hlr->Projector(HLRAlgo_Projector(gp_Ax2(spec.frame.Location(), n, spec.frame.XDirection())));
      hlr->Update();
      hlr->Hide();
      HLRBRep_HLRToShape out(hlr);
      addLocalEdges(out.VCompound(), Pen::Visible, deflection, visible);
      addLocalEdges(out.OutLineVCompound(), Pen::Visible, deflection, visible);
      if (spec.hidden) {
        addLocalEdges(out.HCompound(), Pen::Hidden, deflection, hidden);
        addLocalEdges(out.OutLineHCompound(), Pen::Hidden, deflection, hidden);
      }
    } catch (const Standard_Failure& e) {
      view.notes.push_back("Hidden lines could not be worked out: " + kernelMessage(e));
    }
  }
  // A seen line lying on a cut line is that cut line: drawn once, thick.
  const double same = std::max(1e-3, diag * 1e-7);
  auto onCut = [&](const Item& item) {
    for (const gp_Pnt2d& p : samples(item)) {
      bool on = false;
      for (const Item& c : cut) {
        if (distanceTo(c, p) < same) {
          on = true;
          break;
        }
      }
      if (!on) return false;
    }
    return true;
  };
  for (Item& f : fills) view.items.push_back(std::move(f));
  for (Item& v : visible) {
    if (cut.empty() || !onCut(v)) view.items.push_back(std::move(v));
  }
  for (Item& h : hidden) view.items.push_back(std::move(h));
  for (Item& l : loose) view.items.push_back(std::move(l));
  for (Item& c : cut) view.items.push_back(std::move(c));

  // The extent of the model's lines (annotations are placed around it).
  bool first = true;
  auto extend = [&](const gp_Pnt2d& p) {
    if (first) {
      view.minU = view.maxU = p.X();
      view.minV = view.maxV = p.Y();
      first = false;
      return;
    }
    view.minU = std::min(view.minU, p.X());
    view.maxU = std::max(view.maxU, p.X());
    view.minV = std::min(view.minV, p.Y());
    view.maxV = std::max(view.maxV, p.Y());
  };
  for (const Item& i : view.items) {
    if (i.kind == Item::Kind::Arc) {
      extend(gp_Pnt2d(i.centre.X() - i.radius, i.centre.Y() - i.radius));
      extend(gp_Pnt2d(i.centre.X() + i.radius, i.centre.Y() + i.radius));
    }
    for (const gp_Pnt2d& p : i.points) extend(p);
  }

  // Grids: lines across the view with their names; spans between them.
  if (spec.grid) {
    for (const doc::Object& o : document.objects()) {
      const auto* node = std::get_if<doc::GroupNode>(&o.recipe.base);
      if (!node || !node->grid || !document.isVisible(o.id)) continue;
      const gp_Trsf w = document.worldTransform(o);
      for (int family = 0; family < 2; ++family) {
        const auto& axes = family == 0 ? node->grid->x : node->grid->y;
        const gp_Dir along = (family == 0 ? gp::DY() : gp::DX()).Transformed(w);
        for (const doc::GridAxis& a : axes) {
          const gp_Pnt on = (family == 0 ? gp_Pnt(a.offset, 0, 0) : gp_Pnt(0, a.offset, 0)).Transformed(w);
          GridMark m;
          m.name = a.name;
          m.family = family;
          if (spec.kind == ViewKind::Plan) {
            const gp_Dir d = proj.localDir(along);
            m.point = proj.uv(on);
            m.direction = gp_Dir2d(d.X(), d.Y());
            m.inPlan = true;
          } else {
            // Seen in a section / elevation: the axes running towards the viewer.
            if (!along.IsParallel(n, 1e-6)) continue;
            m.point = proj.uv(on);
            m.direction = gp_Dir2d(0, 1);
          }
          view.grids.push_back(m);
        }
      }
    }
  }
  if (spec.levels && spec.kind != ViewKind::Plan) {
    for (const doc::FloorLevel& l : document.settings().levels) {
      LevelMark m;
      m.v = proj.uv(gp_Pnt(spec.frame.Location().X(), spec.frame.Location().Y(), l.elevation)).Y();
      m.name = l.name;
      m.elevation = l.elevation;
      view.levels.push_back(m);
    }
  }
  if (spec.annotations) {
    for (const doc::Object& o : document.objects()) {
      if (!document.isVisible(o.id) || o.attributes.count("grid_part")) continue;
      const gp_Trsf w = document.worldTransform(o);
      if (const auto* d = std::get_if<doc::LinearDimension>(&o.recipe.base)) {
        const gp_Pnt a = d->start.Transformed(w), b = d->end.Transformed(w);
        const gp_Vec off = d->offset.Transformed(w);
        // Only dimensions lying in the view's plane direction.
        if (std::abs(gp_Vec(a, b).Dot(gp_Vec(n))) > 1e-6 || std::abs(off.Dot(gp_Vec(n))) > 1e-6) continue;
        if (cutting && proj.local(a).Z() > kOnPlane) continue;
        DimensionMark m;
        m.a = proj.uv(a);
        m.b = proj.uv(b);
        const gp_Pnt offEnd = proj.local(a.Translated(off));
        m.offset = gp_Vec2d(offEnd.X() - m.a.X(), offEnd.Y() - m.a.Y());
        m.text = d->text.empty() ? formatMm(a.Distance(b)) : d->text;
        view.dimensions.push_back(m);
      } else if (const auto* t = std::get_if<doc::TextNote>(&o.recipe.base)) {
        const gp_Pnt p = t->position.Transformed(w);
        if (cutting && proj.local(p).Z() > kOnPlane) continue;
        Item text;
        text.kind = Item::Kind::Text;
        text.pen = Pen::Text;
        text.points = {proj.uv(p)};
        text.text = t->text;
        text.height = 3.5;
        view.items.push_back(text);
        if (t->target) view.items.push_back(line(Pen::Text, proj.uv(t->target->Transformed(w)), proj.uv(p)));
      }
    }
  }
  if (view.items.empty() && view.grids.empty()) {
    return R::failure(cutting ? "Nothing on this side of the cut: check the cut height or the side looked at"
                              : "Nothing to draw from this side");
  }
  return view;
}

}  // namespace sbr::draw
