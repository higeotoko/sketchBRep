#include "sbr/geom/Surfaces.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <BRepAdaptor_CompCurve.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <gp_Ax3.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Trsf.hxx>
#include <optional>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeFillet2d.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <cstdio>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_Shape.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include "sbr/geom/Validation.h"

namespace sbr::geom {
namespace {

std::string occtMessage(const Standard_Failure& e) {
  const char* m = e.GetMessageString();
  return m && *m ? m : "geometry kernel error";
}

// The curve of a face (its outline) or a curve, as one wire.
Result<TopoDS_Wire> wireOf(const TopoDS_Shape& s) {
  if (s.IsNull()) return Result<TopoDS_Wire>::failure("Empty curve");
  if (s.ShapeType() == TopAbs_FACE) return BRepTools::OuterWire(TopoDS::Face(s));
  if (s.ShapeType() == TopAbs_WIRE) return TopoDS::Wire(s);
  if (s.ShapeType() == TopAbs_EDGE) return BRepBuilderAPI_MakeWire(TopoDS::Edge(s)).Wire();
  TopExp_Explorer w(s, TopAbs_WIRE);
  if (w.More()) return TopoDS::Wire(w.Current());
  // Loose edges: chained.
  Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
  for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next()) edges->Append(e.Current());
  if (edges->IsEmpty()) return Result<TopoDS_Wire>::failure("Not a curve");
  Handle(TopTools_HSequenceOfShape) wires;
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-4, Standard_False, wires);
  if (wires.IsNull() || wires->Length() != 1) return Result<TopoDS_Wire>::failure("The curve's pieces do not join into one");
  return TopoDS::Wire(wires->Value(1));
}

// Every edge of the curves, in the order they join into one closed loop.
Result<std::vector<TopoDS_Edge>> loopOf(const std::vector<TopoDS_Shape>& curves) {
  using R = Result<std::vector<TopoDS_Edge>>;
  Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
  for (const TopoDS_Shape& c : curves) {
    for (TopExp_Explorer e(c, TopAbs_EDGE); e.More(); e.Next()) edges->Append(e.Current());
  }
  if (edges->IsEmpty()) return R::failure("No curves");
  Handle(TopTools_HSequenceOfShape) wires;
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-3, Standard_False, wires);
  if (wires.IsNull() || wires->Length() != 1) {
    return R::failure("The curves do not join end to end into one outline");
  }
  const TopoDS_Wire wire = TopoDS::Wire(wires->Value(1));
  if (!BRep_Tool::IsClosed(wire)) return R::failure("The curves do not close into one outline (a gap is left)");
  std::vector<TopoDS_Edge> out;
  for (BRepTools_WireExplorer e(wire); e.More(); e.Next()) out.push_back(e.Current());
  return out;
}

Result<TopoDS_Shape> filling(const std::vector<TopoDS_Edge>& loop) {
  BRepOffsetAPI_MakeFilling fill;
  for (const TopoDS_Edge& e : loop) fill.Add(e, GeomAbs_C0);
  fill.Build();
  if (!fill.IsDone()) return Result<TopoDS_Shape>::failure("No surface could be made over these curves");
  const TopoDS_Shape s = fill.Shape();
  if (!BRepCheck_Analyzer(s).IsValid()) return Result<TopoDS_Shape>::failure("The surface over these curves is not valid");
  return s;
}

}  // namespace

Result<TopoDS_Wire> curveWire(const TopoDS_Shape& curve) { return wireOf(curve); }

Result<TopoDS_Shape> sweep2(const TopoDS_Wire& rail1In, const TopoDS_Wire& rail2In, const std::vector<TopoDS_Shape>& profiles) {
  using R = Result<TopoDS_Shape>;
  if (profiles.empty()) return R::failure("Sweep2 needs at least one profile");
  try {
    // Each profile is fitted between the rails at stations along them (by
    // the same share of each rail's length): its chord (the points nearest
    // each rail) is turned and stretched to span rail1 -> rail2 there, its
    // height kept. The fitted sections are then lofted smoothly.
    BRepAdaptor_CompCurve c1(rail1In, Standard_True);
    TopoDS_Wire rail2Wire = rail2In;
    {
      BRepAdaptor_CompCurve probe(rail2In, Standard_True);
      const gp_Pnt s1 = c1.Value(c1.FirstParameter());
      if (s1.Distance(probe.Value(probe.LastParameter())) < s1.Distance(probe.Value(probe.FirstParameter()))) {
        rail2Wire = TopoDS::Wire(rail2In.Reversed());
      }
    }
    BRepAdaptor_CompCurve c2(rail2Wire, Standard_True);
    const double len1 = GCPnts_AbscissaPoint::Length(c1);
    const double len2 = GCPnts_AbscissaPoint::Length(c2);
    if (len1 < 1e-6 || len2 < 1e-6) return R::failure("A rail has no length");
    auto at = [](BRepAdaptor_CompCurve& c, double len, double share) {
      const double u = share <= 0 ? c.FirstParameter()
                       : share >= 1 ? c.LastParameter()
                                    : GCPnts_AbscissaPoint(c, share * len, c.FirstParameter()).Parameter();
      gp_Pnt p;
      gp_Vec t;
      c.D1(u, p, t);
      return std::make_pair(p, t);
    };
    // The frame at a station: X along the chord, Y along rail1, Z the rest.
    auto frame = [](const gp_Pnt& a, const gp_Pnt& b, gp_Vec along) -> std::optional<gp_Ax3> {
      gp_Vec x(a, b);
      if (x.Magnitude() < 1e-6) return std::nullopt;
      x.Normalize();
      along -= x * along.Dot(x);
      if (along.Magnitude() < 1e-9) return std::nullopt;
      const gp_Dir y(along);
      return gp_Ax3(a, gp_Dir(x.Crossed(gp_Vec(y))), gp_Dir(x));
    };
    struct Station { double share; TopoDS_Wire wire; gp_Pnt a, b; gp_Ax3 frame; };
    std::vector<Station> fitted;
    bool closed = true;
    const TopoDS_Shape r1 = rail1In, r2 = rail2Wire;
    for (const TopoDS_Shape& p : profiles) {
      auto wire = wireOf(p);
      if (!wire) return R::failure(wire.error());
      closed = closed && BRep_Tool::IsClosed(*wire);
      BRepExtrema_DistShapeShape d1(*wire, r1), d2(*wire, r2);
      if (!d1.IsDone() || !d2.IsDone() || d1.NbSolution() == 0 || d2.NbSolution() == 0) {
        return R::failure("Could not tell where a profile meets the rails");
      }
      const gp_Pnt a = d1.PointOnShape1(1), b = d2.PointOnShape1(1);
      // Share along rail1 of the point nearest the profile.
      const gp_Pnt onRail = d1.PointOnShape2(1);
      double best = 0, bestD = 1e300;
      for (int i = 0; i <= 400; ++i) {
        const double d = at(c1, len1, i / 400.0).first.Distance(onRail);
        if (d < bestD) bestD = d, best = i / 400.0;
      }
      auto f = frame(a, b, at(c1, len1, best).second);
      if (!f) return R::failure("A profile must reach across from the first rail to the second");
      fitted.push_back({best, *wire, a, b, *f});
    }
    std::sort(fitted.begin(), fitted.end(), [](const Station& x, const Station& y) { return x.share < y.share; });
    // Stations: the rail ends, every profile, and (between) evenly spaced
    // ones so the sections follow curved rails.
    std::vector<std::pair<double, const Station*>> stations;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
      const double share = double(i) / steps;
      const Station* src = &fitted.front();
      for (const Station& s : fitted) if (s.share <= share + 1e-9) src = &s;
      stations.push_back({share, src});
    }
    if (fitted.size() > 1) {
      // Several profiles: only their own stations plus the rail ends, so
      // each one is passed through as drawn.
      stations.clear();
      if (fitted.front().share > 1e-3) stations.push_back({0.0, &fitted.front()});
      for (const Station& s : fitted) stations.push_back({s.share, &s});
      if (fitted.back().share < 1 - 1e-3) stations.push_back({1.0, &fitted.back()});
    }
    BRepOffsetAPI_ThruSections loft(closed ? Standard_True : Standard_False, Standard_False, 1e-4);
    loft.CheckCompatibility(Standard_False);
    for (const auto& [share, src] : stations) {
      const auto [p1, t1] = at(c1, len1, share);
      const gp_Pnt p2 = at(c2, len2, share).first;
      auto f = frame(p1, p2, t1);
      if (!f) return R::failure("The rails touch or run along each other; nothing to span");
      const double stretch = p1.Distance(p2) / src->a.Distance(src->b);
      // Profile -> its own frame -> stretched along X -> the station frame.
      gp_Trsf toLocal;
      toLocal.SetTransformation(src->frame);
      gp_Trsf toWorld;
      toWorld.SetTransformation(*f);
      toWorld.Invert();
      gp_GTrsf scale;
      scale.SetValue(1, 1, stretch);
      gp_GTrsf all = gp_GTrsf(toWorld) * scale * gp_GTrsf(toLocal);
      BRepBuilderAPI_GTransform move(src->wire, all, Standard_True);
      if (!move.IsDone()) return R::failure("Could not fit a profile between the rails");
      TopoDS_Wire w;
      for (TopExp_Explorer e(move.Shape(), TopAbs_WIRE); e.More(); e.Next()) { w = TopoDS::Wire(e.Current()); break; }
      if (w.IsNull()) return R::failure("Could not fit a profile between the rails");
      loft.AddWire(w);
    }
    loft.Build();
    if (!loft.IsDone()) return R::failure("Sweep2 failed: the profiles could not be joined along the rails");
    const TopoDS_Shape s = loft.Shape();
    if (!BRepCheck_Analyzer(s).IsValid()) return R::failure("Sweep2 gave an invalid shape (rails crossing?)");
    return s;
  } catch (const Standard_Failure& e) {
    return R::failure("Sweep2 failed: " + occtMessage(e));
  }
}

Result<TopoDS_Shape> surfaceFromEdges(const std::vector<TopoDS_Shape>& curves) {
  using R = Result<TopoDS_Shape>;
  if (curves.size() < 2 || curves.size() > 4) return R::failure("Edge surface needs 2, 3 or 4 curves");
  try {
    if (curves.size() == 2) {
      // Straight across between the two (ruled).
      BRepOffsetAPI_ThruSections ruled(Standard_False, Standard_True);
      std::vector<TopoDS_Wire> wires;
      for (const TopoDS_Shape& c : curves) {
        auto wire = wireOf(c);
        if (!wire) return R::failure(wire.error());
        wires.push_back(*wire);
      }
      // Run both the same way, so the surface does not twist into a bow tie.
      auto ends = [](const TopoDS_Wire& w) {
        TopoDS_Vertex a, b;
        TopExp::Vertices(w, a, b);
        return std::make_pair(BRep_Tool::Pnt(a), BRep_Tool::Pnt(b));
      };
      const auto [a0, b0] = ends(wires[0]);
      const auto [a1, b1] = ends(wires[1]);
      if (a0.Distance(a1) + b0.Distance(b1) > a0.Distance(b1) + b0.Distance(a1)) {
        wires[1] = TopoDS::Wire(wires[1].Reversed());
      }
      for (const TopoDS_Wire& w : wires) ruled.AddWire(w);
      ruled.CheckCompatibility(Standard_False);
      ruled.Build();
      if (!ruled.IsDone()) return R::failure("No surface between these two curves");
      return ruled.Shape();
    }
    auto loop = loopOf(curves);
    if (!loop) return R::failure(loop.error());
    return filling(*loop);
  } catch (const Standard_Failure& e) {
    return R::failure("Edge surface failed: " + occtMessage(e));
  }
}

Result<TopoDS_Shape> patchSurface(const std::vector<TopoDS_Shape>& curves) {
  using R = Result<TopoDS_Shape>;
  try {
    auto loop = loopOf(curves);
    if (!loop) return R::failure(loop.error());
    // A flat outline: the plain face (exact).
    BRepBuilderAPI_MakeWire wire;
    for (const TopoDS_Edge& e : *loop) wire.Add(e);
    if (wire.IsDone()) {
      BRepBuilderAPI_MakeFace flat(wire.Wire(), Standard_True);
      if (flat.IsDone() && BRepCheck_Analyzer(flat.Face()).IsValid()) return TopoDS_Shape(flat.Face());
    }
    return filling(*loop);
  } catch (const Standard_Failure& e) {
    return R::failure("Patch failed: " + occtMessage(e));
  }
}

namespace {
Result<TopoDS_Shape> tryRoundEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double size,
                                   bool chamfer) {
  using R = Result<TopoDS_Shape>;
  const char* what = chamfer ? "Chamfer" : "Fillet";
  if (!(size > 1e-6) || !std::isfinite(size)) return R::failure(std::string(what) + " size must be positive");
  if (edges.empty()) return R::failure("No edges to " + std::string(chamfer ? "chamfer" : "round"));
  try {
    TopoDS_Shape out;
    if (chamfer) {
      BRepFilletAPI_MakeChamfer mk(solid);
      for (const TopoDS_Edge& e : edges) mk.Add(size, e);
      mk.Build();
      if (!mk.IsDone()) return R::failure("Chamfer failed: the size may be too large for these edges");
      out = mk.Shape();
    } else {
      BRepFilletAPI_MakeFillet mk(solid);
      for (const TopoDS_Edge& e : edges) mk.Add(size, e);
      mk.Build();
      if (!mk.IsDone()) return R::failure("Fillet failed: the radius may be too large for these edges");
      out = mk.Shape();
    }
    if (!BRepCheck_Analyzer(out).IsValid()) {
      return R::failure(std::string(what) + " gave an invalid shape: try a smaller size");
    }
    return out;
  } catch (const Standard_Failure& e) {
    return R::failure(std::string(what) + " failed (the size may be too large): " + occtMessage(e));
  }
}

}  // namespace

Result<TopoDS_Shape> roundEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double size,
                                bool chamfer) {
  auto made = tryRoundEdges(solid, edges, size, chamfer);
  if (made || !(size > 1e-6) || !std::isfinite(size) || edges.empty()) return made;
  // Too large? Find about the largest size these edges take (a few tries,
  // halving the gap), so the message can say "up to N mm".
  double ok = 0.0, bad = size;
  for (double probe = size / 2; probe > size / 1024; probe /= 2) {
    if (tryRoundEdges(solid, edges, probe, chamfer)) {
      ok = probe;
      break;
    }
    bad = probe;
  }
  if (ok <= 0.0) return made;
  for (int i = 0; i < 6; ++i) {
    const double mid = (ok + bad) / 2;
    if (tryRoundEdges(solid, edges, mid, chamfer)) ok = mid; else bad = mid;
  }
  char hint[96];
  std::snprintf(hint, sizeof hint, " (these edges take up to about %.0f mm)", std::floor(ok));
  return Result<TopoDS_Shape>::failure(made.error() + hint);
}

Result<TopoDS_Shape> shellSolid(const TopoDS_Shape& solid, const std::vector<TopoDS_Face>& open, double thickness) {
  using R = Result<TopoDS_Shape>;
  if (!(thickness > 1e-6) || !std::isfinite(thickness)) return R::failure("Shell thickness must be positive");
  try {
    if (open.empty()) {
      // Nothing open: a closed hollow inside (the walls all round).
      BRepOffsetAPI_MakeOffsetShape inner;
      inner.PerformByJoin(solid, -thickness, 1e-3);
      if (!inner.IsDone()) return R::failure("Shell failed: the walls may be thicker than the solid allows");
      TopoDS_Shape core = inner.Shape();
      if (core.ShapeType() != TopAbs_SOLID) {
        TopExp_Explorer sh(core, TopAbs_SHELL);
        if (!sh.More()) return R::failure("Shell failed: no room left inside for a hollow");
        BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(sh.Current()));
        const TopoDS_Shape made = ms.Shape();
        core = made;
      }
      BRepAlgoAPI_Cut cut(solid, core);
      if (!cut.IsDone()) return R::failure("Shell failed: the hollow could not be cut");
      const TopoDS_Shape out = cut.Shape();
      if (!BRepCheck_Analyzer(out).IsValid()) return R::failure("Shell gave an invalid shape: try a thinner wall");
      return out;
    }
    TopTools_ListOfShape faces;
    for (const TopoDS_Face& f : open) faces.Append(f);
    BRepOffsetAPI_MakeThickSolid mk;
    mk.MakeThickSolidByJoin(solid, faces, -thickness, 1e-3);
    mk.Build();
    if (!mk.IsDone()) return R::failure("Shell failed: the walls may be thicker than the solid allows");
    const TopoDS_Shape out = mk.Shape();
    if (!BRepCheck_Analyzer(out).IsValid()) return R::failure("Shell gave an invalid shape: try a thinner wall");
    return out;
  } catch (const Standard_Failure& e) {
    return R::failure("Shell failed (the walls may be too thick): " + occtMessage(e));
  }
}

namespace {

Result<TopoDS_Shape> tryRoundCorners(const TopoDS_Face& face, const std::vector<gp_Pnt>& corners, double size,
                                     bool chamfer) {
  using R = Result<TopoDS_Shape>;
  const char* what = chamfer ? "Chamfer" : "Fillet";
  TopTools_IndexedDataMapOfShapeListOfShape edgesAt;
  TopExp::MapShapesAndAncestors(face, TopAbs_VERTEX, TopAbs_EDGE, edgesAt);
  BRepFilletAPI_MakeFillet2d mk(face);
  if (mk.Status() != ChFi2d_Ready) return R::failure("Only a flat face can have its corners rounded");
  for (const gp_Pnt& c : corners) {
    int found = 0;
    for (int i = 1; i <= edgesAt.Extent(); ++i) {
      const TopoDS_Vertex& v = TopoDS::Vertex(edgesAt.FindKey(i));
      if (BRep_Tool::Pnt(v).Distance(c) > 1e-3) continue;
      found = i;
      break;
    }
    if (!found) return R::failure("A corner is no longer there");
    const TopoDS_Vertex& v = TopoDS::Vertex(edgesAt.FindKey(found));
    if (chamfer) {
      std::vector<TopoDS_Edge> two;
      for (const TopoDS_Shape& e : edgesAt.FindFromIndex(found)) {
        bool dup = false;
        for (const TopoDS_Edge& t : two) dup |= t.IsSame(e);
        if (!dup) two.push_back(TopoDS::Edge(e));
      }
      if (two.size() != 2) return R::failure("A corner to chamfer must join exactly two edges");
      mk.AddChamfer(two[0], two[1], size, size);
    } else {
      mk.AddFillet(v, size);
    }
    if (mk.Status() != ChFi2d_IsDone) {
      return R::failure(std::string(what) + " failed at a corner: the size may be too large for its edges");
    }
  }
  mk.Build();
  if (!mk.IsDone()) return R::failure(std::string(what) + " failed: the size may be too large for these corners");
  TopoDS_Shape out = mk.Shape();
  if (!BRepCheck_Analyzer(out).IsValid()) {
    // The 2D chamfer may leave the new edges without their curves on the
    // plane or loosely joined: mended (same face, same outline).
    ShapeFix_Shape fix(out);
    fix.Perform();
    out = fix.Shape();
  }
  if (!BRepCheck_Analyzer(out).IsValid()) return R::failure(std::string(what) + " gave an invalid face: try a smaller size");
  return out;
}

}  // namespace

Result<TopoDS_Shape> roundCorners(const TopoDS_Shape& shape, const std::vector<gp_Pnt>& corners, double size,
                                  bool chamfer) {
  using R = Result<TopoDS_Shape>;
  const char* what = chamfer ? "Chamfer" : "Fillet";
  if (!(size > 1e-6) || !std::isfinite(size)) return R::failure(std::string(what) + " size must be positive");
  if (corners.empty()) return R::failure("No corners to " + std::string(chamfer ? "chamfer" : "round"));
  TopExp_Explorer f(shape, TopAbs_FACE);
  if (!f.More()) return R::failure("Not a face");
  const TopoDS_Face face = TopoDS::Face(f.Current());
  f.Next();
  if (f.More()) return R::failure("Only the corners of one flat face can be rounded");
  auto attempt = [&](double s) {
    try {
      return tryRoundCorners(face, corners, s, chamfer);
    } catch (const Standard_Failure& e) {
      return R::failure(std::string(what) + " failed (the size may be too large): " + occtMessage(e));
    }
  };
  auto made = attempt(size);
  if (made) return made;
  // As for edges: about the largest size these corners take.
  double ok = 0.0, bad = size;
  for (double probe = size / 2; probe > size / 1024; probe /= 2) {
    if (attempt(probe)) {
      ok = probe;
      break;
    }
    bad = probe;
  }
  if (ok <= 0.0) return made;
  for (int i = 0; i < 6; ++i) {
    const double mid = (ok + bad) / 2;
    if (attempt(mid)) ok = mid; else bad = mid;
  }
  char hint[96];
  std::snprintf(hint, sizeof hint, " (these corners take up to about %.0f mm)", std::floor(ok));
  return R::failure(made.error() + hint);
}

std::vector<gp_Pnt> faceCorners(const TopoDS_Face& face) {
  std::vector<gp_Pnt> out;
  TopTools_IndexedDataMapOfShapeListOfShape edgesAt;
  TopExp::MapShapesAndAncestors(face, TopAbs_VERTEX, TopAbs_EDGE, edgesAt);
  for (int i = 1; i <= edgesAt.Extent(); ++i) {
    const TopoDS_Vertex& v = TopoDS::Vertex(edgesAt.FindKey(i));
    const gp_Pnt at = BRep_Tool::Pnt(v);
    std::vector<gp_Dir> tangents;
    for (const TopoDS_Shape& s : edgesAt.FindFromIndex(i)) {
      BRepAdaptor_Curve c(TopoDS::Edge(s));
      for (double u : {c.FirstParameter(), c.LastParameter()}) {
        gp_Pnt p;
        gp_Vec t;
        c.D1(u, p, t);
        if (p.Distance(at) < 1e-3 && t.Magnitude() > 1e-12) {
          tangents.push_back(gp_Dir(t));
          break;
        }
      }
    }
    if (tangents.size() == 2 && !tangents[0].IsParallel(tangents[1], 1e-3)) out.push_back(at);
  }
  return out;
}

std::pair<gp_Pnt, gp_Dir> edgeKey(const TopoDS_Edge& edge) {
  BRepAdaptor_Curve c(edge);
  const double t = (c.FirstParameter() + c.LastParameter()) / 2;
  gp_Pnt p;
  gp_Vec d;
  c.D1(t, p, d);
  if (d.Magnitude() < 1e-12) d = gp_Vec(1, 0, 0);
  return {p, gp_Dir(d)};
}

Result<TopoDS_Edge> findEdge(const TopoDS_Shape& shape, const gp_Pnt& point, const gp_Dir& direction) {
  using R = Result<TopoDS_Edge>;
  std::vector<TopoDS_Edge> found;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  const TopoDS_Shape vertex = BRepBuilderAPI_MakeVertex(point).Vertex();
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e)) continue;
    BRepExtrema_DistShapeShape dist(vertex, e);
    if (!dist.IsDone() || dist.Value() > 1e-3) continue;
    // The direction there.
    BRepAdaptor_Curve c(e);
    double t = 0;
    if (dist.SupportTypeShape2(1) == BRepExtrema_IsOnEdge) dist.ParOnEdgeS2(1, t);
    else t = (c.FirstParameter() + c.LastParameter()) / 2;
    gp_Pnt p;
    gp_Vec d;
    c.D1(t, p, d);
    if (d.Magnitude() < 1e-12 || !gp_Dir(d).IsParallel(direction, 1e-3)) continue;
    found.push_back(e);
  }
  if (found.empty()) return R::failure("The edge is no longer there");
  if (found.size() > 1) return R::failure("The edge cannot be told apart from another one");
  return found.front();
}

}  // namespace sbr::geom
