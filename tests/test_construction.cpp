#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionGuide.h"
#include "sbr/api/CommandApi.h"
#include "sbr/api/McpServer.h"
#include "sbr/doc/NativeFormat.h"

using nlohmann::json;
using namespace sbr;
namespace {
struct Construction : testing::Test {
  doc::Modeler m;
  api::CommandApi api{m};
  json call(const std::string& command, json args = json::object()) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  }
  json ok(const std::string& command, json args = json::object()) {
    const json response = call(command, args);
    EXPECT_TRUE(response.value("ok", false)) << response.dump();
    return response.value("result", json::object());
  }
  json patch() {
    return {{"source", {{"name", "Test office defaults"}, {"version", "1"}, {"date", "2026-11-04"},
                        {"url", "https://example.org/defaults"}}}, {"normative", "user"},
            {"roles", {{"entries", {{"column", {{"default_section", "TIMBER-120x120"}}}}}}},
            {"presets", {{"entries", {{"module", {{"value", 1000}}}}}}}};
  }
};
}

TEST_F(Construction, CatalogAndGuideComeFromSameTables) {
  const json data = json::parse(construction::guideData("timber"));
  EXPECT_EQ(data["roles"]["entries"].size(), 18u);
  EXPECT_EQ(data["roles"]["entries"]["column"]["default_section"], "TIMBER-105x105");
  const std::string md = construction::guideMarkdown("timber");
  for (const auto& [id, entry] : data["sections"]["entries"].items()) {
    EXPECT_NE(md.find(id), std::string::npos);
    if (entry["kind"] == "rectangle") {
      const auto s = construction::section(id);
      EXPECT_TRUE(geom::validateSection(s));
      EXPECT_NE(md.find(entry["name"].get<std::string>()), std::string::npos);
      ASSERT_NE(geom::findSection(id), nullptr);
      EXPECT_EQ(s, geom::findSection(id)->section);
    }
  }
  EXPECT_NE(md.find("market_practice"), std::string::npos);
  EXPECT_NE(md.find("目安"), std::string::npos);
  EXPECT_NE(md.find("保証しません"), std::string::npos);
  EXPECT_EQ(geom::sectionCatalog().size(), 1204u);  // legacy steel unchanged
}

TEST_F(Construction, AiChoosesTimberColumnAndStaysEditable) {
  auto list = ok("list_sections", {{"system", "timber"}, {"role", "column"}});
  ASSERT_EQ(list["total_matches"], 2);
  const auto id = list["default_section"];
  auto made = ok("create_member", {{"section", id}, {"role", "column"}, {"start", {0,0,0}}, {"length", 2900}});
  EXPECT_EQ(made["material_class"], "timber");
  EXPECT_EQ(made["catalog"], id);
  const std::string object = made["id"];
  EXPECT_EQ(m.document().find(object)->attributes.at("kind"), "column");
  EXPECT_EQ(m.history().undoCount(), 1u);
  doc::Document loaded;
  ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), loaded));
  const auto* member = std::get_if<doc::LinearMember>(&loaded.find(object)->recipe.base);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->section.width, 105);
  auto changed = ok("change_member", {{"id", object}, {"catalog", "TIMBER-120x120"}});
  EXPECT_EQ(changed["catalog"], "TIMBER-120x120");
  // Timber remains freely editable, including its width.
  ok("change_member", {{"id", object}, {"width", 115}});
  EXPECT_TRUE(ok("member_info", {{"id", object}})["catalog"].is_null());
}

TEST_F(Construction, OverridesSavedUndoneAndReflectedInGuideAndCatalog) {
  auto p = patch();
  // Section dimensions also follow project overrides, including selection by id.
  p["sections"]["entries"]["TIMBER-120x120"]["width"] = 125;
  ok("set_construction_overrides", {{"system", "timber"}, {"overrides", p}});
  EXPECT_EQ(m.history().undoCount(), 1u);
  const auto list = ok("list_sections", {{"system", "timber"}, {"query", "TIMBER-120x120"}});
  ASSERT_EQ(list["exact"].size(), 1u);
  EXPECT_EQ(list["exact"][0]["width"], 125);
  EXPECT_EQ(list["overrides"]["source"]["name"], "Test office defaults");
  const std::string md = ok("get_construction_guide", {{"system", "timber"}})["markdown"];
  EXPECT_NE(md.find("1000"), std::string::npos);
  EXPECT_NE(md.find("125"), std::string::npos);
  EXPECT_NE(md.find("Test office defaults"), std::string::npos);
  const std::string file = testing::TempDir() + "construction_defaults.sbr";
  ASSERT_TRUE(m.save(file));
  doc::Modeler reopened;
  ASSERT_TRUE(reopened.load(file));
  EXPECT_EQ(reopened.document().settings().constructionOverrides, m.document().settings().constructionOverrides);
  ok("undo");
  EXPECT_TRUE(m.document().settings().constructionOverrides.empty());
  ok("redo");
  const auto made = ok("create_member", {{"section", "TIMBER-120x120"}, {"start", {0,0,0}}, {"length", 2900}});
  const auto info = m.memberInfo(made["id"].get<std::string>());
  ASSERT_TRUE(info);
  const auto* member = std::get_if<doc::LinearMember>(&m.document().find(made["id"].get<std::string>())->recipe.base);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(member->section.width, 125);
  // Base tables were not changed by project settings.
  EXPECT_EQ(construction::section("TIMBER-120x120").width, 120);
}

TEST_F(Construction, BadOverridesAndUnknownSystemsChangeNothing) {
  for (const auto& bad : {json{{"presets", {{"entries", {{"module", {{"value", 0}}}}}}}},
                         json{{"presets", {{"entries", {{"module", {{"value", "not a number"}}}}}}}},
                         json{{"sections", {{"entries", {{"TIMBER-105x105", {{"width", -5}}}}}}}},
                         json{{"source", nullptr}}, json::array()}) {
    auto p = patch();
    p.merge_patch(bad);
    EXPECT_FALSE(call("set_construction_overrides", {{"system", "timber"}, {"overrides", p}})["ok"]);
  }
  auto p = patch(); p["roles"]["entries"]["column"]["default_section"] = "missing";
  EXPECT_FALSE(call("set_construction_overrides", {{"system", "timber"}, {"overrides", p}})["ok"]);
  EXPECT_EQ(m.history().undoCount(), 0u);
  EXPECT_TRUE(m.document().settings().constructionOverrides.empty());
  for (const char* system : {"rc", "steel", "unknown"})
    EXPECT_FALSE(call("get_construction_guide", {{"system", system}})["ok"]);
  EXPECT_FALSE(call("list_sections", {{"system", "rc"}})["ok"]);
  EXPECT_FALSE(call("list_sections", {{"system", "timber"}, {"role", "missing"}})["ok"]);
}

TEST_F(Construction, ProductsAndSheetsDoNotInventMemberDimensions) {
  const auto list = ok("list_sections", {{"system", "timber"}, {"role", "floor_post"}});
  EXPECT_EQ(list["default_section"], "STEEL-POST-UNSELECTED");
  for (const char* id : {"STEEL-POST-UNSELECTED", "PLYWOOD-28"}) {
    EXPECT_FALSE(call("create_member", {{"section", id}, {"start", {0,0,0}}, {"length", 1000}})["ok"]);
  }
  EXPECT_TRUE(m.document().objects().empty());
}

TEST_F(Construction, SearchPaginationAndReadEffects) {
  const auto a = ok("list_sections", {{"system", "timber"}, {"role", "rafter"}, {"limit", 1}, {"offset", 1}});
  EXPECT_EQ(a["total_matches"], 4);
  EXPECT_EQ(a["matches"].size(), 1u);
  EXPECT_EQ(ok("list_sections", {{"query", "H-400x200x8x13"}})["exact"].size(), 1u);
  for (const char* command : {"list_sections", "get_construction_guide"})
    EXPECT_EQ(api.effectOf(json{{"command", command}}.dump()), api::CommandApi::Effect::Read);
}

TEST_F(Construction, PreviewAndFailedBatchRollBackOverrides) {
  const json set = {{"command", "set_construction_overrides"}, {"args", {{"system", "timber"}, {"overrides", patch()}}}};
  ok("preview", {{"commands", json::array({set})}});
  EXPECT_TRUE(m.document().settings().constructionOverrides.empty());
  EXPECT_FALSE(call("batch", {{"commands", json::array({set, {{"command", "no_such_command"}}})}})["ok"]);
  EXPECT_TRUE(m.document().settings().constructionOverrides.empty());
  EXPECT_EQ(m.history().undoCount(), 0u);
}

TEST_F(Construction, McpResourceAndToolUseSameProjectData) {
  ok("set_construction_overrides", {{"system", "timber"}, {"overrides", patch()}});
  api::McpServer server([this](const std::string& request) { return api.execute(request); });
  auto rpc = [&](std::string method, json params = json::object()) {
    const auto reply = server.handle(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump());
    EXPECT_TRUE(reply.has_value());
    return json::parse(reply.value_or("{}"));
  };
  EXPECT_TRUE(rpc("initialize")["result"]["capabilities"].contains("resources"));
  const auto resources = rpc("resources/list")["result"]["resources"];
  ASSERT_EQ(resources.size(), 1u);
  const auto result = rpc("resources/read", {{"uri", resources[0]["uri"]}})["result"]["contents"][0];
  EXPECT_EQ(result["text"], ok("get_construction_guide", {{"system", "timber"}})["markdown"]);
  EXPECT_TRUE(rpc("resources/read", {{"uri", "sketchbrep://construction/rc"}}).contains("error"));
}
