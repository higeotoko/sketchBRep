// One placement of a component (docs/14_components.md): its own parameters,
// mirrored placements, making a placement unique, moving the component's
// origin / axes, and how these survive definition edits, in-place editing,
// undo and save / reload.

#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>

#include <nlohmann/json.hpp>

#include "TestHelpers.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId box(Modeler& m, double x, double y, double w, double d, double h) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, 0);
  auto id = m.createRectangle(plane, w, d);
  EXPECT_TRUE(m.extrude(*id, h).ok());
  return *id;
}

Bnd_Box bounds(const Modeler& m, const ObjectId& id) { return geom::exactBounds(m.document().worldShape(id)); }

double volume(const TopoDS_Shape& shape) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  return props.Mass();
}

const ComponentInstance& placementOf(const Modeler& m, const ObjectId& id) {
  return std::get<ComponentInstance>(m.document().find(id)->recipe.base);
}

// A 1000 x 200 x 2000 panel made a component, plus a copy 3000 further on x.
struct TwoPanels {
  Modeler m;
  ObjectId panel, first, second;
  std::string definition;
  TwoPanels() {
    panel = box(m, 0, 0, 1000, 200, 2000);
    first = *m.makeComponent({panel}, "Panel");
    gp_Trsf step;
    step.SetTranslation(gp_Vec(3000, 0, 0));
    second = m.copy({first}, step, 1)->front();
    definition = placementOf(m, first).definitionId;
  }
};

}  // namespace

TEST(Placements, OwnParameterChangesOnlyThatPlacement) {
  TwoPanels t;
  Modeler& m = t.m;
  ASSERT_TRUE(m.setPlacementParameter(t.second, t.panel, "width", 1500).ok());
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4500, 200, 2000);
  test::expectBounds(m.document().worldShape(t.first), 0, 0, 0, 1000, 200, 2000);
  test::expectBounds(m.document().findDefinition(t.definition)->shape, 0, 0, 0, 1000, 200, 2000);
  EXPECT_EQ(m.document().instanceCount(t.definition), 2u);  // still the same component

  bool own = false;
  for (const auto& p : m.placementParameters(t.second)) {
    if (p.parameter.key == "width") {
      own = p.own;
      EXPECT_DOUBLE_EQ(p.parameter.value, 1500);
      EXPECT_DOUBLE_EQ(p.shared, 1000);
    }
  }
  EXPECT_TRUE(own);
  EXPECT_FALSE(m.setPlacementParameter(t.second, t.panel, "nonsense", 1).ok());

  // A shared edit reaches both; the placement keeps its own width.
  ASSERT_TRUE(m.setDefinitionParameter(t.definition, t.panel, "op0.distance", 2500).ok());
  test::expectBounds(m.document().worldShape(t.first), 0, 0, 0, 1000, 200, 2500);
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4500, 200, 2500);

  // The definition's value again: no override left.
  ASSERT_TRUE(m.setPlacementParameter(t.second, t.panel, "width", 1000).ok());
  EXPECT_TRUE(placementOf(m, t.second).overrides.empty());
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4000, 200, 2500);

  ASSERT_TRUE(m.undo());
  ASSERT_EQ(placementOf(m, t.second).overrides.size(), 1u);
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4500, 200, 2500);
  ASSERT_TRUE(m.clearPlacementParameters(t.second).ok());
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4000, 200, 2500);
}

TEST(Placements, MirrorInPlaceKeepsBoundsAndFlipsTheShape) {
  Modeler m;
  // An L: a tall post at the left end of a long sill.
  const ObjectId sill = box(m, 0, 0, 1000, 200, 100);
  const ObjectId post = box(m, 0, 0, 100, 200, 1000);
  const ObjectId inst = *m.makeComponent({sill, post}, "L");
  ASSERT_TRUE(m.move({inst}, gp_Vec(500, 0, 0)).ok());
  const double before = volume(m.document().worldShape(inst));

  ASSERT_TRUE(m.mirrorPlacement(inst, 0).ok());
  EXPECT_TRUE(placementOf(m, inst).mirrored);
  const TopoDS_Shape world = m.document().worldShape(inst);
  test::expectBounds(world, 500, 0, 0, 1500, 200, 1000);
  EXPECT_NEAR(volume(world), before, 1e-6);  // outward faces: positive volume
  EXPECT_TRUE(BRepCheck_Analyzer(world).IsValid());
  // The post is now at the right end.
  Bnd_Box top;
  for (TopExp_Explorer e(world, TopAbs_SOLID); e.More(); e.Next()) {
    const Bnd_Box b = geom::exactBounds(e.Current());
    if (b.CornerMax().Z() > 500) top = b;
  }
  ASSERT_FALSE(top.IsVoid());
  EXPECT_NEAR(top.CornerMin().X(), 1400, 1e-6);

  // Mirroring along blue leaves red alone: the post stays right, upside down.
  ASSERT_TRUE(m.mirrorPlacement(inst, 2).ok());
  EXPECT_FALSE(placementOf(m, inst).mirrored);  // two mirrors are a rotation
  test::expectBounds(m.document().worldShape(inst), 500, 0, 0, 1500, 200, 1000);

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_FALSE(placementOf(m, inst).mirrored);
  test::expectBounds(m.document().worldShape(inst), 500, 0, 0, 1500, 200, 1000);
  EXPECT_FALSE(m.mirrorPlacement(inst, 3).ok());
}

TEST(Placements, MakeUniqueWritesOwnParametersIntoACopy) {
  TwoPanels t;
  Modeler& m = t.m;
  ASSERT_TRUE(m.setPlacementParameter(t.second, t.panel, "width", 1500).ok());
  ASSERT_TRUE(m.mirrorPlacement(t.second, 1).ok());
  auto unique = m.makeUnique(t.second);
  ASSERT_TRUE(unique.ok()) << unique.error();
  EXPECT_NE(*unique, t.definition);
  EXPECT_EQ(m.document().findDefinition(*unique)->name, "Panel #2");
  EXPECT_TRUE(placementOf(m, t.second).overrides.empty());
  EXPECT_TRUE(placementOf(m, t.second).mirrored);
  EXPECT_EQ(m.document().instanceCount(t.definition), 1u);
  EXPECT_EQ(m.document().instanceCount(*unique), 1u);
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4500, 200, 2000);

  // The two components now change separately.
  ASSERT_TRUE(m.setDefinitionParameter(t.definition, t.panel, "op0.distance", 2500).ok());
  EXPECT_NEAR(bounds(m, t.second).CornerMax().Z(), 2000, 1e-6);
  EXPECT_FALSE(m.makeUnique(t.first).ok());  // already the only one

  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(placementOf(m, t.second).definitionId, t.definition);
  EXPECT_EQ(m.document().instanceCount(t.definition), 2u);
}

TEST(Placements, ComponentOriginAndAxesMoveNothing) {
  TwoPanels t;
  Modeler& m = t.m;
  ASSERT_TRUE(m.mirrorPlacement(t.second, 0).ok());
  // Origin at the panel's far corner, red axis along world y.
  ASSERT_TRUE(m.setComponentOrigin(t.first, gp_Pnt(1000, 200, 0), gp_Dir(0, 1, 0)).ok());
  test::expectBounds(m.document().worldShape(t.first), 0, 0, 0, 1000, 200, 2000);
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4000, 200, 2000);
  const gp_XYZ origin = m.document().find(t.first)->transform.TranslationPart();
  EXPECT_NEAR(origin.X(), 1000, 1e-6);
  EXPECT_NEAR(origin.Y(), 200, 1e-6);
  // The placement's red axis now points along world y.
  const gp_Dir red = gp_Dir(1, 0, 0).Transformed(m.document().find(t.first)->transform);
  EXPECT_NEAR(red.Y(), 1, 1e-9);

  // Its own parameters still apply about the new origin.
  ASSERT_TRUE(m.setPlacementParameter(t.first, t.panel, "op0.distance", 3000).ok());
  test::expectBounds(m.document().worldShape(t.first), 0, 0, 0, 1000, 200, 3000);
  ASSERT_TRUE(m.undo());
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(m.document().find(t.first)->transform.TranslationPart().X(), 0, 1e-6);
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4000, 200, 2000);
}

TEST(Placements, InPlaceEditKeepsOwnSettingsAndExplodeAppliesThem) {
  TwoPanels t;
  Modeler& m = t.m;
  ASSERT_TRUE(m.setPlacementParameter(t.second, t.panel, "width", 1500).ok());
  ASSERT_TRUE(m.mirrorPlacement(t.first, 0).ok());

  auto group = m.beginComponentEdit(t.first);
  ASSERT_TRUE(group.ok()) << group.error();
  ASSERT_TRUE(m.endComponentEdit(*group).ok());
  EXPECT_TRUE(placementOf(m, t.first).mirrored);
  EXPECT_EQ(placementOf(m, t.second).overrides.size(), 1u);
  test::expectBounds(m.document().worldShape(t.second), 3000, 0, 0, 4500, 200, 2000);

  EXPECT_FALSE(m.explode(t.first).ok());  // mirrored: mirror back first
  auto parts = m.explode(t.second);
  ASSERT_TRUE(parts.ok()) << parts.error();
  test::expectBounds(m.document().worldShape(parts->front()), 3000, 0, 0, 4500, 200, 2000);
}

TEST(Placements, SaveAndReload) {
  TwoPanels t;
  Modeler& m = t.m;
  ASSERT_TRUE(m.setPlacementParameter(t.second, t.panel, "width", 1500).ok());
  ASSERT_TRUE(m.mirrorPlacement(t.second, 0).ok());
  const std::string path = test::tempPath("placements.sbr");
  ASSERT_TRUE(m.save(path).ok());
  for (bool regenerate : {false, true}) {
    Modeler loaded;
    std::vector<std::string> warnings;
    ASSERT_TRUE(loaded.load(path, &warnings, regenerate).ok());
    EXPECT_TRUE(warnings.empty()) << warnings.front();
    const auto& ref = placementOf(loaded, t.second);
    EXPECT_TRUE(ref.mirrored);
    ASSERT_EQ(ref.overrides.size(), 1u);
    EXPECT_EQ(ref.overrides.front().key, "width");
    test::expectBounds(loaded.document().worldShape(t.second), 3000, 0, 0, 4500, 200, 2000);
  }
}

TEST(Placements, AiCommands) {
  using json = nlohmann::json;
  TwoPanels t;
  api::CommandApi api(t.m);
  auto call = [&](const std::string& command, const json& args) {
    const json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  call("set_placement_parameter", {{"id", t.second}, {"object", t.panel}, {"key", "width"}, {"value", 1500}});
  const json read = call("placement_parameters", {{"id", t.second}});
  EXPECT_EQ(read["placements"], 2);
  bool own = false;
  for (const json& p : read["parameters"]) own = own || (p["key"] == "width" && p["own"] == true && p["shared"] == 1000);
  EXPECT_TRUE(own);
  call("mirror_placement", {{"ids", json::array({t.second})}, {"axis", "red"}});
  const json listed = call("query_objects", {{"definition", t.definition}, {"fields", json::array({"definition"})}});
  int mirrored = 0;
  for (const json& o : listed["objects"]) mirrored += o.value("mirrored", false);
  EXPECT_EQ(mirrored, 1);
  call("set_component_axes", {{"id", t.first}, {"origin", {500, 0, 0}}});
  test::expectBounds(t.m.document().worldShape(t.first), 0, 0, 0, 1000, 200, 2000);
  const json unique = call("make_unique", {{"id", t.second}});
  EXPECT_NE(unique["definition"], t.definition);
  call("clear_placement_parameters", {{"id", t.first}});
}
