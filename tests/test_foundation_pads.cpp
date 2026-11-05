#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include "sbr/ConstructionFoundation.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Measure.h"
using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct FoundationPads:testing::Test {
  Modeler m;api::CommandApi api{m};std::vector<ObjectId> posts;
  ObjectId post(double x,double y,double bottom=400,const char* role="floor_post") {
    Modeler::MemberSpec s;s.start={x,y,bottom};s.end={x,y,bottom+600};s.section.width=s.section.depth=90;
    s.role=role;s.materialClass="timber";auto r=m.createMember(s);EXPECT_TRUE(r);if(!r) return {};
    posts.push_back(*r);return *r;
  }
  json inputs() {return {{"type","pads"},{"sources",posts}};}
  json generate(json args=json::object()) {
    if(!args.contains("inputs") && !args.contains("replace")) args["inputs"]=inputs();
    const auto r=m.generateFoundation(args.dump());EXPECT_TRUE(r)<<r.error();return r?json::parse(*r):json::object();
  }
  json check(const ObjectId& root) {
    const auto r=construction::checkFoundation(m.document(),json{{"foundation",root}}.dump());EXPECT_TRUE(r)<<r.error();
    return r?json::parse(*r)["plans"][0]:json::object();
  }
  json supports() {
    const auto r=construction::foundationSupports(m.document(),json{{"ids",posts}}.dump());EXPECT_TRUE(r)<<r.error();return json::parse(*r);
  }
};
}

TEST_F(FoundationPads, MultiplePadsUseActualBasesAndOneUndo) {
  post(1000,1000);post(2000,1000);const auto before=serializeDocument(m.document());const auto undo=m.history().undoCount();
  auto r=generate();const std::string root=r["foundation"];
  EXPECT_EQ(r["quantities"]["foundation_pad"]["ids"].size(),2u);EXPECT_NEAR(r["generated_parts_volume_m3"].get<double>(),.027,1e-9);
  EXPECT_EQ(m.history().undoCount(),undo+1);EXPECT_EQ(check(root)["status"],"current");
  const auto supported=supports();for(const auto& row:supported["rows"]) EXPECT_EQ(row["path_status"],"geometry_detected");
  const auto checked=check(root);for(const auto& row:checked["sources"]) EXPECT_EQ(row["packing_gap_matches"],true);
  ASSERT_TRUE(m.undo());EXPECT_EQ(serializeDocument(m.document()),before);ASSERT_TRUE(m.redo());EXPECT_EQ(check(root)["status"],"current");
}

TEST_F(FoundationPads, CoincidentReferencesShareOnePadWithoutDuplicateVolume) {
  post(1000,1000);post(1000,1000);auto r=generate();EXPECT_EQ(r["quantities"]["foundation_pad"]["ids"].size(),1u);
  EXPECT_NEAR(r["generated_parts_volume_m3"].get<double>(),.0135,1e-9);
  const std::string id=r["quantities"]["foundation_pad"]["id"];
  EXPECT_EQ(json::parse(m.document().find(id)->attributes.at("source_ids")).size(),2u);
}

TEST_F(FoundationPads, DifferentBasesUseSeparateTopsAndManualHeightCanReturnToAuto) {
  post(1000,1000,400);post(2000,1000,500);auto r=generate();const std::string root=r["foundation"];
  EXPECT_TRUE(r["top_z"].is_null());auto c=check(root);EXPECT_EQ(c["status"],"current");
  for(const auto& row:c["sources"]) EXPECT_EQ(row["packing_gap_matches"],true);
  r=generate({{"replace",root},{"inputs",{{"top_z",350}}}});EXPECT_EQ(r["top_z"],350);
  const auto s=supports();for(const auto& row:s["rows"]) EXPECT_EQ(row["status"],"gap");
  r=generate({{"replace",root},{"inputs",{{"top_z",nullptr}}}});EXPECT_TRUE(r["top_z"].is_null());EXPECT_FALSE(r["inputs"].contains("top_z"));
}

TEST_F(FoundationPads, SourceMoveRegeneratesSamePlanAndPreservesRecipesAfterReopen) {
  const auto p=post(1000,1000);auto r=generate();const std::string root=r["foundation"];
  gp_Trsf move;move.SetTranslation(gp_Vec(300,400,100));ASSERT_TRUE(m.transform({p},move,"Move Post"));
  EXPECT_EQ(check(root)["status"],"needs_regeneration");r=generate({{"replace",root}});EXPECT_EQ(r["foundation"],root);EXPECT_EQ(r["top_z"],500);
  Document reopened;ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()),reopened));
  const auto c=construction::checkFoundation(reopened);ASSERT_TRUE(c);const auto value=json::parse(*c);
  EXPECT_EQ(value["plans"][0]["status"],"current");
  for(const auto& o:reopened.objects()) if(o.parent==root) EXPECT_FALSE(checkRecipe(o,reopened.regenContext()).has_value());
}

TEST_F(FoundationPads, IdempotenceAndHandEditsAreProtected) {
  post(1000,1000);auto r=generate();const std::string root=r["foundation"],pad=r["quantities"]["foundation_pad"]["id"];
  const auto steps=m.history().undoCount();EXPECT_TRUE(generate()["unchanged"]);EXPECT_EQ(m.history().undoCount(),steps);
  gp_Trsf move;move.SetTranslation(gp_Vec(50,0,0));ASSERT_TRUE(m.transform({pad},move,"Hand Edit"));
  const auto before=serializeDocument(m.document());EXPECT_FALSE(m.generateFoundation(json{{"replace",root}}.dump()));EXPECT_EQ(serializeDocument(m.document()),before);
  generate({{"replace",root},{"on_modified","replace"}});EXPECT_EQ(check(root)["status"],"current");
}

TEST_F(FoundationPads, OverlappingPadsFailAtomicallyAndDoNotChangeBrep) {
  post(1000,1000);post(1100,1000);const auto before=serializeDocument(m.document());const auto steps=m.history().undoCount();
  EXPECT_FALSE(m.generateFoundation(json{{"inputs",inputs()}}.dump()));EXPECT_EQ(serializeDocument(m.document()),before);EXPECT_EQ(m.history().undoCount(),steps);
}

TEST_F(FoundationPads, ExistingSolidCollisionFailsAndAdjacentContactIsAllowed) {
  post(1000,1000);
  const auto request=json{{"command","create_box"},{"args",{{"origin",{850,850,250}},{"width",300},{"depth",300},{"height",150}}}};
  auto response=json::parse(api.execute(request.dump()));ASSERT_TRUE(response["ok"]);const std::string box=response["result"]["id"];
  const auto before=serializeDocument(m.document());EXPECT_FALSE(m.generateFoundation(json{{"inputs",inputs()}}.dump()));EXPECT_EQ(serializeDocument(m.document()),before);
  gp_Trsf move;move.SetTranslation(gp_Vec(300,0,0));ASSERT_TRUE(m.transform({box},move,"Adjacent"));generate();
}

TEST_F(FoundationPads, TooSmallFootprintAndUnselectedMarkerAreRejected) {
  const auto p=post(1000,1000);auto in=inputs();in["width"]=50;
  EXPECT_FALSE(m.generateFoundation(json{{"inputs",in}}.dump()));
  ASSERT_TRUE(m.setAttribute({p},"display_only","true"));const auto before=serializeDocument(m.document());
  EXPECT_FALSE(m.generateFoundation(json{{"inputs",inputs()}}.dump()));EXPECT_EQ(serializeDocument(m.document()),before);
  EXPECT_FALSE(m.generateFoundation(R"({"inputs":{"type":"pads","frame":"missing"}})"));
}

TEST_F(FoundationPads, PreviewIsReadOnlyAndRevisionIsRequiredToApply) {
  post(1000,1000);const auto before=serializeDocument(m.document());
  auto request=json{{"command","generate_foundation"},{"args",{{"inputs",inputs()}}}};
  auto response=json::parse(api.execute(request.dump()));ASSERT_TRUE(response["ok"])<<response.dump();EXPECT_TRUE(response["result"]["preview"]);
  EXPECT_EQ(serializeDocument(m.document()),before);
  request["args"]["preview"]=false;EXPECT_FALSE(json::parse(api.execute(request.dump()))["ok"]);
  request["args"]["expected_revision"]=m.history().revision();EXPECT_TRUE(json::parse(api.execute(request.dump()))["ok"]);
}

TEST_F(FoundationPads, RotatedAnchoredColumnUsesPhysicalCenterAndWorldXYDimensions) {
  Modeler::MemberSpec spec;spec.start={1000,1000,400};spec.end={1000,1000,1000};spec.section.width=spec.section.depth=120;
  spec.role="column";spec.materialClass="timber";spec.anchor.across=1;spec.roll=.3;
  auto id=m.createMember(spec);ASSERT_TRUE(id);posts.push_back(*id);
  auto group=m.group(posts,"Posts");ASSERT_TRUE(group);gp_Trsf spin;spin.SetRotation(gp_Ax1(gp::Origin(),gp::DZ()),.7);ASSERT_TRUE(m.transform({*group},spin,"Rotate"));
  const auto r=generate();const std::string pad=r["quantities"]["foundation_pad"]["id"];
  const auto a=geom::exactBounds(m.document().worldShape(*m.document().find(*id))),b=geom::exactBounds(m.document().worldShape(*m.document().find(pad)));
  EXPECT_NEAR((a.CornerMin().X()+a.CornerMax().X())/2,(b.CornerMin().X()+b.CornerMax().X())/2,1e-5);
  EXPECT_NEAR((a.CornerMin().Y()+a.CornerMax().Y())/2,(b.CornerMin().Y()+b.CornerMax().Y())/2,1e-5);
}

TEST_F(FoundationPads, DeletedSourceAndDeletedPadAreNotReportedAsComplete) {
  const auto p=post(1000,1000);auto r=generate();const std::string root=r["foundation"],pad=r["quantities"]["foundation_pad"]["id"];
  ASSERT_TRUE(m.remove({pad}));auto c=check(root);EXPECT_EQ(c["status"],"modified");EXPECT_TRUE(c["generated_parts_volume_m3"].is_null());
  generate({{"replace",root},{"on_modified","replace"}});ASSERT_TRUE(m.remove({p}));EXPECT_EQ(check(root)["status"],"source_missing_or_unsupported");
}

TEST_F(FoundationPads, RebindRecreatedPostsExplicitlyAndRejectChangingPlanType) {
  const auto p=post(1000,1000);const auto made=generate();const std::string root=made["foundation"];
  ASSERT_TRUE(m.remove({p}));posts.clear();const auto next=post(1000,1000);
  EXPECT_FALSE(m.generateFoundation(json{{"replace",root}}.dump()));
  auto result=generate({{"replace",root},{"inputs",{{"sources",{next}}}}});EXPECT_EQ(result["foundation"],root);EXPECT_EQ(check(root)["status"],"current");
  const auto before=serializeDocument(m.document());EXPECT_FALSE(m.generateFoundation(json{{"replace",root},{"inputs",{{"type","strip"}}}}.dump()));
  EXPECT_EQ(serializeDocument(m.document()),before);
}

TEST_F(FoundationPads, TwoStoreyHouseCanPlanOnlyMissingRealFloorPostsAndExportFoundationPlan) {
  const auto frame=m.generateFrame(R"({"inputs":{"x_bays":4,"y_bays":4,"sections":{"floor_post":"TIMBER-90x90"}}})");ASSERT_TRUE(frame)<<frame.error();
  const std::string id=json::parse(*frame)["frame"];
  ASSERT_TRUE(m.generateFoundation(json{{"inputs",{{"frame",id}}}}.dump()));
  auto read=construction::foundationSupports(m.document());ASSERT_TRUE(read);auto report=json::parse(*read);
  for(const auto& row:report["rows"]) if(row["role"]=="floor_post" && row["status"]=="missing") posts.push_back(row["id"]);
  ASSERT_FALSE(posts.empty());const auto r=generate();EXPECT_EQ(r["quantities"]["foundation_pad"]["ids"].size(),posts.size());
  report=supports();for(const auto& row:report["rows"]) EXPECT_EQ(row["path_status"],"geometry_detected");
  Document reopened;ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()),reopened));
  const auto checked=construction::foundationSupports(reopened,json{{"ids",posts}}.dump());ASSERT_TRUE(checked);
  const auto restored=json::parse(*checked);for(const auto& row:restored["rows"]) EXPECT_EQ(row["path_status"],"geometry_detected");
  const auto all=construction::foundationSupports(reopened);ASSERT_TRUE(all);const auto allRows=json::parse(*all);
  int count=0;for(const auto& row:allRows["rows"]) if(row["role"]=="floor_post") {++count;EXPECT_EQ(row["path_status"],"geometry_detected");}
  EXPECT_EQ(count,6);
  const auto path=std::filesystem::temp_directory_path()/"foundation_pad_house.dxf";
  const auto exported=json::parse(api.execute(json{{"command","export_drawing"},{"args",{{"kind","framing"},{"preset","foundation"},{"path",path.string()}}}}.dump()));
  EXPECT_TRUE(exported["ok"])<<exported.dump();EXPECT_TRUE(std::filesystem::exists(path));
}
