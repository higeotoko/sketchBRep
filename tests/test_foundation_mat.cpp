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
// Mat foundation (べた基礎): stems under the sills on a slab filling their outline.
// Ring 3000×2000 on 120 sills, bottom at Z=420; defaults stem 150×400, slab 150, packing 20.
struct FoundationMat : testing::Test {
  Modeler m;
  api::CommandApi api{m};
  std::vector<ObjectId> ids;
  ObjectId sill(gp_Pnt a, gp_Pnt b) {
    Modeler::MemberSpec spec; spec.start=a; spec.end=b;
    spec.section.width=spec.section.depth=120; spec.up=gp::DZ(); spec.anchor.up=1;
    spec.role="sill"; spec.materialClass="timber";
    auto id=m.createMember(spec); EXPECT_TRUE(id); if(!id) return {};
    ids.push_back(*id); return *id;
  }
  void ring(double x=0, double y=0) {
    sill({x,y,540},{x+3000,y,540}); sill({x+3000,y,540},{x+3000,y+2000,540});
    sill({x+3000,y+2000,540},{x,y+2000,540}); sill({x,y+2000,540},{x,y,540});
  }
  json inputs(json extra=json::object()) { json in={{"type","mat"},{"sources",ids}}; in.merge_patch(extra); return in; }
  json generate(json args=json::object()) {
    if(!args.contains("inputs") && !args.contains("replace")) args["inputs"]=inputs();
    auto r=m.generateFoundation(args.dump()); EXPECT_TRUE(r) << r.error();
    return r?json::parse(*r):json::object();
  }
  std::string refused(json args) {
    const auto before=serializeDocument(m.document()); const auto steps=m.history().undoCount();
    auto r=m.generateFoundation(args.dump()); EXPECT_FALSE(r);
    EXPECT_EQ(serializeDocument(m.document()),before); EXPECT_EQ(m.history().undoCount(),steps);
    return r?std::string():r.error();
  }
  json check(const std::string& id) {
    auto r=construction::checkFoundation(m.document(),json{{"foundation",id}}.dump());
    EXPECT_TRUE(r) << r.error(); return r?json::parse(*r)["plans"][0]:json::object();
  }
  Bnd_Box bounds(const std::string& id) { return geom::exactBounds(m.document().worldShape(*m.document().find(id))); }
};
}

TEST_F(FoundationMat, RingMakesStemsOnASlabFillingTheOutlineInOneUndo) {
  ring(); const auto before=serializeDocument(m.document()); const auto undo=m.history().undoCount();
  const auto r=generate(); const std::string root=r.at("foundation");
  EXPECT_EQ(r["name"],"べた基礎計画");
  EXPECT_NEAR(r["top_z"].get<double>(),400,1e-6);
  EXPECT_NEAR(r["quantities"]["foundation_stem"]["volume_m3"].get<double>(),1.5*.4,1e-8);
  EXPECT_NEAR(r["quantities"]["foundation_slab"]["volume_m3"].get<double>(),3.15*2.15*.15,1e-8);
  EXPECT_FALSE(r["quantities"].contains("foundation_footing"));
  const auto slab=bounds(r["quantities"]["foundation_slab"]["id"]);
  EXPECT_NEAR(slab.CornerMin().Z(),-150,1e-6); EXPECT_NEAR(slab.CornerMax().Z(),0,1e-6);
  EXPECT_NEAR(slab.CornerMin().X(),-75,1e-6); EXPECT_NEAR(slab.CornerMax().Y(),2075,1e-6);
  const auto stem=bounds(r["quantities"]["foundation_stem"]["id"]);
  EXPECT_NEAR(stem.CornerMin().Z(),0,1e-6); EXPECT_NEAR(stem.CornerMax().Z(),400,1e-6);
  EXPECT_EQ(m.history().undoCount(),undo+1);
  auto c=check(root); EXPECT_EQ(c["status"],"current"); EXPECT_FALSE(c["verified"]);
  EXPECT_EQ(c["inputs"]["type"],"mat");
  for(const auto& [role,q]:c["quantities"].items()) EXPECT_EQ(q["solids"],1) << role;
  for(const auto& source:c["sources"]) EXPECT_EQ(source["packing_gap_matches"],true);
  ASSERT_TRUE(m.undo()); EXPECT_EQ(serializeDocument(m.document()),before);
  ASSERT_TRUE(m.redo()); EXPECT_EQ(check(root)["status"],"current");
}

TEST_F(FoundationMat, InteriorSillAddsAStemButNotSlab) {
  ring(); sill({0,1000,540},{3000,1000,540});
  const auto r=generate();
  EXPECT_NEAR(r["quantities"]["foundation_stem"]["volume_m3"].get<double>(),(1.5+2.85*.15)*.4,1e-8);
  EXPECT_NEAR(r["quantities"]["foundation_slab"]["volume_m3"].get<double>(),3.15*2.15*.15,1e-8);
}

TEST_F(FoundationMat, LShapedOutlineFollowsTheSills) {
  // 3000×2000 with a 1000×1000 corner cut out (an L): slab follows the outer edge of the stems.
  sill({0,0,540},{3000,0,540}); sill({3000,0,540},{3000,1000,540}); sill({3000,1000,540},{2000,1000,540});
  sill({2000,1000,540},{2000,2000,540}); sill({2000,2000,540},{0,2000,540}); sill({0,2000,540},{0,0,540});
  const auto r=generate();
  const double outer=3.15*2.15-1.0*1.0; // the notch shifts with the stem faces but keeps 1000×1000
  EXPECT_NEAR(r["quantities"]["foundation_slab"]["volume_m3"].get<double>(),outer*.15,1e-8);
}

TEST_F(FoundationMat, PassageCutsOneInteriorStem) {
  ring(); sill({0,1000,540},{3000,1000,540});
  const auto r=generate({{"inputs",inputs({{"passages",{{{"at",{1500,1000}}}}}})}});
  EXPECT_NEAR(r["quantities"]["foundation_stem"]["volume_m3"].get<double>(),(1.5+2.85*.15)*.4-.6*.15*.33,1e-8);
  EXPECT_EQ(r["inputs"]["passages"][0]["width"],600); EXPECT_EQ(r["inputs"]["passages"][0]["height"],330);
  EXPECT_EQ(check(r["foundation"])["quantities"]["foundation_stem"]["solids"],1);
}

TEST_F(FoundationMat, PassageOnOuterStemJunctionOrNoStemIsRefused) {
  ring(); sill({0,1000,540},{3000,1000,540});
  EXPECT_NE(refused({{"inputs",inputs({{"passages",{{{"at",{1500,0}}}}}})}}).find("外周"),std::string::npos);
  EXPECT_NE(refused({{"inputs",inputs({{"passages",{{{"at",{300,1000}}}}}})}}).find("交差部"),std::string::npos);
  EXPECT_NE(refused({{"inputs",inputs({{"passages",{{{"at",{1500,500}}}}}})}}).find("立上りがありません"),std::string::npos);
  refused({{"inputs",inputs({{"passages",{{{"at",{1500,1000}},{"height",400}}}}})}});
  refused({{"inputs",inputs({{"passages",{{{"at",{1400,1000}}},{{"at",{1800,1000}}}}}})}});
}

TEST_F(FoundationMat, OpenOrSplitSillsAreRefused) {
  sill({0,0,540},{3000,0,540}); sill({3000,0,540},{3000,2000,540}); sill({3000,2000,540},{0,2000,540});
  EXPECT_NE(refused({{"inputs",inputs()}}).find("閉じた外周"),std::string::npos);
  ids.clear(); ring(0,0); ring(5000,0);
  EXPECT_NE(refused({{"inputs",inputs()}}).find("つながっていません"),std::string::npos);
}

TEST_F(FoundationMat, StripOptionsAreNotAcceptedAndTypeIsKeptOnReplace) {
  ring();
  refused({{"inputs",inputs({{"footing_width",450}})}});
  const auto r=generate(); const std::string root=r["foundation"];
  refused({{"replace",root},{"inputs",{{"type","strip"}}}});
  const auto again=generate({{"replace",root},{"inputs",{{"slab_thickness",200}}}});
  EXPECT_EQ(again["foundation"],root);
  EXPECT_NEAR(again["quantities"]["foundation_slab"]["volume_m3"].get<double>(),3.15*2.15*.2,1e-8);
  EXPECT_TRUE(generate({{"inputs",inputs({{"slab_thickness",200}})}})["unchanged"]);
}

TEST_F(FoundationMat, StripPlanOnTheSameSillsAsksToRemoveOne) {
  ring();
  ASSERT_TRUE(m.generateFoundation(json{{"inputs",{{"sources",ids}}}}.dump()));
  EXPECT_NE(refused({{"inputs",inputs()}}).find("別の種別"),std::string::npos);
}

TEST_F(FoundationMat, ReopenedPlanIsCurrentAndSillMoveAsksForRegeneration) {
  ring(); sill({1500,0,540},{1500,2000,540});
  const auto r=generate({{"inputs",inputs({{"passages",{{{"at",{1500,1000}},{"width",450},{"height",300}}}}})}});
  const std::string root=r["foundation"];
  Document reopened; ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()),reopened));
  const auto c=construction::checkFoundation(reopened); ASSERT_TRUE(c);
  EXPECT_EQ(json::parse(*c)["plans"][0]["status"],"current");
  for(const auto& o:reopened.objects()) if(o.parent==root) EXPECT_FALSE(checkRecipe(o,reopened.regenContext()).has_value());
  gp_Trsf move; move.SetTranslation(gp_Vec(100,0,0)); ASSERT_TRUE(m.transform({ids[4]},move,"Move Inner Sill"));
  EXPECT_EQ(check(root)["status"],"needs_regeneration");
  // One sill raised: mixed heights are not one mat (same rule as strip footings).
  move.SetTranslation(gp_Vec(0,0,30)); ASSERT_TRUE(m.transform({ids[0]},move,"Raise Sill"));
  EXPECT_EQ(check(root)["status"],"source_missing_or_unsupported");
}

TEST_F(FoundationMat, FloorPostStandingOnTheSlabIsDetected) {
  ring(); generate();
  Modeler::MemberSpec s; s.start={1500,1000,0}; s.end={1500,1000,400}; s.section.width=s.section.depth=90;
  s.role="floor_post"; s.materialClass="timber";
  auto post=m.createMember(s); ASSERT_TRUE(post);
  auto r=construction::foundationSupports(m.document(),json{{"ids",{*post}}}.dump()); ASSERT_TRUE(r) << r.error();
  const auto row=json::parse(*r)["rows"][0];
  EXPECT_EQ(row["status"],"detected") << row.dump(); EXPECT_FALSE(row["verified"]);
}

TEST_F(FoundationMat, PreviewIsReadOnlyAndRevisionIsRequiredToApply) {
  ring(); const auto before=serializeDocument(m.document());
  auto request=json{{"command","generate_foundation"},{"args",{{"inputs",inputs()}}}};
  auto response=json::parse(api.execute(request.dump())); ASSERT_TRUE(response["ok"]) << response.dump();
  EXPECT_TRUE(response["result"]["preview"]); EXPECT_EQ(serializeDocument(m.document()),before);
  request["args"]["preview"]=false; EXPECT_FALSE(json::parse(api.execute(request.dump()))["ok"]);
  request["args"]["expected_revision"]=m.history().revision(); EXPECT_TRUE(json::parse(api.execute(request.dump()))["ok"]);
}

TEST_F(FoundationMat, GeneratedTwoStoreyFrameRotatedGivesOneSlabUnderItsOutline) {
  auto frame=m.generateFrame(R"({"inputs":{"x_bays":2,"y_bays":2}})"); ASSERT_TRUE(frame) << frame.error();
  const std::string id=json::parse(*frame)["frame"];
  gp_Trsf rotation; rotation.SetRotation(gp_Ax1(gp::Origin(),gp::DZ()),.4);
  ASSERT_TRUE(m.transform({id},rotation,"Rotate Frame"));
  const auto r=generate({{"inputs",{{"type","mat"},{"frame",id}}}});
  const auto c=check(r["foundation"]);
  EXPECT_EQ(c["status"],"current");
  EXPECT_EQ(c["quantities"]["foundation_slab"]["solids"],1);
  EXPECT_EQ(c["quantities"]["foundation_stem"]["solids"],1);
  EXPECT_GT(c["quantities"]["foundation_slab"]["volume_m3"].get<double>(),0);
}
