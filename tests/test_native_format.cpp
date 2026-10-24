#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "TestHelpers.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"
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

TEST(NativeFormat, RegularPolygonStaysParametric) {
  Modeler m;
  auto hex = m.createRegularPolygon(geom::SketchPlane::ground(), 1000, 6);
  ASSERT_TRUE(hex.ok()) << hex.error();
  const double hexArea = 1.5 * std::sqrt(3.0) * 1000 * 1000;
  EXPECT_NEAR(geom::area(m.document().find(*hex)->shape), hexArea, 1e-6);
  ASSERT_TRUE(m.extrude(*hex, 300).ok());
  ASSERT_TRUE(m.setParameter(*hex, "sides", 4).ok());
  EXPECT_NEAR(geom::volume(m.document().find(*hex)->shape), 2.0 * 1000 * 1000 * 300, 1e-3);
  EXPECT_FALSE(m.setParameter(*hex, "sides", 2.5).ok());
  EXPECT_FALSE(m.setParameter(*hex, "sides", 2).ok());
  EXPECT_FALSE(m.createRegularPolygon(geom::SketchPlane::ground(), 1000, 2).ok());

  const std::string path = test::tempPath("polygon.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path).ok());
  const auto& p = std::get<RegularPolygonProfile>(loaded.document().find(*hex)->recipe.base);
  EXPECT_EQ(p.sides, 4);
  ASSERT_TRUE(loaded.setParameter(*hex, "radius", 500).ok());
  EXPECT_NEAR(geom::volume(loaded.document().find(*hex)->shape), 2.0 * 500 * 500 * 300, 1e-3);
}

// Format 3: geometry shared by component placements is written once and
// shared again after loading (so the viewport keeps one display copy).
TEST(NativeFormat, SharedGeometryIsStoredOnceAndStaysShared) {
  Modeler m;
  auto box = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  ASSERT_TRUE(box.ok() && m.extrude(*box, 1000).ok());
  auto instance = m.makeComponent({*box});
  ASSERT_TRUE(instance.ok());
  gp_Trsf dx;
  dx.SetTranslation(gp_Vec(2000, 0, 0));
  ASSERT_TRUE(m.copy({*instance}, dx, 9).ok());
  const std::string text = serializeDocument(m.document());
  std::size_t breps = 0;
  for (std::size_t p = text.find("\"brep\""); p != std::string::npos; p = text.find("\"brep\"", p + 1)) ++breps;
  EXPECT_EQ(breps, 2u);  // the definition's box and the placements' shape

  const std::string path = test::tempPath("shared.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(path, &warnings).ok());
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  std::vector<const Object*> placements;
  for (const Object& o : loaded.document().objects()) {
    if (o.kind() == ObjectKind::Instance) placements.push_back(&o);
  }
  ASSERT_EQ(placements.size(), 10u);
  for (const Object* o : placements) EXPECT_TRUE(o->shape.IsSame(placements.front()->shape));
  test::expectBounds(loaded.document().worldShape(*placements.back()), 18000, 0, 0, 19000, 1000, 1000);
}

// Fast open: stored geometry only, the same recipe problems are then found
// by the background verification (a slice at a time).
TEST(NativeFormat, FastLoadThenVerifyRecipesInSlices) {
  Modeler m;
  for (int i = 0; i < 5; ++i) {
    geom::SketchPlane plane = geom::SketchPlane::ground();
    plane.origin = gp_Pnt(i * 2000.0, 0, 0);
    auto id = m.createRectangle(plane, 1000, 1000);
    ASSERT_TRUE(id.ok() && m.extrude(*id, 1000 + i).ok());
  }
  std::string text = serializeDocument(m.document());
  const auto pos = text.find("\"distance\": 1003.0");  // break the 4th recipe
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, std::string("\"distance\": 1003.0").size(), "\"distance\": 0.0");
  const std::string path = test::tempPath("verify.sbr");
  std::ofstream(path, std::ios::binary) << text;

  Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(path, &warnings, false).ok());
  EXPECT_TRUE(warnings.empty());  // nothing checked yet
  EXPECT_EQ(loaded.document().objects().size(), 5u);
  std::size_t cursor = 0;
  std::vector<std::string> found;
  while (cursor < loaded.document().objects().size()) {
    for (const std::string& w : loaded.verifyRecipes(cursor, 2)) found.push_back(w);
  }
  ASSERT_EQ(found.size(), 1u);
  EXPECT_NE(found.front().find("could not be regenerated"), std::string::npos) << found.front();
}

// PERF-05: peak memory of saving a large model (VmHWM growth). Skipped
// unless SBR_BENCH_FILE names a .sbr file; prints the numbers.
TEST(NativeFormat, BenchSavePeakMemory) {
  const char* file = std::getenv("SBR_BENCH_FILE");
  if (!file) GTEST_SKIP() << "set SBR_BENCH_FILE to run";
  auto status = [](const char* key) {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind(key, 0) == 0) return std::stod(line.substr(std::strlen(key))) / 1024.0;
    }
    return -1.0;
  };
  Document d;
  ASSERT_TRUE(loadDocument(d, file).ok());
  // Pictures on materials, as a textured model carries (3 MB each).
  const int pictures = std::getenv("SBR_BENCH_PICTURES") ? std::atoi(std::getenv("SBR_BENCH_PICTURES")) : 0;
  for (int k = 0; k < pictures; ++k) {
    std::string bytes(3 << 20, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 2654435761u + k * 97) >> 13);
    Material material;
    material.id = "bench-" + std::to_string(k);
    material.name = material.id;
    MaterialTexture texture;
    texture.image = d.addImage(bytes, material.id + ".png");
    material.texture = texture;
    d.putMaterial(material);
  }
  const std::string text = serializeDocument(d);
  const double before = status("VmRSS:");
  // Peak so far reset (Linux 4.0+): writing 5 to clear_refs resets VmHWM.
  { std::ofstream("/proc/self/clear_refs") << "5"; }
  const std::string out = ::testing::TempDir() + "/bench_save.sbr";
  ASSERT_TRUE(saveDocument(d, out).ok());
  const double peak = status("VmHWM:");
  std::printf("bench save file_mb=%.1f rss_before_mb=%.1f peak_mb=%.1f peak_growth_mb=%.1f\n", text.size() / 1048576.0, before,
              peak, peak - before);
}
