#include <BRepBuilderAPI_Copy.hxx>
#include "sbr/ConstructionQuantity.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/ConstructionWall.h"
#include <nlohmann/json.hpp>
#include <set>
#include "sbr/draw/Drawing.h"
#include "sbr/draw/SheetCheck.h"
#include <map>
#include <functional>
#include <optional>

#include "sbr/OpeningSymbols.h"
#include "sbr/doc/Schedules.h"
#include "sbr/doc/FireSpread.h"
#include "sbr/doc/FireCompartments.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Stair.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
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
    case Pen::HeightLimit: return "HEIGHT_LIMIT";
    case Pen::HeightReview: return "HEIGHT_REVIEW";
    case Pen::FireLimit3: return "FIRE_LIMIT_3M";
    case Pen::FireLimit5: return "FIRE_LIMIT_5M";
    case Pen::FireCompartment: return "FIRE_COMPARTMENT";
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
    case Pen::HeightLimit: return 0.35;
    case Pen::HeightReview: return 0.25;
    case Pen::FireLimit3:
    case Pen::FireLimit5: return 0.25;
    case Pen::FireCompartment: return 0.38;
  }
  return 0.18;
}

std::vector<double> penDashes(Pen pen) {
  switch (pen) {
    case Pen::Hidden: return {1.5, 0.75};
    case Pen::HeightReview: return {2.0, 1.0};
    case Pen::FireLimit3: return {5.0, 1.2};
    case Pen::FireLimit5: return {5.0, 1.2, 0.5, 1.2};
    case Pen::FireCompartment: return {6.0, 1.0, 0.45, 1.0, 0.45, 1.0};
    case Pen::Grid:
    case Pen::Level: return {8.0, 1.0, 0.5, 1.0};  // dash-dot
    default: return {};
  }
}

namespace {
// Where the layers under the upper faces of `world` meet, cut by the upright `plane` (docs/151,
// docs/152): each upper face's cut moved `depth` into the solid at right angles to the face;
// those of two faces meeting at a point (a ridge, a hip) end where they cross.
void layerLinesUnderTops(const TopoDS_Shape& world, const gp_Pln& plane, const std::vector<double>& depths,
                         const std::function<void(const gp_Pnt&, const gp_Pnt&)>& add) {
  struct Top {
    gp_Pnt a, b;
    gp_Vec m;  // in the plane, at right angles to a-b, moving 1 into the solid at right angles to the face
  };
  std::vector<Top> tops;
  const gp_Dir cutNormal = plane.Axis().Direction();
  for (TopExp_Explorer f(world, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    BRepAdaptor_Surface s(face);
    if (s.GetType() != GeomAbs_Plane) continue;
    gp_Dir out = s.Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) out.Reverse();
    if (out.Z() < 0.05) continue;  // not an upper face
    BRepAlgoAPI_Section cut(face, plane, Standard_False);
    cut.SetNonDestructive(Standard_True);
    cut.Build();
    if (!cut.IsDone()) continue;
    for (TopExp_Explorer e(cut.Shape(), TopAbs_EDGE); e.More(); e.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
      if (c.GetType() != GeomAbs_Line) continue;
      const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
      if (a.Distance(b) < 1e-6) continue;
      gp_Vec w = gp_Vec(cutNormal).Crossed(gp_Vec(a, b).Normalized());
      const double along = w.Dot(gp_Vec(out));
      if (std::abs(along) < 1e-6) continue;
      tops.push_back({a, b, w / -along});  // w / along points out: into the solid is the other way
    }
  }
  // Where the lines through p + s u and q + r v cross (in the plane); nothing when parallel.
  auto cross = [](const gp_Pnt& p, const gp_Vec& u, const gp_Pnt& q, const gp_Vec& v) -> std::optional<gp_Pnt> {
    const gp_Vec uv = u.Crossed(v);
    if (uv.SquareMagnitude() < 1e-12 * u.SquareMagnitude() * v.SquareMagnitude()) return std::nullopt;
    const double s = gp_Vec(p, q).Crossed(v).Dot(uv) / uv.SquareMagnitude();
    return p.Translated(u * s);
  };
  for (const double d : depths) {
    std::vector<std::pair<gp_Pnt, gp_Pnt>> lines;
    for (const Top& t : tops) lines.emplace_back(t.a.Translated(t.m * d), t.b.Translated(t.m * d));
    for (std::size_t i = 0; i < tops.size(); ++i) {
      for (std::size_t j = i + 1; j < tops.size(); ++j) {
        const gp_Pnt* ends[2][2] = {{&tops[i].a, &tops[i].b}, {&tops[j].a, &tops[j].b}};
        for (int ei = 0; ei < 2; ++ei) {
          for (int ej = 0; ej < 2; ++ej) {
            if (ends[0][ei]->Distance(*ends[1][ej]) > 1e-6) continue;
            const auto at = cross(lines[i].first, gp_Vec(lines[i].first, lines[i].second), lines[j].first,
                                  gp_Vec(lines[j].first, lines[j].second));
            if (!at) continue;
            (ei == 0 ? lines[i].first : lines[i].second) = *at;
            (ej == 0 ? lines[j].first : lines[j].second) = *at;
          }
        }
      }
    }
    for (const auto& [a, b] : lines)
      if (a.Distance(b) > 1e-6) add(a, b);
  }
}

constexpr double kOnPlane = 1e-4;  // mm: an edge / face on the cut plane

// Where the view puts world points: (u, v) on the sheet, w towards the viewer.
bool selected(const doc::Document& d, const doc::Object& o, const ViewSpec& spec) {
  if (!spec.includeHidden && !d.isVisible(o.id)) return false;
  if (!spec.within.empty() && o.id != spec.within && !d.isAncestor(spec.within, o.id)) return false;
  // docs/260 (owner 2026-11-22 "書かないようにする"): a plan at 1:50 or larger (1:20...) leaves out
  // the site's outline: drawn, it took the paper (at 1:20 the drawing did not fit A1).
  if (spec.kind == ViewKind::Plan && spec.paperScale > 0 && spec.paperScale <= 50) {
    const auto& boundary = d.settings().siteBoundary;
    if (boundary && (o.id == boundary->outline || (boundary->ground && o.id == boundary->ground->outline))) return false;
  }
  if (spec.kind == ViewKind::SitePlan) {
    const auto& boundary = d.settings().siteBoundary;
    if (boundary && (o.id == boundary->outline || (boundary->ground && o.id == boundary->ground->outline)))
      return false;
    const auto kind = o.kind();
    if (kind != doc::ObjectKind::Solid && kind != doc::ObjectKind::Shell &&
        kind != doc::ObjectKind::Face && kind != doc::ObjectKind::Instance) return false;
  }
  return spec.tags.empty() || std::find(spec.tags.begin(), spec.tags.end(), o.layer) != spec.tags.end();
}

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

// The insulation mark (docs/216, docs/217): a zigzag from a to b (on the sheet, model mm) in a band
// `thickness` wide: swinging 30 % of it (10 at least), a peak every 50 % (20 at least; docs/219:
// and 1.2 mm on paper at 1:scale when the scale is known). Nothing when shorter than 1.
constexpr double kZigzagPaperPitch = 1.2;  // mm on paper
std::optional<Item> insulationZigzag(const gp_Pnt2d& a, const gp_Pnt2d& b, double thickness, double scale) {
  const gp_Vec2d along(a, b);
  const double length = along.Magnitude();
  if (length < 1.0) return std::nullopt;
  const gp_Vec2d unit = along / length, across(-unit.Y(), unit.X());
  const double amplitude = std::max(10.0, thickness * 0.3),
               pitch = std::max({20.0, thickness * 0.5, scale > 0.0 ? kZigzagPaperPitch * scale : 0.0});
  const int steps = std::max(2, static_cast<int>(std::ceil(length / pitch)));
  Item zigzag;
  zigzag.kind = Item::Kind::Polyline;
  zigzag.pen = Pen::Visible;
  for (int i = 0; i <= steps; ++i) {
    const double side = i == 0 || i == steps ? 0.0 : (i % 2 == 1 ? amplitude : -amplitude);
    zigzag.points.push_back(a.Translated(unit * (length * i / steps) + across * side));
  }
  return zigzag;
}

// docs/218: where the insulation marks were put ("layer", "core", "middle"), for the legend.
std::vector<std::string> insulationPlaceNotes(const std::set<std::string>& places) {
  std::vector<std::string> out;
  if (places.count("layer")) out.push_back("仕様（層）に断熱の層がある物は、その層の中に描いています");
  if (places.count("core")) out.push_back("充填の断熱は、仕様（層）の柱・間柱の層の中に描いています");
  if (places.count("middle")) out.push_back("仕様（層）で位置の分からない断熱は、厚さの中ほどに描いた印です（層の位置ではありません）");
  out.push_back("基準の値との比較はしていません");
  return out;
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

std::string metres(double distanceMm) {
  char value[64];
  std::snprintf(value, sizeof value, "%.3f m", distanceMm / 1000.0);
  return value;
}

std::optional<DimensionMark> roadClearance(const std::array<gp_Pnt2d, 2>& edge,
                                           const std::vector<Item>& visible) {
  const auto inPlane = [](const gp_Pnt2d& p) { return gp_Pnt(p.X(), p.Y(), 0); };
  BRepBuilderAPI_MakeEdge road(inPlane(edge[0]), inPlane(edge[1]));
  if (!road.IsDone()) return std::nullopt;

  // Rebuild the drawn curves as ordinary planar edges. HLR's projected
  // topology is not suitable for the B-rep distance solver on older OCCT.
  BRep_Builder builder;
  TopoDS_Compound projection;
  builder.MakeCompound(projection);
  for (const Item& item : visible) {
    if (item.kind == Item::Kind::Arc) {
      const gp_Ax2 axes(inPlane(item.centre), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
      const gp_Circ circle(axes, item.radius);
      BRepBuilderAPI_MakeEdge arc(circle, item.start, item.start + item.sweep);
      if (arc.IsDone()) builder.Add(projection, arc.Edge());
    } else if (item.kind == Item::Kind::Line || item.kind == Item::Kind::Polyline) {
      for (std::size_t i = 1; i < item.points.size(); ++i) {
        BRepBuilderAPI_MakeEdge segment(inPlane(item.points[i - 1]), inPlane(item.points[i]));
        if (segment.IsDone()) builder.Add(projection, segment.Edge());
      }
    }
  }
  if (projection.NbChildren() == 0) return std::nullopt;

  BRepExtrema_DistShapeShape distance(road.Edge(), projection);
  if (!distance.IsDone() || distance.NbSolution() < 1) return std::nullopt;
  const auto a = distance.PointOnShape1(1), b = distance.PointOnShape2(1);
  return DimensionMark{gp_Pnt2d(a.X(), a.Y()), gp_Pnt2d(b.X(), b.Y()), gp_Vec2d(0, 0), metres(distance.Value())};
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

// docs/173: hatching of the cut layers. A band of a cut face: where `across` (affine in the
// sheet's x, y: across = a0 + au x + av y) lies between lo and hi; `pattern` 1 single lines, 2
// crossed. Lines at 45 degrees on the sheet, `spacing` apart (view mm), clipped to the face's
// polygon (outer loop and holes, even-odd) and to the band.
struct HatchBand {
  double lo = 0.0, hi = 0.0;
  int pattern = 0;
};
void hatchFace(const Item& fill, double a0, double au, double av, const std::vector<HatchBand>& bands, double spacing, std::vector<Item>& out) {
  if (fill.points.size() < 3 || spacing <= 0.0) return;
  std::vector<const std::vector<gp_Pnt2d>*> loops{&fill.points};
  for (const auto& h : fill.holes) loops.push_back(&h);
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (const gp_Pnt2d& q : fill.points) x0 = std::min(x0, q.X()), y0 = std::min(y0, q.Y()), x1 = std::max(x1, q.X()), y1 = std::max(y1, q.Y());
  for (const HatchBand& band : bands) {
    if (band.pattern == 0) continue;
    const double step = band.pattern == 2 ? spacing * 2 : spacing;
    for (const double angle : band.pattern == 2 ? std::vector<double>{M_PI / 4, -M_PI / 4} : std::vector<double>{M_PI / 4}) {
      const gp_Vec2d d(std::cos(angle), std::sin(angle)), n(-d.Y(), d.X());
      // The lines n . p = c, c over the face's extent, on a grid through the origin (so that
      // neighbouring faces' lines line up).
      double c0 = 1e300, c1 = -1e300;
      for (const double x : {x0, x1})
        for (const double y : {y0, y1}) {
          const double c = n.X() * x + n.Y() * y;
          c0 = std::min(c0, c), c1 = std::max(c1, c);
        }
      if ((c1 - c0) / step > 20000) continue;  // far too many: left out
      for (double c = std::ceil(c0 / step) * step; c <= c1; c += step) {
        const gp_Pnt2d base(n.X() * c, n.Y() * c);
        // Where the line crosses the loops' edges (parameters along d).
        std::vector<double> ts;
        for (const auto* loop : loops) {
          const std::size_t m = loop->size();
          for (std::size_t k = 0; k < m; ++k) {
            const gp_Pnt2d& p = (*loop)[k];
            const gp_Pnt2d& q = (*loop)[(k + 1) % m];
            const double sp = n.X() * p.X() + n.Y() * p.Y() - c, sq = n.X() * q.X() + n.Y() * q.Y() - c;
            if ((sp > 0) == (sq > 0)) continue;  // not crossed (half-open: a vertex on the line counts once)
            const double s = sp / (sp - sq);
            const gp_Pnt2d at(p.X() + (q.X() - p.X()) * s, p.Y() + (q.Y() - p.Y()) * s);
            ts.push_back(d.X() * (at.X() - base.X()) + d.Y() * (at.Y() - base.Y()));
          }
        }
        std::sort(ts.begin(), ts.end());
        // across along the line: A + B t.
        const double A = a0 + au * base.X() + av * base.Y(), B = au * d.X() + av * d.Y();
        for (std::size_t k = 0; k + 1 < ts.size(); k += 2) {
          double t0 = ts[k], t1 = ts[k + 1];
          if (std::abs(B) < 1e-12) {
            if (A < band.lo || A > band.hi) continue;
          } else {
            double u0 = (band.lo - A) / B, u1 = (band.hi - A) / B;
            if (u0 > u1) std::swap(u0, u1);
            t0 = std::max(t0, u0);
            t1 = std::min(t1, u1);
          }
          if (t1 - t0 < spacing * 0.05) continue;
          out.push_back(line(Pen::Visible, gp_Pnt2d(base.X() + d.X() * t0, base.Y() + d.Y() * t0),
                             gp_Pnt2d(base.X() + d.X() * t1, base.Y() + d.Y() * t1)));
        }
      }
    }
  }
}

// Which layers are hatched: structural timber crossed, plywood single lines (docs/173).
int hatchPattern(const std::string& name, const std::string& role) {
  if (role == "core") return 2;
  for (const char* word : {"柱", "間柱", "根太", "垂木", "大引", "梁", "土台"})
    if (name.find(word) != std::string::npos) return 2;
  for (const char* word : {"合板", "野地板"})
    if (name.find(word) != std::string::npos) return 1;
  return 0;
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

ViewSpec sitePlanView() {
  ViewSpec spec = planView(0);
  spec.kind = ViewKind::SitePlan;
  spec.title = "配置図（検討用）";
  spec.fill = spec.grid = spec.levels = spec.annotations = false;
  return spec;
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

Result<ViewSpec> framingView(const doc::Document& d, const std::string& preset, const std::string& within,
                             bool includeHidden, const std::vector<std::string>& tags) {
  try {
    const auto data = nlohmann::json::parse(construction::guideData("timber"));
    const auto& presets = data.at("outputs").at("framing_presets");
    if (!presets.contains(preset)) return Result<ViewSpec>::failure("Unknown framing preset: " + preset);
    if (!within.empty() && (!d.find(within) || !d.find(within)->isContainer()))
      return Result<ViewSpec>::failure("within must name a group");
    ViewSpec spec = planView(0);
    spec.within = within;
    spec.includeHidden = includeHidden;
    spec.tags =
        tags.empty() ? std::vector<std::string>{presets.at(preset).at("tag").get<std::string>()} : tags;
    spec.title = presets.at(preset).at("name");
    spec.hidden = presets.at(preset).value("hidden", true);
    spec.memberLabels = true;
    spec.framingPreset = preset;
    Bnd_Box all;
    for (const auto& o : d.objects())
      if (!o.shape.IsNull() && selected(d, o, spec)) all.Add(d.worldBounds(o));
    if (all.IsVoid())
      return Result<ViewSpec>::failure("Nothing to draw for the framing tag (check scope and visibility)");
    double x, y, z, X, Y, Z;
    all.Get(x, y, z, X, Y, Z);
    spec.frame.SetLocation(gp_Pnt(0, 0, Z + 10));
    return spec;
  } catch (const std::exception& e) {
    return Result<ViewSpec>::failure(e.what());
  }
}

Result<View> makeView(const doc::Document& document, const ViewSpec& spec) {
  using R = Result<View>;
  if (!spec.within.empty() && (!document.find(spec.within) || !document.find(spec.within)->isContainer()))
    return R::failure("within must name a group");
  View view;
  view.spec = spec;
  const bool sitePlan = spec.kind == ViewKind::SitePlan;
  if ((spec.kind == ViewKind::Plan || sitePlan) && document.settings().siteNorth) {
    const double angle = std::remainder(document.settings().siteNorth->angle, 360.0) * std::acos(-1.0) / 180.0;
    const gp_Vec worldNorth(std::sin(angle), std::cos(angle), 0);
    const double u = worldNorth.Dot(gp_Vec(spec.frame.XDirection()));
    const double v = worldNorth.Dot(gp_Vec(spec.frame.YDirection()));
    if (std::hypot(u, v) > 1e-9) view.trueNorth = gp_Dir2d(u, v);
  }
  const Projection proj(spec.frame);
  const gp_Dir n = spec.frame.Direction();
  const gp_Pln plane(spec.frame.Location(), n);
  const bool cutting = spec.kind == ViewKind::Plan || spec.kind == ViewKind::Section;
  // The whole model's size: the clipping box and the curve deflection.
  Bnd_Box all;
  std::optional<std::array<gp_Pnt2d, 2>> roadEdge;
  std::optional<DimensionMark> clearance;
  if (sitePlan) {
    const auto& source = document.settings().siteBoundary;
    if (!source) return R::failure("敷地境界が未入力です。閉じた輪郭を選び、ファイルメニューから指定してください");
    const auto boundary = document.siteBoundaryGeometry(source->outline);
    if (!boundary) return R::failure(boundary.error());
    Item outline;
    outline.kind = Item::Kind::Polyline;
    outline.pen = Pen::Grid;
    outline.closed = true;
    for (const gp_Pnt& vertex : boundary->vertices) {
      all.Add(vertex);
      outline.points.push_back(proj.uv(vertex));
    }
    view.items.push_back(std::move(outline));
    char area[64];
    std::snprintf(area, sizeof area, "%.3f", boundary->areaMm2 / 1e6);
    view.calculationNotes.push_back(std::string("敷地面積（指定輪郭） ") + area + " m²");
    view.calculationNotes.push_back("一点鎖線：敷地境界　実線：建物の上面投影（参考）");
    if (const auto edge = document.siteRoadEdge(*boundary)) {
      roadEdge = std::array<gp_Pnt2d, 2>{proj.uv((*edge)[0]), proj.uv((*edge)[1])};
      view.items.push_back(line(Pen::Cut, (*roadEdge)[0], (*roadEdge)[1]));
      const auto& road = *source->road;
      const auto entered = [](double mm) { return mm > 0 ? metres(mm) : std::string("未入力"); };
      view.calculationNotes.push_back("太実線：道路側境界　道路種別 " +
          (road.kind.empty() ? std::string("未入力") : road.kind) + "　幅員 " + entered(road.widthMm) +
          "　接道長（入力） " + entered(road.frontageMm));
      view.calculationNotes.push_back("道路情報の根拠：" + road.source);
    } else if (source->road) {
      view.calculationNotes.push_back("道路側境界を再指定してください（境界の辺が変わりました）");
    } else {
      view.calculationNotes.push_back("道路側境界・道路情報は未入力");
    }
    if (source->ground) {
      const auto& ground = *source->ground;
      const auto report = document.siteGroundReport(ground);
      const auto placement = document.checkSiteGroundPlacement(ground);
      if (!placement) {
        view.calculationNotes.push_back("接地外周の参考平均は保留。" + placement.message());
      } else if (!report) {
        view.calculationNotes.push_back("接地外周は未評価。輪郭と測点を確認してください");
      } else {
        Item contact;
        contact.kind = Item::Kind::Polyline;
        contact.pen = Pen::Hidden;
        contact.closed = true;
        Table evidence{"接地外周の高さ（参考）", {"辺", "長さ (m)", "始点高 (m)", "終点高 (m)", "長さ×平均高 (m²)"}, {}, {}};
        const auto decimal = [](double value) {
          char buf[96];
          std::snprintf(buf, sizeof buf, "%.3f", value);
          return std::string(buf);
        };
        const auto label = [](std::size_t i) { return "P" + std::to_string(i + 1); };
        for (std::size_t i = 0; i < report->vertices.size(); ++i) {
          const std::size_t j = (i + 1) % report->vertices.size();
          const auto& vertex = report->vertices[i];
          all.Add(vertex);
          contact.points.push_back(proj.uv(vertex));
          view.members.push_back({contact.points.back(), label(i)});
          const double a = report->heightsMm[i] / 1000;
          const double b = report->heightsMm[j] / 1000;
          const double length = report->lengthsMm[i] / 1000;
          evidence.rows.push_back({label(i) + "–" + label(j), decimal(length), decimal(a), decimal(b),
                                   decimal(length * (a / 2 + b / 2))});
        }
        view.items.push_back(std::move(contact));
        view.calculationNotes.push_back("破線・P番号：建物の接地外周と測点");
        evidence.rows.push_back({"合計", decimal(report->lengthMm / 1000), "", "", decimal(report->weightedSumMm2 / 1e6)});
        evidence.notes = {"参考平均（辺内を線形補間、長さで重み付け） " + metres(report->averageMm()),
                          "合計・参考平均は表示丸め前の値で計算",
                          "基準点 " + ground.datum, "確認資料 " + ground.source};
        if (!ground.placement) evidence.notes.push_back("建物との関連付けは未設定");
        evidence.notes.push_back("法令上の地盤面は地盤面算出表（令2条2項）で求める");
        view.siteGroundTable = std::move(evidence);
      }
    } else {
      view.calculationNotes.push_back("接地外周・高さは未入力");
    }
    if (!view.trueNorth) view.notes.push_back("真北が未入力です");
  }
  for (const doc::Object& o : document.objects()) {
    if (!o.shape.IsNull() && selected(document, o, spec)) all.Add(document.worldBounds(o));
  }
  if (all.IsVoid() && spec.kind == ViewKind::Plan && !spec.bracingFloor.empty() && document.settings().wallCalculation != "{}") {
    // docs/208: the bracing walls of the calculation alone (a model with no geometry yet).
    all.Add(gp_Pnt(0, 0, 0));
    all.Add(gp_Pnt(1000, 1000, 1000));
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
  std::vector<Item> cut, fills, loose, layerLines, hatches, compartmentLines;
  const bool compartmentView = (spec.kind == ViewKind::Plan || spec.kind == ViewKind::Section) &&
      spec.within.empty() && spec.framingPreset.empty() && !spec.insulation && spec.bracingFloor.empty() && !spec.memberLabels;
  const auto compartmentLabel = [&](gp_Pnt2d at, const std::string& label, bool beside = false) {
    if (!spec.annotations) return;
    const double scale = spec.paperScale > 0 ? spec.paperScale : 100;
    if (beside) at.SetX(at.X() + 2.4 * scale);
    else at.SetY(at.Y() + 1.4 * scale);
    Item text;
    text.kind = Item::Kind::Text;
    text.pen = Pen::FireCompartment;
    text.points = {at};
    text.text = label;
    text.height = 2.5;
    text.hAlign = beside ? 0 : 1;
    compartmentLines.push_back(std::move(text));
  };
  std::set<doc::ObjectId> drawn, gridIds;
  // docs/217: sections with insulation marks: what was marked, by part ("壁" / "床" / "屋根") and
  // insulation, and what has insulation given but could not be marked.
  std::map<std::string, std::map<std::string, std::set<doc::ObjectId>>> insulationMarked;
  std::set<std::string> insulationNotMarked, insulationPlaces;
  // docs/165: in plans, the cut lines of each wall of a build-up (its layers as the key), so
  // that where two walls of one build-up touch, the line between them can be left out.
  struct WallCut {
    std::string layers;
    std::size_t from = 0, to = 0;  // its items in `cut`
  };
  std::vector<WallCut> wallCuts;
  for (const doc::Object& o : document.objects()) {
    if (o.shape.IsNull() || !selected(document, o, spec)) continue;
    if (o.gridPin) gridIds.insert(o.gridPin->grid);
    if (o.attributes.count("grid_part")) continue;  // drawn from the grid itself
    const doc::ObjectKind kind = o.kind();
    const bool body = kind == doc::ObjectKind::Solid || kind == doc::ObjectKind::Shell ||
                      kind == doc::ObjectKind::Face || kind == doc::ObjectKind::Instance;
    const bool curve = kind == doc::ObjectKind::Curve;
    if (!body && !curve) continue;
    // Where it lies: in front of the cut (gone), behind (kept whole), or across.
    Bnd_Box local;
    // Even read-only OCCT algorithms can change topology bookkeeping flags.
    // Isolate their input, including geometry shared by component placements.
    TopoDS_Shape world;
    try {
      world = BRepBuilderAPI_Copy(document.worldShape(o), true, false).Shape();
    } catch (const Standard_Failure& e) {
      view.notes.push_back(o.name + ": " + kernelMessage(e));
      continue;
    }
    BRepBndLib::Add(world.Moved(TopLoc_Location(proj.toLocal)), local);
    if (local.IsVoid()) continue;
    double ux0, uy0, uz0, ux1, uy1, uz1;
    local.Get(ux0, uy0, uz0, ux1, uy1, uz1);
    const bool gone = cutting && uz0 > kOnPlane;
    const bool beyondDepth = !sitePlan && uz1 < -keepDepth;
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
        drawn.insert(o.id);
        continue;
      }
      if (!across) {
        builder.Add(seen, world);
        anySeen = true;
        ++view.objects;
        drawn.insert(o.id);
        continue;
      }
      // Cut: the lines on the plane, exactly; the part kept seen beyond.
      // Non-destructive: the model's shapes are shared with the window
      // (drawings are made on another thread), never changed here.
      BRepAlgoAPI_Section section(world, plane, Standard_False);
      section.SetNonDestructive(Standard_True);
      section.Build();
      const std::size_t cutFrom = cut.size();
      if (section.IsDone()) addLocalEdges(section.Shape().Moved(TopLoc_Location(proj.toLocal)), Pen::Cut, deflection, cut);
      if (compartmentView && cut.size() > cutFrom) {
        const auto record = doc::fireCompartmentBoundary(o);
        if (record) {
          std::optional<gp_Pnt2d> anchor;
          double length = 0;
          bool upright = false;
          for (std::size_t i = cutFrom; i < cut.size(); ++i) {
            const auto& item = cut[i];
            if (item.kind == Item::Kind::Line && item.points.size() == 2) {
              const auto& a = item.points[0];
              const auto& b = item.points[1];
              const double span = a.Distance(b);
              const gp_Pnt2d mid((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2);
              const bool vertical = std::abs(a.Y() - b.Y()) > std::abs(a.X() - b.X());
              if (span > length + kOnPlane || (anchor && std::abs(span - length) <= kOnPlane &&
                  (vertical ? mid.X() > anchor->X() : mid.Y() > anchor->Y()))) {
                anchor = mid;
                length = span;
                upright = vertical;
              }
            }
            compartmentLines.push_back(item);
            compartmentLines.back().pen = Pen::FireCompartment;
          }
          if (anchor) compartmentLabel(*anchor, record->mark, upright);
        }
      }
      if (spec.kind == ViewKind::Plan && std::abs(n.Z()) > 1 - 1e-9) {
        const auto kindAttr = o.attributes.find("kind"), layersAttr = o.attributes.find("wall_layers");
        if (kindAttr != o.attributes.end() && kindAttr->second == "wall" && layersAttr != o.attributes.end() && !layersAttr->second.empty())
          wallCuts.push_back({layersAttr->second, cutFrom, cut.size()});
      }
      // A floor / roof of a type with layers (docs/151, docs/152), cut upright: thin lines where
      // its layers meet, under its upper faces (across a roof's slope), mitred where two meet.
      if (section.IsDone() && std::abs(n.Z()) < 1e-9) {
        const std::vector<double> depths = doc::Modeler::typeLayerDepths(document, o);
        if (!depths.empty()) layerLinesUnderTops(world, plane, depths, [&](const gp_Pnt& a, const gp_Pnt& b) {
          layerLines.push_back(line(Pen::Visible, proj.uv(a), proj.uv(b)));
        });
        // A wall of a build-up (docs/156): upright lines where its layers cross the cut, broken
        // where the cut passes through its openings.
        const auto planes = doc::Modeler::wallLayerPlanes(document, o);
        if (!planes.empty()) {
          const Bnd_Box box = geom::exactBounds(world);
          const gp_Pnt c = plane.Location();
          for (const auto& [p, m] : planes) {
            // Both planes upright: they meet in an upright line through (x, y).
            const double a1 = n.X(), b1 = n.Y(), c1 = a1 * c.X() + b1 * c.Y();
            const double a2 = m.X(), b2 = m.Y(), c2 = a2 * p.X() + b2 * p.Y();
            const double det = a1 * b2 - a2 * b1;
            if (std::abs(det) < 1e-9) continue;  // the cut runs along the wall
            const double x = (c1 * b2 - c2 * b1) / det, y = (a1 * c2 - a2 * c1) / det;
            BRepBuilderAPI_MakeEdge upright(gp_Pnt(x, y, box.CornerMin().Z() - 1), gp_Pnt(x, y, box.CornerMax().Z() + 1));
            if (!upright.IsDone()) continue;
            BRepAlgoAPI_Common inside(upright.Edge(), world);
            if (!inside.IsDone()) continue;
            for (TopExp_Explorer e(inside.Shape(), TopAbs_EDGE); e.More(); e.Next()) {
              BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
              const gp_Pnt e0 = curve.Value(curve.FirstParameter()), e1 = curve.Value(curve.LastParameter());
              if (e0.Distance(e1) > 1e-6) layerLines.push_back(line(Pen::Visible, proj.uv(e0), proj.uv(e1)));
            }
          }
        }
        // docs/217: the insulation mark in the cut: up a wall's middle (broken at its openings, as
        // the layer lines), along a floor or roof at half its depth under its upper faces.
        if (spec.insulation && spec.kind == ViewKind::Section) {
          if (const auto band = doc::Modeler::insulationBand(document, o)) {
            const std::string kindName = o.attributes.at("kind");
            const std::string part = band->middle ? "壁" : kindName == "slab" ? "床" : kindName == "ceiling" ? "天井" : "屋根";
            std::size_t before = layerLines.size();
            auto mark = [&](const gp_Pnt& a, const gp_Pnt& b) {
              if (auto z = insulationZigzag(proj.uv(a), proj.uv(b), band->thickness, spec.paperScale)) layerLines.push_back(std::move(*z));
            };
            if (band->middle) {
              const auto& [p, m] = *band->middle;
              const gp_Pnt c = plane.Location();
              const double a1 = n.X(), b1 = n.Y(), c1 = a1 * c.X() + b1 * c.Y();
              const double a2 = m.X(), b2 = m.Y(), c2 = a2 * p.X() + b2 * p.Y();
              const double det = a1 * b2 - a2 * b1;
              if (std::abs(det) > 1e-9) {  // not when the cut runs along the wall
                const double x = (c1 * b2 - c2 * b1) / det, y = (a1 * c2 - a2 * c1) / det;
                const Bnd_Box box = geom::exactBounds(world);
                BRepBuilderAPI_MakeEdge upright(gp_Pnt(x, y, box.CornerMin().Z() - 1), gp_Pnt(x, y, box.CornerMax().Z() + 1));
                if (upright.IsDone()) {
                  BRepAlgoAPI_Common inside(upright.Edge(), world);
                  if (inside.IsDone())
                    for (TopExp_Explorer e(inside.Shape(), TopAbs_EDGE); e.More(); e.Next()) {
                      BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
                      mark(curve.Value(curve.FirstParameter()), curve.Value(curve.LastParameter()));
                    }
                }
              }
            } else {
              layerLinesUnderTops(world, plane, {band->depth}, mark);
            }
            insulationPlaces.insert(band->place);
            if (layerLines.size() > before) insulationMarked[part][band->text].insert(o.id);
            else insulationNotMarked.insert(part + "：" + band->text);
          }
        }
      }
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
        // docs/173: the bands of its layers on the cut, as an affine function of the world point.
        std::vector<HatchBand> bands;
        std::function<double(const gp_Pnt&)> acrossOf;
        if (spec.hatchScale > 0.0) {
          const auto usage = doc::Modeler::layerUsage(document, o);
          if (usage && usage->wall) {
            const auto planes = doc::Modeler::wallLayerPlanes(document, o);
            const nlohmann::json layers = nlohmann::json::parse(o.attributes.count("wall_layers") ? o.attributes.at("wall_layers") : "[]", nullptr, false);
            if (!planes.empty() && layers.is_array() && layers.size() == usage->layers.size()) {
              // Across the wall from its drawn line (to its left): the layers' edges e0, e1, ...
              std::vector<double> e{usage->offset};
              for (const auto& l : usage->layers) e.push_back(e.back() + (usage->towardsLeft ? l.thickness : -l.thickness));
              const gp_Pnt p0 = planes.front().first;
              const gp_Vec m(planes.front().second);
              const double e1 = e[1];
              acrossOf = [p0, m, e1](const gp_Pnt& w) { return gp_Vec(p0, w).Dot(m) + e1; };
              for (std::size_t k = 0; k < usage->layers.size(); ++k)
                bands.push_back({std::min(e[k], e[k + 1]), std::max(e[k], e[k + 1]),
                                 hatchPattern(usage->layers[k].name, layers[k].value("role", std::string()))});
            }
          } else if (usage && o.attributes.count("kind") && o.attributes.at("kind") == "slab" && std::abs(n.Z()) < 0.5) {
            // A floor cut upright: its layers from its top down.
            const double top = geom::exactBounds(world).CornerMax().Z();
            acrossOf = [top](const gp_Pnt& w) { return top - w.Z(); };
            double d = 0.0;
            for (const auto& l : usage->layers) {
              bands.push_back({d, d + l.thickness, hatchPattern(l.name, "")});
              d += l.thickness;
            }
          }
        }
        // The cut areas: faces of the kept part on the plane (solids only).
        if ((spec.fill || !bands.empty()) && (kind == doc::ObjectKind::Solid || kind == doc::ObjectKind::Instance)) {
          for (TopExp_Explorer f(kept.Shape(), TopAbs_FACE); f.More(); f.Next()) {
            const TopoDS_Face face = TopoDS::Face(f.Current());
            BRepAdaptor_Surface s(face);
            if (s.GetType() != GeomAbs_Plane) continue;
            const gp_Pln p = s.Plane();
            if (!p.Axis().Direction().IsParallel(n, 1e-9) || plane.Distance(p.Location()) > kOnPlane) continue;
            std::vector<Item> faceFill;
            addFill(TopoDS::Face(face.Moved(TopLoc_Location(proj.toLocal))), deflection, faceFill);
            if (!bands.empty() && acrossOf && !faceFill.empty()) {
              // across(x, y) on the sheet: the world point of (x, y) on the cut.
              const gp_Trsf toWorld = proj.toLocal.Inverted();
              const double c0 = acrossOf(gp_Pnt(0, 0, 0).Transformed(toWorld));
              const double cu = acrossOf(gp_Pnt(1, 0, 0).Transformed(toWorld)) - c0;
              const double cv = acrossOf(gp_Pnt(0, 1, 0).Transformed(toWorld)) - c0;
              hatchFace(faceFill.front(), c0, cu, cv, bands, 1.5 * spec.hatchScale, hatches);
            }
            if (spec.fill)
              for (Item& f : faceFill) fills.push_back(std::move(f));
          }
        }
      }
      anySeen = true;
      ++view.objects;
      drawn.insert(o.id);
    } catch (const Standard_Failure& e) {
      view.notes.push_back("'" + o.name + "' left out: " + kernelMessage(e));
    }
  }

  // The fittings of the openings (docs/146): seen in elevations, and in sections where they lie
  // wholly on the kept side (the cut itself goes through the hole).
  if (spec.kind != ViewKind::Plan && anySeen) {
    std::set<std::string> hosts(drawn.begin(), drawn.end());
    for (const FittingSolid& f : fittingSolids(document, &hosts)) {
      Bnd_Box local;
      BRepBndLib::Add(f.shape.Moved(TopLoc_Location(proj.toLocal)), local);
      if (local.IsVoid()) continue;
      double ux0, uy0, uz0, ux1, uy1, uz1;
      local.Get(ux0, uy0, uz0, ux1, uy1, uz1);
      if (cutting && uz1 > kOnPlane) continue;
      builder.Add(seen, f.shape);
    }
  }
  // Where two walls of one build-up touch in the cut (a corner, mitred or butted, or two walls in
  // one line), the line between them is left out of both: the walls read as one (docs/165).
  std::vector<Item> seams;
  {
    const double same = std::max(1e-3, diag * 1e-7);
    // The parts of straight line `a` that lie on straight line `b` (parameters along a, 0..1).
    auto overlap = [&](const Item& a, const Item& b) -> std::optional<std::pair<double, double>> {
      if (a.kind != Item::Kind::Line || b.kind != Item::Kind::Line) return std::nullopt;
      const gp_Pnt2d &p = a.points[0], &q = a.points[1];
      const gp_Vec2d d(p, q);
      const double len = d.Magnitude();
      if (len < same) return std::nullopt;
      const gp_Vec2d u = d / len;
      auto off = [&](const gp_Pnt2d& r) { return std::abs(u.Crossed(gp_Vec2d(p, r))); };
      if (off(b.points[0]) > same || off(b.points[1]) > same) return std::nullopt;
      double t0 = u.Dot(gp_Vec2d(p, b.points[0])) / len, t1 = u.Dot(gp_Vec2d(p, b.points[1])) / len;
      if (t0 > t1) std::swap(t0, t1);
      t0 = std::max(t0, 0.0);
      t1 = std::min(t1, 1.0);
      if ((t1 - t0) * len < same) return std::nullopt;
      return std::make_pair(t0, t1);
    };
    std::vector<std::vector<std::pair<double, double>>> drop(cut.size());
    for (std::size_t i = 0; i < wallCuts.size(); ++i)
      for (std::size_t j = 0; j < wallCuts.size(); ++j) {
        if (i == j || wallCuts[i].layers != wallCuts[j].layers) continue;
        for (std::size_t a = wallCuts[i].from; a < wallCuts[i].to; ++a)
          for (std::size_t b = wallCuts[j].from; b < wallCuts[j].to; ++b)
            if (const auto o = overlap(cut[a], cut[b])) drop[a].push_back(*o);
      }
    std::vector<Item> kept;
    for (std::size_t a = 0; a < cut.size(); ++a) {
      if (drop[a].empty()) {
        kept.push_back(std::move(cut[a]));
        continue;
      }
      std::sort(drop[a].begin(), drop[a].end());
      const gp_Pnt2d p = cut[a].points[0], q = cut[a].points[1];
      auto at = [&](double s) { return gp_Pnt2d(p.XY() + (q.XY() - p.XY()) * s); };
      double from = 0.0;
      for (const auto& [d0, d1] : drop[a]) {
        if (d0 > from) kept.push_back(line(cut[a].pen, at(from), at(d0)));
        if (d1 > from) seams.push_back(line(cut[a].pen, at(std::max(from, d0)), at(d1)));
        from = std::max(from, d1);
      }
      if (from < 1.0) kept.push_back(line(cut[a].pen, at(from), at(1.0)));
    }
    // Pieces shorter than the tolerance left by the splitting are not lines.
    cut.clear();
    for (Item& k : kept)
      if (k.kind != Item::Kind::Line || k.points[0].Distance(k.points[1]) >= same) cut.push_back(std::move(k));
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
      const auto faces = out.VCompound(), silhouette = out.OutLineVCompound();
      addLocalEdges(faces, Pen::Visible, deflection, visible);
      addLocalEdges(silhouette, Pen::Visible, deflection, visible);
      if (spec.hidden) {
        addLocalEdges(out.HCompound(), Pen::Hidden, deflection, hidden);
        addLocalEdges(out.OutLineHCompound(), Pen::Hidden, deflection, hidden);
      }
    } catch (const Standard_Failure& e) {
      view.notes.push_back("Hidden lines could not be worked out: " + kernelMessage(e));
    }
  }
  if (sitePlan) {
    if (roadEdge) {
      try {
        clearance = roadClearance(*roadEdge, visible);
      } catch (const Standard_Failure& e) {
        view.notes.push_back("Road clearance could not be worked out: " + kernelMessage(e));
      }
    }
    if (visible.empty()) view.calculationNotes[1] = "一点鎖線：敷地境界　建物の上面投影なし";
    if (clearance) {
      view.calculationNotes.push_back("道路側境界から建物上面投影線までの最短距離（参考） " + clearance->text);
      if (clearance->a.Distance(clearance->b) > tolerance::kLinear) view.dimensions.push_back(*clearance);
    } else if (roadEdge) {
      view.calculationNotes.push_back("道路側境界からの距離は未評価（建物投影を確認してください）");
    }
    view.calculationNotes.push_back("法令上の道路・接道、離隔・後退は未評価（地盤面は地盤面算出表）");
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
  auto onSeam = [&](const Item& item) {
    if (seams.empty()) return false;
    for (const gp_Pnt2d& p : samples(item)) {
      bool on = false;
      for (const Item& s : seams) {
        if (distanceTo(s, p) < same) {
          on = true;
          break;
        }
      }
      if (!on) return false;
    }
    return true;
  };
  for (Item& v : visible) {
    if ((cut.empty() || !onCut(v)) && !onSeam(v)) view.items.push_back(std::move(v));
  }
  for (Item& h : hidden) view.items.push_back(std::move(h));
  for (Item& l : loose) view.items.push_back(std::move(l));
  for (Item& l : layerLines) view.items.push_back(std::move(l));  // over the fills
  for (Item& h : hatches) view.items.push_back(std::move(h));     // docs/173
  // Door / window symbols of the openings this plan cuts through, to their real width and
  // the wall's thickness (OpeningSymbols.h).
  if (spec.kind == ViewKind::Plan && std::abs(n.Z()) > 1 - 1e-9) {
    SymbolOptions symbols;
    symbols.z = spec.frame.Location().Z();
    symbols.only = &drawn;
    for (const TopoDS_Edge& e : openingSymbols(document, symbols))
      addLocalEdge(TopoDS::Edge(e.Moved(TopLoc_Location(proj.toLocal))), Pen::Visible, deflection, view.items);
    // Where the layers of walls of a build-up meet, in the cut (docs/124): thin lines.
    for (const auto& [a, b] : doc::Modeler::wallLayerLines(document, symbols.z, drawn))
      view.items.push_back(line(Pen::Visible, proj.uv(a), proj.uv(b)));
    // Stairs (docs/112): the walking line up to the cut with its arrow, the break line, "上".
    for (const doc::Object& o : document.objects()) {
      if (!drawn.count(o.id)) continue;
      const auto kind = o.attributes.find("kind");
      if (kind == o.attributes.end() || kind->second != "stair") continue;
      const auto stair = doc::stairInWorld(document, o);
      if (!stair) continue;
      const auto marks = geom::stairPlanMarks(*stair, symbols.z);
      if (!marks) continue;
      auto uv = [&](const gp_Pnt& p) { return proj.uv(gp_Pnt(p.X(), p.Y(), symbols.z)); };
      for (std::size_t i = 1; i < marks->walk.size(); ++i)
        view.items.push_back(line(Pen::Visible, uv(marks->walk[i - 1]), uv(marks->walk[i])));
      for (const auto& [a, b] : marks->arrow) view.items.push_back(line(Pen::Visible, uv(a), uv(b)));
      for (std::size_t i = 1; i < marks->breakLine.size(); ++i)
        view.items.push_back(line(Pen::Visible, uv(marks->breakLine[i - 1]), uv(marks->breakLine[i])));
      if (spec.annotations) {
        Item up;
        up.kind = Item::Kind::Text;
        up.pen = Pen::Text;
        up.points = {uv(marks->label)};
        up.text = "上";
        up.height = 2.5;
        up.hAlign = 1;
        up.vAlign = 1;
        view.items.push_back(up);
      }
    }
    // Their marks, the same as in the door and window schedule (docs/104), beside the
    // opening on the side it was cut from.
    if (spec.annotations) {
      for (const doc::OpeningRow& r : doc::openingSchedule(document).rows) {
        if (!drawn.count(r.host) || symbols.z < r.bottom || symbols.z >= r.bottom + r.height) continue;
        Item mark;
        mark.kind = Item::Kind::Text;
        mark.pen = Pen::Text;
        mark.points = {proj.uv(gp_Pnt(r.center.X(), r.center.Y(), symbols.z).Translated(gp_Vec(r.normal) * 300.0))};
        mark.text = r.mark;
        mark.height = 2.5;
        mark.hAlign = 1;
        mark.vAlign = 1;
        view.items.push_back(mark);
      }
    }
  }
  if (sitePlan || (spec.kind == ViewKind::Plan && spec.framingPreset.empty() &&
                   !spec.memberLabels && spec.bracingFloor.empty() && !spec.insulation && spec.within.empty())) {
    const auto fire = doc::fireSpreadStudy(document, true, false);
    if (fire.configured && !fire.issue.empty()) view.calculationNotes.push_back(fire.issue);
    if (!fire.conditions.source.empty()) {
      view.calculationNotes.push_back("延焼ラインの基準線資料　" + fire.conditions.source);
      view.calculationNotes.push_back("延焼ラインは登録した基準線からの範囲。例外と防火設備は別途照合。");
    }
    int threshold = 0;
    if (!sitePlan) {
      for (const auto& floor : fire.floors) {
        if (floor.z >= spec.frame.Location().Z()) break;
        threshold = floor.metres;
      }
    }
    const auto addFireLabel = [&](gp_Pnt2d at, Pen pen, const char* label) {
      Item text;
      text.kind = Item::Kind::Text;
      text.pen = pen;
      at.SetY(at.Y() + 0.9 * (spec.paperScale > 0 ? spec.paperScale : 100.0));
      text.points = {at};
      text.text = label;
      text.height = 2.5;
      text.hAlign = 1;
      view.items.push_back(std::move(text));
    };
    const auto addContour = [&](const TopoDS_Shape& shape, Pen pen, const char* label) {
      if (shape.IsNull()) return;
      const auto start = view.items.size();
      addLocalEdges(shape.Moved(TopLoc_Location(proj.toLocal)), pen, deflection, view.items);
      if (start == view.items.size() || (!sitePlan && !spec.annotations)) return;
      const Item* longest = nullptr;
      double length = 0;
      for (std::size_t i = start; i < view.items.size(); ++i) {
        const auto& item = view.items[i];
        if (item.kind != Item::Kind::Line || item.points.size() != 2) continue;
        const double current = item.points[0].Distance(item.points[1]);
        if (current > length) { length = current; longest = &item; }
      }
      gp_Pnt2d at;
      if (longest) {
        const auto& a = longest->points[0];
        const auto& b = longest->points[1];
        at.SetCoord((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2);
      } else {
        const auto& item = view.items[start];
        if (!item.points.empty()) at = item.points.front();
        else if (item.kind == Item::Kind::Arc) {
          const double angle = item.start + item.sweep / 2;
          at = item.centre.Translated(gp_Vec2d(std::cos(angle), std::sin(angle)) * item.radius);
        } else return;
      }
      addFireLabel(at, pen, label);
    };
    if (sitePlan) {
      for (const auto& basis : fire.bases) {
        if (!basis.road) continue;
        const auto a = proj.uv(basis.visibleLine[0]), b = proj.uv(basis.visibleLine[1]);
        view.items.push_back(line(Pen::Level, a, b));
        addFireLabel(gp_Pnt2d((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2), Pen::Level, "道路中心線（延焼の基準）");
      }
    }
    if (sitePlan || threshold == 3) addContour(fire.limits3, Pen::FireLimit3, "延焼ライン 1階 3 m");
    if (sitePlan || threshold == 5) addContour(fire.limits5, Pen::FireLimit5, "延焼ライン 2階以上 5 m");
  }
  for (Item& c : cut) view.items.push_back(std::move(c));
  if (compartmentView && spec.kind == ViewKind::Plan && std::abs(n.Z()) > 1 - 1e-9) {
    const double z = spec.frame.Location().Z();
    for (const auto& entry : doc::fireCompartmentReview(document, true)) {
      if (!drawn.count(entry.host)) continue;
      if (entry.part == doc::FireCompartmentRow::Part::FloorOpening && !entry.marker.IsNull()) {
        Bnd_Box hole;
        BRepBndLib::Add(entry.marker, hole);
        if (hole.IsVoid()) continue;
        const auto hi = hole.CornerMax();
        if (hi.Z() > z + kOnPlane || hi.Z() < z - (spec.depth > 0 ? spec.depth : 1500.0)) continue;
        addLocalEdges(entry.marker.Moved(TopLoc_Location(proj.toLocal)), Pen::FireCompartment, deflection, compartmentLines);
        Bnd_Box projected;
        BRepBndLib::Add(entry.marker.Moved(TopLoc_Location(proj.toLocal)), projected);
        const auto topRight = projected.CornerMax();
        const auto left = projected.CornerMin();
        compartmentLabel(gp_Pnt2d((left.X() + topRight.X()) / 2, topRight.Y()), entry.boundary.mark + " / 床開口");
        continue;
      }
      if (!entry.wallOpening) continue;
      const auto& op = *entry.wallOpening;
      if (z < op.bottom || z >= op.bottom + op.height) continue;
      const gp_Vec across(-op.normal.Y() * op.width / 2, op.normal.X() * op.width / 2, 0);
      const gp_Pnt center = gp_Pnt(op.center.X(), op.center.Y(), z).Translated(gp_Vec(op.normal) * (-op.depth / 2));
      compartmentLines.push_back(line(Pen::FireCompartment, proj.uv(center.Translated(-across)), proj.uv(center.Translated(across))));
      compartmentLabel(proj.uv(center), entry.boundary.mark + " / " + entry.openingMark);
    }
  }
  if (!compartmentLines.empty()) {
    view.calculationNotes.push_back("二点鎖線（青緑） 防火区画の登録部材と開口部。仕様は防火区画・開口部確認表を参照。");
    for (auto& item : compartmentLines) view.items.push_back(std::move(item));
  }

  // docs/208: the bracing walls of one storey of the wall calculation and its quarter division.
  if (spec.kind == ViewKind::Plan && !spec.bracingFloor.empty()) {
    const auto made = construction::checkWallQuantity(document);
    const nlohmann::json report = made ? nlohmann::json::parse(*made, nullptr, false) : nlohmann::json();
    const double z = spec.frame.Location().Z();
    const auto at = [&](const nlohmann::json& xy) {
      return proj.uv(gp_Pnt(xy.at(0).get<double>(), xy.at(1).get<double>(), z));
    };
    int walls = 0;
    if (report.is_object()) {
      for (const auto& w : report.value("walls", nlohmann::json::array())) {
        if (w.value("floor", std::string()) != spec.bracingFloor || !w.contains("start") || !w.contains("end")) continue;
        const gp_Pnt2d a = at(w.at("start")), b = at(w.at("end"));
        Item line;
        line.kind = Item::Kind::Line;
        line.pen = w.value("status", std::string()) == "evaluated" ? Pen::Cut : Pen::Hidden;
        line.points = {a, b};
        view.items.push_back(line);
        const std::string id = w.contains("id") && w.at("id").is_string() ? w.at("id").get<std::string>() : std::string();
        std::string label = id + " " + w.value("direction", std::string());
        if (w.contains("ratio") && w.at("ratio").is_number()) label += " 倍率" + std::to_string(w.at("ratio").get<double>()).substr(0, 3);
        if (w.contains("effective_m") && w.at("effective_m").is_number()) {
          char buf[32];
          std::snprintf(buf, sizeof buf, " %.2fm", w.at("effective_m").get<double>());
          label += buf;
        }
        view.bracingMarks.push_back({{(a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2}, std::move(label), 2.5});
        ++walls;
      }
      for (const auto& row : report.value("quarter_balance", nlohmann::json::array())) {
        if (row.value("floor", std::string()) != spec.bracingFloor) continue;
        for (const auto& strip : row.value("strips", nlohmann::json::array())) {
          if (!strip.contains("rect")) continue;
          const auto& r = strip.at("rect");
          const double x0 = r.at(0).at(0).get<double>(), y0 = r.at(0).at(1).get<double>(), x1 = r.at(1).at(0).get<double>(), y1 = r.at(1).at(1).get<double>();
          Item box;
          box.kind = Item::Kind::Polyline;
          box.pen = Pen::Hidden;
          box.closed = true;
          box.points = {proj.uv(gp_Pnt(x0, y0, z)), proj.uv(gp_Pnt(x1, y0, z)), proj.uv(gp_Pnt(x1, y1, z)), proj.uv(gp_Pnt(x0, y1, z))};
          view.items.push_back(box);
          char buf[96];
          std::snprintf(buf, sizeof buf, "%s 側端 必要%.2fm 存在%.2fm", row.value("direction", std::string()).c_str(),
                        strip.value("required_m", 0.0), strip.value("existing_m", 0.0));
          view.bracingMarks.push_back({proj.uv(gp_Pnt((x0 + x1) / 2, (y0 + y1) / 2, z)), buf, 2.0});
        }
      }
    }
    view.calculationNotes.push_back(walls > 0 ? "太線：壁量に入れた耐力壁（符号・方向・倍率・有効長さ）　破線：入れていない壁、四分割の側端部分"
                                              : "この階の耐力壁は、壁量の計算の入力にありません");
  }

  // docs/247: the words in the insulation ranges, placed after the room tags (so they keep clear of them).
  struct RangeLabel {
    double u0, v0, u1, v1;
    std::string text;
  };
  std::vector<RangeLabel> rangeLabels;
  std::vector<Item> crossLines;  // the ranges' crosses, drawn after the words (kept clear of them)
  // docs/216: the insulation of the walls the plan cuts (a zigzag along the middle of each), with a
  // legend; the floors' and roofs' insulation listed (not drawn on a plan).
  if (spec.kind == ViewKind::Plan && spec.insulation) {
    const double z = spec.frame.Location().Z();
    std::map<std::string, std::set<doc::ObjectId>> walls;
    std::set<std::string> places;
    for (const auto& run : doc::Modeler::insulationRuns(document, z)) {
      auto zigzag = insulationZigzag(proj.uv(run.a), proj.uv(run.b), run.thickness, spec.paperScale);
      if (!zigzag) continue;
      view.items.push_back(std::move(*zigzag));
      walls[run.text].insert(run.wall);
      places.insert(run.place);
    }
    // docs/221, docs/222: the insulated floor of this storey and the roof seen from it, as ranges the
    // way the examples for 省エネ drawings show them: the outline and a cross (×) over it, thin, with
    // the part and the insulation written inside (the roof's a little above the middle).
    std::map<std::string, std::map<std::string, int>> areas;  // part -> insulation -> count
    for (const auto& area : doc::Modeler::insulationAreas(document, z)) {
      const std::size_t from = view.items.size();
      addLocalEdges(area.outline.Moved(TopLoc_Location(proj.toLocal)), Pen::Visible, deflection, view.items);
      if (view.items.size() == from) continue;
      double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
      for (std::size_t k = from; k < view.items.size(); ++k)
        for (const gp_Pnt2d& p : view.items[k].points) {
          u0 = std::min(u0, p.X());
          v0 = std::min(v0, p.Y());
          u1 = std::max(u1, p.X());
          v1 = std::max(v1, p.Y());
        }
      if (u0 > u1) continue;
      // The cross: the diagonals of the range's box, kept where they lie in the range.
      if (!area.region.IsNull()) {
        const Bnd_Box box = geom::exactBounds(area.region);
        if (!box.IsVoid()) {
          const gp_Pnt lo = box.CornerMin(), hi = box.CornerMax();
          const double zm = (lo.Z() + hi.Z()) / 2;
          const gp_Pnt corners[2][2] = {{gp_Pnt(lo.X(), lo.Y(), zm), gp_Pnt(hi.X(), hi.Y(), zm)},
                                        {gp_Pnt(lo.X(), hi.Y(), zm), gp_Pnt(hi.X(), lo.Y(), zm)}};
          for (const auto& d : corners) {
            if (d[0].Distance(d[1]) < 1.0) continue;
            BRepBuilderAPI_MakeEdge diagonal(d[0], d[1]);
            if (!diagonal.IsDone()) continue;
            BRepAlgoAPI_Common inside(diagonal.Edge(), area.region);
            if (inside.IsDone()) addLocalEdges(inside.Shape().Moved(TopLoc_Location(proj.toLocal)), Pen::Visible, deflection, crossLines);
          }
        }
      }
      // docs/247: the part only ("床断熱"): the insulation is in the legend. The whole text was wider
      // than a house at 1:100 and lay on the room tags and the cross (the sheet check found it).
      rangeLabels.push_back({u0, v0, u1, v1, area.part + "断熱"});
      ++areas[area.part][area.text];
    }
    if (walls.empty()) view.calculationNotes.push_back("この階で切る壁に、断熱の入力はありません");
    for (const auto& [text, ids] : walls)
      view.calculationNotes.push_back("波線：壁の断熱　" + text + "（" + std::to_string(ids.size()) + " 枚）");
    for (const char* part : {"床", "天井", "屋根"}) {  // docs/236: ceilings
      const auto it = areas.find(part);
      if (it == areas.end()) continue;
      const std::string name = std::string(part) == "床" ? "この階の床" : std::string(part) == "天井" ? "この階の天井" : "屋根";
      for (const auto& [text, count] : it->second)
        view.calculationNotes.push_back("×印の範囲：" + name + "の断熱　" + text + "（" + std::to_string(count) + " 個）");
    }
    for (std::string& note : insulationPlaceNotes(places)) view.calculationNotes.push_back(std::move(note));
  }
  // docs/217: the legend of a section's insulation marks.
  if (spec.kind == ViewKind::Section && spec.insulation) {
    if (insulationMarked.empty()) view.calculationNotes.push_back("この断面で切る壁・床・屋根に、断熱の入力はありません");
    for (const char* part : {"壁", "床", "天井", "屋根"}) {
      const auto it = insulationMarked.find(part);
      if (it == insulationMarked.end()) continue;
      for (const auto& [text, ids] : it->second)
        view.calculationNotes.push_back(std::string("波線：") + part + "の断熱　" + text + "（" + std::to_string(ids.size()) + " 個）");
    }
    for (const std::string& t : insulationNotMarked)
      view.calculationNotes.push_back(t + "：この断面では印を描けませんでした（壁に沿って切っている等）");
    for (std::string& note : insulationPlaceNotes(insulationPlaces)) view.calculationNotes.push_back(std::move(note));
  }

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

  // Section cut marks (docs/132): plans only.
  if (spec.kind == ViewKind::Plan)
    for (const ViewSpec::CutMark& c : spec.cutMarks) {
      const gp_Dir toward(c.towardsViewer.X(), c.towardsViewer.Y(), 0);
      const gp_Dir right = gp::DZ().Crossed(toward);
      const gp_Dir a = proj.localDir(right), l = proj.localDir(toward.Reversed());
      if (std::hypot(a.X(), a.Y()) < 1e-6 || std::hypot(l.X(), l.Y()) < 1e-6) continue;
      View::CutLine cut;
      cut.point = proj.uv(c.point);
      cut.along = gp_Dir2d(a.X(), a.Y());
      cut.look = gp_Dir2d(l.X(), l.Y());
      cut.label = c.label;
      cut.sheet = c.sheet;
      view.cuts.push_back(cut);
    }
  // Grids: lines across the view with their names; spans between them.
  if (spec.grid) {
    for (const doc::Object& o : document.objects()) {
      const auto* node = std::get_if<doc::GroupNode>(&o.recipe.base);
      if (!node || !node->grid || (!spec.includeHidden && !document.isVisible(o.id))) continue;
      if (!spec.within.empty() && o.id != spec.within && !document.isAncestor(spec.within, o.id) &&
          !gridIds.count(o.id))
        continue;
      if (!spec.tags.empty() && !gridIds.empty() && !gridIds.count(o.id)) continue;
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
    std::optional<std::map<doc::ObjectId, std::string>> roomTags;
    std::map<doc::ObjectId, std::vector<gp_Pnt2d>> roomOutlines;
    for (const doc::Object& o : document.objects()) {
      if (!selected(document, o, spec) || o.attributes.count("grid_part")) continue;
      const gp_Trsf w = document.worldTransform(o);
      if (const auto* d = std::get_if<doc::LinearDimension>(&o.recipe.base)) {
        const gp_Pnt a = d->start.Transformed(w), b = d->end.Transformed(w);
        const gp_Vec off = d->offset.Transformed(w);
        // Only dimensions lying in the view's plane direction.
        if (std::abs(gp_Vec(a, b).Dot(gp_Vec(n))) > 1e-6 || std::abs(off.Dot(gp_Vec(n))) > 1e-6) continue;
        if (cutting && proj.local(a).Z() > kOnPlane) continue;
        if (cutting && spec.kind == ViewKind::Plan && spec.depth > 0 && proj.local(a).Z() < -spec.depth) continue;  // docs/251
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
        // docs/251: as deep as the plan looks and no deeper: the rooms of the storey below were
        // written on the upper plan through its floor (the AI's look at the hands-on pictures).
        if (cutting && spec.kind == ViewKind::Plan && spec.depth > 0 && proj.local(p).Z() < -spec.depth) continue;
        Item text;
        text.kind = Item::Kind::Text;
        text.pen = Pen::Text;
        text.points = {proj.uv(p)};
        text.text = t->text;
        // A room's tag: its name and its area as the walls are now (docs/104).
        if (const auto kind = o.attributes.find("kind"); kind != o.attributes.end() && kind->second == "room") {
          // docs/247: a plan's only (the sheet check found the tags on the floor line of elevations and
          // sections), and centred on the marker (written to its right, it ran over walls and doors).
          if (spec.kind != ViewKind::Plan) continue;
          text.hAlign = 1;
          text.vAlign = 1;
          if (!roomTags) {
            roomTags.emplace();
            for (const doc::RoomRow& r : doc::roomSchedule(document).rows) (*roomTags)[r.id] = doc::roomTagText(r);
            if (!spec.bracingFloor.empty()) {
              for (const doc::RoomInfo& room : doc::Modeler::roomsIn(document)) {
                if (!room.closed || !room.problem.empty()) continue;
                auto& outline = roomOutlines[room.id];
                for (const gp_Pnt& corner : room.innerKnown ? room.inner : room.centre) outline.push_back(proj.uv(corner));
              }
            }
          }
          if (const auto tag = roomTags->find(o.id); tag != roomTags->end()) text.text = tag->second;
          const auto outline = roomOutlines.find(o.id);
          if (!t->target && outline != roomOutlines.end())
            view.roomTags.push_back({view.items.size(), std::move(outline->second)});
        }
        text.height = 3.5;
        view.items.push_back(text);
        if (t->target) view.items.push_back(line(Pen::Text, proj.uv(t->target->Transformed(w)), proj.uv(p)));
      }
    }
  }
  // docs/247: each range's words where no line runs through them and no other words lie: tried along
  // the top of the range first (the cross's diagonals are near the corners there), then lower; the
  // first place the sheet check finds clear. The ranges' words one under another when they share it.
  if (!rangeLabels.empty()) {
    const double scale = spec.paperScale > 0 ? spec.paperScale : 100.0;
    Sheet probe;
    probe.scale = scale;
    probe.paperWidth = probe.paperHeight = 1e12;
    probe.items = view.items;
    probe.items.insert(probe.items.end(), crossLines.begin(), crossLines.end());  // the crosses avoided too
    for (const RangeLabel& r : rangeLabels) {
      Item label;
      label.kind = Item::Kind::Text;
      label.pen = Pen::Text;
      label.text = r.text;
      label.height = 2.5;
      label.hAlign = 1;
      label.vAlign = 1;
      const double w = r.u1 - r.u0, d = r.v1 - r.v0;
      std::optional<gp_Pnt2d> chosen;
      for (const double fv : {0.12, 0.2, 0.3, 0.4, 0.6, 0.7, 0.8, 0.88}) {
        for (const double fu : {0.5, 0.35, 0.65, 0.25, 0.75, 0.15, 0.85}) {
          label.points = {gp_Pnt2d(r.u0 + fu * w, r.v1 - fv * d)};
          probe.items.push_back(label);
          bool clear = true;
          for (const SheetFinding& f : checkSheet(probe, 100000))
            if ((f.kind == "line_through_text" || f.kind == "text_on_text") && (f.text == r.text || f.other == r.text)) clear = false;
          probe.items.pop_back();
          if (clear) {
            chosen = label.points[0];
            break;
          }
        }
        if (chosen) break;
      }
      label.points = {chosen ? *chosen : gp_Pnt2d((r.u0 + r.u1) / 2, r.v1 - 0.12 * d)};
      probe.items.push_back(label);
      view.items.push_back(label);
    }
  }
  // docs/247: the crosses stop short of the words they would run through (room tags, the ranges' words).
  if (!crossLines.empty()) {
    const double scale = spec.paperScale > 0 ? spec.paperScale : 100.0;
    std::vector<const Item*> words;
    for (const Item& i : view.items)
      if (i.kind == Item::Kind::Text) words.push_back(&i);
    std::vector<Item> kept;
    for (const Item& c : crossLines) {
      if (c.kind != Item::Kind::Line || c.points.size() != 2) {
        kept.push_back(c);
        continue;
      }
      for (const auto& [p, q] : clearOfText(c.points[0], c.points[1], words, scale, 0.8)) {
        Item piece = c;
        piece.points = {p, q};
        kept.push_back(piece);
      }
    }
    view.items.insert(view.items.end(), kept.begin(), kept.end());
  }
  if (spec.memberLabels) {
    const auto quantities = construction::takeOff(
        document,
        nlohmann::json{{"tags", spec.tags}, {"within", spec.within}, {"visible_only", !spec.includeHidden}}
            .dump());
    if (!quantities) return R::failure(quantities.error());
    const auto report = nlohmann::json::parse(*quantities);
    for (const auto& part : report.at("parts")) {
      if (!drawn.count(part.at("id").get<std::string>())) continue;
      const auto& at = part.at("position");
      const gp_Pnt p(at[0].get<double>(), at[1].get<double>(), at[2].get<double>());
      const double depth = proj.local(p).Z();
      if (cutting && (depth > kOnPlane || depth < -keepDepth)) continue;
      view.members.push_back(
          {proj.uv(p), part.at("mark").get<std::string>() + " " + part.at("section").get<std::string>()});
    }
    if (!report.at("complete").get<bool>())
      view.notes.push_back("部材情報の不足・編集後の寸法など、拾い出しに未評価があります");
  }
  if (!sitePlan && document.settings().wallCalculation != "{}") {
    const auto wall = construction::checkWallQuantity(document);
    if (wall) {
      const auto report = nlohmann::json::parse(*wall);
      view.calculationNotes.push_back(report.at("notice"));
      view.calculationNotes.push_back(
          "壁量の結果・入力範囲は別の壁量表を参照。図面のタグや表示範囲と計算範囲は同一とは限りません。");
    } else
      view.calculationNotes.push_back("壁量は未評価: " + wall.error());
  }
  if (!spec.framingPreset.empty()) {
    view.notes.push_back(
        "意匠・概略の検討用。断面・接合の照査は行っていません。符号は同じ対象条件の拾い出し表に対応します");
    if (spec.framingPreset == "foundation")
      view.notes.push_back("選択タグの形状だけを描きます。基礎形状の有無・寸法は別途確認してください");
  }
  if (view.items.empty() && view.grids.empty()) {
    return R::failure(cutting ? "Nothing on this side of the cut: check the cut height or the side looked at"
                              : "Nothing to draw from this side");
  }
  return view;
}

}  // namespace sbr::draw
