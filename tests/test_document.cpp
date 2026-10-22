#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

TopoDS_Face faceWithNormal(const TopoDS_Shape& shape, const gp_Dir& n) {
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face f = TopoDS::Face(it.Current());
    const auto fn = geom::planarFaceNormal(f);
    if (fn && fn->Angle(n) < 1e-9) return f;
  }
  return {};
}

const Object& object(const Modeler& m, const ObjectId& id) { return *m.document().find(id); }

}  // namespace

TEST(Modeler, RectanglePushPullCreatesParametricExtrusion) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 4000, 6000);
  ASSERT_TRUE(id.ok()) << id.error();
  EXPECT_EQ(object(m, *id).kind(), ObjectKind::Face);

  const TopoDS_Face face = TopoDS::Face(m.document().worldShape(*id));
  auto pp = m.pushPull(*id, face, 3000);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(pp->parameterKey, "op0.distance");
  EXPECT_EQ(object(m, *id).kind(), ObjectKind::Solid);
  EXPECT_NEAR(geom::volume(object(m, *id).shape), 4000.0 * 6000 * 3000, 1e-3);

  // The extrusion height can be re-edited afterwards: 3000 -> 4500.
  ASSERT_TRUE(m.setParameter(*id, "op0.distance", 4500).ok());
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 4000, 6000, 4500);
  // So can the profile.
  ASSERT_TRUE(m.setParameter(*id, "width", 5000).ok());
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 5000, 6000, 4500);
}

TEST(Modeler, PullingTopCapEditsExtrusionDistance) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*id, 3000).ok());
  const TopoDS_Face top = faceWithNormal(m.document().worldShape(*id), gp_Dir(0, 0, 1));
  auto pp = m.pushPull(*id, top, 500);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(pp->parameterKey, "op0.distance");
  EXPECT_EQ(object(m, *id).recipe.modifiers.size(), 1u);
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 1000, 1000, 3500);
}

TEST(Modeler, SideFacePushPullIsRecordedAndReplayed) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*id, 3000).ok());
  const TopoDS_Face side = faceWithNormal(m.document().worldShape(*id), gp_Dir(1, 0, 0));
  auto pp = m.pushPull(*id, side, 250);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(pp->parameterKey, "op1.distance");
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 1250, 1000, 3000);

  // Changing the upstream height replays the side push on the new solid.
  ASSERT_TRUE(m.setParameter(*id, "op0.distance", 4000).ok());
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 1250, 1000, 4000);
  EXPECT_TRUE(geom::validateShape(object(m, *id).shape, true).valid);
  // And the push distance itself is editable.
  ASSERT_TRUE(m.setParameter(*id, "op1.distance", 750).ok());
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 1750, 1000, 4000);
}

TEST(Modeler, TopologicalNamingFailureIsReportedAndRolledBack) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*id, 1000).ok());
  const TopoDS_Face side = faceWithNormal(m.document().worldShape(*id), gp_Dir(1, 0, 0));
  ASSERT_TRUE(m.pushPull(*id, side, 500).ok());
  const double before = geom::volume(object(m, *id).shape);
  // Widening the profile moves the referenced +X plane: the reference no
  // longer resolves, so the edit is rejected and nothing changes.
  const Status s = m.setParameter(*id, "width", 2000);
  EXPECT_FALSE(s.ok());
  EXPECT_NE(s.message().find("no longer exists"), std::string::npos) << s.message();
  EXPECT_DOUBLE_EQ(geom::volume(object(m, *id).shape), before);
}

TEST(Modeler, InvalidEditIsRejected) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*id, 1000).ok());
  const std::size_t undoCount = m.history().undoCount();
  EXPECT_FALSE(m.setParameter(*id, "op0.distance", 0.0).ok());
  EXPECT_FALSE(m.setParameter(*id, "width", 0.0).ok());
  EXPECT_FALSE(m.setParameter(*id, "nope", 1.0).ok());
  const TopoDS_Face top = faceWithNormal(m.document().worldShape(*id), gp_Dir(0, 0, 1));
  EXPECT_FALSE(m.pushPull(*id, top, -1000).ok());  // would collapse the extrusion
  EXPECT_EQ(m.history().undoCount(), undoCount);
  EXPECT_DOUBLE_EQ(geom::volume(object(m, *id).shape), 1e9);
}

TEST(Modeler, UndoRedoRestoresExactGeometry) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 1000, 2000);
  ASSERT_TRUE(m.extrude(*id, 3000).ok());
  ASSERT_TRUE(m.setParameter(*id, "op0.distance", 4500).ok());
  EXPECT_TRUE(m.history().canUndo());
  EXPECT_EQ(m.history().undoName(), "Edit Extrude distance");

  ASSERT_TRUE(m.undo());
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 1000, 2000, 3000);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(object(m, *id).kind(), ObjectKind::Face);
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.document().objects().empty());
  EXPECT_FALSE(m.undo());
  EXPECT_TRUE(m.history().isClean());

  ASSERT_TRUE(m.redo());
  ASSERT_TRUE(m.redo());
  ASSERT_TRUE(m.redo());
  test::expectBounds(object(m, *id).shape, 0, 0, 0, 1000, 2000, 4500);
  EXPECT_FALSE(m.redo());
}

TEST(Modeler, DeleteAndUndoRestoresOrder) {
  Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  auto b = m.createRectangle(geom::SketchPlane::ground(), 200, 200);
  auto c = m.createRectangle(geom::SketchPlane::ground(), 300, 300);
  ASSERT_TRUE(m.remove({*b}).ok());
  EXPECT_EQ(m.document().objects().size(), 2u);
  ASSERT_TRUE(m.undo());
  ASSERT_EQ(m.document().objects().size(), 3u);
  EXPECT_EQ(m.document().objects()[1].id, *b);
  EXPECT_EQ(m.document().objects()[2].id, *c);
  (void)a;
}

TEST(Modeler, ObjectIdsAreUniqueAndNamesIncrement) {
  Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  auto b = m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  EXPECT_NE(*a, *b);
  EXPECT_EQ(a->size(), 36u);
  EXPECT_EQ(object(m, *a).name, "Rectangle 1");
  EXPECT_EQ(object(m, *b).name, "Rectangle 2");
}

TEST(Modeler, PolygonAndPolyline) {
  Modeler m;
  auto face = m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(500, 800, 0)});
  ASSERT_TRUE(face.ok()) << face.error();
  ASSERT_TRUE(m.extrude(*face, 100).ok());
  EXPECT_NEAR(geom::volume(object(m, *face).shape), 0.5 * 1000 * 800 * 100, 1e-3);

  auto line = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0)});
  ASSERT_TRUE(line.ok());
  EXPECT_EQ(object(m, *line).kind(), ObjectKind::Curve);
  EXPECT_FALSE(m.extrude(*line, 100).ok());
  EXPECT_FALSE(m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(1, 0, 0)}).ok());
}

TEST(Modeler, StepExportImport) {
  Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*a, 1000).ok());
  geom::SketchPlane p = geom::SketchPlane::ground();
  p.origin = gp_Pnt(5000, 0, 0);
  auto b = m.createRectangle(p, 2000, 1000);
  ASSERT_TRUE(m.extrude(*b, 500).ok());

  const std::string path = test::tempPath("doc.step");
  ASSERT_TRUE(m.exportStep(path).ok());

  Modeler other;
  auto ids = other.importStep(path);
  ASSERT_TRUE(ids.ok()) << ids.error();
  ASSERT_EQ(ids->size(), 2u);
  double total = 0;
  for (const auto& summary : other.listObjects()) total += summary.volume;
  EXPECT_NEAR(total, 1e9 + 1e9, 1e-2);

  // Imported solids can still be pushed/pulled.
  const ObjectId first = ids->front();
  const TopoDS_Face top = faceWithNormal(other.document().worldShape(first), gp_Dir(0, 0, 1));
  ASSERT_FALSE(top.IsNull());
  EXPECT_TRUE(other.pushPull(first, top, 100).ok());
}

// Performance step 2: lookups are indexed and changes are logged so views
// update incrementally; the results must stay exactly those of a scan.
TEST(Document, IndexAndChangeLogStayConsistent) {
  Document d;
  auto make = [](const std::string& id, const std::string& parent = {}) {
    Object o;
    o.id = id;
    o.parent = parent;
    o.recipe.base = GroupNode{};
    return o;
  };
  d.insert(make("g"));
  d.insert(make("a", "g"));
  d.insert(make("b", "g"));
  const std::uint64_t start = d.sequence();
  d.insert(make("c"), 0);  // insert in front: positions shift
  EXPECT_EQ(d.indexOf("c"), 0u);
  EXPECT_EQ(d.indexOf("b"), 3u);
  EXPECT_EQ(d.children("g"), (std::vector<ObjectId>{"a", "b"}));

  Object moved = *d.find("b");
  moved.parent = "c";
  d.replace(moved);
  EXPECT_EQ(d.children("g"), (std::vector<ObjectId>{"a"}));
  EXPECT_EQ(d.children("c"), (std::vector<ObjectId>{"b"}));
  d.remove("a");
  EXPECT_EQ(d.find("a"), nullptr);
  EXPECT_TRUE(d.children("g").empty());
  EXPECT_EQ(d.indexOf("g"), 1u);

  const auto changed = d.changedSince(start);
  ASSERT_TRUE(changed);
  EXPECT_EQ(*changed, (std::vector<ObjectId>{"a", "b", "c"}));
  EXPECT_TRUE(d.changedSince(d.sequence())->empty());

  // Settings changes and clearing force a full resync.
  const std::uint64_t before = d.sequence();
  d.settingsChanged();
  EXPECT_FALSE(d.changedSince(before));
  const std::uint64_t afterSettings = d.sequence();
  EXPECT_TRUE(d.changedSince(afterSettings));
  d.clear();
  EXPECT_FALSE(d.changedSince(afterSettings));
  EXPECT_EQ(d.find("g"), nullptr);
}

// Revising the edit just made ("type right after") stays in its undo step.
TEST(Modeler, AmendMergesIntoTheLastUndoStep) {
  Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1000).ok());
  const std::size_t steps = m.history().undoCount();
  m.history().beginAmend();
  ASSERT_TRUE(m.setParameter(*box, "op0.distance", 2000).ok());
  ASSERT_TRUE(m.setParameter(*box, "width", 500).ok());
  m.history().endAmend();
  EXPECT_EQ(m.history().undoCount(), steps);
  EXPECT_NEAR(geom::volume(m.document().find(*box)->shape), 500.0 * 1000 * 2000, 1e-3);
  ASSERT_TRUE(m.undo());  // before the extrusion: the plain face
  EXPECT_EQ(m.document().find(*box)->kind(), ObjectKind::Face);
  ASSERT_TRUE(m.redo());
  EXPECT_NEAR(geom::volume(m.document().find(*box)->shape), 500.0 * 1000 * 2000, 1e-3);

  // Something made and removed again within the step leaves nothing behind.
  auto extra = m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  m.history().beginAmend();
  ASSERT_TRUE(m.remove({*extra}).ok());
  m.history().endAmend();
  EXPECT_FALSE(m.document().find(*extra));
  EXPECT_EQ(m.history().undoCount(), steps);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().find(*box)->kind(), ObjectKind::Face);
}
