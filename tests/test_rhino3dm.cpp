// Rhino .3dm exchange (openNURBS): exact round trips of typical shapes.

#include <cmath>

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeSphere.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Rhino3dm.h"
#include "sbr/geom/Validation.h"

using namespace sbr;

namespace {

void expectRoundTrip(const TopoDS_Shape& shape, const char* what) {
  // One file per test: ctest runs the tests of this suite in parallel.
  const std::string path = ::testing::TempDir() + "roundtrip_" +
                           ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".3dm";
  geom::NamedShape part{shape, what, std::array<double, 3>{0.8, 0.2, 0.1}, "Walls"};
  const Status written = geom::write3dm({part}, path);
  ASSERT_TRUE(written.ok()) << what << ": " << written.message();
  auto read = geom::read3dm(path);
  ASSERT_TRUE(read.ok()) << what << ": " << read.error();
  ASSERT_EQ(read->size(), 1u) << what;
  const geom::NamedShape& back = read->front();
  EXPECT_EQ(back.name, what);
  EXPECT_EQ(back.layer, "Walls");
  ASSERT_TRUE(back.color.has_value());
  EXPECT_NEAR((*back.color)[0], 0.8, 0.01);
  const auto report = geom::validateShape(back.shape, true);
  EXPECT_TRUE(report.valid) << what << ": solids " << report.solids << " closed " << report.closed;
  EXPECT_NEAR(geom::volume(back.shape), geom::volume(shape), geom::volume(shape) * 1e-5) << what;
}

}  // namespace

TEST(Rhino3dm, Available) { EXPECT_TRUE(geom::rhino3dmAvailable()); }

TEST(Rhino3dm, BoxCylinderAndSphereRoundTrip) {
  doc::Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*box, 3000).ok());
  expectRoundTrip(m.document().worldShape(*box), "Wall");

  auto column = m.createCircle(geom::SketchPlane::ground(), 300);
  ASSERT_TRUE(m.extrude(*column, 3000).ok());
  expectRoundTrip(m.document().worldShape(*column), "Column");  // seam edge

  expectRoundTrip(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 500).Shape(), "Dome");  // poles
}

TEST(Rhino3dm, WallWithOpeningAndArchRoundTrip) {
  doc::Modeler m;
  auto wall = m.createRectangle(geom::SketchPlane::ground(), 4000, 200);
  ASSERT_TRUE(m.extrude(*wall, 3000).ok());
  auto plane = geom::SketchPlane::fromNormal(gp_Pnt(1500, 0, 0), -gp::DY());
  auto door = m.createRectangle(plane, 900, 2100);
  const TopoDS_Face doorFace = TopoDS::Face(m.document().find(*door)->shape);
  auto opened = m.pushPull(*door, doorFace, -200);
  ASSERT_TRUE(opened.ok()) << opened.error();
  expectRoundTrip(m.document().worldShape(opened->id), "Wall with door");  // inner loops

  auto arch = m.createArc(gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp::DZ(), 400, true);
  ASSERT_TRUE(m.extrude(*arch, 500).ok());
  expectRoundTrip(m.document().worldShape(*arch), "Arch");  // rational arc
}

TEST(Rhino3dm, ModelerExportImportKeepsNamesAndTags) {
  doc::Modeler m;
  auto a = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*a, 1000).ok());
  ASSERT_TRUE(m.rename(*a, "柱 A").ok());
  ASSERT_TRUE(m.setLayer(*a, "構造").ok());
  ASSERT_TRUE(m.createDimension(gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Vec(0, -300, 0)).ok());
  const std::string path = ::testing::TempDir() + "model.3dm";
  ASSERT_TRUE(m.export3dm(path).ok());

  doc::Modeler other;
  auto ids = other.import3dm(path);
  ASSERT_TRUE(ids.ok()) << ids.error();
  ASSERT_EQ(ids->size(), 1u);  // annotations are not exported
  const doc::Object* o = other.document().find(ids->front());
  EXPECT_EQ(o->name, "柱 A");
  EXPECT_EQ(o->layer, "構造");
  EXPECT_EQ(o->kind(), doc::ObjectKind::Solid);
  EXPECT_NEAR(geom::volume(other.document().worldShape(*o)), 1e9, 1e3);
  // Imported solids can be pushed/pulled.
  TopoDS_Face top;
  for (TopExp_Explorer e(o->shape, TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(e.Current());
  }
  ASSERT_FALSE(top.IsNull());
  ASSERT_TRUE(other.pushPull(o->id, top, 500).ok());
  EXPECT_NEAR(geom::volume(other.document().worldShape(*other.document().find(ids->front()))), 1.5e9, 1e3);
  EXPECT_FALSE(other.import3dm(::testing::TempDir() + "missing.3dm").ok());
}
