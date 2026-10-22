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
       "material"},
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
  EXPECT_NEAR(a.modeler.document().displayColor(*last).b, 0.8, 1e-9);
  EXPECT_NEAR(a.modeler.document().displayColor(*a.modeler.document().find(wall)).b, sbr::doc::Color{}.b,
              1e-12);  // untouched

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

// Meaning attributes: set in bulk, found by filters, grouped, saved.
TEST(Transactions, AttributesAddressObjectsByMeaning) {
  Api a;
  std::vector<std::string> ids;
  for (int i = 0; i < 6; ++i) ids.push_back(a.box(i * 2000.0));
  a.ok("set_attribute", {{"ids", {ids[0], ids[1], ids[2]}}, {"key", "level"}, {"value", "1F"}});
  a.ok("set_attribute", {{"ids", {ids[3], ids[4]}}, {"key", "level"}, {"value", "2F"}});
  a.ok("set_attribute", {{"ids", {ids[4]}}, {"key", "kind"}, {"value", "window"}});
  const std::size_t steps = a.modeler.history().undoCount();
  a.ok("set_attribute", {{"ids", {ids[0]}}, {"key", "level"}, {"value", "1F"}});  // no change
  EXPECT_EQ(a.modeler.history().undoCount(), steps);

  EXPECT_EQ(a.ok("query_objects", {{"attributes", {{"level", "2F"}}}})["total"], 2);
  EXPECT_EQ(a.ok("query_objects", {{"attributes", {{"level", "2F"}, {"kind", "window"}}}})["total"], 1);
  EXPECT_EQ(a.ok("query_objects", {{"attributes", {{"level", "*"}}}})["total"], 5);
  const json groups = a.ok("aggregate", {{"group_by", "attribute:level"}})["groups"];
  ASSERT_EQ(groups.size(), 3u);  // "", 1F, 2F
  EXPECT_EQ(groups[1]["key"], "1F");
  EXPECT_EQ(groups[1]["count"], 3);

  // Bulk edit by meaning; the attribute change is in the change set.
  const json done = a.ok("edit_each", {{"where", {{"attributes", {{"level", "2F"}}}}},
                                       {"commands", {step("set_attribute", {{"ids", {"$id"}}, {"key", "level"},
                                                                            {"value", "3F"}})}},
                                       {"expect_count", 2}});
  EXPECT_EQ(done["changes"]["modified"].size(), 2u);
  EXPECT_EQ(done["changes"]["modified"][0]["aspects"], json::array({"attributes"}));
  EXPECT_EQ(a.ok("get_object", {{"id", ids[3]}})["attributes"]["level"], "3F");
  a.ok("set_attribute", {{"ids", {ids[4]}}, {"key", "kind"}, {"value", ""}});  // removes
  EXPECT_FALSE(a.ok("get_object", {{"id", ids[4]}})["attributes"].contains("kind"));
  EXPECT_FALSE(a.call("set_attribute", {{"ids", {ids[0]}}, {"key", ""}, {"value", "x"}})["ok"]);

  // Saved with the model.
  const std::string path = ::testing::TempDir() + "attributes.sbr";
  a.ok("save", {{"path", path}});
  Api b;
  b.ok("open", {{"path", path}});
  EXPECT_EQ(b.ok("query_objects", {{"attributes", {{"level", "3F"}}}})["total"], 2);
}

// Architectural intents shared by GUI and AI: openings keep their centre
// and sill when resized, levels come from heights, alignment by bounds.
TEST(Transactions, OpeningsResizeAroundCentreAndSill) {
  Api a;
  auto wallWithOpenings = [&](double y) {
    const std::string wall =
        a.ok("create_box", {{"origin", {0, y, 0}}, {"width", 6000}, {"depth", 200}, {"height", 3000}})["id"];
    std::string host = wall;
    for (const auto& [x, z, w, h] : {std::array<double, 4>{1000, 0, 900, 2100}, std::array<double, 4>{3500, 900, 1600, 1200}}) {
      const std::string profile = a.ok("create_rectangle", {{"origin", {x, y, z}}, {"normal", {0, -1, 0}},
                                                            {"width", w}, {"height", h}})["id"];
      host = a.ok("push_pull", {{"id", profile}, {"face", 1}, {"distance", -200}})["id"];
    }
    return host;
  };
  const std::string north = wallWithOpenings(0);
  const std::string south = wallWithOpenings(5000);

  json list = a.ok("list_openings", {{"ids", {north}}})["hosts"];
  ASSERT_EQ(list.size(), 1u);
  const json windowBefore = list[0]["openings"][1];
  EXPECT_NEAR(windowBefore["width"].get<double>(), 1600, 1e-9);
  EXPECT_NEAR(windowBefore["sill"].get<double>(), 900, 1e-6);
  const double centreX = windowBefore["center"][0];

  // Window only, wider and taller: centre x and sill stay.
  const std::size_t window = windowBefore["index"];
  a.ok("set_opening_size", {{"host", north}, {"openings", {window}}, {"width", 2000}, {"height", 1500}});
  json after = a.ok("list_openings", {{"ids", {north}}})["hosts"][0]["openings"];
  EXPECT_NEAR(after[1]["width"].get<double>(), 2000, 1e-9);
  EXPECT_NEAR(after[1]["center"][0].get<double>(), centreX, 1e-6);
  EXPECT_NEAR(after[1]["sill"].get<double>(), 900, 1e-6);
  EXPECT_NEAR(after[0]["width"].get<double>(), 900, 1e-9);  // the door is untouched
  EXPECT_NEAR(a.ok("get_object", {{"id", north}})["volume"].get<double>(),
              6000.0 * 200 * 3000 - 900.0 * 200 * 2100 - 2000.0 * 200 * 1500, 1.0);
  // Keeping the centre instead moves the sill.
  a.ok("set_opening_size", {{"host", north}, {"openings", {window}}, {"height", 1300}, {"keep", "center"}});
  after = a.ok("list_openings", {{"ids", {north}}})["hosts"][0]["openings"];
  EXPECT_NEAR(after[1]["sill"].get<double>(), 900 + 100, 1e-6);

  // Every opening of every wall at once, one undo step; still parametric.
  const std::size_t steps = a.modeler.history().undoCount();
  a.ok("edit_each", {{"where", {{"kind", "Solid"}}},
                     {"commands", {step("set_opening_size", {{"host", "$id"}, {"width", 1200}})}},
                     {"expect_count", 2}});
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  const json all = a.ok("list_openings");
  for (const json& host : all["hosts"]) {
    for (const json& o : host["openings"]) EXPECT_NEAR(o["width"].get<double>(), 1200, 1e-9);
  }
  bool found = false;
  const json southWall = a.ok("get_object", {{"id", south}});
  for (const json& p : southWall["parameters"]) {
    found = found || (p["key"] == "in1.width" && std::abs(p["value"].get<double>() - 1200) < 1e-9);
  }
  EXPECT_TRUE(found);
  a.ok("undo");
  EXPECT_NEAR(a.ok("list_openings", {{"ids", {south}}})["hosts"][0]["openings"][1]["width"].get<double>(), 1600, 1e-9);

  EXPECT_FALSE(a.call("set_opening_size", {{"host", north}, {"openings", {7}}, {"width", 1000}})["ok"]);
  EXPECT_FALSE(a.call("set_opening_size", {{"host", north}, {"width", -5}})["ok"]);
  EXPECT_FALSE(a.call("set_opening_size", {{"host", a.box(20000)}, {"width", 1000}})["ok"]);
}

TEST(Transactions, LevelsFromHeightsAndAlignment) {
  Api a;
  auto boxAt = [&](double x, double z) {
    return a.ok("create_box", {{"origin", {x, 0, z}}, {"width", 1000}, {"depth", 1000}, {"height", 500}})["id"]
        .get<std::string>();
  };
  const std::string ground = boxAt(0, 0), first = boxAt(2000, 3000), second = boxAt(4000, 6150), pit = boxAt(6000, -800);
  const std::size_t steps = a.modeler.history().undoCount();
  const json r = a.ok("assign_levels", {{"levels", {{{"name", "2F"}, {"elevation", 3000}},
                                                    {{"name", "1F"}, {"elevation", 0}},
                                                    {{"name", "3F"}, {"elevation", 6000}}}}});
  EXPECT_EQ(r["below_all_levels"], json::array({pit}));
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  EXPECT_EQ(a.ok("get_object", {{"id", ground}})["attributes"]["level"], "1F");
  EXPECT_EQ(a.ok("get_object", {{"id", first}})["attributes"]["level"], "2F");
  EXPECT_EQ(a.ok("get_object", {{"id", second}})["attributes"]["level"], "3F");
  EXPECT_EQ(a.ok("aggregate", {{"attributes", {{"level", "2F"}}}})["count"], 1);
  // Assigning again changes nothing: no empty undo step.
  a.ok("assign_levels", {{"levels", {{{"name", "1F"}, {"elevation", 0}}, {{"name", "2F"}, {"elevation", 3000}},
                                     {{"name", "3F"}, {"elevation", 6000}}}}});
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);

  // Align the bottoms to the first one, then the right edges to a value.
  a.ok("align", {{"ids", {ground, first, second}}, {"axis", "z"}, {"edge", "min"}});
  for (const std::string& id : {first, second}) {
    EXPECT_NEAR(a.ok("get_object", {{"id", id}})["bounds"]["min"][2].get<double>(), 0, 1e-6);
  }
  a.ok("align", {{"ids", {ground, first}}, {"axis", "x"}, {"edge", "max"}, {"value", 9000}});
  EXPECT_NEAR(a.ok("get_object", {{"id", ground}})["bounds"]["max"][0].get<double>(), 9000, 1e-6);
  EXPECT_NEAR(a.ok("get_object", {{"id", first}})["bounds"]["max"][0].get<double>(), 9000, 1e-6);
  EXPECT_FALSE(a.call("align", {{"ids", {ground}}, {"axis", "w"}, {"edge", "min"}})["ok"]);
  EXPECT_FALSE(a.call("assign_levels", {{"levels", json::array()}})["ok"]);
}

// Walls, slabs and openings for architecture (AI and scripts).
TEST(Transactions, WallsJoinExactlyAtCorners) {
  Api a;
  auto totalVolume = [&](const json& ids) {
    double v = 0;
    for (const json& id : ids) v += a.ok("get_object", {{"id", id}})["volume"].get<double>();
    return v;
  };
  const json loop = {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}};  // counter-clockwise
  // Centred: the band straddles the line, corners neither overlap nor gap.
  json walls = a.ok("create_wall", {{"points", loop}, {"thickness", 200}, {"height", 3000}})["ids"];
  ASSERT_EQ(walls.size(), 4u);
  EXPECT_NEAR(totalVolume(walls), (6200.0 * 4200 - 5800.0 * 3800) * 3000, 1.0);
  EXPECT_EQ(a.ok("get_object", {{"id", walls[0]}})["attributes"]["kind"], "wall");
  EXPECT_EQ(a.modeler.history().undoCount(), 1u);
  a.ok("undo");
  EXPECT_TRUE(a.modeler.document().objects().empty());
  // Left of a counter-clockwise loop = inside: the line is the outer face.
  walls = a.ok("create_wall", {{"points", loop}, {"thickness", 200}, {"height", 3000}, {"justification", "left"}})["ids"];
  EXPECT_NEAR(totalVolume(walls), (6000.0 * 4000 - 5600.0 * 3600) * 3000, 1.0);
  const json all = a.ok("query_objects", {{"attributes", {{"kind", "wall"}}}, {"fields", "bounds"}});
  double x0 = 1e9, x1 = -1e9;
  for (const json& o : all["objects"]) {
    x0 = std::min(x0, o["bounds"]["min"][0].get<double>());
    x1 = std::max(x1, o["bounds"]["max"][0].get<double>());
  }
  EXPECT_NEAR(x0, 0, 1e-6);
  EXPECT_NEAR(x1, 6000, 1e-6);
  a.ok("undo");
  // Open L, right of the line (outside of a left turn): the outer corner
  // is filled by the first wall.
  walls = a.ok("create_wall", {{"points", {{0, 0}, {5000, 0}, {5000, 3000}}}, {"thickness", 250},
                               {"height", 2800}, {"justification", "right"}, {"base", 100}})["ids"];
  ASSERT_EQ(walls.size(), 2u);
  EXPECT_NEAR(totalVolume(walls), (5250.0 * 250 + 3000.0 * 250) * 2800, 1.0);
  EXPECT_NEAR(a.ok("get_object", {{"id", walls[1]}})["bounds"]["min"][2].get<double>(), 100, 1e-6);
  // Parametric afterwards: taller by editing the extrusion.
  a.ok("set_parameter", {{"id", walls[0]}, {"key", "op0.distance"}, {"value", 3000}});
  EXPECT_NEAR(a.ok("get_object", {{"id", walls[0]}})["bounds"]["max"][2].get<double>(), 3100, 1e-6);

  EXPECT_FALSE(a.call("create_wall", {{"points", {{0, 0}}}, {"thickness", 200}, {"height", 3000}})["ok"]);
  EXPECT_FALSE(a.call("create_wall", {{"points", {{0, 0}, {0, 0}}}, {"thickness", 200}, {"height", 3000}})["ok"]);
  EXPECT_FALSE(a.call("create_wall", {{"points", {{0, 0}, {10, 0}}}, {"thickness", -1}, {"height", 3000}})["ok"]);
}

TEST(Transactions, GridWallsSlabAndEvenlyPlacedOpenings) {
  Api a;
  a.ok("create_grid", {{"x_spacings", {6000}}, {"y_spacings", {4000}}});
  const json walls = a.ok("create_wall", {{"points", {"X1/Y1", "X2/Y1", "X2/Y2", "X1/Y2", "X1/Y1"}},
                                          {"thickness", 200}, {"height", 3000}})["ids"];
  ASSERT_EQ(walls.size(), 4u);
  const json slab = a.ok("create_slab", {{"grid", {{"x", {"X1", "X2"}}, {"y", {"Y1", "Y2"}}}}, {"overhang", 100},
                                         {"top", 0}, {"thickness", 150}});
  const json s = a.ok("get_object", {{"id", slab["id"]}});
  EXPECT_NEAR(s["volume"].get<double>(), 6200.0 * 4200 * 150, 1.0);
  EXPECT_NEAR(s["bounds"]["max"][2].get<double>(), 0, 1e-6);
  EXPECT_EQ(s["attributes"]["kind"], "slab");
  // Clockwise outline works the same.
  const json cw = a.ok("create_slab", {{"outline", {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}}}, {"top", 3150},
                                       {"thickness", 150}});
  EXPECT_NEAR(a.ok("get_object", {{"id", cw["id"]}})["bounds"]["min"][2].get<double>(), 3000, 1e-6);

  // Three windows evenly along the south wall (its -Y face).
  const std::string south = walls[0];
  const std::size_t steps = a.modeler.history().undoCount();
  const std::string host = a.ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 3},
                                                   {"width", 900}, {"height", 1200}, {"sill", 900}})["id"];
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  const json openings = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  ASSERT_EQ(openings.size(), 3u);
  // The south wall runs x 100 .. 6100: the corner square at its start
  // belongs to the west wall (butt joint), the one at its end to itself.
  const double length = 6000, gap = (length - 3 * 900) / 4;
  for (int k = 0; k < 3; ++k) {
    EXPECT_NEAR(openings[k]["center"][0].get<double>(), 100 + gap * (k + 1) + 900 * (k + 0.5), 1e-6) << k;
    EXPECT_NEAR(openings[k]["sill"].get<double>(), 900, 1e-6);
    EXPECT_NEAR(openings[k]["width"].get<double>(), 900, 1e-9);
  }
  EXPECT_NEAR(a.ok("get_object", {{"id", host}})["volume"].get<double>(),
              (6000.0 * 200 * 3000) - 3 * 900.0 * 200 * 1200, 1.0);
  a.ok("undo");
  // Spacing around the middle; too many / too tall are refused unchanged.
  const std::string spaced = a.ok("place_openings", {{"host", south}, {"face_normal", {0, -1, 0}}, {"count", 2},
                                                     {"width", 800}, {"height", 2000}, {"spacing", 1800}})["id"];
  const json two = a.ok("list_openings", {{"ids", {spaced}}})["hosts"][0]["openings"];
  EXPECT_NEAR(two[0]["center"][0].get<double>(), 3100 - 900, 1e-6);  // around the wall's middle
  EXPECT_NEAR(two[1]["center"][0].get<double>(), 3100 + 900, 1e-6);
  const std::size_t before = a.modeler.history().undoCount();
  EXPECT_FALSE(a.call("place_openings", {{"host", spaced}, {"face_normal", {0, -1, 0}}, {"count", 9},
                                         {"width", 900}, {"height", 1000}})["ok"]);
  EXPECT_FALSE(a.call("place_openings", {{"host", spaced}, {"face_normal", {0, -1, 0}}, {"count", 1},
                                         {"width", 900}, {"height", 2800}, {"sill", 900}})["ok"]);
  EXPECT_FALSE(a.call("place_openings", {{"host", spaced}, {"face_normal", {0, 0, 1}}, {"count", 1},
                                         {"width", 900}, {"height", 1000}})["ok"]);
  EXPECT_EQ(a.modeler.history().undoCount(), before);
  // One door where it was clicked (only the position along the wall counts);
  // off the end of the wall is refused.
  const std::string door = a.ok("place_openings", {{"host", spaced}, {"face_normal", {0, -1, 0}},
                                                   {"at", {5500, -700, 1234}}, {"width", 900}, {"height", 2100}})["id"];
  const json three = a.ok("list_openings", {{"ids", {door}}})["hosts"][0]["openings"];
  ASSERT_EQ(three.size(), 3u);
  EXPECT_NEAR(three[2]["center"][0].get<double>(), 5500, 1e-6);
  EXPECT_NEAR(three[2]["sill"].get<double>(), 0, 1e-6);
  EXPECT_FALSE(a.call("place_openings", {{"host", door}, {"face_normal", {0, -1, 0}}, {"at", {6000, 0, 0}},
                                         {"width", 900}, {"height", 2100}})["ok"]);
  // Openings never run into each other (the holes would merge): not by
  // placing one over another (also from the other side of the wall), not
  // by growing one; touching is fine.
  const std::size_t kept = a.modeler.history().undoCount();
  const json over = a.call("place_openings", {{"host", door}, {"face_normal", {0, -1, 0}}, {"at", {4300, 0, 0}},
                                              {"width", 900}, {"height", 2100}});
  EXPECT_FALSE(over["ok"]);
  EXPECT_NE(over["error"].get<std::string>().find("overlap opening 2"), std::string::npos) << over.dump();
  EXPECT_FALSE(a.call("place_openings", {{"host", door}, {"face_normal", {0, 1, 0}}, {"at", {4300, 0, 0}},
                                         {"width", 900}, {"height", 2100}})["ok"]);
  EXPECT_FALSE(a.call("set_opening_size", {{"host", door}, {"openings", {2}}, {"width", 2800}})["ok"]);
  EXPECT_EQ(a.modeler.history().undoCount(), kept);
  a.ok("set_opening_size", {{"host", door}, {"openings", {2}}, {"width", 1600}});  // 3200 .. 4800
  // Above an opening with a gap in height is fine.
  a.ok("place_openings", {{"host", door}, {"face_normal", {0, -1, 0}}, {"at", {2200, 0, 0}}, {"width", 800},
                          {"height", 500}, {"sill", 2300}});
}

// Review §5: choosing a face by its direction never picks one of several
// equally good candidates silently; "near" says which one is meant.
TEST(Transactions, FaceByNormalRefusesTiesAndTakesNear) {
  Api a;
  const std::string wall = a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 200},
                                               {"height", 3000}})["id"];
  a.ok("split_face", {{"points", {{2000, 0, 0}, {2000, 0, 3000}}}});  // front face in two
  const json tie = a.call("push_pull", {{"id", wall}, {"face", {{"normal", {0, -1, 0}}}}, {"distance", 100}});
  EXPECT_FALSE(tie["ok"]);
  EXPECT_NE(tie["error"].get<std::string>().find("ambiguous face"), std::string::npos) << tie.dump();
  const double before = a.ok("get_object", {{"id", wall}})["volume"];
  a.ok("push_pull", {{"id", wall}, {"face", {{"normal", {0, -1, 0}}, {"near", {3000, 0, 1500}}}}, {"distance", 100}});
  const json pushed = a.ok("get_object", {{"id", wall}});
  EXPECT_NEAR(pushed["volume"].get<double>() - before, 2000.0 * 3000 * 100, 1.0);
  EXPECT_NEAR(pushed["bounds"]["min"][1].get<double>(), -100, 1e-6);
  // Openings still go along the whole back side when it is split too.
  a.ok("split_face", {{"points", {{1000, 200, 0}, {1000, 200, 3000}}}});
  const std::string host = a.ok("place_openings", {{"host", wall}, {"face_normal", {0, 1, 0}}, {"count", 2},
                                                   {"width", 900}, {"height", 1200}, {"sill", 900}})["id"];
  const json two = a.ok("list_openings", {{"ids", {host}}})["hosts"][0]["openings"];
  ASSERT_EQ(two.size(), 2u);
  const double gap = (4000.0 - 2 * 900) / 3;
  EXPECT_NEAR(two[0]["center"][0].get<double>() + two[1]["center"][0].get<double>(), 4000, 1e-6);
  EXPECT_NEAR(std::abs(two[0]["center"][0].get<double>() - two[1]["center"][0].get<double>()), gap + 900, 1e-6);
}

// Materials on the common edit path: "the material of this face on every
// wall" = material_at + edit_each (preview / commit report the same change
// set, one undo step); a failing batch takes back the library edit too.
TEST(Transactions, MaterialOfAFaceOnAllWallsAndRollback) {
  Api a;
  const json walls = a.ok("create_wall", {{"points", {{0, 0}, {6000, 0}, {6000, 4000}, {0, 4000}, {0, 0}}},
                                          {"thickness", 200}, {"height", 2800}})["ids"];
  const std::string brick = a.ok("create_material", {{"name", "レンガ"}, {"color", "#a0522d"}})["id"];
  const std::string sample = a.box(9000);
  a.ok("paint_faces", {{"id", sample}, {"faces", {{{"normal", {0, 0, 1}}}}}, {"material", "レンガ"}});
  const json at = a.ok("material_at", {{"id", sample}, {"face", {{"normal", {0, 0, 1}}}}});
  EXPECT_EQ(at["material"], brick);
  EXPECT_EQ(at["source"], "face");
  EXPECT_EQ(a.ok("material_at", {{"id", sample}, {"face", {{"normal", {1, 0, 0}}}}})["source"], "none");

  const std::size_t steps = a.modeler.history().undoCount();
  const json where = {{"attributes", {{"kind", "wall"}}}};
  const json commands = {step("assign_material", {{"ids", {"$id"}}, {"material", at["material"]}})};
  const json preview = a.ok("preview", {{"commands", {step("edit_each", {{"where", where}, {"commands", commands}})}}});
  ASSERT_TRUE(preview["would_succeed"].get<bool>()) << preview.dump();
  ASSERT_EQ(preview["changes"]["modified"].size(), 4u);
  EXPECT_TRUE(hasAspect(preview["changes"]["modified"][0], "material"));
  EXPECT_EQ(a.modeler.history().undoCount(), steps);  // preview changed nothing
  const json done = a.ok("edit_each", {{"where", where}, {"commands", commands}});
  EXPECT_EQ(done["changes"], preview["changes"]);
  EXPECT_EQ(a.modeler.history().undoCount(), steps + 1);
  const json painted = a.ok("query_objects", {{"material", "レンガ"}, {"fields", {"id"}}});
  EXPECT_EQ(painted["objects"].size(), 4u);  // the walls (the sample only has a painted face)
  const json counts = a.ok("aggregate", {{"attributes", {{"kind", "wall"}}}, {"group_by", "material"}});
  ASSERT_EQ(counts["groups"].size(), 1u) << counts.dump();
  EXPECT_EQ(counts["groups"][0]["key"], brick);
  EXPECT_EQ(counts["groups"][0]["count"].get<int>(), 4);
  a.ok("undo");
  EXPECT_TRUE(a.ok("query_objects", {{"material", brick}, {"fields", {"id"}}})["objects"].empty());

  // A new material, assigned, then a failure: everything goes back,
  // including the library entry.
  const std::size_t before = a.modeler.history().undoCount();
  const json batch = a.call(
      "batch", {{"commands",
                 {step("create_material", {{"name", "杉板"}, {"color", {0.8, 0.6, 0.4}}}),
                  step("assign_material", {{"ids", {walls[0]}}, {"material", "杉板"}}),
                  step("assign_material", {{"ids", {"no-such-object"}}, {"material", "杉板"}})}}});
  EXPECT_FALSE(batch["ok"]);
  EXPECT_EQ(a.modeler.history().undoCount(), before);
  for (const auto& mat : a.modeler.document().materials()) EXPECT_NE(mat.name, "杉板");
  EXPECT_TRUE(a.modeler.document().find(walls[0].get<std::string>())->material.empty());
  // Material edits are part of the change set.
  const json renamed = a.ok("preview", {{"commands", {step("update_material", {{"material", brick}, {"name", "赤レンガ"}})}}});
  EXPECT_EQ(renamed["changes"]["materials"], json::array({brick}));
}

// The eraser through the API: dividing lines go, corners stay.
TEST(Transactions, EraseEdgeMergesSplitFaces) {
  Api a;
  const std::string wall = a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 200},
                                               {"height", 3000}})["id"];
  a.ok("split_face", {{"points", {{0, 0, 1000}, {4000, 0, 1000}}}});
  EXPECT_EQ(a.ok("get_object", {{"id", wall}})["faces"].size(), 7u);
  a.ok("erase_edge", {{"id", wall}, {"edge", {{"near", {2000, 0, 1000}}}}});
  EXPECT_EQ(a.ok("get_object", {{"id", wall}})["faces"].size(), 6u);
  EXPECT_FALSE(a.call("erase_edge", {{"id", wall}, {"edge", {{"near", {2000, 0, 3000}}}}})["ok"]);
}
