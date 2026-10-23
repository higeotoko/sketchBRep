// Rhino .3dm import report: what does not come through is counted and the
// import refuses by default instead of dropping it silently. The fixture is
// written with openNURBS directly, so it can hold what SketchBRep never
// writes (meshes, points, curves, broken Breps).

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "opennurbs.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Rhino3dm.h"

using namespace sbr;

namespace {

ON_Brep* box(double x0) {
  const ON_3dPoint corners[8] = {ON_3dPoint(x0, 0, 0),       ON_3dPoint(x0 + 10, 0, 0),
                                 ON_3dPoint(x0 + 10, 10, 0),  ON_3dPoint(x0, 10, 0),
                                 ON_3dPoint(x0, 0, 10),       ON_3dPoint(x0 + 10, 0, 10),
                                 ON_3dPoint(x0 + 10, 10, 10), ON_3dPoint(x0, 10, 10)};
  return ON_BrepBox(corners);
}

// A file with 2 whole boxes, 1 box missing a trimming curve, 1 box whose
// surfaces are all gone, a mesh, a point and a curve.
std::string writeMixedFixture() {
  (void)geom::read3dm(::testing::TempDir() + "none.3dm");  // starts openNURBS
  ONX_Model model;
  model.m_settings.m_ModelUnitsAndTolerances.m_unit_system = ON_UnitSystem(ON::LengthUnitSystem::Meters);
  model.AddDefaultLayer(nullptr, ON_Color::UnsetColor);
  model.AddManagedModelGeometryComponent(box(0), nullptr);
  model.AddManagedModelGeometryComponent(box(20), nullptr);
  ON_Brep* partial = box(40);
  partial->m_T[0].m_c2i = -1;  // its first trim has no curve
  model.AddManagedModelGeometryComponent(partial, nullptr);
  ON_Brep* broken = box(60);
  for (int i = 0; i < broken->m_F.Count(); ++i) broken->m_F[i].m_si = -1;
  model.AddManagedModelGeometryComponent(broken, nullptr);
  auto* mesh = new ON_Mesh(1, 3, false, false);
  mesh->SetVertex(0, ON_3dPoint(0, 0, 0));
  mesh->SetVertex(1, ON_3dPoint(1, 0, 0));
  mesh->SetVertex(2, ON_3dPoint(0, 1, 0));
  mesh->SetTriangle(0, 0, 1, 2);
  model.AddManagedModelGeometryComponent(mesh, nullptr);
  model.AddManagedModelGeometryComponent(new ON_Point(ON_3dPoint(5, 5, 5)), nullptr);
  model.AddManagedModelGeometryComponent(new ON_LineCurve(ON_3dPoint(0, 0, 0), ON_3dPoint(0, 0, 5)), nullptr);
  const std::string path = ::testing::TempDir() + "mixed.3dm";
  EXPECT_TRUE(model.Write(path.c_str(), 0, nullptr));
  return path;
}

}  // namespace

TEST(Rhino3dmReport, CountsWhatDoesNotComeThrough) {
  const std::string path = writeMixedFixture();
  geom::Import3dmReport report;
  auto parts = geom::read3dm(path, &report);
  ASSERT_TRUE(parts.ok()) << parts.error();
  EXPECT_EQ(report.objects, 7);
  EXPECT_EQ(report.imported, 3);
  EXPECT_EQ(static_cast<int>(parts->size()), 3);
  EXPECT_EQ(report.partial, 1);
  EXPECT_GE(report.skippedTrims + report.skippedFaces, 1);
  EXPECT_EQ(report.failed, 1);
  EXPECT_EQ(report.unsupported["mesh"], 1);
  EXPECT_EQ(report.unsupported["point"], 1);
  EXPECT_EQ(report.unsupported["curve"], 1);
  EXPECT_EQ(report.units, "meters");
  EXPECT_DOUBLE_EQ(report.scaleToMm, 1000.0);
  EXPECT_TRUE(report.incomplete());
  EXPECT_NE(report.summary().find("1 failed to convert"), std::string::npos) << report.summary();
}

TEST(Rhino3dmReport, PartialImportIsTheUsersChoice) {
  const std::string path = writeMixedFixture();
  doc::Modeler m;
  geom::Import3dmReport report;
  auto refused = m.import3dm(path, false, &report);
  ASSERT_FALSE(refused.ok());
  EXPECT_NE(refused.error().find("not supported: curve 1, mesh 1, point 1"), std::string::npos) << refused.error();
  EXPECT_TRUE(m.listObjects().empty());  // nothing was imported

  auto taken = m.import3dm(path, true, &report);
  ASSERT_TRUE(taken.ok()) << taken.error();
  EXPECT_EQ(taken->size(), 3u);
  EXPECT_EQ(m.listObjects().size(), 3u);
}

TEST(Rhino3dmReport, ApiReturnsTheReport) {
  const std::string path = writeMixedFixture();
  doc::Modeler m;
  api::CommandApi api(m);
  const auto refused = api.execute(nlohmann::json{{"command", "import_3dm"}, {"args", {{"path", path}}}}.dump());
  EXPECT_FALSE(nlohmann::json::parse(refused)["ok"].get<bool>()) << refused;
  const auto taken = nlohmann::json::parse(
      api.execute(nlohmann::json{{"command", "import_3dm"}, {"args", {{"path", path}, {"allow_partial", true}}}}.dump()));
  ASSERT_TRUE(taken["ok"].get<bool>()) << taken.dump();
  const auto& r = taken["result"]["report"];
  EXPECT_EQ(r["objects"], 7);
  EXPECT_EQ(r["imported"], 3);
  EXPECT_EQ(r["failed"], 1);
  EXPECT_EQ(r["unsupported"]["mesh"], 1);
  EXPECT_FALSE(r["complete"].get<bool>());
}
