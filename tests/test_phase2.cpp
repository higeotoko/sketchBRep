// Phase 2 core: placement, hierarchy, components, Booleans, offset, files.

#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Annotation.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/ShapeIO.h"
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
  // Outwards the new face is the frame around the original (SketchUp).
  auto out = m.offset(*rect, 100);
  ASSERT_TRUE(out.ok()) << out.error();
  EXPECT_NE(out->id, *rect);
  EXPECT_NEAR(geom::area(m.document().find(out->id)->shape), 1200.0 * 2200 - 1000.0 * 2000, 1e-3);
  EXPECT_NEAR(geom::area(m.document().find(*rect)->shape), 1000.0 * 2000, 1e-3);
  ASSERT_TRUE(m.setParameter(out->id, out->parameterKey, 200).ok());
  EXPECT_NEAR(geom::area(m.document().find(out->id)->shape), 1400.0 * 2400 - 1000.0 * 2000, 1e-3);
  EXPECT_FALSE(m.setParameter(out->id, out->parameterKey, -600).ok());  // collapses

  // The frame is a normal profile: push/pull extrudes it upwards.
  const TopoDS_Face face = TopoDS::Face(m.document().worldShape(out->id));
  auto pp = m.pushPull(out->id, face, 300);
  ASSERT_TRUE(pp.ok()) << pp.error();
  test::expectBounds(m.document().worldShape(out->id), -200, -200, 0, 1200, 2200, 300);
  EXPECT_NEAR(geom::volume(m.document().worldShape(out->id)), (1400.0 * 2400 - 1000.0 * 2000) * 300, 1e-1);
}

// Request T2 / OFF-01: a 4000 x 3000 face offset 200 inwards becomes the
// frame around the new inner face; the frame pushed up 1000 is a frame
// (the centre is not filled), the inner face stays as it was.
TEST(Offset, InwardsTheOriginalBecomesTheFrame) {
  Modeler m;
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 4000, 3000);
  const std::size_t steps = m.history().undoCount();
  auto in = m.offset(*rect, -200);
  ASSERT_TRUE(in.ok()) << in.error();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_NEAR(geom::area(m.document().find(*rect)->shape), 2640000.0, 1e-3);
  EXPECT_NEAR(geom::area(m.document().find(in->id)->shape), 3600.0 * 2600, 1e-3);
  // The hole follows a new distance.
  ASSERT_TRUE(m.setParameter(in->id, in->parameterKey, -300).ok());
  EXPECT_NEAR(geom::area(m.document().find(*rect)->shape), 12e6 - 3400.0 * 2400, 1e-3);
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::area(m.document().find(*rect)->shape), 2640000.0, 1e-3);

  auto pp = m.pushPull(*rect, TopoDS::Face(m.document().worldShape(*rect)), 1000);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_NEAR(geom::volume(m.document().worldShape(*rect)), 2.64e9, 1.0);
  EXPECT_EQ(m.document().find(in->id)->kind(), ObjectKind::Face);
  EXPECT_NEAR(geom::area(m.document().find(in->id)->shape), 3600.0 * 2600, 1e-3);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(geom::area(m.document().find(*rect)->shape), 12e6, 1e-3);
  EXPECT_FALSE(m.document().find(in->id));
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
  const auto pos = text.find("\"version\": 3");
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, std::string("\"version\": 3").size(), "\"version\": 1");
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

TEST(GroupEditing, NewGeometryGoesIntoActiveGroupAndKeepsWorldPlacement) {
  Modeler m;
  const ObjectId wall = box(m, 0, 0, 0, 4000, 200, 3000, "Wall");
  auto g = m.group({wall}, "House");
  ASSERT_TRUE(m.move({*g}, gp_Vec(10000, 0, 0)).ok());
  ASSERT_TRUE(m.setActiveGroup(*g).ok());
  EXPECT_FALSE(m.setActiveGroup(wall).ok());  // only groups

  // Door outline drawn in world coordinates on the moved wall.
  auto door = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(11000, 0, 0), gp_Dir(0, -1, 0)),
                                900, 2100);
  ASSERT_TRUE(door.ok());
  EXPECT_EQ(m.document().find(*door)->parent, *g);
  test::expectBounds(m.document().worldShape(*door), 11000, 0, 0, 11900, 0, 2100);

  // Pushing it in cuts the wall and the result stays in the group.
  auto pp = m.pushPull(*door, TopoDS::Face(m.document().worldShape(*door)), -200);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(m.document().find(pp->id)->parent, *g);
  EXPECT_NEAR(worldVolume(m, pp->id), 4000.0 * 200 * 3000 - 900.0 * 2100 * 200, 1e-2);
  test::expectBounds(m.document().worldShape(pp->id), 10000, 0, 0, 14000, 200, 3000);

  // A Boolean between children keeps them in the group too.
  const ObjectId slab = box(m, 10000, 0, 3000, 4000, 1000, 200, "Slab");
  EXPECT_EQ(m.document().find(slab)->parent, *g);
  auto u = m.boolean(BooleanKind::Union, {pp->id, slab});
  ASSERT_TRUE(u.ok()) << u.error();
  EXPECT_EQ(m.document().find(*u)->parent, *g);
  test::expectBounds(m.document().worldShape(*u), 10000, 0, 0, 14000, 1000, 3200);

  ASSERT_TRUE(m.setActiveGroup({}).ok());
  auto outside = m.createRectangle(geom::SketchPlane::ground(), 10, 10);
  EXPECT_TRUE(m.document().find(*outside)->parent.empty());
}

// Request T2 / OFF-02: inwards on a face of a solid the outline is drawn
// onto the face, dividing it into frame and inner face of the same solid;
// each is pushed / pulled on its own; the distance stays editable.
TEST(Offset, FaceOfSolidThenPushInMakesRecess) {
  Modeler m;
  const ObjectId block = box(m, 0, 0, 0, 1000, 1000, 1000, "Block");
  const TopoDS_Face top = faceWithNormal(m.document().worldShape(block), gp::DZ());
  auto inset = m.offsetSolidFace(block, top, -100);
  ASSERT_TRUE(inset.ok()) << inset.error();
  EXPECT_EQ(*inset, block);
  EXPECT_EQ(m.document().objects().size(), 1u);
  const Object& b = *m.document().find(block);
  EXPECT_EQ(b.kind(), ObjectKind::Solid);
  int faces = 0;
  TopoDS_Face inner, frame;
  for (TopExp_Explorer e(m.document().worldShape(b), TopAbs_FACE); e.More(); e.Next()) {
    ++faces;
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const auto n = geom::planarFaceNormal(f);
    if (!n || n->Z() < 0.99) continue;
    (std::abs(geom::area(f) - 800.0 * 800) < 1e-3 ? inner : frame) = f;
  }
  EXPECT_EQ(faces, 7);
  ASSERT_FALSE(inner.IsNull());
  ASSERT_FALSE(frame.IsNull());
  EXPECT_NEAR(geom::area(frame), 1e6 - 800.0 * 800, 1e-3);
  EXPECT_NEAR(geom::volume(m.document().worldShape(b)), 1e9, 1e-2);
  // Pushing the inner face down cuts a recess (one object).
  auto pp = m.pushPull(block, inner, -300);
  ASSERT_TRUE(pp.ok()) << pp.error();
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_NEAR(geom::volume(m.document().worldShape(block)), 1e9 - 800.0 * 800 * 300, 1e-2);
  // The distance is a parameter of the block.
  ASSERT_TRUE(m.setParameter(block, "op1.distance", -200).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(block)), 1e9 - 600.0 * 600 * 300, 1e-2);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  // The frame pulled up instead: a rim.
  auto rim = m.pushPull(block, frame, 500);
  ASSERT_TRUE(rim.ok()) << rim.error();
  EXPECT_NEAR(geom::volume(m.document().worldShape(block)), 1e9 + (1e6 - 800.0 * 800) * 500, 1e-2);
  EXPECT_FALSE(m.offsetSolidFace(block, top, -600).ok());  // collapses
}

TEST(Paint, GroupsPaintTheirChildrenInOneStep) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 0, 10, 10, 10);
  const ObjectId b = box(m, 20, 0, 0, 10, 10, 10);
  auto g = m.group({a, b});
  const Color red{0.9, 0.1, 0.1};
  ASSERT_TRUE(m.paint({*g}, red).ok());
  // The group's material colours its children (they have none of their own).
  EXPECT_EQ(m.document().displayColor(*m.document().find(a)), red);
  EXPECT_EQ(m.document().displayColor(*m.document().find(b)), red);
  EXPECT_EQ(m.history().undoName(), "Paint");
  ASSERT_TRUE(m.undo());
  EXPECT_FALSE(m.document().displayColor(*m.document().find(a)) == red);
  EXPECT_TRUE(m.document().materials().empty());  // the colour's material came with the step
}

// ---- Annotations (dimensions, text) ---------------------------------------------------


TEST(Annotations, DimensionGeometryTextAndFile) {
  doc::Modeler m;
  auto id = m.createDimension(gp_Pnt(0, 0, 0), gp_Pnt(3600, 0, 0), gp_Vec(0, -600, 0));
  ASSERT_TRUE(id.ok()) << id.error();
  const doc::Object* o = m.document().find(*id);
  EXPECT_EQ(o->kind(), doc::ObjectKind::Annotation);
  EXPECT_EQ(o->layer, "Annotations");
  auto label = doc::annotationLabel(o->recipe);
  ASSERT_TRUE(label);
  EXPECT_EQ(label->text, "3,600");
  EXPECT_NEAR(label->position.Y(), -600, 1e-9);
  // The dimension line sits at the offset (bounds reach y = -600 and beyond with ticks).
  const Bnd_Box b = geom::exactBounds(o->shape);
  EXPECT_LT(b.CornerMin().Y(), -599.0);
  EXPECT_GT(b.CornerMax().X(), 3600.0);

  ASSERT_TRUE(m.setAnnotationText(*id, "W 3,600").ok());
  EXPECT_EQ(doc::annotationLabel(m.document().find(*id)->recipe)->text, "W 3,600");
  ASSERT_TRUE(m.setAnnotationText(*id, "").ok());  // back to the measured value
  EXPECT_EQ(doc::annotationLabel(m.document().find(*id)->recipe)->text, "3,600");
  EXPECT_FALSE(m.createDimension(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0), gp_Vec()).ok());

  auto note = m.createText(gp_Pnt(500, 500, 0), gp_Pnt(100, 100, 0), "居間 12.00 m²");
  ASSERT_TRUE(note.ok());
  EXPECT_FALSE(m.createText(gp_Pnt(), std::nullopt, "").ok());
  EXPECT_FALSE(m.setAnnotationText(*note, "").ok());

  // Saved and reloaded with UTF-8 text; moving keeps the measured length.
  const std::string text = doc::serializeDocument(m.document());
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(text, d).ok());
  EXPECT_EQ(doc::annotationLabel(d.find(*note)->recipe)->text, "居間 12.00 m²");
  ASSERT_TRUE(std::get_if<doc::TextNote>(&d.find(*note)->recipe.base)->target.has_value());
  ASSERT_TRUE(m.move({*id}, gp_Vec(1000, 0, 0)).ok());
  EXPECT_EQ(doc::annotationLabel(m.document().find(*id)->recipe)->text, "3,600");
}

TEST(Annotations, NotExportedToStep) {
  doc::Modeler m;
  geom::SketchPlane plane = geom::SketchPlane::ground();
  auto box = m.createRectangle(plane, 1000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1000).ok());
  ASSERT_TRUE(m.createDimension(gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Vec(0, -300, 0)).ok());
  const std::string path = ::testing::TempDir() + "annotations.step";
  ASSERT_TRUE(m.exportStep(path).ok());
  auto parts = geom::readStepParts(path);
  ASSERT_TRUE(parts.ok());
  EXPECT_EQ(parts->size(), 1u);
  // Only annotations: nothing to export.
  doc::Modeler only;
  ASSERT_TRUE(only.createText(gp_Pnt(), std::nullopt, "note").ok());
  EXPECT_FALSE(only.exportStep(path).ok());
}

// ---- Splitting faces with a line ----------------------------------------------------

namespace {

TopoDS_Face faceWithCentroid(const TopoDS_Shape& shape, const gp_Dir& normal, const gp_Pnt& near) {
  TopoDS_Face best;
  double bestDistance = 1e300;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const auto ref = geom::FaceRef::fromFace(f);
    if (!ref || ref->normal.Angle(normal) > 1e-6) continue;
    if (ref->centroid.Distance(near) < bestDistance) {
      bestDistance = ref->centroid.Distance(near);
      best = f;
    }
  }
  return best;
}

int faceCount(const TopoDS_Shape& shape) {
  int n = 0;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

}  // namespace

TEST(SplitFace, LineAcrossWallThenPushUpperPart) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  const double v0 = 4000.0 * 200 * 3000;

  // A horizontal line across the front (-Y) face at 1 m.
  auto split = m.splitWithLine({gp_Pnt(0, 0, 1000), gp_Pnt(4000, 0, 1000)});
  ASSERT_TRUE(split.ok()) << split.error();
  EXPECT_EQ(*split, *wall);
  const TopoDS_Shape s1 = m.document().worldShape(*wall);
  EXPECT_EQ(faceCount(s1), 7);
  EXPECT_NEAR(geom::volume(s1), v0, 1e-3);
  EXPECT_EQ(m.history().undoName(), "Split Face");

  // Push the upper part out by 300 (the lower part stays).
  const TopoDS_Face upper = faceWithCentroid(s1, -gp::DY(), gp_Pnt(2000, 0, 2000));
  ASSERT_FALSE(upper.IsNull());
  EXPECT_NEAR(geom::area(upper), 4000.0 * 2000, 1e-3);
  ASSERT_TRUE(m.pushPull(*wall, upper, 300).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*wall)), v0 + 4000.0 * 2000 * 300, 1e-2);

  // Parametric: raising the wall keeps the split at 1 m and the pushed part.
  ASSERT_TRUE(m.setParameter(*wall, "op0.distance", 3500).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*wall)), 4000.0 * 200 * 3500 + 4000.0 * 2500 * 300, 1e-2);

  // Saved and reloaded.
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(geom::volume(d.worldShape(*d.find(*wall))), 4000.0 * 200 * 3500 + 4000.0 * 2500 * 300, 1e-2);

  // Undo back to the unsplit wall.
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(faceCount(m.document().worldShape(*wall)), 6);
}

// Review §5: a face is found again by its plane plus a point inside it,
// not by the nearest centroid. Widening the wall moves the right part's
// centroid far away; the nearest one is then the left part's, and
// regenerating used to push the wrong part (silently "valid").
TEST(SplitFace, PushedPartStaysTheSameAfterResizing) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(600, 0, 0), gp_Pnt(600, 0, 3000)}).ok());
  const TopoDS_Face right = faceWithCentroid(m.document().worldShape(*wall), -gp::DY(), gp_Pnt(2300, 0, 1500));
  ASSERT_FALSE(right.IsNull());
  ASSERT_TRUE(m.pushPull(*wall, right, 300).ok());
  ASSERT_TRUE(m.setParameter(*wall, "width", 10000).ok());
  EXPECT_NEAR(worldVolume(m, *wall), 10000.0 * 200 * 3000 + 9400.0 * 3000 * 300, 1e-2);
}

// Narrowing the wall past the remembered point: the right part still
// overlaps where it was (the left one only touches it) — still the right.
TEST(SplitFace, ShrunkPartIsStillFound) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 3000)}).ok());
  const TopoDS_Face right = faceWithCentroid(m.document().worldShape(*wall), -gp::DY(), gp_Pnt(3000, 0, 1500));
  ASSERT_FALSE(right.IsNull());
  ASSERT_TRUE(m.pushPull(*wall, right, 300).ok());
  ASSERT_TRUE(m.setParameter(*wall, "width", 2500).ok());
  EXPECT_NEAR(worldVolume(m, *wall), 2500.0 * 200 * 3000 + 500.0 * 3000 * 300, 1e-2);
}

// The pushed top face is cut in two by a tunnel raised through it, with
// the remembered point in the gap: either half could be meant, so the
// edit is refused (pick again) instead of one being guessed.
TEST(SplitFace, AmbiguousFaceIsRefusedNotGuessed) {
  doc::Modeler m;
  auto block = m.createRectangle(geom::SketchPlane::ground(), 3000, 1000);
  ASSERT_TRUE(m.extrude(*block, 1000).ok());
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(1400, -100, 300);  // through the side; the bottom keeps it one solid
  auto tunnel = m.createRectangle(plane, 200, 1200);
  ASSERT_TRUE(m.extrude(*tunnel, 500).ok());
  auto host = m.boolean(doc::BooleanKind::Subtract, {*block, *tunnel});
  ASSERT_TRUE(host.ok()) << host.error();
  const TopoDS_Face top = faceWithCentroid(m.document().worldShape(*host), gp::DZ(), gp_Pnt(1500, 500, 1000));
  ASSERT_FALSE(top.IsNull());
  ASSERT_TRUE(m.pushPull(*host, top, 500).ok());
  const double before = worldVolume(m, *host);
  std::string key;
  for (const doc::Parameter& p : m.parameters(*host)) {
    if (p.key.rfind("in", 0) == 0 && p.key.find("distance") != std::string::npos && std::abs(p.value - 500) < 1e-9) key = p.key;
  }
  ASSERT_FALSE(key.empty());
  const Status s = m.setParameter(*host, key, 1000);  // now out through the top
  EXPECT_FALSE(s.ok());
  EXPECT_NE(s.message().find("ambiguous"), std::string::npos) << s.message();
  EXPECT_NEAR(worldVolume(m, *host), before, 1e-6);  // unchanged
}

// References keep their identity through save / load.
TEST(SplitFace, IdentitySurvivesSaveAndLoad) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(600, 0, 0), gp_Pnt(600, 0, 3000)}).ok());
  ASSERT_TRUE(m.pushPull(*wall, faceWithCentroid(m.document().worldShape(*wall), -gp::DY(), gp_Pnt(2300, 0, 1500)), 300).ok());
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  const auto* push = std::get_if<doc::PushPullOp>(&d.find(*wall)->recipe.modifiers.back());
  ASSERT_TRUE(push);
  ASSERT_TRUE(push->face.inside && push->face.extent);
  EXPECT_NEAR(push->face.extent->first.X(), 600, 1e-3);
  const std::string path = test::tempPath("identity.sbr");
  ASSERT_TRUE(m.save(path).ok());
  doc::Modeler reloaded;
  ASSERT_TRUE(reloaded.load(path).ok());
  ASSERT_TRUE(reloaded.setParameter(*wall, "width", 10000).ok());
  EXPECT_NEAR(worldVolume(reloaded, *wall), 10000.0 * 200 * 3000 + 9400.0 * 3000 * 300, 1e-2);
}

namespace {
// The edge of `shape` whose middle is closest to `p`.
TopoDS_Edge edgeNear(const TopoDS_Shape& shape, const gp_Pnt& p) {
  TopoDS_Edge best;
  double bestD = 1e300;
  for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
    const gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    if (mid.Distance(p) < bestD) {
      bestD = mid.Distance(p);
      best = TopoDS::Edge(e.Current());
    }
  }
  return best;
}
}  // namespace

// The eraser on a dividing line merges the face again; on a real corner it
// refuses (the shape never changes).
TEST(SplitFace, ErasingTheLineMergesTheFaceAgain) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  const double v0 = worldVolume(m, *wall);
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(0, 0, 1000), gp_Pnt(4000, 0, 1000)}).ok());
  ASSERT_EQ(faceCount(m.document().worldShape(*wall)), 7);
  auto erased = m.eraseEdge(*wall, edgeNear(m.document().worldShape(*wall), gp_Pnt(2000, 0, 1000)));
  ASSERT_TRUE(erased) << erased.error();
  EXPECT_EQ(*erased, *wall);
  EXPECT_EQ(faceCount(m.document().worldShape(*wall)), 6);
  EXPECT_NEAR(worldVolume(m, *wall), v0, 1e-3);
  EXPECT_EQ(m.history().undoName(), "Erase Line");
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(faceCount(m.document().worldShape(*wall)), 7);

  // A real corner (top edge) is refused, unchanged.
  const std::size_t steps = m.history().undoCount();
  EXPECT_FALSE(m.eraseEdge(*wall, edgeNear(m.document().worldShape(*wall), gp_Pnt(2000, 0, 3000))).ok());
  // After pushing the upper part out, the line is a step, not a division.
  const TopoDS_Face upper = faceWithCentroid(m.document().worldShape(*wall), -gp::DY(), gp_Pnt(2000, 0, 2000));
  ASSERT_TRUE(m.pushPull(*wall, upper, 300).ok());
  EXPECT_FALSE(m.eraseEdge(*wall, edgeNear(m.document().worldShape(*wall), gp_Pnt(2000, 0, 1000))).ok());
  EXPECT_EQ(m.history().undoCount(), steps + 1);  // only the push
}

TEST(SplitFace, ErasingTheLineOnAProfileJoinsItsParts) {
  doc::Modeler m;
  auto profile = m.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  ASSERT_TRUE(profile);
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(800, 0, 0), gp_Pnt(800, 1000, 0)}).ok());
  ASSERT_EQ(m.document().objects().size(), 2u);
  const doc::ObjectId part = m.document().objects().front().id;
  auto merged = m.eraseEdge(part, edgeNear(m.document().worldShape(part), gp_Pnt(800, 500, 0)));
  ASSERT_TRUE(merged) << merged.error();
  ASSERT_EQ(m.document().objects().size(), 1u);
  EXPECT_NEAR(geom::area(m.document().worldShape(*merged)), 2000.0 * 1000, 1e-3);
  // Still the rectangle: its width parameter edits it.
  ASSERT_TRUE(m.setParameter(*merged, "width", 3000).ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(*merged)), 3000.0 * 1000, 1e-3);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), 2u);
}

// Deleting a face leaves an open shape that work goes on with; filling the
// hole makes it a solid again.
TEST(OpenShells, DeleteFaceKeepWorkingAndFillAgain) {
  doc::Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1500).ok());
  const double v0 = worldVolume(m, *box);
  const TopoDS_Face top = faceWithCentroid(m.document().worldShape(*box), gp::DZ(), gp_Pnt(1000, 500, 1500));
  ASSERT_TRUE(m.removeFaces(*box, {top}).ok());
  const doc::Object* o = m.document().find(*box);
  EXPECT_EQ(o->kind(), doc::ObjectKind::Shell);
  EXPECT_EQ(faceCount(o->shape), 5);
  const geom::ValidationReport r = geom::validateShape(o->shape, false);
  EXPECT_TRUE(r.drawingValid) << r.summary();
  EXPECT_FALSE(r.solidValid);
  EXPECT_EQ(r.freeEdges, 4);
  EXPECT_EQ(m.history().undoName(), "Delete Face");

  // Still workable: paint a face, change the height (the deleted top stays
  // deleted), save and reload.
  const std::string wood = *m.createMaterial("木", {0.7, 0.5, 0.3});
  ASSERT_TRUE(m.paintFaces(*box, {faceWithCentroid(o->shape, -gp::DY(), gp_Pnt(1000, 0, 750))}, wood).ok());
  ASSERT_TRUE(m.setParameter(*box, "op0.distance", 2500).ok());
  EXPECT_EQ(faceCount(m.document().find(*box)->shape), 5);
  EXPECT_EQ(doc::resolveFaceMaterials(*m.document().find(*box)).faces.size(), 1u);
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_EQ(d.find(*box)->kind(), doc::ObjectKind::Shell);

  // Solid-only operations say so instead of failing oddly.
  EXPECT_FALSE(m.pushPull(*box, faceWithCentroid(m.document().worldShape(*box), -gp::DY(), gp_Pnt(1000, 0, 1250)),
                          100).ok());
  const doc::ObjectId other = *m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  ASSERT_TRUE(m.extrude(other, 100).ok());
  EXPECT_FALSE(m.boolean(doc::BooleanKind::Union, {*box, other}).ok());

  // Front / back: open shapes can be turned over (twice = as before).
  const TopoDS_Shape before = m.document().find(*box)->shape;
  ASSERT_TRUE(m.reverseFaces({*box}).ok());
  EXPECT_NE(m.document().find(*box)->shape.Orientation(), before.Orientation());
  ASSERT_TRUE(m.reverseFaces({*box}).ok());
  EXPECT_EQ(m.document().find(*box)->shape.Orientation(), before.Orientation());
  EXPECT_FALSE(std::holds_alternative<doc::ReverseOp>(m.document().find(*box)->recipe.modifiers.back()));
  EXPECT_FALSE(m.reverseFaces({other}).ok());  // solids always face out

  // Fill the hole: a solid again, and push/pull works.
  ASSERT_TRUE(m.fillHoles({*box}).ok());
  EXPECT_EQ(m.document().find(*box)->kind(), doc::ObjectKind::Solid);
  EXPECT_NEAR(worldVolume(m, *box), 2000.0 * 1000 * 2500, 1e-3);
  EXPECT_TRUE(geom::validateShape(m.document().find(*box)->shape, true).solidValid);
  ASSERT_TRUE(m.pushPull(*box, faceWithCentroid(m.document().worldShape(*box), gp::DZ(), gp_Pnt(1000, 500, 2500)),
                         500).ok());
  EXPECT_NEAR(worldVolume(m, *box), 2000.0 * 1000 * 3000, 1e-2);
  (void)v0;
  // Undo walks back through it all.
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().find(*box)->kind(), doc::ObjectKind::Shell);
}

TEST(SplitFace, RejectsLinesThatDoNotDivideAFace) {
  doc::Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*box, 1000).ok());
  // Ends inside the face (not on its boundary).
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(200, 200, 1000), gp_Pnt(800, 800, 1000)}).ok());
  // Not on any face.
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(0, 0, 5000), gp_Pnt(1000, 0, 5000)}).ok());
  // Along an existing edge: does not divide.
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000)}).ok());
  EXPECT_EQ(m.history().undoCount(), 2u);
  // An L-shaped polyline from one edge to an adjacent one (corner cut) works.
  auto ok = m.splitWithLine({gp_Pnt(300, 0, 1000), gp_Pnt(300, 300, 1000), gp_Pnt(0, 300, 1000)});
  ASSERT_TRUE(ok.ok()) << ok.error();
  EXPECT_EQ(faceCount(m.document().worldShape(*box)), 7);
}

TEST(Clipboard, CopyPasteKeepsRecipesAndSurvivesDeletion) {
  Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 1000, 2000);
  ASSERT_TRUE(a.ok() && m.extrude(*a, 3000).ok());
  auto b = m.createRectangle(geom::SketchPlane::ground(), 500, 500);
  ASSERT_TRUE(b.ok() && m.extrude(*b, 500).ok());
  ASSERT_TRUE(m.move({*b}, gp_Vec(2000, 0, 0)).ok());
  auto g = m.group({*a, *b});
  ASSERT_TRUE(g.ok());
  ASSERT_TRUE(m.move({*g}, gp_Vec(0, 0, 100)).ok());

  auto clip = m.copyToClipboard({*g});
  ASSERT_TRUE(clip.ok());
  EXPECT_EQ(clip->objects.size(), 3u);
  EXPECT_NEAR(clip->anchor.Distance(gp_Pnt(0, 0, 100)), 0.0, 1e-6);
  ASSERT_TRUE(m.remove({*g}).ok());  // cut
  EXPECT_TRUE(m.document().objects().empty());

  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(5000, 0, -100));
  const std::size_t steps = m.history().undoCount();
  auto pasted = m.paste(*clip, delta);
  ASSERT_TRUE(pasted.ok()) << pasted.error();
  ASSERT_EQ(pasted->size(), 1u);
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_EQ(m.document().objects().size(), 3u);
  EXPECT_NE(pasted->front(), *g);  // fresh ids
  const auto children = m.document().children(pasted->front());
  ASSERT_EQ(children.size(), 2u);
  test::expectBounds(m.document().worldShape(children[0]), 5000, 0, 0, 6000, 2000, 3000, 1e-6);
  // Still parametric: re-edit the pasted height.
  ASSERT_TRUE(m.setParameter(children[0], "op0.distance", 2400).ok());
  test::expectBounds(m.document().worldShape(children[0]), 5000, 0, 0, 6000, 2000, 2400, 1e-6);

  // A second paste is independent of the first.
  auto again = m.paste(*clip);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(m.document().objects().size(), 6u);
  m.undo();
  EXPECT_EQ(m.document().objects().size(), 3u);
  EXPECT_FALSE(m.paste(Modeler::Clipboard{}).ok());
}

TEST(Clipboard, PasteGoesIntoTheEditedGroup) {
  Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(a.ok() && m.extrude(*a, 1000).ok());
  auto g = m.group({*a});
  ASSERT_TRUE(g.ok());
  ASSERT_TRUE(m.move({*g}, gp_Vec(10000, 0, 0)).ok());
  auto loose = m.createRectangle(geom::SketchPlane::ground(), 300, 300);
  ASSERT_TRUE(loose.ok());
  auto clip = m.copyToClipboard({*loose});
  ASSERT_TRUE(clip.ok());
  ASSERT_TRUE(m.setActiveGroup(*g).ok());
  auto pasted = m.paste(*clip);
  ASSERT_TRUE(pasted.ok());
  const Object* p = m.document().find(pasted->front());
  EXPECT_EQ(p->parent, *g);
  test::expectBounds(m.document().worldShape(*p), 0, 0, 0, 300, 300, 0, 1e-6);  // same world place
}

TEST(Scale, UniformScaleIsParametricAndExact) {
  Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 1000, 2000);
  ASSERT_TRUE(box.ok() && m.extrude(*box, 500).ok());
  ASSERT_TRUE(m.move({*box}, gp_Vec(1000, 0, 0)).ok());
  ASSERT_TRUE(m.scale({*box}, gp_Pnt(1000, 0, 0), 2.0).ok());
  test::expectBounds(m.document().worldShape(*box), 1000, 0, 0, 3000, 4000, 1000, 1e-6);
  // Twice in a row about the same point: one ScaleOp with the product.
  ASSERT_TRUE(m.scale({*box}, gp_Pnt(1000, 0, 0), 1.5).ok());
  const Object* o = m.document().find(*box);
  ASSERT_EQ(o->recipe.modifiers.size(), 2u);
  EXPECT_NEAR(std::get<ScaleOp>(o->recipe.modifiers.back()).factor, 3.0, 1e-12);
  // Planes stay planes: push/pull still works afterwards.
  for (TopExp_Explorer e(o->shape, TopAbs_FACE); e.More(); e.Next()) {
    EXPECT_EQ(BRepAdaptor_Surface(TopoDS::Face(e.Current())).GetType(), GeomAbs_Plane);
  }
  ASSERT_TRUE(m.setParameter(*box, "op1.scale", 0.5).ok());
  test::expectBounds(m.document().worldShape(*box), 1000, 0, 0, 1500, 1000, 250, 1e-6);
  ASSERT_TRUE(m.setParameter(*box, "op0.distance", 1000).ok());  // the original height, scaled
  test::expectBounds(m.document().worldShape(*box), 1000, 0, 0, 1500, 1000, 500, 1e-6);
  EXPECT_FALSE(m.setParameter(*box, "op1.scale", 0).ok());
  EXPECT_FALSE(m.scale({*box}, gp_Pnt(), -1).ok());
  m.undo();
  m.undo();
  m.undo();
  m.undo();
  test::expectBounds(m.document().worldShape(*box), 1000, 0, 0, 2000, 2000, 500, 1e-6);
}

TEST(Scale, GroupsGuidesAndComponents) {
  Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(a.ok() && m.extrude(*a, 1000).ok());
  auto b = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(b.ok() && m.extrude(*b, 1000).ok());
  ASSERT_TRUE(m.move({*b}, gp_Vec(2000, 0, 0)).ok());
  auto guide = m.createGuide(gp_Pnt(3000, 0, 0), gp::DY());
  ASSERT_TRUE(guide.ok());
  auto g = m.group({*a, *b, *guide});
  ASSERT_TRUE(g.ok());
  ASSERT_TRUE(m.move({*g}, gp_Vec(0, 0, 100)).ok());
  const std::size_t steps = m.history().undoCount();
  ASSERT_TRUE(m.scale({*g}, gp_Pnt(0, 0, 100), 2.0).ok());
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  test::expectBounds(m.document().worldShape(*a), 0, 0, 100, 2000, 2000, 2100, 1e-6);
  test::expectBounds(m.document().worldShape(*b), 4000, 0, 100, 6000, 2000, 2100, 1e-6);
  const auto& line = std::get<GuideLine>(m.document().find(*guide)->recipe.base);
  EXPECT_NEAR(line.point.X(), 6000, 1e-6);

  // Saved and loaded with the scale.
  const std::string path = test::tempPath("scaled.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path).ok());
  test::expectBounds(loaded.document().worldShape(*b), 4000, 0, 100, 6000, 2000, 2100, 1e-6);

  auto component = m.makeComponent({*g});
  ASSERT_TRUE(component.ok()) << component.error();
  EXPECT_FALSE(m.scale({*component}, gp_Pnt(), 2.0).ok());
}
