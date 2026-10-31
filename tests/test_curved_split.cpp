// Lines and circles lying exactly on cylinders and cones divide those faces
// (docs/39): a straight line along the side, a circle round it (an edge
// copied down), each crossing a face from edge to edge or closed on it.
// Never projected and never a chord: curves stopping on the face, off it or
// through the inside divide nothing. The eraser joins the parts again;
// Undo, saving and push/pull of the top keep the division.

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Circ.hxx>

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>

#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

TopoDS_Shape world(const Modeler& m, const ObjectId& id) { return m.document().worldShape(*m.document().find(id)); }

int faces(const Modeler& m, const ObjectId& id) {
  int n = 0;
  for (TopExp_Explorer e(world(m, id), TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

int curvedFaces(const Modeler& m, const ObjectId& id) {
  int n = 0;
  for (TopExp_Explorer e(world(m, id), TopAbs_FACE); e.More(); e.Next()) n += geom::isCylinderOrCone(TopoDS::Face(e.Current()));
  return n;
}

int count(const Modeler& m, ObjectKind k) {
  int n = 0;
  for (const Object& o : m.document().objects()) n += o.kind() == k;
  return n;
}

double length(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::LinearProperties(s, props);
  return props.Mass();
}

double volumeOf(const Modeler& m, const ObjectId& id) { return geom::volume(world(m, id)); }

bool valid(const Modeler& m, const ObjectId& id) { return geom::validateShape(world(m, id), true).valid; }

// A cylinder of radius 500 and height 1000 standing on the ground at the
// origin (circle pushed up, as drawn).
ObjectId cylinder(Modeler& m) {
  auto disc = m.createCircle(geom::SketchPlane::ground(), 500);
  EXPECT_TRUE(disc);
  auto solid = m.extrude(*disc, 1000);
  EXPECT_TRUE(solid) << solid.error();
  return solid->id;
}

gp_Pnt round(double angle, double z, double r = 500) { return gp_Pnt(r * std::cos(angle), r * std::sin(angle), z); }

ObjectId line(Modeler& m, const gp_Pnt& a, const gp_Pnt& b) {
  auto id = m.createPolyline({a, b});
  EXPECT_TRUE(id);
  auto settled = m.settleOverlaps(std::vector<ObjectId>{*id});
  EXPECT_TRUE(settled) << settled.error();
  return *id;
}

ObjectId circleCurve(Modeler& m, double z, double r = 500) {
  auto id = m.importShape(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(0, 0, z), gp::DZ()), r)).Edge(), "Circle", "test");
  EXPECT_TRUE(id);
  auto settled = m.settleOverlaps(std::vector<ObjectId>{*id});
  EXPECT_TRUE(settled) << settled.error();
  return *id;
}

// The edge of the shape through p (world), for the eraser.
TopoDS_Edge edgeThrough(const Modeler& m, const ObjectId& id, const gp_Pnt& p) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(world(m, id), TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(p).Vertex(), edges(i));
    if (d.IsDone() && d.Value() < 1e-3) return TopoDS::Edge(edges(i));
  }
  return {};
}

}  // namespace

TEST(CurvedFaceSplit, LineAlongTheSideDividesTheCylinder) {
  Modeler m;
  const ObjectId c = cylinder(m);
  const double volume = volumeOf(m, c);
  ASSERT_EQ(faces(m, c), 3);
  line(m, round(M_PI, 0), round(M_PI, 1000));
  EXPECT_EQ(faces(m, c), 4);  // the seam and the line: two halves
  EXPECT_EQ(curvedFaces(m, c), 2);
  EXPECT_EQ(count(m, ObjectKind::Curve), 0);  // the line became the division
  EXPECT_NEAR(volumeOf(m, c), volume, 1e-6 * volume);
  EXPECT_TRUE(valid(m, c));
  line(m, round(M_PI / 2, 0), round(M_PI / 2, 1000));
  EXPECT_EQ(curvedFaces(m, c), 3);
  // The whole step is one undo.
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(curvedFaces(m, c), 2);
  ASSERT_TRUE(m.redo());
  EXPECT_EQ(curvedFaces(m, c), 3);
}

TEST(CurvedFaceSplit, LongerLineDividesTheSideAndTheRestStaysALine) {
  Modeler m;
  const ObjectId c = cylinder(m);
  line(m, round(M_PI, -200), round(M_PI, 1300));
  EXPECT_EQ(curvedFaces(m, c), 2);
  EXPECT_EQ(count(m, ObjectKind::Curve), 2);  // below and above it
  double length = 0;
  for (const Object& o : m.document().objects()) {
    if (o.kind() == ObjectKind::Curve) length += ::length(world(m, o.id));
  }
  EXPECT_NEAR(length, 500, 1e-6);
}

TEST(CurvedFaceSplit, CircleRoundTheSideDividesItAndSurvivesSaving) {
  Modeler m;
  const ObjectId c = cylinder(m);
  const double volume = volumeOf(m, c);
  circleCurve(m, 400);
  EXPECT_EQ(curvedFaces(m, c), 2);
  EXPECT_EQ(count(m, ObjectKind::Curve), 0);
  EXPECT_NEAR(volumeOf(m, c), volume, 1e-6 * volume);
  EXPECT_TRUE(valid(m, c));
  // Copying the top edge down (the drawing workflow) does the same.
  TopoDS_Edge top = edgeThrough(m, c, round(0.3, 1000));
  ASSERT_FALSE(top.IsNull());
  gp_Trsf down;
  down.SetTranslation(gp_Vec(0, 0, -200));
  auto copies = m.copyEdges({top}, down, 2);
  ASSERT_TRUE(copies) << copies.error();
  EXPECT_EQ(curvedFaces(m, c), 4);
  EXPECT_TRUE(valid(m, c));

  const std::string file = ::testing::TempDir() + "curved_split.sbr";
  ASSERT_TRUE(m.save(file));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(file));
  ObjectId again;
  for (const Object& o : loaded.document().objects()) {
    if (o.kind() == ObjectKind::Solid) again = o.id;
  }
  EXPECT_EQ(curvedFaces(loaded, again), 4);
  EXPECT_NEAR(volumeOf(loaded, again), volume, 1e-6 * volume);
}

TEST(CurvedFaceSplit, CurvesNotRunningAcrossDivideNothing) {
  Modeler m;
  const ObjectId c = cylinder(m);
  line(m, round(2.0, 100), round(2.0, 600));                              // stops on the face
  line(m, round(M_PI, 0, 520), round(M_PI, 1000, 520));                   // a little off it
  line(m, round(2.5, 500), round(4.0, 500));                               // a chord through the inside
  auto half = m.createArc(round(0, 300), round(M_PI, 300), gp::DZ(), 500, false);  // ends inside the face
  ASSERT_TRUE(half);
  ASSERT_TRUE(m.settleOverlaps(std::vector<ObjectId>{*half}));
  circleCurve(m, 400, 499);                                                // inside, not on it
  EXPECT_EQ(faces(m, c), 3);
  EXPECT_EQ(count(m, ObjectKind::Curve), 5);
}

TEST(CurvedFaceSplit, EraserJoinsThePartsAgain) {
  Modeler m;
  const ObjectId c = cylinder(m);
  const double volume = volumeOf(m, c);
  line(m, round(M_PI, 0), round(M_PI, 1000));
  circleCurve(m, 500);
  ASSERT_EQ(curvedFaces(m, c), 4);
  // The circle on one half only: the two quarters of that half join.
  const TopoDS_Edge arc = edgeThrough(m, c, round(M_PI / 2, 500));
  ASSERT_FALSE(arc.IsNull());
  auto erased = m.eraseEdge(c, arc);
  ASSERT_TRUE(erased) << erased.error();
  EXPECT_EQ(curvedFaces(m, c), 3);
  EXPECT_NEAR(volumeOf(m, c), volume, 1e-6 * volume);
  EXPECT_TRUE(valid(m, c));
  const TopoDS_Edge side = edgeThrough(m, c, round(M_PI, 200));
  ASSERT_FALSE(side.IsNull());
  auto joined = m.eraseEdge(c, side);
  ASSERT_TRUE(joined) << joined.error();
  EXPECT_EQ(curvedFaces(m, c), 2);
  EXPECT_TRUE(valid(m, c));
  // The seam of the round face is no line to erase.
  EXPECT_FALSE(m.eraseEdge(c, edgeThrough(m, c, round(0, 800))));
}

TEST(CurvedFaceSplit, DeletingTheSelectedLineJoinsThePartsToo) {
  Modeler m;
  const ObjectId c = cylinder(m);
  line(m, round(M_PI, 0), round(M_PI, 1000));
  ASSERT_EQ(curvedFaces(m, c), 2);
  DeleteTarget target;
  target.id = c;
  target.edges = {edgeThrough(m, c, round(M_PI, 500))};
  auto deleted = m.deleteSelection({target});
  ASSERT_TRUE(deleted) << deleted.error();
  EXPECT_EQ(deleted->merged, 1);
  EXPECT_EQ(curvedFaces(m, c), 1);
  EXPECT_TRUE(valid(m, c));
}

TEST(CurvedFaceSplit, PaintedSideKeepsItsMaterialOnBothParts) {
  Modeler m;
  const ObjectId c = cylinder(m);
  auto red = m.createMaterial("Red", Color{1, 0, 0});
  ASSERT_TRUE(red);
  TopoDS_Face side;
  for (TopExp_Explorer e(world(m, c), TopAbs_FACE); e.More(); e.Next()) {
    if (geom::isCylinderOrCone(TopoDS::Face(e.Current()))) side = TopoDS::Face(e.Current());
  }
  ASSERT_TRUE(m.paintFaces(c, {side}, *red).ok());
  ASSERT_EQ(resolveFaceMaterials(*m.document().find(c)).faces.size(), 1u);
  circleCurve(m, 500);
  const FaceMaterialMap painted = resolveFaceMaterials(*m.document().find(c));
  EXPECT_EQ(painted.faces.size(), 2u);
  for (const auto& [face, material] : painted.faces) EXPECT_EQ(material, *red);
  const std::string file = ::testing::TempDir() + "curved_split_paint.sbr";
  ASSERT_TRUE(m.save(file));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(file));
  EXPECT_EQ(resolveFaceMaterials(*loaded.document().find(c)).faces.size(), 2u);
}

TEST(CurvedFaceSplit, PushPullOfTheTopKeepsTheDivision) {
  Modeler m;
  const ObjectId c = cylinder(m);
  line(m, round(M_PI, 0), round(M_PI, 1000));
  circleCurve(m, 500);
  ASSERT_EQ(curvedFaces(m, c), 4);
  TopoDS_Face top;
  for (TopExp_Explorer e(world(m, c), TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->Z() > 0.5) top = TopoDS::Face(e.Current());
  }
  ASSERT_FALSE(top.IsNull());
  auto pulled = m.pushPull(c, top, 300);
  ASSERT_TRUE(pulled) << pulled.error();
  EXPECT_NEAR(volumeOf(m, c), M_PI * 500 * 500 * 1300, 1e-3 * M_PI * 500 * 500);
  // The four parts as drawn, and the new part of the side as a face of its
  // own (the old top edge stays a line, as SketchUp).
  EXPECT_EQ(curvedFaces(m, c), 5);
  // Regenerated (as opening the file) the same.
  Object again = *m.document().find(c);
  ASSERT_TRUE(rebuild(again, m.document().regenContext()));
  EXPECT_NEAR(geom::volume(again.shape), M_PI * 500 * 500 * 1300, 1e-3 * M_PI * 500 * 500);
  EXPECT_TRUE(valid(m, c));
}

TEST(CurvedFaceSplit, ConeSideIsDividedToo) {
  Modeler m;
  auto cone = m.importShape(BRepPrimAPI_MakeCone(500, 100, 1000).Shape(), "Cone", "test");
  ASSERT_TRUE(cone);
  const double volume = volumeOf(m, *cone);
  // A generator line from the bottom edge to the top edge.
  line(m, round(M_PI, 0, 500), round(M_PI, 1000, 100));
  EXPECT_EQ(curvedFaces(m, *cone), 2);
  // A circle round it halfway up (radius 300 there).
  circleCurve(m, 500, 300);
  EXPECT_EQ(curvedFaces(m, *cone), 4);
  EXPECT_NEAR(volumeOf(m, *cone), volume, 1e-6 * volume);
  EXPECT_TRUE(valid(m, *cone));
  const std::string file = ::testing::TempDir() + "curved_split_cone.sbr";
  ASSERT_TRUE(m.save(file));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(file));
  EXPECT_EQ(curvedFaces(loaded, *cone), 4);
  // One part pushed out: valid, larger, the rest where it was.
  TopoDS_Face part;
  for (TopExp_Explorer e(world(m, *cone), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    if (geom::isCylinderOrCone(f) && geom::SurfaceRef::fromFace(f)->point.Z() < 500) part = f;
  }
  ASSERT_FALSE(part.IsNull());
  auto out = m.pushPull(*cone, part, 50);
  ASSERT_TRUE(out) << out.error();
  EXPECT_GT(volumeOf(m, *cone), volume);
  EXPECT_TRUE(valid(m, *cone));
}

TEST(CurvedFaceSplit, ManyLinesRegenerateInOneGo) {
  Modeler m;
  const ObjectId c = cylinder(m);
  const auto steps = m.history().undoCount();
  m.history().beginGroup("Grid on a cylinder");
  const auto start = std::chrono::steady_clock::now();
  constexpr int kLines = 12;
  for (int i = 0; i < kLines; ++i) line(m, round(0.1 + 2 * M_PI * i / kLines, 0), round(0.1 + 2 * M_PI * i / kLines, 1000));
  for (int i = 1; i < 4; ++i) circleCurve(m, 250.0 * i);
  m.history().endGroup();
  const double drawn = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  EXPECT_EQ(curvedFaces(m, c), (kLines + 1) * 4);  // the seam is one more line
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  // Regenerating the whole recipe (as opening the file does).
  Object copy = *m.document().find(c);
  const auto again = std::chrono::steady_clock::now();
  ASSERT_TRUE(rebuild(copy, m.document().regenContext()));
  const double regenerated = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - again).count();
  int n = 0;
  for (TopExp_Explorer e(copy.shape, TopAbs_FACE); e.More(); e.Next()) n += geom::isCylinderOrCone(TopoDS::Face(e.Current()));
  EXPECT_EQ(n, (kLines + 1) * 4);
  std::cout << kLines << " lines + 3 circles on a cylinder: drawn " << drawn << " ms, regenerated " << regenerated
            << " ms\n";
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(faces(m, c), 3);
}
TEST(CurvedFaceSplit, PushPullOfOnePartMakesAStep) {
  Modeler m;
  const ObjectId c = cylinder(m);
  circleCurve(m, 500);
  auto lowerSide = [&] {
    for (TopExp_Explorer e(world(m, c), TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face f = TopoDS::Face(e.Current());
      if (geom::isCylinderOrCone(f) && geom::SurfaceRef::fromFace(f)->point.Z() < 500) return f;
    }
    return TopoDS_Face();
  };
  ASSERT_FALSE(lowerSide().IsNull());
  // Out by 100: the lower half gets radius 600, the upper one stays 500.
  auto out = m.pushPull(c, lowerSide(), 100);
  ASSERT_TRUE(out) << out.error();
  const double stepped = M_PI * (600.0 * 600 * 500 + 500.0 * 500 * 500);
  EXPECT_NEAR(volumeOf(m, c), stepped, 1e-6 * stepped);
  EXPECT_TRUE(valid(m, c));
  EXPECT_EQ(faces(m, c), 5);  // bottom, lower side, step, upper side, top
  // The distance stays editable, and the file keeps it.
  ASSERT_TRUE(m.setParameter(c, out->parameterKey, -100));
  const double inwards = M_PI * (400.0 * 400 * 500 + 500.0 * 500 * 500);
  EXPECT_NEAR(volumeOf(m, c), inwards, 1e-6 * inwards);
  const std::string file = ::testing::TempDir() + "curved_split_step.sbr";
  ASSERT_TRUE(m.save(file));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(file));
  EXPECT_NEAR(volumeOf(loaded, c), inwards, 1e-6 * inwards);
  // A side not divided still changes as a whole (the radius).
  Modeler plain;
  const ObjectId p = cylinder(plain);
  TopoDS_Face side;
  for (TopExp_Explorer e(world(plain, p), TopAbs_FACE); e.More(); e.Next()) {
    if (geom::isCylinderOrCone(TopoDS::Face(e.Current()))) side = TopoDS::Face(e.Current());
  }
  ASSERT_TRUE(plain.pushPull(p, side, 100));
  const auto* circle = std::get_if<CircleProfile>(&plain.document().find(p)->recipe.base);
  ASSERT_TRUE(circle);
  EXPECT_NEAR(circle->radius, 600, 1e-9);
}

// Optional: SBR_CURVED_BENCH_N vertical lines and as many circles on a
// cylinder, one at a time as drawn; times the last ones and regeneration.
TEST(CurvedFaceSplit, Bench) {
  const char* env = std::getenv("SBR_CURVED_BENCH_N");
  const int n = env ? std::atoi(env) : 0;
  if (n <= 0) GTEST_SKIP() << "set SBR_CURVED_BENCH_N to run";
  Modeler m;
  const ObjectId c = cylinder(m);
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point s) { return std::chrono::duration<double, std::milli>(Clock::now() - s).count(); };
  double lastLine = 0, lastCircle = 0;
  const auto start = Clock::now();
  for (int i = 0; i < n; ++i) {
    auto s = Clock::now();
    line(m, round(0.1 + 2 * M_PI * i / n, 0), round(0.1 + 2 * M_PI * i / n, 1000));
    lastLine = ms(s);
    s = Clock::now();
    circleCurve(m, 1000.0 * (i + 1) / (n + 1));
    lastCircle = ms(s);
  }
  const double total = ms(start);
  Object copy = *m.document().find(c);
  const auto again = Clock::now();
  ASSERT_TRUE(rebuild(copy, m.document().regenContext()));
  std::cout << n << "+" << n << " strokes, " << faces(m, c) << " faces: last line " << lastLine << " ms, last circle "
            << lastCircle << " ms, all " << total << " ms, regenerate " << ms(again) << " ms\n";
}
