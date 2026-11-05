#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionFoundation.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct Foundation : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  std::vector<ObjectId> ids;
  ObjectId sill(gp_Pnt a, gp_Pnt b) {
    Modeler::MemberSpec spec; spec.start=a; spec.end=b;
    spec.section.width=spec.section.depth=120; spec.up=gp::DZ(); spec.anchor.up=1;
    spec.role="sill"; spec.materialClass="timber";
    auto id=m.createMember(spec); EXPECT_TRUE(id); if(!id) return {};
    ids.push_back(*id);return *id;
  }
  void ring() {
    sill({0,0,540},{3000,0,540}); sill({3000,0,540},{3000,2000,540});
    sill({3000,2000,540},{0,2000,540}); sill({0,2000,540},{0,0,540});
  }
  json inputs() { return {{"sources",ids}}; }
  json call(const char* command,json args=json::object()) {
    return json::parse(api.execute(json{{"command",command},{"args",args}}.dump()));
  }
  json generate(json args=json::object()) {
    if(!args.contains("inputs") && !args.contains("replace")) args["inputs"]=inputs();
    auto r=m.generateFoundation(args.dump()); EXPECT_TRUE(r) << r.error();
    return r?json::parse(*r):json::object();
  }
  json check(const std::string& id) {
    auto r=construction::checkFoundation(m.document(),json{{"foundation",id}}.dump());
    EXPECT_TRUE(r) << r.error();return r?json::parse(*r)["plans"][0]:json::object();
  }
};
}

TEST_F(Foundation, RingUsesNetVolumesAndOneUndo) {
  ring(); const auto before=serializeDocument(m.document()); const auto undo=m.history().undoCount();
  const auto r=generate(); const std::string root=r.at("foundation");
  EXPECT_NEAR(r["top_z"].get<double>(),400,1e-6);
  EXPECT_NEAR(r["quantities"]["foundation_stem"]["volume_m3"].get<double>(),.9,1e-8);
  EXPECT_NEAR(r["quantities"]["foundation_footing"]["volume_m3"].get<double>(),.675,1e-8);
  EXPECT_EQ(m.history().undoCount(),undo+1); EXPECT_EQ(m.document().objects().size(),7u);
  auto c=check(root); EXPECT_EQ(c["status"],"current"); EXPECT_FALSE(c["verified"]);
  for(const auto& [role,q]:c["quantities"].items()) EXPECT_EQ(q["solids"],1);
  for(const auto& source:c["sources"]) EXPECT_EQ(source["packing_gap_matches"],true);
  ASSERT_TRUE(m.undo()); EXPECT_EQ(serializeDocument(m.document()),before);
  ASSERT_TRUE(m.redo()); EXPECT_EQ(check(root)["status"],"current");
}

TEST_F(Foundation, TeeAndReversedDuplicateHaveNoDoubleVolume) {
  ring(); sill({0,1000,540},{3000,1000,540}); sill({3000,1000,540},{0,1000,540});
  const auto r=generate();
  EXPECT_NEAR(r["generated_parts_volume_m3"].get<double>(),2.003625,1e-8);
}

TEST_F(Foundation, PreviewIsReadOnlyApplyNeedsRevisionAndBatchUndoesTogether) {
  ring(); const auto before=serializeDocument(m.document());const auto undo=m.history().undoCount();
  const auto p=call("generate_foundation",{{"inputs",inputs()}});
  ASSERT_TRUE(p["ok"]) << p.dump(); EXPECT_TRUE(p["result"]["preview"]);
  EXPECT_EQ(serializeDocument(m.document()),before);EXPECT_EQ(m.history().undoCount(),undo);
  EXPECT_FALSE(call("generate_foundation",{{"inputs",inputs()},{"preview",false}})["ok"]);
  auto stale=call("generate_foundation",{{"inputs",inputs()},{"preview",false},{"expected_revision",0}});
  EXPECT_FALSE(stale["ok"]);
  auto applied=call("batch",{{"commands",json::array({{{"command","generate_foundation"},
      {"args",{{"inputs",inputs()},{"preview",false}}}}})}});
  ASSERT_TRUE(applied["ok"])<<applied.dump(); EXPECT_EQ(m.history().undoCount(),undo+1);
  auto revision=m.history().revision();ASSERT_TRUE(call("check_foundation")["ok"]);
  EXPECT_EQ(m.history().revision(),revision);
  ASSERT_TRUE(m.undo());EXPECT_EQ(serializeDocument(m.document()),before);
}

TEST_F(Foundation, IdenticalRunIsIdempotentProfileChangeRequiresReplace) {
  ring();auto r=generate();const std::string root=r["foundation"];const auto undo=m.history().undoCount();
  EXPECT_TRUE(generate()["unchanged"]); EXPECT_EQ(m.history().undoCount(),undo);
  auto in=inputs();in["stem_width"]=180;
  EXPECT_FALSE(m.generateFoundation(json{{"inputs",in}}.dump()));
  r=generate({{"replace",root},{"inputs",{{"stem_width",180}}}});
  EXPECT_EQ(r["foundation"],root);EXPECT_EQ(m.history().undoCount(),undo+1);
  EXPECT_NEAR(r["quantities"]["foundation_stem"]["volume_m3"].get<double>(),1.08,1e-8);
  ASSERT_TRUE(m.undo());EXPECT_NEAR(check(root)["quantities"]["foundation_stem"]["volume_m3"].get<double>(),.9,1e-8);
}

TEST_F(Foundation, SourceMoveMarksStaleRegenerationFollowsAndPersistsRecipes) {
  ring(); const auto r=generate();const std::string root=r["foundation"];
  gp_Trsf move; move.SetTranslation(gp_Vec(123,456,100)); ASSERT_TRUE(m.transform(ids,move,"Move Sills"));
  auto c=check(root);EXPECT_EQ(c["status"],"needs_regeneration");
  for(const auto& s:c["sources"]) EXPECT_TRUE(s["packing_gap_matches"].is_null());
  const auto fresh=generate({{"replace",root}});EXPECT_NEAR(fresh["top_z"].get<double>(),500,1e-6);
  Document reopened;ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()),reopened));
  auto result=construction::checkFoundation(reopened);ASSERT_TRUE(result);
  EXPECT_EQ(json::parse(*result)["plans"][0]["status"],"current");
  for(const auto& o:reopened.objects()) if(o.parent==root) EXPECT_FALSE(checkRecipe(o,reopened.regenContext()).has_value());
}

TEST_F(Foundation, ManualConcreteAndRootEditsAreProtected) {
  ring();auto r=generate();const std::string root=r["foundation"],part=r["quantities"]["foundation_stem"]["id"];
  gp_Trsf move;move.SetTranslation(gp_Vec(50,0,0)); ASSERT_TRUE(m.transform({part},move,"Manual Edit"));
  EXPECT_EQ(check(root)["status"],"modified");const auto before=serializeDocument(m.document());
  EXPECT_FALSE(m.generateFoundation(json{{"replace",root}}.dump()));EXPECT_EQ(serializeDocument(m.document()),before);
  generate({{"replace",root},{"on_modified","replace"}});EXPECT_EQ(check(root)["status"],"current");
  ASSERT_TRUE(m.transform({root},move,"Manual Root Move"));EXPECT_EQ(check(root)["status"],"modified");
  EXPECT_FALSE(m.generateFoundation(json{{"replace",root}}.dump()));
}

TEST_F(Foundation, InvalidSourcesFailWithoutMutation) {
  ring();const auto before=serializeDocument(m.document());
  auto bad=inputs();bad["footing_width"]=100;EXPECT_FALSE(m.generateFoundation(json{{"inputs",bad}}.dump()));
  EXPECT_FALSE(m.generateFoundation(R"({"inputs":{"sources":["missing"]}})"));
  EXPECT_FALSE(m.generateFoundation(R"({"inputs":{"sources":[],"typo":2}})"));
  EXPECT_EQ(serializeDocument(m.document()),before);
  gp_Trsf move;move.SetTranslation(gp_Vec(0,0,100));ASSERT_TRUE(m.transform({ids[0]},move,"Step"));
  const auto stepped=serializeDocument(m.document());EXPECT_FALSE(m.generateFoundation(json{{"inputs",inputs()}}.dump()));
  EXPECT_EQ(serializeDocument(m.document()),stepped);
}

TEST_F(Foundation, SharedFrameSillsAndRotatedParentAreSupported) {
  auto frame=m.generateFrame(R"({"inputs":{"x_bays":2,"y_bays":2,"storeys":1}})");ASSERT_TRUE(frame)<<frame.error();
  const std::string id=json::parse(*frame)["frame"];
  gp_Trsf rotation;rotation.SetRotation(gp_Ax1(gp::Origin(),gp::DZ()),.4);
  ASSERT_TRUE(m.transform({id},rotation,"Rotate Frame"));
  auto r=generate({{"inputs",{{"frame",id}}}});const std::string root=r.at("foundation");
  auto c=check(root);EXPECT_EQ(c["status"],"current");EXPECT_GT(c["sources"].size(),3u);
  EXPECT_NEAR(c["top_z"].get<double>(),400,1e-6);
  for(const auto& s:c["sources"]) EXPECT_EQ(s["packing_gap_matches"],true);
  EXPECT_FALSE(m.levels().empty());
}

TEST_F(Foundation, ManualTopReportsPackingMismatchAndCanReturnToAutomatic) {
  ring();auto r=generate({{"inputs",{{"sources",ids},{"top_z",300}}}});const std::string root=r["foundation"];
  for(const auto& s:check(root)["sources"]) EXPECT_EQ(s["packing_gap_matches"],false);
  r=generate({{"replace",root},{"inputs",{{"top_z",nullptr}}}});
  EXPECT_NEAR(r["top_z"].get<double>(),400,1e-6);
  EXPECT_FALSE(r["inputs"].contains("top_z"));
}

TEST_F(Foundation, DeletedPartNeverClaimsCompleteVolumeAndSourceDeletionIsReported) {
  ring();auto r=generate();const std::string root=r["foundation"],part=r["quantities"]["foundation_stem"]["id"];
  ASSERT_TRUE(m.remove({part}));auto c=check(root);EXPECT_EQ(c["status"],"modified");
  EXPECT_TRUE(c["generated_parts_volume_m3"].is_null());EXPECT_NEAR(c["known_parts_volume_m3"].get<double>(),.675,1e-8);
  EXPECT_FALSE(m.generateFoundation(json{{"replace",root}}.dump()));
  generate({{"replace",root},{"on_modified","replace"}});EXPECT_EQ(check(root)["status"],"current");
  ASSERT_TRUE(m.remove({ids[0]}));EXPECT_EQ(check(root)["status"],"source_missing_or_unsupported");
  const auto before=serializeDocument(m.document());EXPECT_FALSE(m.generateFoundation(json{{"replace",root}}.dump()));
  EXPECT_EQ(serializeDocument(m.document()),before);
}

TEST_F(Foundation, FrameRegenerationUsesNewSillsWithoutChangingLevelsOrFrame) {
  auto frame=m.generateFrame(R"({"inputs":{"x_bays":2,"y_bays":2,"storeys":1}})");ASSERT_TRUE(frame);
  const std::string id=json::parse(*frame)["frame"];
  const auto r=generate({{"inputs",{{"frame",id}}}});const std::string root=r["foundation"];
  frame=m.generateFrame(json{{"replace",id},{"inputs",{{"x_bays",3}}}}.dump());ASSERT_TRUE(frame)<<frame.error();
  EXPECT_EQ(check(root)["status"],"needs_regeneration");
  const auto count=m.levels().size(),objects=m.document().objects().size();
  generate({{"replace",root}});EXPECT_EQ(m.levels().size(),count);EXPECT_EQ(m.document().objects().size(),objects);
  EXPECT_EQ(check(root)["status"],"current");
}

TEST_F(Foundation, ApiEffectsPreserveReadOnlyInspectionAndDefaultPreview) {
  EXPECT_EQ(api.effectOf(R"({"command":"check_foundation"})"),api::CommandApi::Effect::Read);
  EXPECT_EQ(api.effectOf(R"({"command":"generate_foundation"})"),api::CommandApi::Effect::Read);
  EXPECT_EQ(api.effectOf(R"({"command":"generate_foundation","args":{"preview":false}})"),api::CommandApi::Effect::ModelWrite);
}
