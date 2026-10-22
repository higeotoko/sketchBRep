// CommandApi: the JSON command layer shared by the CLI, the MCP server and the in-app bridge.

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
};

double volumeOf(Api& a, const std::string& id) { return a.ok("get_object", {{"id", id}}).value("volume", 0.0); }

}  // namespace

TEST(CommandApi, BoxPushPullAndParameterEdit) {
  Api a;
  const std::string id =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 1000}, {"depth", 2000}, {"height", 3000}})["id"];
  EXPECT_NEAR(volumeOf(a, id), 1000.0 * 2000 * 3000, 1e-3);
  EXPECT_EQ(a.modeler.history().undoCount(), 1u);  // create_box is one undo step

  const json obj = a.ok("get_object", {{"id", id}});
  EXPECT_EQ(obj["faces"].size(), 6u);
  EXPECT_EQ(obj["kind"], "Solid");

  // Push the +X face out by 500 (selected by normal).
  a.ok("push_pull", {{"id", id}, {"face", {{"normal", {1, 0, 0}}}}, {"distance", 500}});
  EXPECT_NEAR(volumeOf(a, id), 1500.0 * 2000 * 3000, 1e-3);

  // Re-edit the height through its parameter key.
  a.ok("set_parameter", {{"id", id}, {"key", "op0.distance"}, {"value", 4000}});
  EXPECT_NEAR(volumeOf(a, id), 1500.0 * 2000 * 4000, 1e-3);

  a.ok("undo");
  EXPECT_NEAR(volumeOf(a, id), 1500.0 * 2000 * 3000, 1e-3);
}

TEST(CommandApi, ErrorsDoNotChangeTheModel) {
  Api a;
  json r = a.call("no_such_command");
  EXPECT_FALSE(r["ok"]);
  EXPECT_NE(r["error"].get<std::string>().find("unknown command"), std::string::npos);

  r = a.call("create_rectangle", {{"origin", {0, 0}}, {"width", 1}, {"height", 1}});
  EXPECT_FALSE(r["ok"]);
  r = a.call("create_rectangle", {{"origin", {0, 0, 0}}, {"width", 0}, {"height", 1}});
  EXPECT_FALSE(r["ok"]);
  r = a.call("push_pull", {{"id", "missing"}, {"face", 1}, {"distance", 10}});
  EXPECT_FALSE(r["ok"]);
  EXPECT_FALSE(json::parse(a.api.execute("{not json"))["ok"]);
  EXPECT_TRUE(a.modeler.document().objects().empty());
  EXPECT_EQ(a.modeler.history().undoCount(), 0u);
}

TEST(CommandApi, BatchIsOneUndoStepAndCanReferenceEarlierSteps) {
  Api a;
  const json result = a.ok("batch", {{"name", "Columns"},
                                     {"commands",
                                      {{{"command", "create_box"},
                                        {"args", {{"origin", {0, 0, 0}}, {"width", 300}, {"depth", 300}, {"height", 3000}}}},
                                       {{"command", "copy_array"},
                                        {"args", {{"ids", {"$1.id"}}, {"vector", {3000, 0, 0}}, {"count", 4}}}},
                                       {{"command", "paint"}, {"args", {{"ids", "$2.ids"}, {"color", "#8040c0"}}}}}}});
  EXPECT_EQ(result["results"].size(), 3u);
  EXPECT_EQ(a.modeler.document().objects().size(), 5u);
  EXPECT_EQ(a.modeler.history().undoCount(), 1u);
  EXPECT_EQ(a.modeler.history().undoName(), "Columns");

  a.ok("undo");
  EXPECT_TRUE(a.modeler.document().objects().empty());
  a.ok("redo");
  EXPECT_EQ(a.modeler.document().objects().size(), 5u);
}

TEST(CommandApi, FailedAtomicBatchRevertsEverything) {
  Api a;
  a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 1000}, {"depth", 1000}, {"height", 1000}});
  const json r = a.call("batch", {{"commands",
                                   {{{"command", "create_box"},
                                     {"args", {{"origin", {5000, 0, 0}}, {"width", 100}, {"depth", 100}, {"height", 100}}}},
                                    {{"command", "set_parameter"},
                                     {"args", {{"id", "$1.id"}, {"key", "op0.distance"}, {"value", 0}}}}}}});
  EXPECT_FALSE(r["ok"]);
  EXPECT_NE(r["error"].get<std::string>().find("step 2"), std::string::npos);
  EXPECT_EQ(a.modeler.document().objects().size(), 1u);
  EXPECT_EQ(a.modeler.history().undoCount(), 1u);

  // Nested batches and forward references are rejected.
  EXPECT_FALSE(a.call("batch", {{"commands", {{{"command", "batch"}, {"args", {{"commands", json::array()}}}}}}})["ok"]);
  EXPECT_FALSE(a.call("batch", {{"commands", {{{"command", "delete"}, {"args", {{"ids", {"$3.id"}}}}}}}})["ok"]);
  EXPECT_EQ(a.modeler.history().undoCount(), 1u);
}

TEST(CommandApi, WallWithOpeningViaProfileOnFace) {
  Api a;
  const std::string wall =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 200}, {"height", 3000}})["id"];
  // A door outline on the wall's -Y face, pushed through the wall.
  const std::string door = a.ok("create_rectangle", {{"origin", {1000, 0, 0}},
                                                      {"normal", {0, -1, 0}},
                                                      {"width", 900},
                                                      {"height", 2100}})["id"];
  const json r = a.ok("push_pull", {{"id", door}, {"face", 1}, {"distance", -200}});
  // The wall and the door become one parametric object (a new id is returned).
  const std::string merged = r["id"];
  EXPECT_NE(merged, door);
  EXPECT_EQ(a.modeler.document().objects().size(), 1u);
  EXPECT_NEAR(volumeOf(a, merged), 4000.0 * 200 * 3000 - 900.0 * 200 * 2100, 1.0);
}

TEST(CommandApi, SchemaListsEveryCommandWithInputSchema) {
  Api a;
  const json schema = json::parse(a.api.schemaJson());
  ASSERT_TRUE(schema.is_array());
  EXPECT_GT(schema.size(), 25u);
  bool hasBatch = false;
  for (const json& tool : schema) {
    EXPECT_TRUE(tool["name"].is_string());
    EXPECT_TRUE(tool["description"].is_string());
    EXPECT_EQ(tool["input_schema"]["type"], "object") << tool["name"];
    hasBatch = hasBatch || tool["name"] == "batch";
  }
  EXPECT_TRUE(hasBatch);
  EXPECT_EQ(a.ok("describe").size(), schema.size());
}

TEST(CommandApi, ExtensionsAndDisabledFileCommands) {
  sbr::doc::Modeler modeler;
  CommandApi api(modeler, CommandApi::Options{false});
  api.addExtension({"echo", "Echo the args.", R"({"type": "object"})",
                    [](const std::string& args) { return args; }});
  api.addExtension({"fail", "Always fails.", R"({"type": "object"})",
                    [](const std::string&) -> std::string { throw std::runtime_error("nope"); }});
  json r = json::parse(api.execute(R"({"command": "echo", "args": {"x": 1}})"));
  EXPECT_TRUE(r["ok"]);
  EXPECT_EQ(r["result"]["x"], 1);
  r = json::parse(api.execute(R"({"command": "fail"})"));
  EXPECT_FALSE(r["ok"]);
  EXPECT_EQ(r["error"], "nope");
  r = json::parse(api.execute(R"({"command": "save", "args": {"path": "x.sbr"}})"));
  EXPECT_FALSE(r["ok"]);

  bool hasEcho = false, hasSave = false;
  for (const json& tool : json::parse(api.schemaJson())) {
    hasEcho = hasEcho || tool["name"] == "echo";
    hasSave = hasSave || tool["name"] == "save";
  }
  EXPECT_TRUE(hasEcho);
  EXPECT_FALSE(hasSave);
}

// ---- MCP (JSON-RPC over stdio) --------------------------------------------------------

#include "sbr/api/McpServer.h"

namespace {

json rpc(sbr::api::McpServer& server, const json& message) {
  const auto response = server.handle(message.dump());
  return response ? json::parse(*response) : json();
}

}  // namespace

TEST(McpServer, InitializeListAndCallTools) {
  sbr::doc::Modeler modeler;
  CommandApi api(modeler);
  sbr::api::McpServer server([&](const std::string& r) { return api.execute(r); });

  json r = rpc(server, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                        {"params", {{"protocolVersion", "2024-11-05"}, {"capabilities", json::object()}}}});
  EXPECT_EQ(r["id"], 1);
  EXPECT_EQ(r["result"]["protocolVersion"], "2024-11-05");
  EXPECT_TRUE(r["result"]["capabilities"].contains("tools"));
  EXPECT_FALSE(r["result"]["instructions"].get<std::string>().empty());

  // Notifications get no response.
  EXPECT_FALSE(server.handle(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})").has_value());

  r = rpc(server, {{"jsonrpc", "2.0"}, {"id", "a"}, {"method", "tools/list"}});
  ASSERT_TRUE(r["result"]["tools"].is_array());
  bool found = false;
  for (const json& t : r["result"]["tools"]) {
    EXPECT_TRUE(t.contains("inputSchema"));
    found = found || t["name"] == "create_box";
  }
  EXPECT_TRUE(found);

  const json box = {{"origin", {0, 0, 0}}, {"width", 100}, {"depth", 100}, {"height", 100}};
  r = rpc(server, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                   {"params", {{"name", "create_box"}, {"arguments", box}}}});
  EXPECT_FALSE(r["result"]["isError"]);
  const json created = json::parse(r["result"]["content"][0]["text"].get<std::string>());
  EXPECT_TRUE(modeler.document().find(created["id"]));

  r = rpc(server, {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
                   {"params", {{"name", "extrude"}, {"arguments", {{"id", "missing"}, {"distance", 1}}}}}});
  EXPECT_TRUE(r["result"]["isError"]);  // tool errors are results, not protocol errors

  r = rpc(server, {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "resources/list"}});
  EXPECT_EQ(r["error"]["code"], -32601);
  r = json::parse(*server.handle("{oops"));
  EXPECT_EQ(r["error"]["code"], -32700);
  EXPECT_EQ(rpc(server, {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "ping"}})["result"], json::object());
}

TEST(McpServer, AppCommandSpecsAreValidSchemas) {
  const auto specs = CommandApi::appCommandSpecs();
  EXPECT_EQ(specs.size(), 8u);
  for (const auto& s : specs) {
    EXPECT_EQ(json::parse(s.inputSchema)["type"], "object") << s.name;
    EXPECT_FALSE(s.description.empty());
  }
}

TEST(CommandApi, SplitFaceThenPushPart) {
  Api a;
  const std::string wall =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 200}, {"height", 3000}})["id"];
  a.ok("split_face", {{"points", {{0, 0, 1000}, {4000, 0, 1000}}}});
  EXPECT_EQ(a.ok("get_object", {{"id", wall}})["faces"].size(), 7u);
  EXPECT_FALSE(a.call("split_face", {{"points", {{100, 0, 100}, {200, 0, 200}}}})["ok"]);
}

TEST(CommandApi, ArchedOutlineFromArcAndLines) {
  Api a;
  const json r = a.ok("batch", {{"commands",
      {{{"command", "create_arc"}, {"args", {{"start", {0, 0, 0}}, {"end", {1000, 0, 0}}, {"bulge", -500}}}},
       {{"command", "create_line"}, {"args", {{"points", {{1000, 0, 0}, {1000, 1000, 0}, {0, 1000, 0}, {0, 0, 0}}}}}}}}});
  // Like the line tool, create_line closes the loop into a face by itself.
  EXPECT_TRUE(r["results"][1]["result"]["face"].get<bool>());
  const std::string face = r["results"][1]["result"]["id"];
  EXPECT_NEAR(a.ok("get_object", {{"id", face}})["area"].get<double>(), 1e6 + M_PI * 250000 / 2, 1e-2);
  EXPECT_EQ(a.ok("list_objects")["objects"].size(), 1u);
}

TEST(CommandApi, SectionsAndTags) {
  Api a;
  const std::string box =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 1000}, {"depth", 1000}, {"height", 1000}})["id"];
  a.ok("create_dimension", {{"start", {0, 0, 0}}, {"end", {1000, 0, 0}}, {"offset", {0, -300, 0}}});
  a.ok("set_section", {{"origin", {0, 0, 500}}, {"normal", {0, 0, 1}}});
  EXPECT_TRUE(a.modeler.sectionVisible());
  a.ok("set_section");
  EXPECT_FALSE(a.modeler.sectionVisible());
  const json tags = a.ok("list_tags")["tags"];
  EXPECT_EQ(tags.size(), 2u);
  a.ok("set_tag_visible", {{"tag", "Annotations"}, {"visible", false}});
  EXPECT_EQ(a.modeler.document().settings().hiddenTags.count("Annotations"), 1u);
  EXPECT_TRUE(a.modeler.document().isVisible(box));
  a.ok("set_tag_visible", {{"tag", "Default"}, {"visible", false}});
  EXPECT_FALSE(a.modeler.document().isVisible(box));
  EXPECT_FALSE(a.call("set_tag_visible", {{"tag", "Default"}, {"visible", "no"}})["ok"]);
}

TEST(CommandApi, DimensionsAndTextNotes) {
  Api a;
  const std::string dim = a.ok("create_dimension", {{"start", {0, 0, 0}}, {"end", {7200, 0, 0}},
                                                    {"offset", {0, -800, 0}}})["id"];
  EXPECT_EQ(a.ok("get_object", {{"id", dim}})["text"], "7,200");
  a.ok("set_text", {{"id", dim}, {"text", "X1-X3"}});
  EXPECT_EQ(a.ok("get_object", {{"id", dim}})["kind"], "Annotation");
  EXPECT_EQ(a.ok("get_object", {{"id", dim}})["text"], "X1-X3");
  const std::string note = a.ok("create_text", {{"position", {1000, 1000, 0}}, {"text", "LDK 24.0 m²"}})["id"];
  EXPECT_EQ(a.ok("get_object", {{"id", note}})["text"], "LDK 24.0 m²");
  EXPECT_FALSE(a.call("create_text", {{"position", {0, 0, 0}}, {"text", ""}})["ok"]);
}

// Spec 07 §9.2 / AC-20: AI proposals are previewed on a copy and applied only
// if the model has not changed since.
TEST(CommandApi, AC20_PreviewThenApplyWithRevisionCheck) {
  Api a;
  const std::string wall =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 4000}, {"depth", 200}, {"height", 3000}})["id"];
  const json proposal = {
      {{"command", "create_box"}, {"args", {{"origin", {0, 0, 3000}}, {"width", 4000}, {"depth", 1400}, {"height", 150}}}},
      {{"command", "set_parameter"}, {"args", {{"id", wall}, {"key", "op0.distance"}, {"value", 2800}}}}};
  const std::size_t steps = a.modeler.history().undoCount();
  const json preview = a.ok("preview", {{"commands", proposal}});
  EXPECT_TRUE(preview["would_succeed"].get<bool>());
  EXPECT_EQ(preview["added"].size(), 1u);
  ASSERT_EQ(preview["changed"].size(), 1u);
  EXPECT_EQ(preview["changed"][0]["id"], wall);
  EXPECT_TRUE(preview["removed"].empty());
  // Nothing changed for real.
  EXPECT_EQ(a.modeler.document().objects().size(), 1u);
  EXPECT_EQ(a.modeler.history().undoCount(), steps);
  EXPECT_NEAR(a.ok("get_object", {{"id", wall}})["volume"].get<double>(), 4000.0 * 200 * 3000, 1e-3);

  // A failing proposal is reported, still without side effects.
  const json bad = a.ok("preview", {{"commands", {{{"command", "set_parameter"},
                                                  {"args", {{"id", wall}, {"key", "op0.distance"}, {"value", 0}}}}}}});
  EXPECT_FALSE(bad["would_succeed"].get<bool>());
  EXPECT_FALSE(bad["error"].get<std::string>().empty());

  // Applying with the previewed revision works; after another edit it is refused.
  const long long revision = preview["revision"];
  a.ok("batch", {{"commands", proposal}, {"expected_revision", revision}});
  EXPECT_EQ(a.modeler.document().objects().size(), 2u);
  a.ok("undo");
  a.ok("rename", {{"id", wall}, {"name", "Wall"}});
  const json refused = a.call("batch", {{"commands", proposal}, {"expected_revision", revision}});
  EXPECT_FALSE(refused["ok"]);
  EXPECT_NE(refused["error"].get<std::string>().find("preview it again"), std::string::npos);
  EXPECT_EQ(a.modeler.document().objects().size(), 1u);
}

TEST(CommandApi, FollowMeAlongAndAround) {
  Api a;
  const std::string box =
      a.ok("create_box", {{"origin", {0, 0, 0}}, {"width", 2000}, {"depth", 1000}, {"height", 500}})["id"];
  const std::string profile =
      a.ok("create_rectangle", {{"origin", {0, 0, 500}}, {"normal", {1, 0, 0}}, {"width", 100}, {"height", 100}})["id"];
  const std::string ring =
      a.ok("follow_me", {{"profile", profile}, {"around", {{"id", box}, {"face", {{"normal", {0, 0, 1}}}}}}})["id"];
  EXPECT_GT(a.ok("get_object", {{"id", ring}})["volume"].get<double>(), 0.0);
  EXPECT_FALSE(a.call("get_object", {{"id", profile}})["ok"]);  // consumed
}

TEST(CommandApi, GuidesAreCreatedAndCleared) {
  Api a;
  const std::string guide = a.ok("create_guide", {{"point", {0, 900, 0}}, {"direction", {0, 0, 1}}})["id"];
  EXPECT_EQ(a.ok("get_object", {{"id", guide}})["kind"], "Guide");
  EXPECT_FALSE(a.call("create_guide", {{"point", {0, 0, 0}}, {"direction", {0, 0, 0}}})["ok"]);
  a.ok("clear_guides");
  EXPECT_TRUE(a.modeler.document().objects().empty());
  EXPECT_FALSE(a.call("clear_guides")["ok"]);
}

TEST(CommandApi, StructuralGridIsOneUndoStep) {
  Api a;
  const json grid = a.ok("create_grid", {{"x_spacings", {3600, 3600, 2700}}, {"y_spacings", {4500, 4500}}});
  ASSERT_EQ(grid["lines"].size(), 7u);  // X1..X4, Y1..Y3
  const json x4 = a.ok("get_object", {{"id", grid["lines"]["X4"]}});
  EXPECT_EQ(x4["kind"], "Guide");
  EXPECT_EQ(x4["tag"], "Grid");
  EXPECT_EQ(grid["labels"].size(), 7u);
  ASSERT_EQ(grid["dimensions"].size(), 5u);
  EXPECT_EQ(a.ok("get_object", {{"id", grid["dimensions"][2]}})["text"], "2,700");
  EXPECT_EQ(a.ok("get_object", {{"id", grid["labels"][3]}})["text"], "X4");

  // "Delete all guides" keeps the grid.
  a.ok("create_guide", {{"point", {0, 0, 0}}, {"direction", {1, 1, 0}}});
  a.ok("clear_guides");
  EXPECT_EQ(a.modeler.document().objects().size(), 19u);

  a.ok("undo");  // the guide comes back
  a.ok("undo");  // and its creation
  a.ok("undo");  // the whole grid in one step
  EXPECT_TRUE(a.modeler.document().objects().empty());
  EXPECT_FALSE(a.call("create_grid", {{"x_spacings", {3600, -1}}})["ok"]);
  EXPECT_TRUE(a.modeler.document().objects().empty());
}
