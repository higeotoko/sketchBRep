// Spec 07 (docs/07_ux_principles.md) Gate 1 acceptance tests: draw -> face -> solid.
// Test names carry the acceptance criterion ids (AC-xx) and requirement ids (F-xx).

#include <cmath>

#include <gtest/gtest.h>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

using namespace sbr;

namespace {

int faceCount(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

}  // namespace

TEST(Gate1, AC03_SeparateLinesCloseIntoFace_OneUndo) {
  doc::Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1000, 0)}).ok());
  const std::size_t before = m.history().undoCount();
  // Drawing the closing line and the face are one intent (F-08).
  m.history().beginGroup("Line");
  auto line = m.createPolyline({gp_Pnt(2000, 1000, 0), gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0)});
  ASSERT_TRUE(line.ok());
  auto face = m.faceFromLoop(*line);
  m.history().endGroup();
  ASSERT_TRUE(face.ok()) << face.error();
  EXPECT_EQ(m.history().undoCount(), before + 1);
  EXPECT_EQ(m.document().find(*face)->kind(), doc::ObjectKind::Face);
  EXPECT_NEAR(geom::area(m.document().worldShape(*face)), 2000.0 * 1000, 1e-6);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), 1u);  // back to before the closing line
}

TEST(Gate1, AC05_GapLargerThanToleranceStaysOpen) {
  doc::Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0)}).ok());
  // Ends 0.01 mm short of the first line's start (tolerance is 0.001 mm).
  auto line = m.createPolyline({gp_Pnt(1000, 1000, 0), gp_Pnt(0, 1000, 0), gp_Pnt(0, 0.01, 0)});
  ASSERT_TRUE(line.ok());
  bool loopFound = true;
  EXPECT_FALSE(m.faceFromLoop(*line, &loopFound).ok());
  EXPECT_FALSE(loopFound);
  EXPECT_EQ(m.document().objects().size(), 2u);
}

TEST(Gate1, AC06_SelfIntersectingLoopIsRejectedAndKept) {
  doc::Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 1000, 0), gp_Pnt(1000, 0, 0)}).ok());
  auto line = m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0)});
  ASSERT_TRUE(line.ok());
  const std::size_t steps = m.history().undoCount();
  bool loopFound = false;
  auto face = m.faceFromLoop(*line, &loopFound);
  EXPECT_FALSE(face.ok());
  EXPECT_TRUE(loopFound);
  EXPECT_NE(face.error().find("crosses itself"), std::string::npos) << face.error();
  EXPECT_EQ(m.document().objects().size(), 2u);
  EXPECT_EQ(m.history().undoCount(), steps);
}

TEST(Gate1, F06_NonPlanarLoopIsNotFilled) {
  doc::Modeler m;
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 500)}).ok());
  auto line = m.createPolyline({gp_Pnt(1000, 1000, 500), gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0)});
  bool loopFound = false;
  auto face = m.faceFromLoop(*line, &loopFound);
  EXPECT_FALSE(face.ok());
  EXPECT_TRUE(loopFound);
  EXPECT_NE(face.error().find("not planar"), std::string::npos) << face.error();
}

TEST(Gate1, AC07_LineSplitsProfileFaceIntoParametricParts) {
  doc::Modeler m;
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  ASSERT_TRUE(rect.ok());
  ASSERT_TRUE(m.splitTarget({gp_Pnt(800, 0, 0), gp_Pnt(800, 1000, 0)}).has_value());
  const std::size_t steps = m.history().undoCount();
  auto split = m.splitWithLine({gp_Pnt(800, 0, 0), gp_Pnt(800, 1000, 0)});
  ASSERT_TRUE(split.ok()) << split.error();
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  ASSERT_EQ(m.document().objects().size(), 2u);
  double total = 0;
  for (const doc::Object& o : m.document().objects()) {
    EXPECT_EQ(o.kind(), doc::ObjectKind::Face);
    total += geom::area(m.document().worldShape(o));
  }
  EXPECT_NEAR(total, 2000.0 * 1000, 1e-3);

  // Each part keeps the original rectangle's parameters and can be extruded.
  const doc::ObjectId part = m.document().objects().front().id;
  const double a0 = geom::area(m.document().worldShape(part));
  ASSERT_TRUE(m.setParameter(part, "in0.height", 1500).ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(part)), a0 * 1.5, 1e-3);
  const TopoDS_Face face = TopoDS::Face(m.document().find(part)->shape);
  ASSERT_TRUE(m.pushPull(part, face, 300).ok());

  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(geom::volume(d.worldShape(*d.find(part))), a0 * 1.5 * 300, 1e-2);

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());  // the split: the original rectangle is back
  ASSERT_EQ(m.document().objects().size(), 1u);
  EXPECT_EQ(m.document().objects().front().id, *rect);
}

TEST(Gate1, AC07_LinesThatDoNotDivideAreRejected) {
  doc::Modeler m;
  ASSERT_TRUE(m.createRectangle(geom::SketchPlane::ground(), 2000, 1000).ok());
  // Stops halfway, runs along an edge, or leaves the face.
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(800, 0, 0), gp_Pnt(800, 500, 0)}).ok());
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0)}).ok());
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(800, 0, 0), gp_Pnt(800, 1500, 0)}).ok());
  EXPECT_EQ(m.document().objects().size(), 1u);
}

TEST(Gate1, AC08_ClosedOutlineOnSolidFaceLeavesSolidUnchanged) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  const double v0 = geom::volume(m.document().worldShape(*wall));
  const int f0 = faceCount(m.document().worldShape(*wall));
  // A window outline drawn on the wall's -Y face: a separate profile.
  auto plane = geom::SketchPlane::fromNormal(gp_Pnt(1000, 0, 900), -gp::DY());
  auto window = m.createRectangle(plane, 1200, 1000);
  ASSERT_TRUE(window.ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*wall)), v0, 1e-6);
  EXPECT_EQ(faceCount(m.document().worldShape(*wall)), f0);
}

TEST(Gate1, AC09_LoopsDoNotCloseAcrossEditingContexts) {
  doc::Modeler m;
  auto outside = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0)});
  ASSERT_TRUE(outside.ok());
  auto anchor = m.createRectangle(geom::SketchPlane::ground(), 10, 10);
  auto group = m.group({*anchor});
  ASSERT_TRUE(group.ok());
  ASSERT_TRUE(m.setActiveGroup(*group).ok());
  // Inside the group, a line that would close the outside line's loop.
  auto inside = m.createPolyline({gp_Pnt(1000, 1000, 0), gp_Pnt(0, 1000, 0), gp_Pnt(0, 0, 0)});
  ASSERT_TRUE(inside.ok());
  EXPECT_EQ(m.document().find(*inside)->parent, *group);
  bool loopFound = true;
  EXPECT_FALSE(m.faceFromLoop(*inside, &loopFound).ok());
  EXPECT_FALSE(loopFound);
  // Nor does a line in the group split a profile outside it.
  ASSERT_TRUE(m.setActiveGroup({}).ok());
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  ASSERT_TRUE(m.setActiveGroup(*group).ok());
  EXPECT_FALSE(m.splitWithLine({gp_Pnt(800, 0, 0), gp_Pnt(800, 1000, 0)}).ok());
  EXPECT_TRUE(m.document().find(*rect));
}

// ---- Gate 3: F-05 faces with holes ---------------------------------------------------

TEST(Gate3, F05_ProfileInsideProfileCutsAHole) {
  doc::Modeler m;
  auto floor = m.createRectangle(geom::SketchPlane::ground(), 4000, 3000);
  ASSERT_TRUE(floor.ok());
  geom::SketchPlane at = geom::SketchPlane::ground();
  at.origin = gp_Pnt(1000, 1000, 0);
  const std::size_t steps = m.history().undoCount();
  auto opening = m.createRectangle(at, 1000, 800);  // e.g. a stair opening
  ASSERT_TRUE(opening.ok());
  EXPECT_EQ(m.history().undoCount(), steps + 1);  // drawing + hole: one step
  EXPECT_NEAR(geom::area(m.document().worldShape(*floor)), 4000.0 * 3000 - 1000.0 * 800, 1e-3);
  EXPECT_NEAR(geom::area(m.document().worldShape(*opening)), 1000.0 * 800, 1e-3);

  // The slab keeps its hole when extruded and when its size changes.
  ASSERT_TRUE(m.extrude(*floor, 200).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*floor)), (12e6 - 0.8e6) * 200, 1.0);
  ASSERT_TRUE(m.setParameter(*floor, "width", 5000).ok());
  EXPECT_NEAR(geom::volume(m.document().worldShape(*floor)), (15e6 - 0.8e6) * 200, 1.0);

  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(geom::volume(d.worldShape(*d.find(*floor))), (15e6 - 0.8e6) * 200, 1.0);

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());  // the opening and its hole go together
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_NEAR(geom::area(m.document().worldShape(*floor)), 4000.0 * 3000, 1e-3);
}

TEST(Gate3, F05_TouchingOrOtherPlaneDoesNotCut) {
  doc::Modeler m;
  auto floor = m.createRectangle(geom::SketchPlane::ground(), 4000, 3000);
  // Shares the floor's corner: not strictly inside.
  ASSERT_TRUE(m.createRectangle(geom::SketchPlane::ground(), 1000, 1000).ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(*floor)), 12e6, 1e-3);
  // Inside in plan but 10 mm higher: a different plane.
  geom::SketchPlane up = geom::SketchPlane::ground();
  up.origin = gp_Pnt(2000, 1000, 10);
  ASSERT_TRUE(m.createCircle(up, 300).ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(*floor)), 12e6, 1e-3);
  // A circle strictly inside cuts a round hole.
  up.origin = gp_Pnt(3000, 1500, 0);
  ASSERT_TRUE(m.createCircle(up, 300).ok());
  EXPECT_NEAR(geom::area(m.document().worldShape(*floor)), 12e6 - M_PI * 300 * 300, 1e-2);
}

// ---- AC-22 / §6.2: editing a component in place -----------------------------------------

TEST(Gate3, AC22_EditComponentInPlaceUpdatesAllInstances) {
  doc::Modeler m;
  auto column = m.createRectangle(geom::SketchPlane::ground(), 400, 400);
  ASSERT_TRUE(m.extrude(*column, 3000).ok());
  auto first = m.makeComponent({*column}, "Column");
  ASSERT_TRUE(first.ok()) << first.error();
  gp_Trsf step;
  step.SetTranslation(gp_Vec(3000, 0, 0));
  ASSERT_TRUE(m.copy({*first}, step, 2).ok());
  ASSERT_EQ(m.document().objects().size(), 3u);
  const auto* inst = std::get_if<doc::ComponentInstance>(&m.document().find(*first)->recipe.base);
  ASSERT_TRUE(inst);
  const std::string definitionId = inst->definitionId;
  EXPECT_EQ(m.instanceCount(definitionId), 3);

  // Enter: the instance is replaced by an editable group in the same place.
  auto group = m.beginComponentEdit(*first);
  ASSERT_TRUE(group.ok()) << group.error();
  EXPECT_EQ(m.activeGroup(), *group);
  EXPECT_FALSE(m.document().find(*first));
  EXPECT_EQ(m.componentEditGroup(*group), *group);
  // Edit the column inside: make it taller.
  doc::ObjectId inner;
  for (const doc::Object& o : m.document().objects()) {
    if (o.parent == *group) inner = o.id;
  }
  ASSERT_FALSE(inner.empty());
  EXPECT_EQ(m.componentEditGroup(inner), *group);
  ASSERT_TRUE(m.setParameter(inner, "op0.distance", 3500).ok());

  // Leave: the definition and every instance update; the instance is back.
  ASSERT_TRUE(m.endComponentEdit(*group).ok());
  EXPECT_TRUE(m.activeGroup().empty());
  ASSERT_TRUE(m.document().find(*first));
  EXPECT_EQ(m.document().objects().size(), 3u);
  for (const doc::Object& o : m.document().objects()) {
    EXPECT_NEAR(geom::exactBounds(m.document().worldShape(o)).CornerMax().Z(), 3500, 1e-6) << o.name;
  }
  EXPECT_EQ(m.instanceCount(definitionId), 3);

  // Saved and reloaded with the new definition.
  doc::Document d;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), d).ok());
  EXPECT_NEAR(geom::exactBounds(d.worldShape(*d.find(*first))).CornerMax().Z(), 3500, 1e-6);

  // Undo: leaving, the edit, entering — each one step, back to 3000.
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.document().find(*first));
  for (const doc::Object& o : m.document().objects()) {
    EXPECT_NEAR(geom::exactBounds(m.document().worldShape(o)).CornerMax().Z(), 3000, 1e-6);
  }
}

TEST(Gate3, AC22_EmptiedComponentIsNotSaved) {
  doc::Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 100, 100);
  ASSERT_TRUE(m.extrude(*box, 100).ok());
  auto c = m.makeComponent({*box}, "Box");
  auto group = m.beginComponentEdit(*c);
  ASSERT_TRUE(group.ok());
  std::vector<doc::ObjectId> inside;
  for (const doc::Object& o : m.document().objects()) {
    if (o.parent == *group) inside.push_back(o.id);
  }
  ASSERT_TRUE(m.remove(inside).ok());
  EXPECT_FALSE(m.endComponentEdit(*group).ok());
  EXPECT_TRUE(m.document().find(*group));  // still editing; nothing lost
}

// §3.3: a split line stays a real edge after later, unrelated push/pulls.
TEST(Gate3, SplitLineSurvivesLaterPushPull) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(0, 0, 1000), gp_Pnt(4000, 0, 1000)}).ok());
  EXPECT_EQ(faceCount(m.document().worldShape(*wall)), 7);
  // Push the +X end face out: the front face grows but stays split.
  TopoDS_Face side;
  for (TopExp_Explorer e(m.document().worldShape(*wall), TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->X() > 0.99) side = TopoDS::Face(e.Current());
  }
  ASSERT_FALSE(side.IsNull());
  ASSERT_TRUE(m.pushPull(*wall, side, 500).ok());
  // The front stays divided at 1 m (the new 500 mm piece is its own face).
  int front = 0;
  for (TopExp_Explorer e(m.document().worldShape(*wall), TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    front += n && n->Y() < -0.99;
  }
  EXPECT_GE(front, 2);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*wall)), 4500.0 * 200 * 3000, 1e-2);
}
