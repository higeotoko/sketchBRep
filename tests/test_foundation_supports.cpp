#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "sbr/ConstructionFoundation.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct FoundationSupports:testing::Test {
  Modeler m;api::CommandApi api{m};
  ObjectId member(const char* role,gp_Pnt a,gp_Pnt b,double width=120) {
    Modeler::MemberSpec s;s.start=a;s.end=b;s.section.width=s.section.depth=width;s.role=role;s.materialClass="timber";
    if(std::string(role)=="sill") {s.up=gp::DZ();s.anchor.up=1;}
    auto r=m.createMember(s);EXPECT_TRUE(r);return r?*r:"";
  }
  ObjectId sill() {return member("sill",{0,0,540},{3000,0,540});}
  json query(json args=json::object()) {
    auto r=construction::foundationSupports(m.document(),args.dump());EXPECT_TRUE(r)<<r.error();return r?json::parse(*r):json::object();
  }
  json row(const std::string& id) {
    auto r=query({{"ids",{id}}});EXPECT_EQ(r["rows"].size(),1u);return r["rows"][0];
  }
  json foundation(const std::string& id) {
    auto r=m.generateFoundation(json{{"inputs",{{"sources",{id}}}}}.dump());EXPECT_TRUE(r)<<r.error();return json::parse(*r);
  }
  std::string pad(double x,double y,double z,double width,double depth,double height) {
    const auto r=json::parse(api.execute(json{{"command","create_box"},{"args",{{"origin",{x,y,z}},{"width",width},{"depth",depth},{"height",height}}}}.dump()));
    EXPECT_TRUE(r["ok"])<<r.dump();const std::string id=r["result"]["id"];
    EXPECT_TRUE(m.setAttribute({id},"kind","foundation_pad"));EXPECT_TRUE(m.setAttribute({id},"material_class","rc"));return id;
  }
};
}

TEST_F(FoundationSupports, ColumnSillConcreteChainIsReadOnly) {
  const auto s=sill();foundation(s);const auto c=member("column",{1500,0,540},{1500,0,3000},105);
  const auto before=serializeDocument(m.document());const auto revision=m.history().revision();
  auto r=row(c);EXPECT_EQ(r["status"],"detected");EXPECT_EQ(r["path_status"],"geometry_detected")<<r.dump();
  ASSERT_EQ(r["via"].size(),1u);EXPECT_EQ(r["via"][0]["status"],"detected");
  EXPECT_NEAR(r["candidates"][0]["coverage_ratio"].get<double>(),1,1e-6);
  EXPECT_NEAR(r["via"][0]["candidates"][0]["gap_mm"].get<double>(),20,1e-5);EXPECT_FALSE(r["verified"]);
  EXPECT_EQ(serializeDocument(m.document()),before);EXPECT_EQ(m.history().revision(),revision);
}

TEST_F(FoundationSupports, FloatingAndPenetratingColumnAreNotDetectedAsContact) {
  sill();const auto c=member("column",{1500,0,550},{1500,0,3000},105);
  EXPECT_EQ(row(c)["status"],"gap");
  gp_Trsf move;move.SetTranslation(gp_Vec(0,0,-20));ASSERT_TRUE(m.transform({c},move,"Move"));
  EXPECT_EQ(row(c)["status"],"interference");EXPECT_EQ(row(c)["path_status"],"needs_review");
}

TEST_F(FoundationSupports, FloorPostNeedsARealPadAndReportsPartialSupport) {
  const auto c=member("floor_post",{1500,1000,400},{1500,1000,800},90);
  EXPECT_EQ(row(c)["status"],"missing");const auto p=pad(1450,950,200,100,100,200);
  EXPECT_EQ(row(c)["status"],"detected");EXPECT_EQ(row(c)["path_status"],"geometry_detected");
  gp_Trsf move;move.SetTranslation(gp_Vec(50,0,0));ASSERT_TRUE(m.transform({p},move,"Move Pad"));
  auto r=row(c);EXPECT_EQ(r["status"],"partial");EXPECT_NEAR(r["candidates"][0]["coverage_ratio"].get<double>(),.5,1e-5);
}

TEST_F(FoundationSupports, MultipleFullSupportsAreAmbiguousAndPartialSupportsAreNotAdded) {
  const auto c=member("floor_post",{1000,1000,400},{1000,1000,800},100);
  pad(950,950,200,100,100,200);pad(950,950,200,100,100,200);
  EXPECT_EQ(row(c)["status"],"ambiguous");
  auto all=m.document().objects();std::vector<ObjectId> pads;
  for(const auto& o:all) if(o.attributes.count("kind") && o.attributes.at("kind")=="foundation_pad") pads.push_back(o.id);
  ASSERT_TRUE(m.remove(pads));pad(950,950,200,50,100,200);pad(1000,950,200,50,100,200);
  EXPECT_EQ(row(c)["status"],"partial");
}

TEST_F(FoundationSupports, SideContactDoesNotCountAndNearestLayerWins) {
  const auto c=member("floor_post",{1000,1000,400},{1000,1000,800},100);
  pad(1050,950,200,100,100,200);EXPECT_EQ(row(c)["status"],"missing");
  pad(950,950,100,100,100,200);auto r=row(c);EXPECT_EQ(r["status"],"gap");
  pad(950,950,200,100,100,200);r=row(c);EXPECT_EQ(r["status"],"detected");EXPECT_EQ(r["candidates"].size(),1u);
}

TEST_F(FoundationSupports, SourceChangesAndEditedFoundationRequireReviewDespiteGeometry) {
  const auto s=sill();const auto f=foundation(s);const std::string root=f["foundation"];
  const auto c=member("column",{1500,0,540},{1500,0,3000},105);
  auto a=query();ASSERT_TRUE(m.setAttribute({s},"note","changed"));auto b=query();
  EXPECT_NE(a["source_signature"],b["source_signature"]);EXPECT_EQ(row(c)["status"],"detected");
  EXPECT_EQ(row(c)["path_status"],"needs_review");
  ASSERT_TRUE(m.generateFoundation(json{{"replace",root}}.dump()));EXPECT_EQ(row(c)["path_status"],"geometry_detected");
  ASSERT_TRUE(m.setAttribute({root},"note","hand edit"));EXPECT_EQ(row(c)["path_status"],"needs_review");
}

TEST_F(FoundationSupports, SharedFramePlaceholdersAndUpperRoutesAreUnassessed) {
  const auto frame=m.generateFrame(R"({"inputs":{"x_bays":2,"y_bays":2,"columns":[[1,1]]}})");ASSERT_TRUE(frame);
  const std::string id=json::parse(*frame)["frame"];
  auto f=m.generateFoundation(json{{"inputs",{{"frame",id}}}}.dump());ASSERT_TRUE(f);
  auto report=query();int markers=0,upper=0;
  for(const auto& r:report["rows"]) {
    if(r["role"]=="floor_post") {EXPECT_EQ(r["status"],"not_evaluated");++markers;}
    if(r["role"]=="column" && r["status"]=="not_evaluated") ++upper;
  }
  EXPECT_GT(markers,0);EXPECT_GT(upper,0);
}

TEST_F(FoundationSupports, RotationHiddenSupportAndSaveReopenPreserveResults) {
  const auto s=sill();foundation(s);const auto c=member("column",{1500,0,540},{1500,0,3000},105);
  std::vector<ObjectId> top;for(const auto& o:m.document().objects()) if(o.parent.empty()) top.push_back(o.id);
  auto group=m.group(top,"Moved Assembly");ASSERT_TRUE(group);
  gp_Trsf spin;spin.SetRotation(gp_Ax1(gp::Origin(),gp::DZ()),.7);ASSERT_TRUE(m.transform({*group},spin,"Rotate"));
  EXPECT_EQ(row(c)["status"],"detected"); // moved generated root is marked edited, geometry still measured
  auto before=row(c);Document reopened;ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()),reopened));
  auto report=construction::foundationSupports(reopened,json{{"ids",{c}}}.dump());ASSERT_TRUE(report);
  EXPECT_EQ(json::parse(*report)["rows"][0]["status"],before["status"]);
  m.setTagVisible("基礎・土台",false);EXPECT_EQ(row(c)["status"],"detected");
}

TEST_F(FoundationSupports, QueryScopesAndBudgetCannotInventSuccess) {
  const auto s=sill();foundation(s);auto c=member("column",{1500,0,540},{1500,0,3000},105);
  auto q=query({{"ids",json::array()}});EXPECT_TRUE(q["rows"].empty());
  q=query({{"pair_budget",1}});EXPECT_EQ(q["incomplete"],true);
  bool unknown=false;for(const auto& r:q["rows"]) if(r["status"]=="not_evaluated") unknown=true;EXPECT_TRUE(unknown);
  EXPECT_FALSE(construction::foundationSupports(m.document(),R"({"tolerance_mm":20})"));
  EXPECT_FALSE(construction::foundationSupports(m.document(),R"({"ids":["missing"]})"));
  EXPECT_EQ(api.effectOf(R"({"command":"check_foundation_supports"})"),api::CommandApi::Effect::Read);
  const auto result=json::parse(api.execute(R"({"command":"check_foundation_supports"})"));EXPECT_TRUE(result["ok"]);
}

TEST_F(FoundationSupports, HoleInSupportIsMeasuredFromFacesRatherThanBounds) {
  const auto c=member("floor_post",{1000,1000,400},{1000,1000,800},100);
  const auto outer=pad(950,950,200,100,100,200),hole=pad(975,975,150,50,50,300);
  const auto cut=m.boolean(BooleanKind::Subtract,{outer,hole});ASSERT_TRUE(cut);
  ASSERT_TRUE(m.setAttribute({*cut},"kind","foundation_pad"));ASSERT_TRUE(m.setAttribute({*cut},"material_class","rc"));
  const auto before=serializeDocument(m.document());auto r=row(c);
  EXPECT_EQ(r["status"],"partial");EXPECT_NEAR(r["candidates"][0]["coverage_ratio"].get<double>(),.75,1e-5);
  EXPECT_EQ(serializeDocument(m.document()),before);
}

TEST_F(FoundationSupports, SubjectGroupCanUseOutsideSupportsAndBadGeometryIsExplicit) {
  const auto s=sill();foundation(s);const auto c=member("column",{1500,0,540},{1500,0,3000},105);
  const auto group=m.group({c},"Columns");ASSERT_TRUE(group);
  auto q=query({{"within",*group}});ASSERT_EQ(q["rows"].size(),1u);EXPECT_EQ(q["rows"][0]["path_status"],"geometry_detected");
  const auto p=pad(1450,950,200,100,100,200);ASSERT_TRUE(m.setAttribute({p},"material_class","unknown"));
  q=query();ASSERT_EQ(q["unassessed_supports"].size(),1u);EXPECT_EQ(q["unassessed_supports"][0]["id"],p);
}

TEST_F(FoundationSupports, SteppedConcreteDoesNotSilentlyIgnoreLowerTopFaces) {
  const auto c=member("floor_post",{1000,1000,400},{1000,1000,800},100);
  const auto a=pad(950,950,200,100,100,200),b=pad(1050,950,200,100,100,250);
  const auto stepped=m.boolean(BooleanKind::Union,{a,b});ASSERT_TRUE(stepped);
  ASSERT_TRUE(m.setAttribute({*stepped},"kind","foundation_pad"));ASSERT_TRUE(m.setAttribute({*stepped},"material_class","rc"));
  const auto q=query();ASSERT_EQ(q["unassessed_supports"].size(),1u);EXPECT_EQ(row(c)["status"],"missing");
}

TEST_F(FoundationSupports, ShortFloorPostUsesConcreteBelowInsteadOfSillWhollyAboveItsBase) {
  const auto s=sill();foundation(s);
  const auto p=member("floor_post",{1500,0,400},{1500,0,410},90);
  const auto r=row(p);EXPECT_EQ(r["status"],"detected");EXPECT_EQ(r["path_status"],"geometry_detected");
  ASSERT_EQ(r["candidates"].size(),1u);EXPECT_EQ(r["candidates"][0]["role"],"foundation_stem");
}
