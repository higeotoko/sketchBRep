// Lines and faces drawn, moved, copied or pasted onto what is already there
// in the same edit context are not stacked twice (Modeler::settleOverlaps):
// a line on an edge is not drawn again, a partly overlapping one keeps only
// its new pieces and its connection points; faces on the same plane are
// divided so that every area is covered once (by the face already there),
// holes kept; other groups, components and solids are never merged.

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <set>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

geom::SketchPlane at(double x, double y, double z = 0) {
  geom::SketchPlane p = geom::SketchPlane::ground();
  p.origin = gp_Pnt(x, y, z);
  return p;
}

int count(const Modeler& m, ObjectKind kind) {
  int n = 0;
  for (const Object& o : m.document().objects()) n += o.kind() == kind;
  return n;
}

double faceArea(const Modeler& m) {
  double a = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() == ObjectKind::Face) a += geom::area(m.document().worldShape(o));
  }
  return a;
}

// The face object holding point p (z = 0 faces), if any.
const Object* faceAt(const Modeler& m, const gp_Pnt& p) {
  for (const Object& o : m.document().objects()) {
    if (o.kind() != ObjectKind::Face) continue;
    for (TopExp_Explorer f(m.document().worldShape(o), TopAbs_FACE); f.More(); f.Next()) {
      if (geom::pointOnFace(TopoDS::Face(f.Current()), p)) return &o;
    }
  }
  return nullptr;
}

int facesOver(const Modeler& m, const gp_Pnt& p) {
  int n = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() != ObjectKind::Face) continue;
    for (TopExp_Explorer f(m.document().worldShape(o), TopAbs_FACE); f.More(); f.Next()) {
      n += geom::pointOnFace(TopoDS::Face(f.Current()), p);
    }
  }
  return n;
}

TopoDS_Face onlyFace(const Modeler& m, const ObjectId& id) {
  return TopoDS::Face(TopExp_Explorer(m.document().worldShape(*m.document().find(id)), TopAbs_FACE).Current());
}

// Straight pieces of the loose lines, as (x0,y0)-(x1,y1) rounded, sorted.
std::multiset<std::string> lines(const Modeler& m) {
  std::multiset<std::string> out;
  for (const Object& o : m.document().objects()) {
    if (o.kind() != ObjectKind::Curve) continue;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(m.document().worldShape(o), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
      gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(TopoDS::Edge(edges(i))));
      gp_Pnt b = BRep_Tool::Pnt(TopExp::LastVertex(TopoDS::Edge(edges(i))));
      if (std::make_pair(b.X(), b.Y()) < std::make_pair(a.X(), a.Y())) std::swap(a, b);
      out.insert(std::to_string(std::lround(a.X())) + "," + std::to_string(std::lround(a.Y())) + "-" +
                 std::to_string(std::lround(b.X())) + "," + std::to_string(std::lround(b.Y())));
    }
  }
  return out;
}

}  // namespace

// 1. The same rectangle twice: one face; the second call names that face.
TEST(Overlaps, SameFaceTwice) {
  Modeler m;
  auto a = m.createRectangle(at(0, 0), 1000, 1000);
  const std::size_t steps = m.history().undoCount();
  auto b = m.createRectangle(at(0, 0), 1000, 1000);
  ASSERT_TRUE(b.ok()) << b.error();
  EXPECT_EQ(*b, *a);
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
  EXPECT_NEAR(faceArea(m), 1e6, 1e-3);
  EXPECT_EQ(m.history().undoCount(), steps + 1);  // still one step
  // Drawn the other way round (facing down): the same place, still once.
  auto c = m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(0, 1000, 0), gp_Pnt(1000, 1000, 0), gp_Pnt(1000, 0, 0)});
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
  // The one there keeps its side (up).
  EXPECT_GT(geom::planarFaceNormal(onlyFace(m, *a))->Z(), 0.5);
  // The API draws through the same rule.
  api::CommandApi api(m);
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "create_rectangle"}, {"args", {{"origin", {0, 0, 0}}, {"width", 1000}, {"height", 1000}}}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(r["result"]["id"].get<std::string>(), *a);
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
}

// 2. Partly overlapping squares: three regions, each once, each editable;
// the overlap keeps the material of the face already there.
TEST(Overlaps, PartlyOverlappingFacesAreDivided) {
  Modeler m;
  auto a = m.createRectangle(at(0, 0), 1000, 1000);
  auto red = m.createMaterial("Red", {1, 0, 0});
  ASSERT_TRUE(m.assignMaterial({*a}, *red).ok());
  auto b = m.createRectangle(at(500, 500), 1000, 1000);
  ASSERT_TRUE(b.ok()) << b.error();
  EXPECT_EQ(count(m, ObjectKind::Face), 3);
  EXPECT_NEAR(faceArea(m), 1.75e6, 1e-3);
  for (const gp_Pnt& p : {gp_Pnt(250, 250, 0), gp_Pnt(750, 750, 0), gp_Pnt(1250, 1250, 0)}) {
    EXPECT_EQ(facesOver(m, p), 1) << p.X();
  }
  EXPECT_EQ(faceAt(m, gp_Pnt(750, 750, 0))->material, *red);   // overlap: the one there
  EXPECT_EQ(faceAt(m, gp_Pnt(250, 250, 0))->material, *red);
  EXPECT_NE(faceAt(m, gp_Pnt(1250, 1250, 0))->material, *red);  // the new part: its own
  // Each region pushed / pulled on its own.
  const Object* middle = faceAt(m, gp_Pnt(750, 750, 0));
  const ObjectId middleId = middle->id;
  ASSERT_TRUE(m.pushPull(middleId, onlyFace(m, middleId), 300).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(middleId))), 0.25e6 * 300, 1e-2);
  // Undo / redo, and the file.
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
  EXPECT_NEAR(faceArea(m), 1e6, 1e-3);
  ASSERT_TRUE(m.redo());
  EXPECT_EQ(count(m, ObjectKind::Face), 3);
  const std::string path = ::testing::TempDir() + "overlaps.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  EXPECT_EQ(count(back, ObjectKind::Face), 3);
  EXPECT_NEAR(faceArea(back), 1.75e6, 1e-3);
  EXPECT_EQ(faceAt(back, gp_Pnt(750, 750, 0))->material, *red);
}

// 3. A face with a hole: its inner outline is kept, and the hole is not
// taken for part of the face.
TEST(Overlaps, HolesAreKept) {
  Modeler m;
  auto a = m.createRectangle(at(0, 0), 2000, 1000);
  auto disc = m.createCircle(at(500, 500), 200);
  ASSERT_TRUE(m.remove({*disc}).ok());  // an empty hole
  const double ring = 1e6 - M_PI * 200 * 200;
  EXPECT_NEAR(geom::area(m.document().worldShape(*m.document().find(*a))), 2e6 - M_PI * 200 * 200, 1e-2);
  auto b = m.createRectangle(at(0, 0), 1000, 1000);  // over the left half, hole and all
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(facesOver(m, gp_Pnt(100, 100, 0)), 1);
  EXPECT_EQ(facesOver(m, gp_Pnt(500, 500, 0)), 1);   // the hole: the new face fills it
  EXPECT_EQ(facesOver(m, gp_Pnt(1500, 500, 0)), 1);
  EXPECT_NEAR(faceArea(m), 2e6, 1e-2);
  // The left half of the face there still has its round hole.
  EXPECT_NEAR(geom::area(m.document().worldShape(*faceAt(m, gp_Pnt(100, 100, 0)))), ring, 1e-2);
  EXPECT_NEAR(geom::area(m.document().worldShape(*faceAt(m, gp_Pnt(500, 500, 0)))), M_PI * 200 * 200, 1e-2);
}

// 4. Other groups, solids and other heights stay as they are.
TEST(Overlaps, OtherGroupsSolidsAndHeightsStayApart) {
  Modeler m;
  auto inside = m.createRectangle(at(0, 0), 1000, 1000);
  auto group = m.group({*inside}, "G");
  ASSERT_TRUE(group.ok());
  auto loose = m.createRectangle(at(0, 0), 1000, 1000);
  ASSERT_TRUE(loose.ok());
  EXPECT_NE(*loose, *inside);
  EXPECT_NEAR(geom::area(m.document().worldShape(*m.document().find(*inside))), 1e6, 1e-3);
  EXPECT_NEAR(geom::area(m.document().worldShape(*m.document().find(*loose))), 1e6, 1e-3);
  // 10 mm higher: another plane, nothing divided.
  auto higher = m.createRectangle(at(500, 0, 10), 1000, 1000);
  ASSERT_TRUE(higher.ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(*m.document().find(*loose))), 1e6, 1e-3);
  // On a solid's top face: the solid stays closed and whole.
  auto base = m.createRectangle(at(5000, 0), 1000, 1000);
  ASSERT_TRUE(m.extrude(*base, 1000).ok());
  auto onTop = m.createRectangle(at(5000, 0, 1000), 1000, 1000);
  ASSERT_TRUE(onTop.ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*base))), 1e9, 1e-2);
  // Two solids side by side touch but are not joined.
  auto other = m.createRectangle(at(6000, 0), 1000, 1000);
  ASSERT_TRUE(m.extrude(*other, 1000).ok());
  EXPECT_EQ(count(m, ObjectKind::Solid), 2);
}

// 5. Taking away the edge two faces share: the faces apart from that are
// not lost, and nothing else goes.
TEST(Overlaps, SharedEdgeDeletedKeepsTheRest) {
  Modeler m;
  auto a = m.createRectangle(at(0, 0), 1000, 1000);
  auto b = m.createRectangle(at(1000, 0), 1000, 1000);
  auto box = m.createRectangle(at(5000, 0), 1000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1000).ok());
  TopoDS_Edge shared;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*b)), TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
    if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(1000, 500, 0)) < 1e-6) {
      shared = TopoDS::Edge(e.Current());
    }
  }
  ASSERT_FALSE(shared.IsNull());
  DeleteTarget t;
  t.id = *b;
  t.edges = {shared};
  ASSERT_TRUE(m.deleteSelection({t}).ok());
  EXPECT_TRUE(m.document().find(*a));
  EXPECT_NEAR(geom::area(m.document().worldShape(*m.document().find(*a))), 1e6, 1e-3);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*box))), 1e9, 1e-2);
}

// Lines: on a line already there, not drawn again; partly on it, only the
// new piece; ending on one, that one gets a vertex there; along a face's
// edge, not drawn.
TEST(Overlaps, Lines) {
  Modeler m;
  api::CommandApi api(m);
  auto line = [&](std::vector<std::array<double, 2>> pts) {
    nlohmann::json points = nlohmann::json::array();
    for (const auto& p : pts) points.push_back({p[0], p[1], 0});
    return nlohmann::json::parse(api.execute(
        nlohmann::json{{"command", "create_line"}, {"args", {{"points", points}}}}.dump()));
  };
  ASSERT_TRUE(line({{0, 0}, {1000, 0}})["ok"].get<bool>());
  const std::size_t steps = m.history().undoCount();
  auto again = line({{1000, 0}, {0, 0}});  // the same, the other way
  ASSERT_TRUE(again["ok"].get<bool>()) << again.dump();
  EXPECT_TRUE(again["result"]["ids"].empty());
  EXPECT_EQ(lines(m), (std::multiset<std::string>{"0,0-1000,0"}));
  // Partly on it: the piece beyond.
  ASSERT_TRUE(line({{500, 0}, {1500, 0}})["ok"].get<bool>());
  EXPECT_EQ(lines(m), (std::multiset<std::string>{"0,0-1000,0", "1000,0-1500,0"}));
  // Inside it: nothing.
  ASSERT_TRUE(line({{200, 0}, {800, 0}})["ok"].get<bool>());
  EXPECT_EQ(lines(m), (std::multiset<std::string>{"0,0-1000,0", "1000,0-1500,0"}));
  // Ending on it: it is divided there (a connection point).
  ASSERT_TRUE(line({{300, 0}, {300, 700}})["ok"].get<bool>());
  EXPECT_EQ(lines(m), (std::multiset<std::string>{"0,0-300,0", "300,0-1000,0", "1000,0-1500,0", "300,0-300,700"}));
  // Undo goes back step by step.
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(lines(m), (std::multiset<std::string>{"0,0-1000,0", "1000,0-1500,0"}));
  EXPECT_GE(m.history().undoCount(), steps);

  // Along a face's edge: the edge is there already.
  auto face = m.createRectangle(at(5000, 0), 1000, 1000);
  ASSERT_TRUE(face.ok());
  auto onEdge = line({{5000, 0}, {6000, 0}});
  ASSERT_TRUE(onEdge["ok"].get<bool>());
  EXPECT_TRUE(onEdge["result"]["ids"].empty());
  auto longer = line({{5500, 0}, {7000, 0}});  // half on it
  ASSERT_TRUE(longer["ok"].get<bool>());
  EXPECT_EQ(lines(m).count("6000,0-7000,0"), 1u);
}

// Copies and moves follow the same rule as drawing.
TEST(Overlaps, CopiesAndMoves) {
  Modeler m;
  auto a = m.createRectangle(at(0, 0), 1000, 1000);
  gp_Trsf none;
  auto copied = m.copy({*a}, none, 1);  // copied onto itself: not made at all
  ASSERT_TRUE(copied.ok());
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
  EXPECT_TRUE(copied->empty());
  // Moved half onto another face: divided like a drawn one.
  auto b = m.createRectangle(at(3000, 0), 1000, 1000);
  ASSERT_TRUE(m.move({*b}, gp_Vec(-2500, 0, 0)).ok());
  EXPECT_EQ(count(m, ObjectKind::Face), 3);
  EXPECT_NEAR(faceArea(m), 1.5e6, 1e-3);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(count(m, ObjectKind::Face), 2);
  EXPECT_NEAR(faceArea(m), 2e6, 1e-3);
  // Line copies partly on lines already there.
  auto l = m.createPolyline({gp_Pnt(0, 5000, 0), gp_Pnt(1000, 5000, 0)});
  gp_Trsf half;
  half.SetTranslation(gp_Vec(500, 0, 0));
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m.document().worldShape(*m.document().find(*l)), TopAbs_EDGE, edges);
  auto made = m.copyEdges({TopoDS::Edge(edges(1))}, half, 1);
  ASSERT_TRUE(made.ok()) << made.error();
  EXPECT_EQ(lines(m).count("1000,5000-1500,5000"), 1u);
  EXPECT_EQ(lines(m).count("500,5000-1500,5000"), 0u);
}

// A face's outline copied (its edges, not the face): the copy closes, so it
// is a face too, as drawn; copied onto itself, nothing doubles.
TEST(Overlaps, CopiedOutlineBecomesAFace) {
  Modeler m;
  auto a = m.createRectangle(at(0, 0), 1000, 1000);
  std::vector<TopoDS_Edge> outline;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*a)), TopAbs_EDGE); e.More(); e.Next()) {
    outline.push_back(TopoDS::Edge(e.Current()));
  }
  gp_Trsf up;
  up.SetTranslation(gp_Vec(0, 0, 500));
  auto made = m.copyEdges(outline, up, 2);
  ASSERT_TRUE(made.ok()) << made.error();
  EXPECT_EQ(count(m, ObjectKind::Face), 3);
  EXPECT_EQ(count(m, ObjectKind::Curve), 0);
  ASSERT_EQ(made->size(), 2u);
  auto pushed = m.pushPull(made->back(), onlyFace(m, made->back()), 300);
  EXPECT_TRUE(pushed.ok()) << pushed.error();
  gp_Trsf none;
  auto onto = m.copyEdges(outline, none, 1);
  ASSERT_TRUE(onto.ok());
  EXPECT_EQ(count(m, ObjectKind::Face), 2);  // one of them pushed into a solid
  EXPECT_EQ(facesOver(m, gp_Pnt(500, 500, 0)), 1);
}

// CLIP-01 / SEL-01: faces and edges picked on a box copy just those, never
// the box: an edge as a line, a face as a face of its own (material kept);
// the box is unchanged. The API copies the same.
TEST(SelectionTargets, ElementsCopyOnlyThemselves) {
  Modeler m;
  auto box = m.createRectangle(at(0, 0), 1000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1000).ok());
  auto red = m.createMaterial("Red", {1, 0, 0});
  TopoDS_Face top;
  TopoDS_Edge edge;
  for (TopExp_Explorer f(m.document().worldShape(*m.document().find(*box)), TopAbs_FACE); f.More(); f.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(f.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(f.Current());
  }
  ASSERT_TRUE(m.paintFaces(*box, {top}, *red).ok());
  for (TopExp_Explorer f(m.document().worldShape(*m.document().find(*box)), TopAbs_FACE); f.More(); f.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(f.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(f.Current());
  }
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*box)), TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
    if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(500, 0, 0)) < 1e-6) {
      edge = TopoDS::Edge(e.Current());
    }
  }
  ASSERT_FALSE(top.IsNull());
  ASSERT_FALSE(edge.IsNull());
  // An edge: one line.
  auto clip = m.copyToClipboard(std::vector<DeleteTarget>{{*box, false, {}, {edge}}});
  ASSERT_TRUE(clip.ok()) << clip.error();
  ASSERT_EQ(clip->objects.size(), 1u);
  gp_Trsf away;
  away.SetTranslation(gp_Vec(0, -3000, 0));
  auto pasted = m.paste(*clip, away);
  ASSERT_TRUE(pasted.ok());
  EXPECT_EQ(m.document().find(pasted->front())->kind(), ObjectKind::Curve);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*box))), 1e9, 1e-2);
  // A face: a face, red, 1000 x 1000.
  auto copied = m.copyElements({DeleteTarget{*box, false, {top}, {}}}, away, 2);
  ASSERT_TRUE(copied.ok()) << copied.error();
  ASSERT_EQ(copied->size(), 2u);
  for (const ObjectId& id : *copied) {
    const Object* o = m.document().find(id);
    EXPECT_EQ(o->kind(), ObjectKind::Face);
    EXPECT_EQ(o->material, *red);
    EXPECT_NEAR(geom::area(m.document().worldShape(*o)), 1e6, 1e-3);
  }
  EXPECT_EQ(count(m, ObjectKind::Solid), 1);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*box))), 1e9, 1e-2);
  ASSERT_TRUE(m.undo());  // the two copies, one step
  EXPECT_EQ(count(m, ObjectKind::Face), 0);
  // The API, with the same targets.
  api::CommandApi api(m);
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "copy_array"},
      {"args", {{"elements", {{{"id", *box}, {"faces", {{{"normal", {0, 0, 1}}}}}}}},
                {"vector", {0, 0, 2000}}, {"count", 1}}}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(count(m, ObjectKind::Face), 1);
  EXPECT_EQ(count(m, ObjectKind::Solid), 1);
}

// XFORM-02: a polar array going full circle does not double the original.
TEST(SelectionTargets, PolarArrayFullCircleSkipsTheOriginal) {
  Modeler m;
  auto box = m.createRectangle(at(1000, -100), 400, 200);
  ASSERT_TRUE(m.extrude(*box, 200).ok());
  gp_Trsf step;
  step.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI / 6);
  auto made = m.copy({*box}, step, 12);
  ASSERT_TRUE(made.ok());
  EXPECT_EQ(made->size(), 11u);
  EXPECT_EQ(count(m, ObjectKind::Solid), 12);
}
