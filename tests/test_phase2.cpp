// Phase 2 core: placement, hierarchy, components, Booleans, offset, files.

#include <cmath>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId box(Modeler& m, double x, double y, double z, double w, double d, double h,
             const std::string& name = {}) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, z);
  auto id = m.createRectangle(plane, w, d, name);
  EXPECT_TRUE(id.ok()) << id.error();
  EXPECT_TRUE(m.extrude(*id, h).ok());
  return *id;
}

TopoDS_Face faceWithNormal(const TopoDS_Shape& shape, const gp_Dir& n) {
  for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
    const TopoDS_Face f = TopoDS::Face(it.Current());
    const auto fn = geom::planarFaceNormal(f);
    if (fn && fn->Angle(n) < 1e-9) return f;
  }
  return {};
}

double worldVolume(const Modeler& m, const ObjectId& id) {
  return geom::volume(m.document().worldShape(id));
}

}  // namespace

// --- Move / Rotate / Copy ---------------------------------------------------------

TEST(Placement, MoveKeepsLocalGeometryAndUndoes) {
  Modeler m;
  const ObjectId id = box(m, 0, 0, 0, 1000, 1000, 1000);
  ASSERT_TRUE(m.move({id}, gp_Vec(2000, 500, 0)).ok());
  test::expectBounds(m.document().worldShape(id), 2000, 500, 0, 3000, 1500, 1000);
  test::expectBounds(m.document().find(id)->shape, 0, 0, 0, 1000, 1000, 1000);
  EXPECT_EQ(m.history().undoName(), "Move");
  ASSERT_TRUE(m.undo());
  test::expectBounds(m.document().worldShape(id), 0, 0, 0, 1000, 1000, 1000);
}

TEST(Placement, RotateAboutAxis) {
  Modeler m;
  const ObjectId id = box(m, 0, 0, 0, 2000, 1000, 500);
  ASSERT_TRUE(m.rotate({id}, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI / 2).ok());
  test::expectBounds(m.document().worldShape(id), -1000, 0, 0, 0, 2000, 500);
  EXPECT_NEAR(worldVolume(m, id), 1e9, 1e-3);
}

TEST(Placement, LinearArrayIsOneCommand) {
  Modeler m;
  const ObjectId id = box(m, 0, 0, 0, 100, 100, 3000, "Column");
  gp_Trsf step;
  step.SetTranslation(gp_Vec(6000, 0, 0));
  auto copies = m.copy({id}, step, 3);
  ASSERT_TRUE(copies.ok()) << copies.error();
  ASSERT_EQ(copies->size(), 3u);
  EXPECT_EQ(m.document().objects().size(), 4u);
  test::expectBounds(m.document().worldShape(copies->back()), 18000, 0, 0, 18100, 100, 3000);
  EXPECT_EQ(m.document().find(copies->front())->name, "Column");
  EXPECT_EQ(m.history().undoName(), "Array");
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), 1u);
}

TEST(Placement, PolarArray) {
  Modeler m;
  const ObjectId id = box(m, 4000, -50, 0, 100, 100, 100);
  gp_Trsf step;
  step.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI / 2);
  auto copies = m.copy({id}, step, 3);
  ASSERT_TRUE(copies.ok());
  const Bnd_Box b = geom::exactBounds(m.document().worldShape((*copies)[1]));  // 180 degrees
  EXPECT_NEAR(b.CornerMin().X(), -4100, 1e-6);
  EXPECT_NEAR(b.CornerMax().X(), -4000, 1e-6);
}

TEST(Placement, PushPullOnMovedObjectUsesWorldFaces) {
  Modeler m;
  const ObjectId id = box(m, 0, 0, 0, 1000, 1000, 1000);
  ASSERT_TRUE(m.move({id}, gp_Vec(5000, 0, 0)).ok());
  const TopoDS_Face side = faceWithNormal(m.document().worldShape(id), gp::DX());
  auto pp = m.pushPull(id, side, 500);
  ASSERT_TRUE(pp.ok()) << pp.error();
  test::expectBounds(m.document().worldShape(id), 5000, 0, 0, 6500, 1000, 1000);
}

TEST(Placement, SetPosition) {
  Modeler m;
  const ObjectId id = box(m, 0, 0, 0, 10, 10, 10);
  ASSERT_TRUE(m.setPosition(id, gp_Vec(100, 200, 300)).ok());
  test::expectBounds(m.document().worldShape(id), 100, 200, 300, 110, 210, 310);
}

// --- Groups ------------------------------------------------------------------------

TEST(Groups, GroupMoveUngroup) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 100, 100, 100);
  const ObjectId b = box(m, 500, 0, 0, 100, 100, 100);
  auto g = m.group({a, b}, "Furniture");
  ASSERT_TRUE(g.ok()) << g.error();
  EXPECT_EQ(m.document().find(a)->parent, *g);
  EXPECT_EQ(m.document().find(*g)->kind(), ObjectKind::Group);
  EXPECT_EQ(m.document().rootOf(b), *g);

  ASSERT_TRUE(m.move({*g}, gp_Vec(0, 1000, 0)).ok());
  test::expectBounds(m.document().worldShape(b), 500, 1000, 0, 600, 1100, 100);
  // Moving a group and a child together moves the child only once.
  ASSERT_TRUE(m.move({*g, a}, gp_Vec(0, 0, 10)).ok());
  test::expectBounds(m.document().worldShape(a), 0, 1000, 10, 100, 1100, 110);

  ASSERT_TRUE(m.ungroup(*g).ok());
  EXPECT_FALSE(m.document().find(*g));
  EXPECT_TRUE(m.document().find(a)->parent.empty());
  test::expectBounds(m.document().worldShape(b), 500, 1000, 10, 600, 1100, 110);
}

TEST(Groups, ChildMovesInWorldSpaceUnderRotatedParent) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 100, 100, 100);
  auto g = m.group({a});
  ASSERT_TRUE(m.rotate({*g}, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI / 2).ok());
  ASSERT_TRUE(m.move({a}, gp_Vec(1000, 0, 0)).ok());
  test::expectBounds(m.document().worldShape(a), 900, 0, 0, 1000, 100, 100);
}

TEST(Groups, DeleteRemovesChildrenAndUndoRestores) {
  Modeler m;
  const ObjectId first = box(m, 0, 0, 0, 10, 10, 10);
  const ObjectId a = box(m, 100, 0, 0, 10, 10, 10);
  const ObjectId b = box(m, 200, 0, 0, 10, 10, 10);
  const ObjectId last = box(m, 300, 0, 0, 10, 10, 10);
  auto g = m.group({a, b});
  ASSERT_TRUE(m.remove({*g}).ok());
  EXPECT_EQ(m.document().objects().size(), 2u);
  ASSERT_TRUE(m.undo());
  ASSERT_EQ(m.document().objects().size(), 5u);
  EXPECT_EQ(m.document().objects().front().id, first);
  EXPECT_EQ(m.document().find(b)->parent, *g);
  EXPECT_TRUE(m.document().find(last));
}

TEST(Groups, CopyOfGroupCopiesChildren) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 10, 10, 10);
  auto g = m.group({a});
  gp_Trsf t;
  t.SetTranslation(gp_Vec(100, 0, 0));
  auto copies = m.copy({*g}, t, 1);
  ASSERT_TRUE(copies.ok());
  const auto kids = m.document().children(copies->front());
  ASSERT_EQ(kids.size(), 1u);
  EXPECT_NE(kids.front(), a);
  test::expectBounds(m.document().worldShape(kids.front()), 100, 0, 0, 110, 10, 10);
}

TEST(Groups, RequiresSameParent) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 10, 10, 10);
  const ObjectId b = box(m, 20, 0, 0, 10, 10, 10);
  auto g = m.group({a});
  EXPECT_FALSE(m.group({a, b}).ok());
  (void)g;
}

// --- Components --------------------------------------------------------------------

TEST(Components, InstancesShareDefinition) {
  Modeler m;
  const ObjectId column = box(m, 1000, 0, 0, 300, 300, 3000, "Column");
  auto instance = m.makeComponent({column}, "Column C1");
  ASSERT_TRUE(instance.ok()) << instance.error();
  EXPECT_FALSE(m.document().find(column));
  const Object* inst = m.document().find(*instance);
  ASSERT_EQ(inst->kind(), ObjectKind::Instance);
  test::expectBounds(m.document().worldShape(*instance), 1000, 0, 0, 1300, 300, 3000);
  ASSERT_EQ(m.document().definitions().size(), 1u);
  const std::string defId = m.document().definitions().front().id;
  EXPECT_EQ(m.document().definitions().front().name, "Column C1");

  gp_Trsf step;
  step.SetTranslation(gp_Vec(6000, 0, 0));
  auto copies = m.copy({*instance}, step, 2);
  ASSERT_TRUE(copies.ok());
  EXPECT_EQ(m.document().instanceCount(defId), 3u);

  // Editing the definition updates every instance.
  const auto params = m.definitionParameters(defId, column);
  ASSERT_FALSE(params.empty());
  ASSERT_TRUE(m.setDefinitionParameter(defId, column, "op0.distance", 4000).ok());
  for (const ObjectId& id : {*instance, (*copies)[0], (*copies)[1]}) {
    const Bnd_Box b = geom::exactBounds(m.document().worldShape(id));
    EXPECT_NEAR(b.CornerMax().Z(), 4000, 1e-6);
  }
  test::expectBounds(m.document().worldShape((*copies)[1]), 13000, 0, 0, 13300, 300, 4000);
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::exactBounds(m.document().worldShape((*copies)[0])).CornerMax().Z(), 3000, 1e-6);
}

TEST(Components, ExplodeAndPushPullRules) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 100, 100, 100);
  const ObjectId b = box(m, 200, 0, 0, 100, 100, 100);
  auto g = m.group({b});
  auto instance = m.makeComponent({a, *g});
  ASSERT_TRUE(instance.ok()) << instance.error();
  ASSERT_TRUE(m.move({*instance}, gp_Vec(0, 1000, 0)).ok());

  const TopoDS_Face top = faceWithNormal(m.document().worldShape(*instance), gp::DZ());
  EXPECT_FALSE(m.pushPull(*instance, top, 10).ok());
  EXPECT_FALSE(m.makeComponent({*instance}).ok());  // no nesting yet

  auto parts = m.explode(*instance);
  ASSERT_TRUE(parts.ok()) << parts.error();
  ASSERT_EQ(parts->size(), 2u);
  EXPECT_FALSE(m.document().find(*instance));
  Bnd_Box all;
  for (const Object& o : m.document().objects()) all.Add(geom::exactBounds(m.document().worldShape(o)));
  EXPECT_NEAR(all.CornerMin().Y(), 1000, 1e-6);
  EXPECT_NEAR(all.CornerMax().X(), 300, 1e-6);
  EXPECT_EQ(m.document().objects().size(), 3u);  // box, group, box-in-group
}

// --- Booleans -----------------------------------------------------------------------

TEST(Booleans, SubtractIsParametric) {
  Modeler m;
  const ObjectId wall = box(m, 0, 0, 0, 4000, 200, 3000, "Wall");
  const ObjectId opening = box(m, 1000, -100, 0, 900, 400, 2100, "Door");
  auto cut = m.boolean(BooleanKind::Subtract, {wall, opening});
  ASSERT_TRUE(cut.ok()) << cut.error();
  EXPECT_FALSE(m.document().find(wall));
  EXPECT_EQ(m.document().objects().size(), 1u);
  const double expected = 4000.0 * 200 * 3000 - 900.0 * 200 * 2100;
  EXPECT_NEAR(worldVolume(m, *cut), expected, 1e-2);
  EXPECT_TRUE(geom::validateShape(m.document().find(*cut)->shape, true).valid);

  // The opening height is still editable: in1 = second operand.
  bool found = false;
  for (const Parameter& p : m.parameters(*cut)) found |= p.key == "in1.op0.distance";
  ASSERT_TRUE(found);
  ASSERT_TRUE(m.setParameter(*cut, "in1.op0.distance", 2400).ok());
  EXPECT_NEAR(worldVolume(m, *cut), 4000.0 * 200 * 3000 - 900.0 * 200 * 2400, 1e-2);

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), 2u);
}

TEST(Booleans, UnionIntersectAndSplit) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 1000, 1000, 1000);
  const ObjectId b = box(m, 500, 0, 0, 1000, 1000, 1000);
  auto u = m.boolean(BooleanKind::Union, {a, b});
  ASSERT_TRUE(u.ok()) << u.error();
  EXPECT_NEAR(worldVolume(m, *u), 1.5e9, 1e-2);
  EXPECT_EQ(geom::validateShape(m.document().find(*u)->shape, true).faces, 6);
  ASSERT_TRUE(m.undo());

  auto i = m.boolean(BooleanKind::Intersect, {a, b});
  ASSERT_TRUE(i.ok()) << i.error();
  EXPECT_NEAR(worldVolume(m, *i), 0.5e9, 1e-2);
  ASSERT_TRUE(m.undo());

  // Cutting all the way through leaves two solids.
  const ObjectId slab = box(m, -100, 400, -100, 3000, 200, 1200);
  auto split = m.boolean(BooleanKind::Subtract, {a, slab});
  ASSERT_TRUE(split.ok()) << split.error();
  EXPECT_NEAR(worldVolume(m, *split), 1e9 - 200.0 * 1000 * 1000, 1e-2);
}

TEST(Booleans, FailuresChangeNothing) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 100, 100, 100);
  const ObjectId far = box(m, 1000, 0, 0, 100, 100, 100);
  auto face = m.createRectangle(geom::SketchPlane::ground(), 50, 50);
  const std::size_t undo = m.history().undoCount();
  EXPECT_FALSE(m.boolean(BooleanKind::Intersect, {a, far}).ok());
  EXPECT_FALSE(m.boolean(BooleanKind::Union, {a, *face}).ok());
  EXPECT_FALSE(m.boolean(BooleanKind::Union, {a}).ok());
  EXPECT_EQ(m.history().undoCount(), undo);
  EXPECT_EQ(m.document().objects().size(), 3u);
}

TEST(Booleans, ResultCanBePushedAndMovedOperandsKeepPlacement) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 1000, 1000, 1000);
  const ObjectId b = box(m, 0, 0, 0, 500, 500, 500);
  ASSERT_TRUE(m.move({b}, gp_Vec(750, 250, 250)).ok());
  auto u = m.boolean(BooleanKind::Union, {a, b});
  ASSERT_TRUE(u.ok());
  test::expectBounds(m.document().worldShape(*u), 0, 0, 0, 1250, 1000, 1000);
}

// --- Offset ------------------------------------------------------------------------------

TEST(Offset, OutwardsInwardsAndExtrude) {
  Modeler m;
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 1000, 2000);
  auto out = m.offset(*rect, 100);
  ASSERT_TRUE(out.ok()) << out.error();
  EXPECT_NE(out->id, *rect);
  EXPECT_NEAR(geom::area(m.document().find(out->id)->shape), 1200.0 * 2200, 1e-3);
  ASSERT_TRUE(m.setParameter(out->id, out->parameterKey, -100).ok());
  EXPECT_NEAR(geom::area(m.document().find(out->id)->shape), 800.0 * 1800, 1e-3);
  EXPECT_FALSE(m.setParameter(out->id, out->parameterKey, -600).ok());  // collapses

  // The offset face is a normal profile: push/pull extrudes it upwards.
  const TopoDS_Face face = TopoDS::Face(m.document().worldShape(out->id));
  auto pp = m.pushPull(out->id, face, 300);
  ASSERT_TRUE(pp.ok()) << pp.error();
  test::expectBounds(m.document().worldShape(out->id), 100, 100, 0, 900, 1900, 300);
  EXPECT_EQ(pp->parameterKey, "op1.distance");
}

TEST(Offset, PolygonOffsetAndRejections) {
  Modeler m;
  auto tri = m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(3000, 0, 0), gp_Pnt(0, 4000, 0)});
  auto out = m.offset(*tri, -100);
  ASSERT_TRUE(out.ok()) << out.error();
  EXPECT_LT(geom::area(m.document().find(out->id)->shape), 6e6);
  const ObjectId solid = box(m, 0, 0, 0, 10, 10, 10);
  EXPECT_FALSE(m.offset(solid, 10).ok());
}

// --- Files ----------------------------------------------------------------------------------

TEST(Phase2Files, NativeRoundTripOfHierarchyComponentsAndBooleans) {
  Modeler m;
  const ObjectId wall = box(m, 0, 0, 0, 4000, 200, 3000, "Wall");
  const ObjectId door = box(m, 1000, -100, 0, 900, 400, 2100, "Door");
  auto cut = m.boolean(BooleanKind::Subtract, {wall, door});
  const ObjectId column = box(m, 0, 1000, 0, 300, 300, 3000, "Column");
  auto inst = m.makeComponent({column});
  gp_Trsf step;
  step.SetTranslation(gp_Vec(3000, 0, 0));
  auto copies = m.copy({*inst}, step, 1);
  auto g = m.group({*cut, *inst, copies->front()}, "Storey 1");
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  auto off = m.offset(*rect, 50);
  ASSERT_TRUE(g.ok() && off.ok());
  ASSERT_TRUE(m.rotate({*g}, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), 0.3).ok());

  const std::string path = test::tempPath("phase2.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  std::vector<std::string> warnings;
  const Status s = loaded.load(path, &warnings);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  ASSERT_EQ(loaded.document().objects().size(), m.document().objects().size());
  for (const Object& o : m.document().objects()) {
    const Object* l = loaded.document().find(o.id);
    ASSERT_TRUE(l) << o.name;
    EXPECT_EQ(l->parent, o.parent);
    EXPECT_EQ(l->kind(), o.kind());
    if (!o.shape.IsNull()) {
      EXPECT_NEAR(geom::volume(loaded.document().worldShape(*l)),
                  geom::volume(m.document().worldShape(o)), 1e-3);
      const Bnd_Box a = geom::exactBounds(m.document().worldShape(o));
      const Bnd_Box b = geom::exactBounds(loaded.document().worldShape(*l));
      EXPECT_NEAR(a.CornerMax().Distance(b.CornerMax()), 0, 1e-6) << o.name;
    }
  }
  // Still parametric after reload.
  const std::string defId = loaded.document().definitions().front().id;
  ASSERT_TRUE(loaded.setDefinitionParameter(defId, column, "op0.distance", 3500).ok());
  ASSERT_TRUE(loaded.setParameter(*cut, "in1.op0.distance", 2400).ok());
  ASSERT_TRUE(loaded.setParameter(off->id, "op0.distance", 80).ok());
}

TEST(Phase2Files, ReadsVersion1Files) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  std::string text = serializeDocument(m.document());
  const auto pos = text.find("\"version\": 2");
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, std::string("\"version\": 2").size(), "\"version\": 1");
  Document d;
  ASSERT_TRUE(deserializeDocument(text, d).ok());
  EXPECT_TRUE(d.find(*id));
}

TEST(Phase2Files, StepKeepsNamesColorsAndPlacement) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 1000, 1000, 1000, "Core wall");
  ASSERT_TRUE(m.setColor(a, Color{0.8, 0.2, 0.1}).ok());
  const ObjectId b = box(m, 0, 0, 0, 500, 500, 500, "Column");
  auto g = m.group({b});
  ASSERT_TRUE(m.move({*g}, gp_Vec(3000, 0, 0)).ok());

  const std::string path = test::tempPath("names.step");
  ASSERT_TRUE(m.exportStep(path).ok());
  Modeler other;
  auto ids = other.importStep(path);
  ASSERT_TRUE(ids.ok()) << ids.error();
  ASSERT_EQ(ids->size(), 2u);
  const Object* wall = nullptr;
  const Object* col = nullptr;
  for (const ObjectId& id : *ids) {
    const Object* o = other.document().find(id);
    if (o->name == "Core wall") wall = o;
    if (o->name == "Column") col = o;
  }
  ASSERT_TRUE(wall && col);
  EXPECT_NEAR(wall->color.r, 0.8, 1e-3);
  EXPECT_NEAR(wall->color.g, 0.2, 1e-3);
  test::expectBounds(other.document().worldShape(*col), 3000, 0, 0, 3500, 500, 500, 1e-5);
}

// --- Push/pull of profiles drawn on solids ---------------------------------------------

TEST(PushPullOnSolids, RectangleOnWallCutsOpening) {
  Modeler m;
  const ObjectId wall = box(m, 0, 0, 0, 4000, 200, 3000, "Wall");
  // Door outline drawn on the wall's -Y face.
  auto door = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(1000, 0, 0), gp_Dir(0, -1, 0)),
                                900, 2100);
  ASSERT_TRUE(door.ok());
  const TopoDS_Face face = TopoDS::Face(m.document().worldShape(*door));
  auto pp = m.pushPull(*door, face, -200);  // into the wall
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_EQ(m.document().find(pp->id)->name, "Wall");
  EXPECT_EQ(pp->parameterKey, "in1.op0.distance");
  const double full = 4000.0 * 200 * 3000;
  EXPECT_NEAR(worldVolume(m, pp->id), full - 900.0 * 2100 * 200, 1e-2);

  // Re-edit the depth: a 100 mm niche instead of an opening.
  ASSERT_TRUE(m.setParameter(pp->id, pp->parameterKey, -100).ok());
  EXPECT_NEAR(worldVolume(m, pp->id), full - 900.0 * 2100 * 100, 1e-2);

  // One undo step restores wall and outline.
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), 2u);
  EXPECT_TRUE(m.document().find(wall));
}

TEST(PushPullOnSolids, RectangleOnTopJoins) {
  Modeler m;
  box(m, 0, 0, 0, 1000, 1000, 1000);
  geom::SketchPlane top = geom::SketchPlane::ground();
  top.origin = gp_Pnt(250, 250, 1000);
  auto rect = m.createRectangle(top, 500, 500);
  auto pp = m.pushPull(*rect, TopoDS::Face(m.document().worldShape(*rect)), 500);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_NEAR(worldVolume(m, pp->id), 1e9 + 500.0 * 500 * 500, 1e-2);
  EXPECT_TRUE(geom::validateShape(m.document().find(pp->id)->shape, true).valid);
}

TEST(PushPullOnSolids, ProfileOffTheSolidStaysSeparate) {
  Modeler m;
  box(m, 0, 0, 0, 1000, 1000, 1000);
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(800, 800, 1000);  // overhangs the top face
  auto rect = m.createRectangle(plane, 500, 500);
  auto pp = m.pushPull(*rect, TopoDS::Face(m.document().worldShape(*rect)), 300);
  ASSERT_TRUE(pp.ok());
  EXPECT_EQ(pp->id, *rect);
  EXPECT_EQ(m.document().objects().size(), 2u);
}

TEST(Circles, ExtrudeEditRadiusAndCutRoundOpening) {
  Modeler m;
  geom::SketchPlane plane = geom::SketchPlane::ground();
  auto column = m.createCircle(plane, 250);
  ASSERT_TRUE(column.ok()) << column.error();
  ASSERT_TRUE(m.extrude(*column, 3000).ok());
  EXPECT_NEAR(worldVolume(m, *column), M_PI * 250 * 250 * 3000, 1.0);
  ASSERT_TRUE(m.setParameter(*column, "radius", 300).ok());
  EXPECT_NEAR(worldVolume(m, *column), M_PI * 300 * 300 * 3000, 1.0);
  EXPECT_FALSE(m.setParameter(*column, "radius", 0).ok());

  // Round window: circle drawn on a wall and pushed through.
  const ObjectId wall = box(m, 2000, 0, 0, 3000, 200, 3000, "Wall");
  auto hole = m.createCircle(geom::SketchPlane::fromNormal(gp_Pnt(3500, 0, 1500), gp_Dir(0, -1, 0)), 400);
  ASSERT_TRUE(hole.ok());
  auto pp = m.pushPull(*hole, TopoDS::Face(m.document().worldShape(*hole)), -200);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_NEAR(worldVolume(m, pp->id), 3000.0 * 200 * 3000 - M_PI * 400 * 400 * 200, 1.0);
  EXPECT_FALSE(m.document().find(wall));

  // Saved and reloaded with its radius.
  const std::string text = serializeDocument(m.document());
  Document d;
  ASSERT_TRUE(deserializeDocument(text, d).ok());
  EXPECT_NE(text.find("\"type\": \"circle\""), std::string::npos);
}
