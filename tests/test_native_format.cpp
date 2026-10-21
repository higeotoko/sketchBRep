#include <filesystem>
#include <fstream>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/ShapeIO.h"

using namespace sbr;
using namespace sbr::doc;

TEST(NativeFormat, SaveLoadKeepsDimensionsRecipeAndIds) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 4000.25, 6000.125, "Room");
  ASSERT_TRUE(m.extrude(*id, 3000).ok());
  ASSERT_TRUE(m.rename(*id, "Room A").ok());
  m.document().settings().camera.perspective = false;

  const std::string path = test::tempPath("project.sbr");
  ASSERT_TRUE(m.save(path).ok());
  EXPECT_TRUE(m.history().isClean());
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

  Modeler loaded;
  std::vector<std::string> warnings;
  const Status s = loaded.load(path, &warnings);
  ASSERT_TRUE(s.ok()) << s.message();
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  ASSERT_EQ(loaded.document().objects().size(), 1u);
  const Object& o = loaded.document().objects().front();
  EXPECT_EQ(o.id, *id);
  EXPECT_EQ(o.name, "Room A");
  EXPECT_FALSE(loaded.document().settings().camera.perspective);
  test::expectBounds(o.shape, 0, 0, 0, 4000.25, 6000.125, 3000, 1e-9);

  // Re-edit after reload: the parametric recipe survived.
  ASSERT_TRUE(loaded.setParameter(o.id, "op0.distance", 4500).ok());
  test::expectBounds(loaded.document().objects().front().shape, 0, 0, 0, 4000.25, 6000.125, 4500);
}

TEST(NativeFormat, ImportedGeometrySurvives) {
  Modeler m;
  auto face = geom::makeRectangleFace(geom::SketchPlane::ground(), 100, 100);
  auto id = m.importShape(face.value(), "Imported face", "test");
  ASSERT_TRUE(id.ok());
  const std::string text = serializeDocument(m.document());
  Document d;
  ASSERT_TRUE(deserializeDocument(text, d).ok());
  ASSERT_EQ(d.objects().size(), 1u);
  EXPECT_NEAR(geom::area(d.objects().front().shape), 10000, 1e-9);
}

TEST(NativeFormat, CorruptFilesAreRejected) {
  Document d;
  EXPECT_FALSE(deserializeDocument("{not json", d).ok());
  EXPECT_FALSE(deserializeDocument(R"({"format":"Other","version":1})", d).ok());
  EXPECT_FALSE(deserializeDocument(R"({"format":"SketchBRep","version":999,"objects":[]})", d).ok());
  EXPECT_FALSE(deserializeDocument(
                   R"({"format":"SketchBRep","version":1,"objects":[{"id":"x","recipe":{"base":{"type":"warp"},"modifiers":[]}}]})",
                   d)
                   .ok());
}

TEST(NativeFormat, StoredGeometryWinsWhenRecipeCannotRegenerate) {
  Modeler m;
  auto id = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(m.extrude(*id, 1000).ok());
  std::string text = serializeDocument(m.document());
  // Break the recipe but keep the geometry.
  const auto pos = text.find("\"distance\": 1000.0");
  ASSERT_NE(pos, std::string::npos) << text.substr(0, 400);
  text.replace(pos, std::string("\"distance\": 1000.0").size(), "\"distance\": 0.0");
  Document d;
  std::vector<std::string> warnings;
  ASSERT_TRUE(deserializeDocument(text, d, &warnings).ok());
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NEAR(geom::volume(d.objects().front().shape), 1e9, 1e-3);
}

TEST(NativeFormat, FailedSaveKeepsPreviousFile) {
  Modeler m;
  ASSERT_TRUE(m.createRectangle(geom::SketchPlane::ground(), 100, 100).ok());
  const std::string dir = test::tempPath("missing_dir/sub/project.sbr");
  EXPECT_FALSE(m.save(dir).ok());
  EXPECT_FALSE(m.history().isClean());
}

TEST(NativeFormat, GuidesRoundTripAndStayOutOfExports) {
  Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(box.ok() && m.extrude(*box, 1000).ok());
  auto guide = m.createGuide(gp_Pnt(0, 600, 0), gp::DX());
  ASSERT_TRUE(guide.ok()) << guide.error();
  EXPECT_EQ(m.document().find(*guide)->kind(), ObjectKind::Guide);
  EXPECT_EQ(m.document().find(*guide)->layer, "Guides");
  // Guides are drawing aids: a model of only guides has nothing to export.
  Modeler only;
  ASSERT_TRUE(only.createGuide(gp_Pnt(0, 0, 0), gp::DY()).ok());
  EXPECT_FALSE(only.exportStep(test::tempPath("guides.step"), {}).ok());

  const std::string path = test::tempPath("guides.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path).ok());
  const Object* g = loaded.document().find(*guide);
  ASSERT_TRUE(g);
  ASSERT_EQ(g->kind(), ObjectKind::Guide);
  const auto& line = std::get<GuideLine>(g->recipe.base);
  EXPECT_NEAR(line.point.Distance(gp_Pnt(0, 600, 0)), 0.0, 1e-9);
  EXPECT_TRUE(line.direction.IsParallel(gp::DX(), 1e-12));

  ASSERT_TRUE(loaded.clearGuides().ok());
  EXPECT_EQ(loaded.document().objects().size(), 1u);
  EXPECT_FALSE(loaded.clearGuides().ok());  // nothing left to clear
  loaded.undo();
  EXPECT_EQ(loaded.document().objects().size(), 2u);
}
