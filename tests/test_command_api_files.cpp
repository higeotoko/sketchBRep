// docs/111: an agent in the app may save a copy as a new file only.
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"

using json = nlohmann::json;

TEST(SaveCopy, ANewFileOnlyAndTheDocumentStaysAsItIs) {
  sbr::doc::Modeler modeler;
  sbr::api::CommandApi::Options options;
  options.allowFileCommands = false;  // as in the app
  sbr::api::CommandApi api(modeler, options);
  auto call = [&](const std::string& command, json args) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  ASSERT_TRUE(call("create_box", {{"origin", {0, 0, 0}}, {"width", 100}, {"depth", 100}, {"height", 100}}).value("ok", false));
  const auto dir = std::filesystem::temp_directory_path() / "sbr_save_copy_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "copy.sbr").u8string();

  // Off: refused, and not offered.
  EXPECT_FALSE(call("save_copy", {{"path", path}}).value("ok", true));
  EXPECT_EQ(api.schemaJson().find("save_copy"), std::string::npos);
  api.setAllowSaveCopy(true);
  EXPECT_NE(api.schemaJson().find("save_copy"), std::string::npos);
  for (const json& c : json::parse(api.schemaJson()))  // the others stay off
    EXPECT_TRUE(c["name"] != "open" && c["name"] != "save" && c["name"] != "export_ifc") << c["name"];
  EXPECT_FALSE(call("save", {{"path", path}}).value("ok", true));

  // On: a new file, readable; the document is not marked saved.
  const bool dirtyBefore = !modeler.history().isClean();
  const json saved = call("save_copy", {{"path", path}});
  ASSERT_TRUE(saved.value("ok", false)) << saved.dump();
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(!modeler.history().isClean(), dirtyBefore);
  sbr::doc::Modeler other;
  ASSERT_TRUE(other.load(path));
  EXPECT_EQ(other.document().objects().size(), 1u);

  // Never over a file that is there, nor another kind of file, nor a relative path.
  const auto size = std::filesystem::file_size(path);
  ASSERT_TRUE(call("create_box", {{"origin", {500, 0, 0}}, {"width", 100}, {"depth", 100}, {"height", 100}}).value("ok", false));
  EXPECT_FALSE(call("save_copy", {{"path", path}}).value("ok", true));
  EXPECT_EQ(std::filesystem::file_size(path), size);
  { std::ofstream(dir / "notes.txt") << "mine"; }
  EXPECT_FALSE(call("save_copy", {{"path", (dir / "notes.txt").u8string()}}).value("ok", true));
  EXPECT_FALSE(call("save_copy", {{"path", "relative.sbr"}}).value("ok", true));
  EXPECT_FALSE(call("save_copy", {{"path", (dir / "missing" / "x.sbr").u8string()}}).value("ok", true));
  // Not inside a preview (a scratch copy never writes files).
  const json preview = call("preview", {{"commands", {{{"command", "save_copy"}, {"args", {{"path", (dir / "p.sbr").u8string()}}}}}}});
  EXPECT_FALSE(std::filesystem::exists(dir / "p.sbr")) << preview.dump();
  std::filesystem::remove_all(dir);
}
