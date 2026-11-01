// COLLADA (.dae) for SketchUp (docs/46): solids and lines out as meshes
// with their colours, meshes in as faces joined back and solids closed,
// in the file's units and up axis, components placed.

#include <gtest/gtest.h>
#include <cmath>
#include <fstream>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Collada.h"
#include "sbr/geom/Measure.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId box(Modeler& m, double x, double y, double w, double d, double h) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, 0);
  const ObjectId id = *m.createRectangle(plane, w, d);
  EXPECT_TRUE(m.extrude(id, h));
  return id;
}

int faceCount(const TopoDS_Shape& s) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  return faces.Extent();
}

std::string write(const std::string& name, const std::string& text) {
  const std::string path = ::testing::TempDir() + name;
  std::ofstream(path) << text;
  return path;
}

}  // namespace

TEST(Collada, BoxAndLineRoundTrip) {
  Modeler m;
  const ObjectId b = box(m, 0, 0, 1000, 2000, 3000);
  // The top painted red on its own.
  TopoDS_Face top;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(b)), TopAbs_FACE); e.More(); e.Next()) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(e.Current(), p);
    if (std::abs(p.CentreOfMass().Z() - 3000) < 1e-6) top = TopoDS::Face(e.Current());
  }
  ASSERT_FALSE(top.IsNull());
  const auto red = m.createMaterial("Red", Color{1, 0, 0});
  ASSERT_TRUE(red);
  ASSERT_TRUE(m.paintFaces(b, {top}, *red));
  ASSERT_TRUE(m.createPolyline({gp_Pnt(0, 3000, 0), gp_Pnt(1000, 3000, 0)}));
  const std::string path = ::testing::TempDir() + "roundtrip.dae";
  ASSERT_TRUE(m.exportCollada(path));

  geom::ColladaReport report;
  Modeler back;
  const auto ids = back.importCollada(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  EXPECT_EQ(report.units, "millimeter");
  EXPECT_EQ(report.solids, 1) << report.summary();
  ASSERT_EQ(ids->size(), 2u) << report.summary();
  const Object* solid = back.document().find((*ids)[0]);
  EXPECT_EQ(solid->kind(), ObjectKind::Solid);
  const TopoDS_Shape shape = back.document().worldShape(*solid);
  EXPECT_NEAR(geom::volume(shape), 1000.0 * 2000 * 3000, 1.0);
  EXPECT_EQ(faceCount(shape), 6);  // the triangles joined back into the box's faces
  // The red top came back as a face painted on its own.
  ASSERT_EQ(solid->faceMaterials.size(), 1u);
  const Material* painted = back.document().findMaterial(solid->faceMaterials.front().material);
  ASSERT_TRUE(painted);
  EXPECT_NEAR(painted->color.r, 1.0, 1e-6);
  EXPECT_NEAR(painted->color.g, 0.0, 1e-6);
  EXPECT_EQ(back.document().find((*ids)[1])->kind(), ObjectKind::Curve);
  // One undo step takes the import away.
  ASSERT_TRUE(back.undo());
  EXPECT_TRUE(back.document().objects().empty());
}

TEST(Collada, CylinderComesBackFaceted) {
  Modeler m;
  geom::SketchPlane plane = geom::SketchPlane::ground();
  const ObjectId c = *m.createCircle(plane, 500);
  ASSERT_TRUE(m.extrude(c, 1000));
  const std::string path = ::testing::TempDir() + "cylinder.dae";
  ASSERT_TRUE(m.exportCollada(path));
  Modeler back;
  geom::ColladaReport report;
  const auto ids = back.importCollada(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  ASSERT_EQ(ids->size(), 1u);
  const TopoDS_Shape shape = back.document().worldShape(*back.document().find(ids->front()));
  const double exact = M_PI * 500 * 500 * 1000;
  EXPECT_NEAR(geom::volume(shape), exact, exact * 0.03);  // facets of about 15 degrees
  EXPECT_EQ(report.solids, 1);
}

TEST(Collada, SketchUpFileInInchesWithComponents) {
  // As SketchUp writes it: inches, Z up, a component (library_nodes) placed
  // twice, materials bound by symbol, triangles with normals interleaved.
  const std::string dae = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit meter="0.0254" name="inch" /><up_axis>Z_UP</up_axis></asset>
  <library_effects>
    <effect id="wood-fx"><profile_COMMON><technique sid="COMMON"><lambert>
      <diffuse><color>0.6 0.4 0.2 1</color></diffuse></lambert></technique></profile_COMMON></effect>
  </library_effects>
  <library_materials><material id="wood" name="wood"><instance_effect url="#wood-fx" /></material></library_materials>
  <library_geometries>
    <geometry id="cube"><mesh>
      <source id="cube-p"><float_array id="cube-pa" count="24">0 0 0 1 0 0 1 1 0 0 1 0 0 0 1 1 0 1 1 1 1 0 1 1</float_array>
        <technique_common><accessor count="8" source="#cube-pa" stride="3"><param name="X" type="float" /><param name="Y" type="float" /><param name="Z" type="float" /></accessor></technique_common></source>
      <source id="cube-n"><float_array id="cube-na" count="3">0 0 1</float_array>
        <technique_common><accessor count="1" source="#cube-na" stride="3"><param name="X" type="float" /><param name="Y" type="float" /><param name="Z" type="float" /></accessor></technique_common></source>
      <vertices id="cube-v"><input semantic="POSITION" source="#cube-p" /></vertices>
      <triangles count="12" material="Material2">
        <input offset="0" semantic="VERTEX" source="#cube-v" /><input offset="1" semantic="NORMAL" source="#cube-n" />
        <p>0 0 2 0 1 0  0 0 3 0 2 0  4 0 5 0 6 0  4 0 6 0 7 0  0 0 1 0 5 0  0 0 5 0 4 0
           1 0 2 0 6 0  1 0 6 0 5 0  2 0 3 0 7 0  2 0 7 0 6 0  3 0 0 0 4 0  3 0 4 0 7 0</p>
      </triangles>
    </mesh></geometry>
  </library_geometries>
  <library_nodes>
    <node id="comp" name="Block"><instance_geometry url="#cube"><bind_material><technique_common>
      <instance_material symbol="Material2" target="#wood" /></technique_common></bind_material></instance_geometry></node>
  </library_nodes>
  <library_visual_scenes><visual_scene id="scene">
    <node name="SketchUp">
      <node name="Block#1"><matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix><instance_node url="#comp" /></node>
      <node name="Block#2"><matrix>1 0 0 10 0 1 0 0 0 0 1 0 0 0 0 1</matrix><instance_node url="#comp" /></node>
    </node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#scene" /></scene>
</COLLADA>
)";
  const std::string path = write("sketchup.dae", dae);
  Modeler m;
  geom::ColladaReport report;
  const auto ids = m.importCollada(path, &report);
  ASSERT_TRUE(ids) << ids.error();
  EXPECT_EQ(report.units, "inch");
  EXPECT_NEAR(report.scaleToMm, 25.4, 1e-9);
  ASSERT_EQ(ids->size(), 2u) << report.summary();
  EXPECT_EQ(report.solids, 2);
  const TopoDS_Shape second = m.document().worldShape(*m.document().find((*ids)[1]));
  EXPECT_NEAR(geom::volume(second), 25.4 * 25.4 * 25.4, 1e-3);
  const Bnd_Box b = geom::exactBounds(second);
  EXPECT_NEAR(b.CornerMin().X(), 254.0, 1e-6);  // 10 inches along
  EXPECT_EQ(faceCount(second), 6);
  const Material* wood = m.document().findMaterial(m.document().find((*ids)[0])->material);
  ASSERT_TRUE(wood);
  EXPECT_NEAR(wood->color.r, 0.6, 1e-6);
  EXPECT_EQ(m.document().find((*ids)[0])->name, "Block");  // the component's own name
}

TEST(Collada, YUpAndBrokenFiles) {
  const std::string dae = R"(<COLLADA><asset><unit meter="1"/><up_axis>Y_UP</up_axis></asset>
<library_geometries><geometry id="t"><mesh><source id="s"><float_array id="a" count="9">0 0 0 1 0 0 0 0 -1</float_array>
<technique_common><accessor source="#a" count="3" stride="3"/></technique_common></source>
<vertices id="v"><input semantic="POSITION" source="#s"/></vertices>
<polylist count="1"><input semantic="VERTEX" source="#v" offset="0"/><vcount>3</vcount><p>0 1 2</p></polylist>
</mesh></geometry></library_geometries>
<library_visual_scenes><visual_scene id="vs"><node><instance_geometry url="#t"/></node></visual_scene></library_visual_scenes>
<scene><instance_visual_scene url="#vs"/></scene></COLLADA>)";
  Modeler m;
  geom::ColladaReport report;
  const auto ids = m.importCollada(write("yup.dae", dae), &report);
  ASSERT_TRUE(ids) << ids.error();
  // Metres to mm; Y up turned to Z up: (0, 0, -1) m -> (0, 1000, 0) mm.
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(*m.document().find(ids->front())));
  EXPECT_NEAR(b.CornerMax().Y(), 1000, 1e-6);
  EXPECT_NEAR(b.CornerMax().Z(), 0, 1e-6);
  EXPECT_NEAR(b.CornerMax().X(), 1000, 1e-6);
  EXPECT_FALSE(m.importCollada(write("broken.dae", "<COLLADA><library_geometries>")));
  EXPECT_FALSE(m.importCollada(write("notdae.dae", "<html></html>")));
  EXPECT_FALSE(m.importCollada(::testing::TempDir() + "missing.dae"));
}

TEST(Collada, Api) {
  Modeler m;
  box(m, 0, 0, 500, 500, 500);
  api::CommandApi commands(m);
  auto run = [&](const nlohmann::json& c) { return nlohmann::json::parse(commands.execute(c.dump())); };
  const std::string path = ::testing::TempDir() + "api.dae";
  const auto out = run({{"command", "export_dae"}, {"args", {{"path", path}}}});
  ASSERT_TRUE(out.value("ok", false)) << out.dump();
  const auto in = run({{"command", "import_dae"}, {"args", {{"path", path}}}});
  ASSERT_TRUE(in.value("ok", false)) << in.dump();
  EXPECT_EQ(in["result"]["report"]["solids"], 1);
  EXPECT_EQ(in["result"]["report"]["units"], "millimeter");
  EXPECT_EQ(m.document().objects().size(), 2u);
}
