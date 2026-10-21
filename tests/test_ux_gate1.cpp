// Spec 07 (docs/07_ux_principles.md) Gate 1 acceptance tests: draw -> face -> solid.
// Test names carry the acceptance criterion ids (AC-xx) and requirement ids (F-xx).

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
