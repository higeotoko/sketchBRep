#include "sbr/geom/Freeform.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopExp.hxx>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <ShapeFix_Solid.hxx>
#include <BRepTools.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Face.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopoDS.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>

#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

namespace sbr::geom {
namespace {
Result<TopoDS_Shape> checked(const TopoDS_Shape& shape, const char* what) {
  const bool hasSolid = TopExp_Explorer(shape, TopAbs_SOLID).More();
  const ValidationReport r = validateShape(shape, false);
  if (!r.valid || (hasSolid && r.volume <= 0)) {
    return Result<TopoDS_Shape>::failure(std::string(what) + " produced an invalid shape: " + r.summary());
  }
  return shape;
}
}  // namespace

gp_Pnt arcApex(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge) {
  const gp_Vec chord(start, end);
  const gp_Pnt mid((start.XYZ() + end.XYZ()) / 2.0);
  const gp_Vec side = gp_Vec(normal).Crossed(chord);
  if (side.Magnitude() < 1e-12) return mid;
  return mid.Translated(side.Normalized() * bulge);
}

Result<TopoDS_Shape> makeArc(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& normal, double bulge,
                             bool closed) {
  const gp_Vec chord(start, end);
  if (chord.Magnitude() < tolerance::kMinDimension) return Result<TopoDS_Shape>::failure("Arc ends coincide");
  if (std::abs(gp_Vec(normal).Dot(chord.Normalized())) > 1e-6) {
    return Result<TopoDS_Shape>::failure("Arc chord must lie in the plane");
  }
  if (std::abs(bulge) < tolerance::kMinDimension) return Result<TopoDS_Shape>::failure("Arc is flat");
  try {
    GC_MakeArcOfCircle arc(start, arcApex(start, end, normal, bulge), end);
    if (!arc.IsDone()) return Result<TopoDS_Shape>::failure("Arc could not be built");
    const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(arc.Value());
    if (!closed) return TopoDS_Shape(BRepBuilderAPI_MakeWire(edge).Wire());
    BRepBuilderAPI_MakeWire wire(edge, BRepBuilderAPI_MakeEdge(end, start));
    BRepBuilderAPI_MakeFace face(wire.Wire(), true);
    if (!face.IsDone()) return Result<TopoDS_Shape>::failure("Arc segment face could not be built");
    TopoDS_Face f = face.Face();
    // Profiles face along their plane normal, so extrusion goes that way.
    if (const auto n = planarFaceNormal(f); n && n->Dot(normal) < 0) f.Reverse();
    return TopoDS_Shape(f);
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Arc failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Face> faceFromCurves(const std::vector<TopoDS_Shape>& curves) {
  Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape;
  for (const TopoDS_Shape& c : curves) {
    for (TopExp_Explorer e(c, TopAbs_EDGE); e.More(); e.Next()) edges->Append(e.Current());
  }
  if (edges->Length() < 1) return Result<TopoDS_Face>::failure("No edges");
  try {
    Handle(TopTools_HSequenceOfShape) wires;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-3, Standard_False, wires);
    if (wires.IsNull() || wires->Length() != 1) {
      return Result<TopoDS_Face>::failure("The curves do not form a single loop");
    }
    const TopoDS_Wire wire = TopoDS::Wire(wires->Value(1));
    if (!BRep_Tool::IsClosed(wire)) return Result<TopoDS_Face>::failure("The loop is not closed");
    BRepBuilderAPI_MakeFace maker(wire, Standard_True);
    if (!maker.IsDone()) return Result<TopoDS_Face>::failure("The loop is not planar");
    TopoDS_Face face = maker.Face();
    ShapeFix_Face fix(face);
    fix.Perform();
    face = fix.Face();
    if (const auto n = planarFaceNormal(face)) {
      const double key = std::abs(n->Z()) > 1e-6 ? n->Z() : std::abs(n->Y()) > 1e-6 ? n->Y() : n->X();
      if (key < 0) face.Reverse();
    }
    BRepCheck_Analyzer check(face);
    if (!check.IsValid()) return Result<TopoDS_Face>::failure("The loop crosses itself");
    return face;
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Face>::failure(std::string("Face failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Wire> makeInterpolatedCurve(const std::vector<gp_Pnt>& points, bool closed) {
  if (points.size() < 2) return Result<TopoDS_Wire>::failure("A curve needs at least 2 points");
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    if (points[i].Distance(points[i + 1]) < tolerance::kMinDimension) {
      return Result<TopoDS_Wire>::failure("Curve has coincident consecutive points");
    }
  }
  try {
    Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, static_cast<int>(points.size()));
    for (std::size_t i = 0; i < points.size(); ++i) pts->SetValue(static_cast<int>(i) + 1, points[i]);
    GeomAPI_Interpolate interpolate(pts, closed ? Standard_True : Standard_False, tolerance::kLinear);
    interpolate.Perform();
    if (!interpolate.IsDone()) return Result<TopoDS_Wire>::failure("Curve interpolation failed");
    const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(interpolate.Curve()).Edge();
    return BRepBuilderAPI_MakeWire(edge).Wire();
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Wire>::failure(std::string("Curve failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> loft(const std::vector<TopoDS_Wire>& sections, bool solid) {
  if (sections.size() < 2) return Result<TopoDS_Shape>::failure("Loft needs at least 2 sections");
  try {
    BRepOffsetAPI_ThruSections maker(solid ? Standard_True : Standard_False, Standard_False);
    for (const TopoDS_Wire& w : sections) maker.AddWire(w);
    maker.Build();
    if (!maker.IsDone()) return Result<TopoDS_Shape>::failure("Loft failed");
    return checked(maker.Shape(), "Loft");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Loft failed: ") + e.GetMessageString());
  }
}

Result<TopoDS_Shape> revolve(const TopoDS_Shape& profile, const gp_Ax1& axis, double angle) {
  if (!std::isfinite(angle) || std::abs(angle) < 1e-9) {
    return Result<TopoDS_Shape>::failure("Revolve angle must be non-zero");
  }
  try {
    BRepPrimAPI_MakeRevol maker(profile, axis, std::min(std::abs(angle), 2 * M_PI) * (angle < 0 ? -1 : 1));
    if (!maker.IsDone()) return Result<TopoDS_Shape>::failure("Revolve failed");
    return checked(maker.Shape(), "Revolve");
  } catch (const Standard_Failure& e) {
    return Result<TopoDS_Shape>::failure(std::string("Revolve failed: ") + e.GetMessageString());
  }
}

namespace {

// One outline swept with mitred corners and closed into a solid.
Result<TopoDS_Shape> mitredOutline(const TopoDS_Wire& outline, const TopoDS_Wire& spine) {
  BRepOffsetAPI_MakePipeShell shell(spine);
  shell.SetMode(Standard_False);  // corrected Frenet: keeps the profile upright
  shell.SetTransitionMode(BRepBuilderAPI_RightCorner);
  shell.Add(outline);
  shell.Build();
  if (!shell.IsDone()) return Result<TopoDS_Shape>::failure("Sweep failed");
  if (!BRep_Tool::IsClosed(spine)) shell.MakeSolid();
  TopoDS_Shape result = shell.Shape();
  // A closed path gives a closed shell (a ring): make it a solid.
  if (BRep_Tool::IsClosed(spine)) {
    TopExp_Explorer sx(result, TopAbs_SHELL);
    if (sx.More()) {
      BRepBuilderAPI_MakeSolid solid(TopoDS::Shell(sx.Current()));
      if (solid.IsDone()) {
        ShapeFix_Solid fix(solid.Solid());
        fix.Perform();
        result = fix.Solid();
      }
    }
  }
  return checked(result, "Sweep");
}

// Sweep with mitred corners (like moulding around a room): the profile's
// outline is swept as a pipe shell and capped into a solid; each hole is
// swept the same way and taken out, so a hollow section stays hollow
// (never filled).
Result<TopoDS_Shape> mitredSweep(const TopoDS_Shape& profile, const TopoDS_Wire& spine) {
  if (profile.ShapeType() != TopAbs_FACE) return Result<TopoDS_Shape>::failure("Profile is not a face");
  const TopoDS_Face face = TopoDS::Face(profile);
  const TopoDS_Wire outer = BRepTools::OuterWire(face);
  auto body = mitredOutline(outer, spine);
  if (!body) return body;
  TopoDS_Shape result = body.value();
  for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
    const TopoDS_Wire wire = TopoDS::Wire(w.Current());
    if (wire.IsSame(outer)) continue;
    auto hole = mitredOutline(wire, spine);
    if (!hole) return Result<TopoDS_Shape>::failure("The hole of the profile could not be swept: " + hole.error());
    BRepAlgoAPI_Cut cut(result, hole.value());
    if (!cut.IsDone() || cut.HasErrors()) return Result<TopoDS_Shape>::failure("The hole of the profile could not be cut through");
    result = cut.Shape();
  }
  return checked(result, "Sweep");
}

}  // namespace

namespace {

// The sharpest corner of the path, as "(x, y, z)" (for messages).
std::string sharpestCorner(const TopoDS_Wire& spine) {
  double worst = -1;
  gp_Pnt at;
  std::optional<gp_Vec> before;
  for (BRepTools_WireExplorer e(spine); e.More(); e.Next()) {
    BRepAdaptor_Curve c(e.Current());
    const bool reversed = e.Current().Orientation() == TopAbs_REVERSED;
    gp_Pnt p0, p1;
    gp_Vec t0, t1;
    c.D1(reversed ? c.LastParameter() : c.FirstParameter(), p0, t0);
    c.D1(reversed ? c.FirstParameter() : c.LastParameter(), p1, t1);
    if (reversed) {
      t0.Reverse();
      t1.Reverse();
    }
    if (before && before->Magnitude() > 1e-12 && t0.Magnitude() > 1e-12 && before->Angle(t0) > worst) {
      worst = before->Angle(t0);
      at = p0;
    }
    before = t1;
  }
  char where[128];
  std::snprintf(where, sizeof where, "(%.1f, %.1f, %.1f)", at.X(), at.Y(), at.Z());
  return where;
}

// The swept shape passes through itself (BRepCheck does not see it).
bool crossesItself(const TopoDS_Shape& shape) {
  BOPAlgo_ArgumentAnalyzer analyzer;
  analyzer.SetShape1(shape);
  analyzer.SelfInterMode() = Standard_True;
  analyzer.Perform();
  return analyzer.HasFaulty();
}

// Where one edge of the path ends at an angle to the next (a polyline's
// corner): the plain pipe would carry the profile round it unturned
// (sheared, the wrong volume), so such paths are mitred.
bool hasCorners(const TopoDS_Wire& spine) {
  std::vector<std::pair<gp_Vec, gp_Vec>> tangents;  // at the start and end of each edge, in path order
  for (BRepTools_WireExplorer e(spine); e.More(); e.Next()) {
    BRepAdaptor_Curve c(e.Current());
    gp_Pnt p;
    gp_Vec t0, t1;
    c.D1(c.FirstParameter(), p, t0);
    c.D1(c.LastParameter(), p, t1);
    if (e.Current().Orientation() == TopAbs_REVERSED) {
      std::swap(t0, t1);
      t0.Reverse();
      t1.Reverse();
    }
    if (t0.Magnitude() < 1e-12 || t1.Magnitude() < 1e-12) continue;
    tangents.emplace_back(t0, t1);
  }
  for (std::size_t i = 1; i < tangents.size(); ++i) {
    if (tangents[i - 1].second.Angle(tangents[i].first) > 1e-6) return true;
  }
  if (BRep_Tool::IsClosed(spine) && tangents.size() > 1 && tangents.back().second.Angle(tangents.front().first) > 1e-6) {
    return true;
  }
  return false;
}

}  // namespace

Result<TopoDS_Shape> sweep(const TopoDS_Shape& profile, const TopoDS_Wire& spine) {
  try {
    if (hasCorners(spine)) {
      auto mitred = mitredSweep(profile, spine);
      const bool invalid = !mitred && mitred.error().find("invalid shape") != std::string::npos;
      if (invalid || (mitred && crossesItself(mitred.value()))) {
        return Result<TopoDS_Shape>::failure("The swept shape would cross itself near " + sharpestCorner(spine) +
                                             ": the path turns back within the profile's size");
      }
      return mitred;
    }
    BRepOffsetAPI_MakePipe maker(spine, profile);
    maker.Build();
    if (maker.IsDone()) {
      auto result = checked(maker.Shape(), "Sweep");
      if (result) return result;
    }
    // Paths with corners: mitre them instead.
    return mitredSweep(profile, spine);
  } catch (const Standard_Failure& e) {
    try {
      return mitredSweep(profile, spine);
    } catch (const Standard_Failure&) {
      return Result<TopoDS_Shape>::failure(std::string("Sweep failed: ") + e.GetMessageString());
    }
  }
}

Result<TopoDS_Wire> pathFromEdges(const std::vector<TopoDS_Edge>& input, const gp_Pnt& near) {
  using R = Result<TopoDS_Wire>;
  constexpr double kJoin = 1e-4;
  if (input.empty()) return R::failure("No edges for the path");
  // Distinct edges (the same edge picked twice, or two copies on top of
  // each other, cannot be told apart in a path).
  std::vector<TopoDS_Edge> edges;
  auto ends = [](const TopoDS_Edge& e) {
    return std::make_pair(BRep_Tool::Pnt(TopExp::FirstVertex(e, true)), BRep_Tool::Pnt(TopExp::LastVertex(e, true)));
  };
  auto midpoint = [](const TopoDS_Edge& e) {
    BRepAdaptor_Curve c(e);
    return c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
  };
  for (const TopoDS_Edge& e : input) {
    if (e.IsNull() || BRep_Tool::Degenerated(e)) continue;
    const auto [a, b] = ends(e);
    const gp_Pnt m = midpoint(e);
    for (const TopoDS_Edge& o : edges) {
      const auto [c, d] = ends(o);
      const bool sameEnds = (a.Distance(c) < kJoin && b.Distance(d) < kJoin) || (a.Distance(d) < kJoin && b.Distance(c) < kJoin);
      if (sameEnds && m.Distance(midpoint(o)) < kJoin) return R::failure("An edge is selected twice (or two lines lie on top of each other)");
    }
    edges.push_back(e);
  }
  // Vertices joined within the tolerance, and how many edges meet at each.
  std::vector<gp_Pnt> nodes;
  auto node = [&](const gp_Pnt& p) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].Distance(p) < kJoin) return i;
    }
    nodes.push_back(p);
    return nodes.size() - 1;
  };
  std::vector<std::pair<std::size_t, std::size_t>> links;
  for (const TopoDS_Edge& e : edges) {
    const auto [a, b] = ends(e);
    links.emplace_back(node(a), node(b));
  }
  std::vector<int> degree(nodes.size(), 0);
  for (const auto& [a, b] : links) {
    ++degree[a];
    ++degree[b];
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (degree[i] > 2) {
      char where[128];
      std::snprintf(where, sizeof where, "(%.1f, %.1f, %.1f)", nodes[i].X(), nodes[i].Y(), nodes[i].Z());
      return R::failure(std::string("The selected edges branch at ") + where + "; select one path without branches");
    }
  }
  std::vector<std::size_t> openEnds;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (degree[i] == 1) openEnds.push_back(i);
  }
  // Start: the open end nearer the profile, or on a loop the vertex nearest it.
  std::size_t start = 0;
  if (!openEnds.empty()) {
    start = openEnds.front();
    for (std::size_t i : openEnds) {
      if (nodes[i].Distance(near) < nodes[start].Distance(near)) start = i;
    }
  } else {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].Distance(near) < nodes[start].Distance(near)) start = i;
    }
  }
  // Walk the chain.
  std::vector<bool> used(edges.size(), false);
  BRepBuilderAPI_MakeWire wire;
  std::size_t at = start;
  std::size_t walked = 0;
  for (bool moved = true; moved;) {
    moved = false;
    for (std::size_t k = 0; k < edges.size(); ++k) {
      if (used[k] || (links[k].first != at && links[k].second != at)) continue;
      const bool forward = links[k].first == at;
      wire.Add(forward ? edges[k] : TopoDS::Edge(edges[k].Reversed()));
      if (!wire.IsDone()) return R::failure("The selected edges do not join into one path");
      used[k] = true;
      at = forward ? links[k].second : links[k].first;
      ++walked;
      moved = true;
      break;
    }
  }
  if (walked != edges.size()) {
    char where[128];
    std::snprintf(where, sizeof where, "(%.1f, %.1f, %.1f)", nodes[at].X(), nodes[at].Y(), nodes[at].Z());
    return R::failure(std::string("The selected edges are not connected: the path breaks off at ") + where + " (" +
                      std::to_string(edges.size() - walked) + " of them lie apart from it); select one connected path");
  }
  return wire.Wire();
}

}  // namespace sbr::geom
