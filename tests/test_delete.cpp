// Deleting faces (SketchUp, request T1 / DEL-01..03): the faces go and
// their edges stay as lines, unless a face that is left still uses them or
// they are taken away too. One undo step; lines keep their exact geometry
// (a circle stays a circle) through save and reload.

#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId box(Modeler& m, double w, double d, double h) {
  auto id = m.createRectangle(geom::SketchPlane::ground(), w, d);
  EXPECT_TRUE(m.extrude(*id, h).ok());
  return *id;
}

// The face of `id` whose outward normal is `n`.
TopoDS_Face faceFacing(const Modeler& m, const ObjectId& id, const gp_Dir& n) {
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const auto normal = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (normal && normal->Angle(n) < 1e-6) return TopoDS::Face(e.Current());
  }
  return {};
}

int count(const TopoDS_Shape& s, TopAbs_ShapeEnum type) {
  int n = 0;
  for (TopExp_Explorer e(s, type); e.More(); e.Next()) ++n;
  return n;
}

// Line objects and their total length.
std::pair<int, double> lines(const Modeler& m) {
  int n = 0;
  double length = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() != ObjectKind::Curve) continue;
    ++n;
    GProp_GProps props;
    BRepGProp::LinearProperties(m.document().worldShape(o), props);
    length += props.Mass();
  }
  return {n, length};
}

}  // namespace

// DEL-01: a loose rectangle, its face deleted: the 4 sides stay as lines.
TEST(DeleteFaces, LooseFaceLeavesItsOutline) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 4000, 3000);
  ASSERT_TRUE(face.ok());
  const std::size_t steps = m.history().undoCount();
  auto made = m.deleteFaces(*face, {faceFacing(m, *face, gp::DZ())});
  ASSERT_TRUE(made.ok()) << made.error();
  EXPECT_FALSE(m.document().find(*face));
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  const auto [n, length] = lines(m);
  EXPECT_EQ(n, 1);  // one polyline around
  EXPECT_NEAR(length, 14000.0, 1e-6);
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.document().find(*face));
  EXPECT_EQ(lines(m).first, 0);
}

// The face and its edges selected (double-click): nothing is left.
TEST(DeleteFaces, FaceWithItsEdgesLeavesNothing) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  std::vector<TopoDS_Edge> edges;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(*face)), TopAbs_EDGE); e.More(); e.Next()) {
    edges.push_back(TopoDS::Edge(e.Current()));
  }
  auto made = m.deleteFaces(*face, {faceFacing(m, *face, gp::DZ())}, edges);
  ASSERT_TRUE(made.ok()) << made.error();
  EXPECT_TRUE(made->empty());
  EXPECT_TRUE(m.document().objects().empty());
}

// A face with a round hole: the outline and the circle stay, the circle
// exactly (not as a polygon), also after save and reload.
TEST(DeleteFaces, HoleStaysAsAnExactCircle) {
  Modeler m;
  auto face = m.createRectangle(geom::SketchPlane::ground(), 4000, 3000);
  geom::SketchPlane centre = geom::SketchPlane::ground();
  centre.origin = gp_Pnt(2000, 1500, 0);
  auto disc = m.createCircle(centre, 500);
  ASSERT_TRUE(disc.ok());
  ASSERT_TRUE(m.remove({*disc}).ok());  // the hole stays in the rectangle
  ASSERT_EQ(count(m.document().worldShape(*m.document().find(*face)), TopAbs_WIRE), 2);
  auto made = m.deleteFaces(*face, {faceFacing(m, *face, gp::DZ())});
  ASSERT_TRUE(made.ok()) << made.error();
  EXPECT_EQ(made->size(), 2u);
  const std::string path = ::testing::TempDir() + "delete_faces_circle.sbr";
  ASSERT_TRUE(m.save(path).ok());
  Modeler back;
  ASSERT_TRUE(back.load(path).ok());
  bool circle = false;
  for (const Object& o : back.document().objects()) {
    for (TopExp_Explorer e(back.document().worldShape(o), TopAbs_EDGE); e.More(); e.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
      if (c.GetType() == GeomAbs_Circle) {
        circle = true;
        EXPECT_NEAR(c.Circle().Radius(), 500.0, 1e-9);
      }
    }
  }
  EXPECT_TRUE(circle);
  EXPECT_NEAR(lines(back).second, 14000.0 + 2 * M_PI * 500.0, 1e-6);
}

// DEL-02 / DEL-03 on a box: the top face alone leaves an open shape (its
// edges are the sides' too, so no lines); the top with its edges takes
// the sides along and the 4 vertical edges stay as lines; every face
// leaves the 12 edges.
TEST(DeleteFaces, BoxTopAloneWithItsEdgesOrEverything) {
  {
    Modeler m;
    const ObjectId b = box(m, 1000, 1000, 1000);
    auto made = m.deleteFaces(b, {faceFacing(m, b, gp::DZ())});
    ASSERT_TRUE(made.ok()) << made.error();
    EXPECT_TRUE(made->empty());
    EXPECT_EQ(m.document().find(b)->kind(), ObjectKind::Shell);
    EXPECT_EQ(count(m.document().find(b)->shape, TopAbs_FACE), 5);
    ASSERT_TRUE(m.undo());
    EXPECT_EQ(m.document().find(b)->kind(), ObjectKind::Solid);
  }
  {
    Modeler m;
    const ObjectId b = box(m, 1000, 1000, 1000);
    const TopoDS_Face top = faceFacing(m, b, gp::DZ());
    std::vector<TopoDS_Edge> edges;
    for (TopExp_Explorer e(top, TopAbs_EDGE); e.More(); e.Next()) edges.push_back(TopoDS::Edge(e.Current()));
    const std::size_t steps = m.history().undoCount();
    auto made = m.deleteFaces(b, {top}, edges);
    ASSERT_TRUE(made.ok()) << made.error();
    EXPECT_EQ(m.history().undoCount(), steps + 1);
    EXPECT_EQ(count(m.document().find(b)->shape, TopAbs_FACE), 1);  // the bottom
    const auto [n, length] = lines(m);
    EXPECT_EQ(n, 4);
    EXPECT_NEAR(length, 4000.0, 1e-6);
  }
  {
    Modeler m;
    const ObjectId b = box(m, 1000, 2000, 3000);
    std::vector<TopoDS_Face> faces;
    for (TopExp_Explorer e(m.document().worldShape(*m.document().find(b)), TopAbs_FACE); e.More(); e.Next()) {
      faces.push_back(TopoDS::Face(e.Current()));
    }
    auto made = m.deleteFaces(b, faces);
    ASSERT_TRUE(made.ok()) << made.error();
    EXPECT_FALSE(m.document().find(b));
    EXPECT_NEAR(lines(m).second, 4 * (1000.0 + 2000.0 + 3000.0), 1e-6);  // each edge once
  }
}

// The same through the AI command, with a loose face.
TEST(DeleteFaces, ApiKeepsTheOutline) {
  Modeler m;
  api::CommandApi api(m);
  auto face = m.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  const auto r = nlohmann::json::parse(api.execute(
      nlohmann::json{{"command", "delete_faces"}, {"args", {{"id", *face}, {"faces", {1}}}}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(r["result"]["lines"].size(), 1u);
  EXPECT_FALSE(r["result"].contains("id"));
  EXPECT_NEAR(lines(m).second, 6000.0, 1e-6);
}

// --- Delete of a selection (Modeler::deleteSelection, shared by the GUI's
// Delete key and the API's "delete") -------------------------------------------

namespace {

std::vector<TopoDS_Face> facesWhere(const Modeler& m, const ObjectId& id, bool (*keep)(const gp_Dir&)) {
  std::vector<TopoDS_Face> out;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    if (keep(*geom::planarFaceNormal(f))) out.push_back(f);
  }
  return out;
}

TopoDS_Edge edgeThrough(const Modeler& m, const ObjectId& id, const gp_Pnt& mid) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m.document().worldShape(*m.document().find(id)), TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(mid) < 1e-6) return TopoDS::Edge(edges(i));
  }
  return {};
}

}  // namespace

// Bug 1: the five faces of a cube other than the top, and lines that have
// nothing to do with it: the top stays, the selected lines go, the line
// not selected stays.
TEST(DeleteSelection, FiveFacesAndLinesKeepTheTop) {
  Modeler m;
  const ObjectId b = box(m, 1000, 1000, 1000);
  const ObjectId l1 = *m.createPolyline({gp_Pnt(3000, 0, 0), gp_Pnt(4000, 0, 0)});
  const ObjectId l2 = *m.createPolyline({gp_Pnt(3000, 500, 0), gp_Pnt(4000, 500, 0)});
  const ObjectId kept = *m.createPolyline({gp_Pnt(3000, 1000, 0), gp_Pnt(4000, 1000, 0)});
  DeleteTarget cube{b, false, facesWhere(m, b, [](const gp_Dir& n) { return n.Z() < 0.99; }), {}};
  DeleteTarget a{l1, false, {}, {edgeThrough(m, l1, gp_Pnt(3500, 0, 0))}};
  DeleteTarget c{l2, false, {}, {edgeThrough(m, l2, gp_Pnt(3500, 500, 0))}};
  const std::size_t steps = m.history().undoCount();
  auto out = m.deleteSelection({cube, a, c});
  ASSERT_TRUE(out.ok()) << out.error();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  ASSERT_TRUE(m.document().find(b));
  const TopoDS_Shape left = m.document().worldShape(*m.document().find(b));
  EXPECT_EQ(count(left, TopAbs_FACE), 1);
  EXPECT_NEAR(geom::planarFaceNormal(TopoDS::Face(TopExp_Explorer(left, TopAbs_FACE).Current()))->Z(), 1.0, 1e-9);
  EXPECT_FALSE(m.document().find(l1));
  EXPECT_FALSE(m.document().find(l2));
  EXPECT_TRUE(m.document().find(kept));
  // The cube's other 8 edges stay as lines (SketchUp: deleting faces keeps edges).
  EXPECT_NEAR(lines(m).second, 1000.0 /*kept line*/ + 8 * 1000.0, 1e-6);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(count(m.document().worldShape(*m.document().find(b)), TopAbs_FACE), 6);
  EXPECT_TRUE(m.document().find(l1));
}

// The top's own edges selected as well: then the top goes too (it cannot
// stay without them). Not the same as the case above.
TEST(DeleteSelection, TopEdgesSelectedTakeTheTop) {
  Modeler m;
  const ObjectId b = box(m, 1000, 1000, 1000);
  DeleteTarget cube{b, false, facesWhere(m, b, [](const gp_Dir& n) { return n.Z() < 0.99; }),
                    {edgeThrough(m, b, gp_Pnt(500, 0, 1000))}};
  auto out = m.deleteSelection({cube});
  ASSERT_TRUE(out.ok()) << out.error();
  EXPECT_FALSE(m.document().find(b));  // every face went
}

// Bug 2 and the line rules: a lone line goes alone; one edge of a polyline
// leaves the rest; an outline edge of a face takes the face, the other
// edges stay as lines; a line dividing a face joins the faces.
TEST(DeleteSelection, Lines) {
  Modeler m;
  const ObjectId lone = *m.createPolyline({gp_Pnt(0, 5000, 0), gp_Pnt(1000, 5000, 0)});
  const ObjectId poly = *m.createPolyline({gp_Pnt(0, 7000, 0), gp_Pnt(1000, 7000, 0), gp_Pnt(1000, 8000, 0)});
  auto out = m.deleteSelection({{lone, false, {}, {edgeThrough(m, lone, gp_Pnt(500, 5000, 0))}},
                                {poly, false, {}, {edgeThrough(m, poly, gp_Pnt(500, 7000, 0))}}});
  ASSERT_TRUE(out.ok()) << out.error();
  EXPECT_FALSE(m.document().find(lone));
  EXPECT_NEAR(lines(m).second, 1000.0, 1e-6);  // the polyline's other leg

  Modeler f;
  auto face = f.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  auto outline = f.deleteSelection({{*face, false, {}, {edgeThrough(f, *face, gp_Pnt(1000, 0, 0))}}});
  ASSERT_TRUE(outline.ok()) << outline.error();
  EXPECT_FALSE(f.document().find(*face));
  EXPECT_NEAR(lines(f).second, 1000.0 + 2000.0 + 1000.0, 1e-6);  // three sides

  Modeler d;
  const ObjectId solid = box(d, 2000, 1000, 1000);
  ASSERT_TRUE(d.splitWithLine({gp_Pnt(1000, 0, 1000), gp_Pnt(1000, 1000, 1000)}).ok());
  ASSERT_EQ(count(d.document().worldShape(*d.document().find(solid)), TopAbs_FACE), 7);
  auto joined = d.deleteSelection({{solid, false, {}, {edgeThrough(d, solid, gp_Pnt(1000, 500, 1000))}}});
  ASSERT_TRUE(joined.ok()) << joined.error();
  EXPECT_EQ(joined->merged, 1);
  EXPECT_EQ(count(d.document().worldShape(*d.document().find(solid)), TopAbs_FACE), 6);
  EXPECT_EQ(d.document().find(solid)->kind(), ObjectKind::Solid);
}

// A failure anywhere changes nothing and says why.
TEST(DeleteSelection, AllOrNothing) {
  Modeler m;
  const ObjectId b = box(m, 1000, 1000, 1000);
  const ObjectId l = *m.createPolyline({gp_Pnt(3000, 0, 0), gp_Pnt(4000, 0, 0)});
  ASSERT_TRUE(m.setLocked({b}, true).ok());
  const std::size_t steps = m.history().undoCount();
  auto out = m.deleteSelection({{l, true, {}, {}}, {b, false, facesWhere(m, b, [](const gp_Dir& n) { return n.Z() > 0.99; }), {}}});
  ASSERT_FALSE(out.ok());
  EXPECT_NE(out.error().find("locked"), std::string::npos) << out.error();
  EXPECT_TRUE(m.document().find(l));
  EXPECT_EQ(m.history().undoCount(), steps);
}

// The API's delete goes through the same call.
TEST(DeleteSelection, Api) {
  Modeler m;
  api::CommandApi api(m);
  const ObjectId b = box(m, 1000, 1000, 1000);
  const ObjectId l = *m.createPolyline({gp_Pnt(3000, 0, 0), gp_Pnt(4000, 0, 0)});
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{
      {"command", "delete"},
      {"args", {{"elements", {{{"id", b}, {"faces", {{{"normal", {0, 0, -1}}}}}}, {{"id", l}, {"edges", {1}}}}}}}}
                                                       .dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  EXPECT_EQ(count(m.document().worldShape(*m.document().find(b)), TopAbs_FACE), 5);
  EXPECT_FALSE(m.document().find(l));
}
