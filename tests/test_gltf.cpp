// docs/85: glTF for renderers, with the materials' physically based values.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"

using namespace sbr;
using json = nlohmann::json;

namespace {
struct Api {
  doc::Modeler modeler;
  api::CommandApi api{modeler};
  json ok(const std::string& command, json args) {
    const json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
};
std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
}  // namespace

TEST(Gltf, MaterialsMetresAndYUp) {
  Api a;
  const std::string metal = a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 1000}, {"depth", 2000}, {"height", 3000}})["id"];
  const std::string plain = a.ok("create_box", {{"origin", {5000, 0, 0}}, {"width", 500}, {"depth", 500}, {"height", 500}})["id"];
  a.ok("create_line", {{"points", {{0, 0, 0}, {0, 5000, 0}}}});  // a line: not rendered
  doc::Modeler& m = a.modeler;
  auto material = m.createMaterial("Steel", doc::Color{0.7, 0.72, 0.75});
  ASSERT_TRUE(material);
  doc::Material changed = *m.document().findMaterial(*material);
  changed.metallic = 1.0;
  changed.roughness = 0.25;
  ASSERT_TRUE(m.editMaterial(changed));
  ASSERT_TRUE(m.assignMaterial({metal}, *material));

  const std::string path = ::testing::TempDir() + "model.gltf";
  ASSERT_TRUE(m.exportGltf(path)) << m.exportGltf(path).message();
  const json g = json::parse(slurp(path));
  EXPECT_EQ(g.at("asset").at("version"), "2.0");
  // Two meshes: the line is left out.
  EXPECT_EQ(g.at("meshes").size(), 2u);
  // The steel: its colour, metal, smooth (OCCT 7.6 names materials mat_N;
  // the values are what counts); the plain box: the default non-metal.
  bool steel = false, plainMaterial = false;
  for (const json& mat : g.at("materials")) {
    const json& pbr = mat.at("pbrMetallicRoughness");
    const double metallic = pbr.value("metallicFactor", 1.0), roughness = pbr.value("roughnessFactor", 1.0);
    if (std::abs(metallic - 1.0) < 1e-6 && std::abs(roughness - 0.25) < 1e-6) {
      steel = true;
      EXPECT_NEAR(pbr.at("baseColorFactor")[0].get<double>(), 0.7, 1e-6);
    }
    if (std::abs(metallic) < 1e-6 && std::abs(roughness - 0.6) < 1e-6) plainMaterial = true;
  }
  EXPECT_TRUE(steel);
  EXPECT_TRUE(plainMaterial);
  EXPECT_EQ(g.at("materials").size(), 2u);
  // Metres and Y up: the 1 x 2 x 3 m box spans 3 m along Y (its height) and
  // 2 m along Z.
  bool found = false;
  for (const json& mesh : g.at("meshes")) {
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (const json& primitive : mesh.at("primitives")) {
      const json& acc = g.at("accessors").at(primitive.at("attributes").at("POSITION").get<int>());
      for (int i = 0; i < 3; ++i) {
        lo[i] = std::min(lo[i], acc.at("min")[i].get<double>());
        hi[i] = std::max(hi[i], acc.at("max")[i].get<double>());
      }
    }
    if (std::abs(hi[0] - lo[0] - 1.0) < 1e-6 && std::abs(hi[1] - lo[1] - 3.0) < 1e-6 &&
        std::abs(hi[2] - lo[2] - 2.0) < 1e-6) {
      found = true;
      EXPECT_EQ(mesh.at("primitives").size(), 1u);  // faces merged: one draw per object
    }
  }
  EXPECT_TRUE(found) << "no mesh with the box's extent in metres, Y up";
  // The model's shapes are not meshed by the export.
  for (TopExp_Explorer f(m.document().find(metal)->shape, TopAbs_FACE); f.More(); f.Next()) {
    TopLoc_Location loc;
    EXPECT_TRUE(BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc).IsNull());
  }

  // Binary glTF: one file starting "glTF"; by the AI the same; only boxes asked.
  const std::string glb = ::testing::TempDir() + "model.glb";
  a.ok("export_gltf", {{"path", glb}, {"ids", {plain}}});
  EXPECT_EQ(slurp(glb).substr(0, 4), "glTF");
  EXPECT_FALSE(m.exportGltf(::testing::TempDir() + "model.obj"));
}
