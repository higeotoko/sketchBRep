#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>

#include "sbr/ConstructionFrame.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct Frames : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  json call(const char* command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const char* command, json args = json::object()) {
    const auto r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << r.dump();
    return r.value("result", json::object());
  }
  json frame(json inputs = json::object(), json extra = json::object()) {
    extra["inputs"] = inputs;
    extra["preview"] = false;
    extra["expected_revision"] = m.history().revision();
    return ok("generate_frame", extra);
  }
  std::vector<ObjectId> role(const std::string& kind) {
    std::vector<ObjectId> out;
    for (const auto& o : m.document().objects())
      if (o.attributes.count("kind") && o.attributes.at("kind") == kind) out.push_back(o.id);
    return out;
  }
  gp_Pnt endpoint(const ObjectId& id, bool end) {
    const auto* o = m.document().find(id);
    EXPECT_NE(o, nullptr);
    if (const auto* member = std::get_if<LinearMember>(&o->recipe.base))
      return (end ? member->end : member->start).Transformed(m.document().worldTransform(*o));
    const auto& ref = std::get<ComponentInstance>(o->recipe.base);
    const auto* stored = m.document().findDefinition(ref.definitionId);
    EXPECT_NE(stored, nullptr);
    const auto own = applyOverrides(*stored, ref.overrides, m.document().regenContext());
    EXPECT_TRUE(own);
    const auto& member = std::get<LinearMember>(own->objects[0].recipe.base);
    return (end ? member.end : member.start)
        .Transformed(m.document().worldTransform(*o) * own->objects[0].transform);
  }
  void moveLevel(const std::string& id, double delta) {
    auto levels = m.levels();
    for (auto& level : levels)
      if (level.id == id) level.elevation += delta;
    ASSERT_TRUE(m.setLevels(levels));
  }
};
}  // namespace

TEST_F(Frames, StandardFramePreviewApplyAndOneUndo) {
  const auto preview = ok("generate_frame");
  EXPECT_TRUE(preview["preview"]);
  EXPECT_GT(preview["parts"].get<int>(), 700);
  EXPECT_TRUE(m.document().objects().empty());
  EXPECT_TRUE(m.document().definitions().empty());
  EXPECT_TRUE(m.levels().empty());
  const auto made = frame();
  EXPECT_EQ(made["counts"]["through_column"], 36);
  EXPECT_EQ(made["counts"]["floor_sheathing"], 80);
  EXPECT_EQ(made["counts"]["rafter"], 46);
  EXPECT_LT(made["new_definitions"].get<int>(), made["parts"].get<int>() / 10);
  ASSERT_EQ(m.levels().size(), 6u);
  EXPECT_EQ(m.history().undoCount(), 1u);
  const auto count = m.document().objects().size(), defs = m.document().definitions().size();
  EXPECT_GT(defs, 0u);
  const auto through = role("through_column");
  ASSERT_EQ(through.size(), 36u);
  EXPECT_NEAR(endpoint(through[0], false).Z(), 540, 1e-6);
  EXPECT_NEAR(endpoint(through[0], true).Z(), 5960, 1e-6);
  const auto first = std::get<ComponentInstance>(m.document().find(through[0])->recipe.base).definitionId;
  const auto next = std::get<ComponentInstance>(m.document().find(through[1])->recipe.base).definitionId;
  EXPECT_EQ(first, next);
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(m.document().objects().empty());
  EXPECT_TRUE(m.document().definitions().empty());
  EXPECT_TRUE(m.levels().empty());
  ASSERT_TRUE(m.redo());
  EXPECT_EQ(m.document().objects().size(), count);
  EXPECT_EQ(m.document().definitions().size(), defs);
  std::cout << "Frame: " << made["parts"] << " parts, " << made["hardware"] << " hardware/markers, " << defs
            << " definitions, " << made["generation_ms"] << " ms\n";
}

TEST_F(Frames, LevelsStretchSharedColumnsAndMoveRoofWithOneUndo) {
  const auto made = frame();
  const auto column = role("through_column").front(), rafter = role("rafter").front(),
             beam = role("floor_beam").front();
  const auto columnFoot = endpoint(column, false), roofBefore = endpoint(rafter, true),
             beamBefore = endpoint(beam, false);
  moveLevel(made["levels"]["eave_top"], 300);
  EXPECT_NEAR(endpoint(column, false).Z(), columnFoot.Z(), 1e-6);
  EXPECT_NEAR(endpoint(column, true).Z(), 6260, 1e-6);
  EXPECT_NEAR(endpoint(rafter, true).Z(), roofBefore.Z() + 300, 1e-6);
  EXPECT_NEAR(endpoint(beam, false).Z(), beamBefore.Z(), 1e-6);
  const auto ref = std::get<ComponentInstance>(m.document().find(column)->recipe.base);
  EXPECT_FALSE(ref.overrides.empty());
  const auto* def = m.document().findDefinition(ref.definitionId);
  EXPECT_NEAR(std::get<LinearMember>(def->objects[0].recipe.base).end.Z(), 5420, 1e-6);
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(endpoint(column, true).Z(), 5960, 1e-6);
  ASSERT_TRUE(m.redo());
  const auto regenerated = m.generateFrame(json{{"replace", made["frame"]}}.dump());
  ASSERT_TRUE(regenerated) << regenerated.error();
  EXPECT_NEAR(endpoint(role("through_column").front(), true).Z(), 6260, 1e-6);
}

TEST_F(Frames, ReopenedFrameRegeneratesAndSharesDefinitions) {
  auto made = frame({{"x_bays", 4}, {"y_bays", 4}});
  const std::string path = testing::TempDir() + "timber_frame.sbr";
  ASSERT_TRUE(m.save(path));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  const auto generated = loaded.generateFrame(json{{"replace", made["frame"]}}.dump());
  ASSERT_TRUE(generated) << generated.error();
  EXPECT_EQ(json::parse(*generated)["new_definitions"], 0);
  const auto count = loaded.document().objects().size(), defs = loaded.document().definitions().size();
  ASSERT_TRUE(loaded.undo());
  ASSERT_TRUE(loaded.redo());
  EXPECT_EQ(loaded.document().objects().size(), count);
  EXPECT_EQ(loaded.document().definitions().size(), defs);
}

TEST_F(Frames, HandEditedPartsAreProtectedAndCanBePreserved) {
  auto made = frame({{"x_bays", 4}, {"y_bays", 4}});
  const auto id = role("through_column").front();
  const auto key = m.document().find(id)->attributes.at("frame_key");
  Modeler::MemberChange change;
  change.width = 137;
  ASSERT_TRUE(m.changeMember(id, change));
  EXPECT_TRUE(std::holds_alternative<LinearMember>(m.document().find(id)->recipe.base));
  const auto history = m.history().undoCount(), count = m.document().objects().size();
  const auto failed = m.generateFrame(json{{"replace", made["frame"]}}.dump());
  EXPECT_FALSE(failed);
  EXPECT_NE(failed.error().find("requires_decision"), std::string::npos);
  EXPECT_EQ(m.history().undoCount(), history);
  EXPECT_EQ(m.document().objects().size(), count);
  const auto kept = m.generateFrame(json{{"replace", made["frame"]}, {"on_modified", "preserve"}}.dump());
  ASSERT_TRUE(kept) << kept.error();
  ASSERT_NE(m.document().find(id), nullptr);
  EXPECT_EQ(std::get<LinearMember>(m.document().find(id)->recipe.base).section.width, 137);
  int copies = 0;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key") && o.attributes.at("frame_key") == key) ++copies;
  EXPECT_EQ(copies, 1);
  EXPECT_FALSE(m.generateFrame(json{{"replace", made["frame"]}}.dump()));
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().objects().size(), count);
}

TEST_F(Frames, DeletedPartsStayDeletedWhenPreservingEdits) {
  auto made = frame({{"x_bays", 3}, {"y_bays", 3}});
  const auto id = role("rafter").front();
  const std::string key = m.document().find(id)->attributes.at("frame_key");
  ASSERT_TRUE(m.remove({id}));
  EXPECT_FALSE(m.generateFrame(json{{"replace", made["frame"]}}.dump()));
  const auto kept = m.generateFrame(json{{"replace", made["frame"]}, {"on_modified", "preserve"}}.dump());
  ASSERT_TRUE(kept) << kept.error();
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key")) EXPECT_NE(o.attributes.at("frame_key"), key);
  const auto repeated = m.generateFrame(json{{"replace", made["frame"]}, {"on_modified", "preserve"}}.dump());
  ASSERT_TRUE(repeated) << repeated.error();
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key")) EXPECT_NE(o.attributes.at("frame_key"), key);
}

TEST_F(Frames, ReusingAnOwnedGridKeepsItAndBeamDepthKeepsTheTop) {
  const auto made = frame({{"x_bays", 3}, {"y_bays", 3}});
  const auto grid = made["grid"].get<std::string>();
  const auto changed = m.generateFrame(json{{"replace", made["frame"]}, {"inputs", {{"grid", grid}}}}.dump());
  ASSERT_TRUE(changed) << changed.error();
  ASSERT_NE(m.document().find(grid), nullptr);
  ASSERT_NE(m.gridSpec(grid), nullptr);
  const auto id = role("floor_beam").front();
  const double before = geom::exactBounds(m.document().worldShape(id)).CornerMax().Z();
  const auto info = m.memberInfo(id);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->anchor.up, 1);
  Modeler::MemberChange deep;
  deep.depth = 300;
  ASSERT_TRUE(m.changeMember(id, deep));
  EXPECT_NEAR(geom::exactBounds(m.document().worldShape(id)).CornerMax().Z(), before, 1e-6);
}

TEST_F(Frames, RoofVariantsPartialSecondFloorAndMixedFloorZones) {
  const auto made = frame(
      {{"x_bays", 6},
       {"y_bays", 4},
       {"roof", "shed"},
       {"ridge_axis", "y"},
       {"second_floor", {{"x", {0, 4}}, {"y", {0, 4}}}},
       {"floor_zones", json::array({{{"storey", 1}, {"x", {0, 2}}, {"y", {0, 4}}, {"system", "joistless"}}})},
       {"braces", json::array({{{"storey", 1}, {"from", {0, 0}}, {"to", {1, 0}}}})}});
  EXPECT_EQ(made["counts"]["brace"], 1);
  EXPECT_EQ(made["counts"]["brace_plate"], 2);
  EXPECT_TRUE(role("ridge").empty());
  bool lower = false;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("frame_key") && o.attributes.at("frame_key").find("roof/lower/") == 0)
      lower = true;
  EXPECT_TRUE(lower);
  EXPECT_GT(made["warnings"].size(), 2u);
  ASSERT_TRUE(m.undo());
  const auto single =
      frame({{"storeys", 1}, {"x_bays", 3}, {"y_bays", 2}, {"roof", "gable"}, {"ridge_axis", "y"}});
  EXPECT_FALSE(single["levels"].contains("second_fl"));
  EXPECT_TRUE(role("through_column").empty());
  EXPECT_FALSE(role("column").empty());
  EXPECT_EQ(single["counts"]["ridge"], 1);
}

TEST_F(Frames, ExistingGridAndOpeningHeadersUseWorldCoordinates) {
  const auto g = m.createGridLines(gp_Pnt(1000, 2000, 0), {910, 910, 910}, {910, 910});
  ASSERT_TRUE(g);
  auto walls = m.createWalls({gp_Pnt(1000, 2000, 600), gp_Pnt(3730, 2000, 600)}, 120, 2400);
  ASSERT_TRUE(walls);
  const auto cut = m.placeOpenings(walls->front(), gp::DY(), 1, 800, 1000, 700);
  ASSERT_TRUE(cut);
  const auto made = frame({{"grid", g->grid}, {"storeys", 1}, {"opening_hosts", {*cut}}});
  EXPECT_EQ(made["grid"], g->grid);
  EXPECT_EQ(made["counts"]["lintel"], 1);
  EXPECT_EQ(made["counts"]["window_sill"], 1);
  const auto lintel = role("lintel").front();
  EXPECT_NEAR(endpoint(lintel, false).Distance(endpoint(lintel, true)), 800, 1e-6);
  EXPECT_GT(endpoint(lintel, false).X(), 1000);
  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(100, 200, 0));
  const auto before = endpoint(role("column").front(), false);
  ASSERT_TRUE(m.transform({g->grid}, delta, "Move Grid"));
  const auto after = endpoint(role("column").front(), false);
  EXPECT_NEAR(after.X() - before.X(), 100, 1e-6);
  EXPECT_NEAR(after.Y() - before.Y(), 200, 1e-6);
}

TEST_F(Frames, StalePreviewBadInputsAndBatchAreAtomic) {
  const auto preview = ok("generate_frame", {{"inputs", {{"x_bays", 3}, {"y_bays", 3}}}});
  const auto grid = m.createGridLines(gp::Origin(), {910}, {910});
  ASSERT_TRUE(grid);
  EXPECT_FALSE(
      call("generate_frame", {{"preview", false}, {"expected_revision", preview["revision"]}})["ok"]);
  const auto size = m.document().objects().size(), defs = m.document().definitions().size(),
             history = m.history().undoCount();
  for (const auto& inputs :
       {json{{"roof_pitch", 0}}, json{{"x_bays", 0}}, json{{"storeys", 3}}, json{{"module", -1}}}) {
    EXPECT_FALSE(
        call("generate_frame",
             {{"preview", false}, {"expected_revision", m.history().revision()}, {"inputs", inputs}})["ok"]);
    EXPECT_EQ(m.document().objects().size(), size);
    EXPECT_EQ(m.document().definitions().size(), defs);
    EXPECT_EQ(m.history().undoCount(), history);
  }
  const json step = {{"command", "generate_frame"},
                     {"args", {{"preview", false}, {"inputs", {{"x_bays", 2}, {"y_bays", 2}}}}}};
  const auto trial = ok("preview", {{"commands", json::array({step})}});
  EXPECT_TRUE(trial["would_succeed"]);
  EXPECT_FALSE(call("batch", {{"commands", json::array({step, {{"command", "no_such_command"}}})}})["ok"]);
  EXPECT_EQ(m.document().objects().size(), size);
  EXPECT_EQ(m.document().definitions().size(), defs);
  EXPECT_EQ(m.history().undoCount(), history);
}

TEST_F(Frames, HardwareUsesSharedShapesAndDoesNotFollowAnEditedColumn) {
  auto made = frame();
  const auto bolt = role("anchor_bolt").front(), column = role("through_column").front();
  const auto before = m.document().worldBounds(*m.document().find(bolt)).CornerMin();
  gp_Trsf delta;
  delta.SetTranslation(gp_Vec(200, 0, 0));
  ASSERT_TRUE(m.transform({column}, delta, "Move Column"));
  EXPECT_NEAR(m.document().worldBounds(*m.document().find(bolt)).CornerMin().X(), before.X(), 1e-6);
  const auto started = std::chrono::steady_clock::now();
  m.setTagVisible("金物", false);
  for (const auto& o : m.document().objects())
    if (o.layer == "金物") EXPECT_FALSE(m.document().isVisible(o.id));
  const double elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::cout << "Frame tag toggle and visibility checks: " << elapsed << " ms\n";
  EXPECT_GT(made["hardware"].get<int>(), 250);
}

TEST_F(Frames, MovingAnAncestorProtectsTheFramesWorldPlacement) {
  const auto parent = m.createGridLines(gp::Origin(), {910}, {910});
  ASSERT_TRUE(parent);
  ASSERT_TRUE(m.setActiveGroup(parent->grid));
  const auto made = frame({{"x_bays", 2}, {"y_bays", 2}});
  const auto column = role("through_column").front();
  const auto before = endpoint(column, false);
  gp_Trsf shift;
  shift.SetTranslation(gp_Vec(200, 300, 0));
  ASSERT_TRUE(m.transform({parent->grid}, shift, "Move Building"));
  const auto count = m.history().undoCount();
  const auto result = m.generateFrame(json{{"replace", made["frame"]}}.dump());
  EXPECT_FALSE(result);
  EXPECT_NE(result.error().find("requires_decision"), std::string::npos);
  EXPECT_EQ(m.history().undoCount(), count);
  EXPECT_NEAR(endpoint(column, false).X(), before.X() + 200, 1e-6);
  EXPECT_NEAR(endpoint(column, false).Y(), before.Y() + 300, 1e-6);
}
