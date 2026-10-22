// Transaction guarantees shared by GUI and AI (architecture review A / B):
// atomic batches also roll back persistent settings, and preview, proposal
// and commit report the same typed change set.

#include <cmath>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"

using nlohmann::json;
using sbr::api::CommandApi;

namespace {

struct Api {
  sbr::doc::Modeler modeler;
  CommandApi api{modeler};

  json call(const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const std::string& command, json args = json::object()) {
    json r = call(command, args);
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  }
  std::string box(double x = 0) {
    return ok("create_box", {{"origin", {x, 0, 0}}, {"width", 1000}, {"depth", 1000}, {"height", 1000}})["id"];
  }
};

json step(const std::string& command, json args) { return {{"command", command}, {"args", args}}; }

// The modified entry for `id` in a change set, or null.
json modifiedEntry(const json& changes, const std::string& id) {
  for (const json& m : changes["modified"]) {
    if (m["id"] == id) return m;
  }
  return nullptr;
}

bool hasAspect(const json& entry, const std::string& aspect) {
  if (entry.is_null()) return false;
  for (const json& a : entry["aspects"]) {
    if (a == aspect) return true;
  }
  return false;
}

}  // namespace

// R01: settings changed inside a failing atomic batch come back too.
TEST(Transactions, R01_FailedBatchRestoresSettingsAndHistoryState) {
  Api a;
  a.box();
  a.modeler.history().markClean();
  const std::size_t undo = a.modeler.history().undoCount();
  const long long revision = a.modeler.history().revision();

  const json r = a.call("batch", {{"commands",
                                   {step("set_tag_visible", {{"tag", "Default"}, {"visible", false}}),
                                    step("set_section", {{"origin", {0, 0, 1200}}, {"normal", {0, 0, 1}}}),
                                    step("create_box", {{"origin", {0, 0, 0}}, {"width", 500}, {"depth", 500},
                                                        {"height", 500}}),
                                    step("extrude", {{"id", "no-such-object"}, {"distance", 100}})}}});
  ASSERT_FALSE(r["ok"]);
  const auto& settings = a.modeler.document().settings();
  EXPECT_TRUE(settings.hiddenTags.empty());
  EXPECT_TRUE(settings.sections.empty());
  EXPECT_EQ(a.modeler.document().objects().size(), 1u);
  EXPECT_EQ(a.modeler.history().undoCount(), undo);
  EXPECT_EQ(a.modeler.history().revision(), revision);
  EXPECT_TRUE(a.modeler.history().isClean());
}

// Settings-only batches (and single settings edits) are one undo step.
TEST(Transactions, R01_SettingsEditsAreUndoSteps) {
  Api a;
  a.box();
  const std::size_t undo = a.modeler.history().undoCount();
  a.ok("batch", {{"commands",
                  {step("set_tag_visible", {{"tag", "Default"}, {"visible", false}}),
                   step("set_section", {{"origin", {0, 0, 1200}}, {"normal", {0, 0, 1}}})}}});
  EXPECT_EQ(a.modeler.history().undoCount(), undo + 1);
  EXPECT_EQ(a.modeler.document().settings().hiddenTags.count("Default"), 1u);
  ASSERT_EQ(a.modeler.document().settings().sections.size(), 1u);
  a.ok("undo");
  EXPECT_TRUE(a.modeler.document().settings().hiddenTags.empty());
  EXPECT_TRUE(a.modeler.document().settings().sections.empty());
  a.ok("redo");
  EXPECT_EQ(a.modeler.document().settings().hiddenTags.count("Default"), 1u);

  // Outside a batch as well: a tag toggle is undoable and moves the revision
  // (so a pending proposal based on the old state is refused).
  const long long revision = a.modeler.history().revision();
  a.ok("set_tag_visible", {{"tag", "Default"}, {"visible", true}});
  EXPECT_NE(a.modeler.history().revision(), revision);
  a.ok("undo");
  EXPECT_EQ(a.modeler.document().settings().hiddenTags.count("Default"), 1u);
  // Setting the same value again records nothing.
  const std::size_t steps = a.modeler.history().undoCount();
  a.ok("set_tag_visible", {{"tag", "Default"}, {"visible", false}});
  EXPECT_EQ(a.modeler.history().undoCount(), steps);
}

// R03: each kind of change is reported, and preview reports exactly what
// the commit then records.
TEST(Transactions, R03_PreviewAndCommitReportTheSameChanges) {
  struct Case {
    const char* name;
    std::function<json(Api&, const std::string& box, const std::string& other)> commands;
    const char* aspect;
  };
  const std::vector<Case> cases = {
      {"rotate about the origin",
       [](Api&, const std::string& box, const std::string&) {
         return json::array({step("rotate", {{"ids", {box}}, {"axis_origin", {0, 0, 0}}, {"axis_direction", {0, 0, 1}},
                                              {"angle", 90}})});
       },
       "placement"},
      {"hide",
       [](Api&, const std::string& box, const std::string&) {
         return json::array({step("hide", {{"ids", {box}}})});
       },
       "visibility"},
      {"paint",
       [](Api&, const std::string& box, const std::string&) {
         return json::array({step("paint", {{"ids", {box}}, {"color", {1, 0, 0}}})});
       },
       "attributes"},
      {"rename",
       [](Api&, const std::string& box, const std::string&) {
         return json::array({step("rename", {{"id", box}, {"name", "Wall"}})});
       },
       "attributes"},
      {"group (parent change)",
       [](Api&, const std::string& box, const std::string& other) {
         return json::array({step("group", {{"ids", {box, other}}})});
       },
       "hierarchy"},
      {"parameter",
       [](Api&, const std::string& box, const std::string&) {
         return json::array({step("set_parameter", {{"id", box}, {"key", "op0.distance"}, {"value", 2500}}),
                             step("set_tag_visible", {{"tag", "Default"}, {"visible", false}})});
       },
       "geometry"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    Api a;
    const std::string box = a.box();
    const std::string other = a.box(3000);
    const json commands = c.commands(a, box, other);
    const json preview = a.ok("preview", {{"commands", commands}});
    ASSERT_TRUE(preview["would_succeed"].get<bool>()) << preview.dump();
    const json previewed = preview["changes"];
    EXPECT_TRUE(hasAspect(modifiedEntry(previewed, box), c.aspect)) << previewed.dump();
    // Legacy summary stays consistent with the typed change set.
    EXPECT_EQ(preview["changed"].size(), previewed["modified"].size());

    const json committed = a.ok("batch", {{"commands", commands}, {"expected_revision", preview["revision"]}});
    // Ids of objects created by the batch differ between the scratch copy
    // and the real model: compare everything else.
    json p = previewed, k = committed["changes"];
    for (json* j : {&p, &k}) (*j)["added"] = (*j)["added"].size();
    EXPECT_EQ(p, k) << "preview " << previewed.dump() << "\ncommit " << committed["changes"].dump();
  }
}

// Definition edits name the definition and the instances they move.
TEST(Transactions, R03_DefinitionEditsAreReported) {
  Api a;
  const std::string box = a.box();
  const std::string instance = a.ok("make_component", {{"ids", {box}}})["id"];
  a.ok("copy_array", {{"ids", {instance}}, {"vector", {2000, 0, 0}}, {"count", 2}});
  const auto& placed = std::get<sbr::doc::ComponentInstance>(a.modeler.document().find(instance)->recipe.base);
  const std::string definition = placed.definitionId;
  const std::string child = a.modeler.document().findDefinition(definition)->objects.front().id;
  const json preview =
      a.ok("preview", {{"commands", {step("set_definition_parameter", {{"definition", definition},
                                                                       {"object", child},
                                                                       {"key", "op0.distance"},
                                                                       {"value", 2000}})}}});
  const json& changes = preview["changes"];
  ASSERT_EQ(changes["definitions"].size(), 1u) << changes.dump();
  EXPECT_EQ(changes["definitions"][0], definition);
  EXPECT_EQ(changes["modified"].size(), 3u);  // every instance shows the new geometry
  EXPECT_TRUE(hasAspect(modifiedEntry(changes, instance), "geometry"));
}

// Review C: requests are classified before running, so hosts can tell
// reads from writes (unknown or malformed requests count as writes).
TEST(Transactions, EffectClassification) {
  Api a;
  using E = CommandApi::Effect;
  auto effect = [&](const json& request) { return a.api.effectOf(request.dump()); };
  EXPECT_EQ(effect(step("list_objects", json::object())), E::Read);
  EXPECT_EQ(effect(step("get_object", {{"id", "x"}})), E::Read);
  EXPECT_EQ(effect(step("describe", json::object())), E::Read);
  EXPECT_EQ(effect(step("preview", {{"commands", json::array()}})), E::Read);
  EXPECT_EQ(effect(step("create_box", json::object())), E::ModelWrite);
  EXPECT_EQ(effect(step("set_tag_visible", json::object())), E::ModelWrite);
  EXPECT_EQ(effect(step("save", {{"path", "x.sbr"}})), E::ExternalIo);
  EXPECT_EQ(effect(step("no_such_command", json::object())), E::ModelWrite);
  EXPECT_EQ(a.api.effectOf("not json"), E::ModelWrite);
  // A batch has the largest effect of its steps.
  EXPECT_EQ(effect(step("batch", {{"commands", {step("list_objects", json::object())}}})), E::Read);
  EXPECT_EQ(effect(step("batch", {{"commands", {step("get_object", {{"id", "x"}}), step("delete", {{"ids", {"x"}}})}}})),
            E::ModelWrite);
  // App commands carry their own effect.
  for (CommandApi::Extension e : CommandApi::appCommandSpecs()) {
    e.run = [](const std::string&) { return std::string("{}"); };
    a.api.addExtension(e);
  }
  EXPECT_EQ(effect(step("get_selection", json::object())), E::Read);
  EXPECT_EQ(effect(step("capture_view", json::object())), E::Read);
  EXPECT_EQ(effect(step("set_view", {{"view", "top"}})), E::ViewWrite);
  EXPECT_EQ(effect(step("go_to_scene", {{"save", true}})), E::ModelWrite);
  // The schema tells agents too.
  for (const json& tool : json::parse(a.api.schemaJson())) {
    EXPECT_TRUE(tool.contains("effect")) << tool["name"];
    if (tool["name"] == "list_objects") EXPECT_EQ(tool["effect"], "read");
    if (tool["name"] == "batch") EXPECT_EQ(tool["effect"], "model_write");
  }
}

// Review §8: one instruction edits a whole set of objects, refuses when the
// set is not what was meant, and is one undo step that fails as a whole.
TEST(Transactions, EditEachIsGuardedAtomicAndPreviewable) {
  Api a;
  std::vector<std::string> windows;
  for (int i = 0; i < 4; ++i) {
    const std::string w = a.box(i * 2000.0);
    a.ok("rename", {{"id", w}, {"name", "Window " + std::to_string(i + 1)}});
    windows.push_back(w);
  }
  const std::string wall = a.box(20000);
  a.ok("rename", {{"id", wall}, {"name", "Wall"}});
  const std::size_t steps = a.modeler.history().undoCount();
  const json where = {{"name", "Window"}};
  const json paint = step("paint", {{"ids", {"$id"}}, {"color", "#3366cc"}});

  // Wrong expectation: refused before anything changes.
  const json refused = a.call("edit_each", {{"where", where}, {"commands", {paint}}, {"expect_count", 3}});
  EXPECT_FALSE(refused["ok"]);
  EXPECT_NE(refused["error"].get<std::string>().find("matches 4"), std::string::npos);
  EXPECT_EQ(a.modeler.history().undoCount(), steps);
  EXPECT_FALSE(a.call("edit_each", {{"where", {{"name", "Door"}}}, {"commands", {paint}}})["ok"]);

  // Preview, then commit with the same change set; "$index" staggers.
  const json commands = {paint, step("move", {{"ids", {"$id"}}, {"vector", {0, 0, "$index"}}})};
  const json edit = step("edit_each", {{"where", where}, {"commands", commands}, {"expect_count", 4}});
  const json preview = a.ok("preview", {{"commands", {edit}}});
  ASSERT_TRUE(preview["would_succeed"].get<bool>()) << preview.dump();
  EXPECT_EQ(preview["changes"]["modified"].size(), 4u);
  const json done = a.ok("edit_each", {{"where", where}, {"commands", commands}, {"expect_count", 4}});
  EXPECT_EQ(done["targets"].size(), 4u);
  EXPECT_EQ(done["changes"], preview["changes"]);
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  const auto* last = a.modeler.document().find(windows[3]);
  EXPECT_NEAR(last->transform.TranslationPart().Z(), 3.0, 1e-9);
  EXPECT_NEAR(last->color.b, 0.8, 1e-9);
  EXPECT_NEAR(a.modeler.document().find(wall)->color.b, sbr::doc::Color{}.b, 1e-12);  // untouched

  // A failure on any target reverts every target.
  a.ok("undo");
  a.ok("delete", {{"ids", {windows[2]}}});
  const json bad = a.call("edit_each", {{"where", where},
                                        {"commands", {step("set_parameter", {{"id", "$id"}, {"key", "op0.distance"},
                                                                             {"value", "$index"}})}}});
  EXPECT_FALSE(bad["ok"]);  // the first window would get distance 0
  EXPECT_NEAR(a.ok("get_object", {{"id", windows[1]}})["volume"].get<double>(), 1e9, 1e-3);
}

// Columns on a structural grid: one undo step, refuses ambiguous names.
TEST(Transactions, PlaceOnGridPutsCopiesOnCrossings) {
  Api a;
  a.ok("create_grid", {{"x_spacings", {3600, 3600, 3600}}, {"y_spacings", {6000, 6000}}});
  const std::string column =
      a.ok("create_box", {{"origin", {-5000, -5000, 0}}, {"width", 400}, {"depth", 400}, {"height", 3000}})["id"];
  const std::size_t steps = a.modeler.history().undoCount();
  const json r = a.ok("place_on_grid", {{"source", column}, {"skip", {"X4/Y3"}}});
  EXPECT_EQ(r["count"], 11);  // 4 x 3 crossings, one skipped
  const std::string at = r["placed"]["X3/Y2"];
  const json b = a.ok("get_object", {{"id", at}})["bounds"];
  EXPECT_NEAR(b["min"][0].get<double>(), 7200 - 200, 1e-6);
  EXPECT_NEAR(b["min"][1].get<double>(), 6000 - 200, 1e-6);
  EXPECT_NEAR(b["min"][2].get<double>(), 0, 1e-6);
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  a.ok("undo");
  EXPECT_FALSE(a.call("get_object", {{"id", at}})["ok"]);

  const json some = a.ok("place_on_grid", {{"source", column}, {"x", {"X1", "X2"}}, {"y", "Y1"}});
  EXPECT_EQ(some["count"], 2);
  EXPECT_FALSE(a.call("place_on_grid", {{"source", column}, {"x", {"X9"}}})["ok"]);
  // A second grid reuses the names: refused instead of guessing.
  a.ok("create_grid", {{"origin", {50000, 0, 0}}, {"x_spacings", {3000}}});
  EXPECT_FALSE(a.call("place_on_grid", {{"source", column}, {"x", {"X1"}}, {"y", {"Y1"}}})["ok"]);
}
