#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "sbr/ConstructionFrame.h"
#include "sbr/ConstructionQuantity.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/doc/Schedules.h"
#include "sbr/doc/ColumnCheck.h"
#include "sbr/doc/FoundationGround.h"
#include <algorithm>
#include "sbr/draw/Drawing.h"
#include "sbr/geom/Dxf.h"
#include "sbr/geom/Jww.h"

using namespace sbr;
using namespace sbr::doc;
using nlohmann::json;
namespace {
struct Quantities : testing::Test {
  Modeler m;
  ObjectId member(double length = 3000, double width = 100, double depth = 200) {
    Modeler::MemberSpec spec;
    spec.start = gp::Origin();
    spec.end = gp_Pnt(length, 0, 0);
    spec.section.width = width;
    spec.section.depth = depth;
    spec.role = "floor_beam";
    spec.materialClass = "timber";
    const auto id = m.createMember(spec);
    EXPECT_TRUE(id) << id.error();
    return id ? *id : "";
  }
  json report(json options = json::object()) {
    const auto result = construction::takeOff(m.document(), options.dump());
    EXPECT_TRUE(result) << result.error();
    return result ? json::parse(*result) : json::object();
  }
  json frame() {
    const auto result = m.generateFrame(R"({"inputs":{"x_bays":2,"y_bays":2}})");
    EXPECT_TRUE(result) << result.error();
    return result ? json::parse(*result) : json::object();
  }
};
}  // namespace
TEST_F(Quantities, CountsAndVolumeMatchHandCalculation) {
  member();
  member();
  const auto r = report();
  ASSERT_EQ(r["rows"].size(), 1u);
  EXPECT_TRUE(r["complete"]);
  EXPECT_EQ(r["totals"]["count"], 2);
  const auto& row = r["rows"][0];
  EXPECT_EQ(row["count"], 2);
  EXPECT_DOUBLE_EQ(row["total_length_mm"].get<double>(), 6000);
  EXPECT_NEAR(row["design_volume_m3"].get<double>(), 2 * .1 * .2 * 3, 1e-12);
  EXPECT_NEAR(row["net_volume_m3"].get<double>(), .12, 1e-10);
  EXPECT_DOUBLE_EQ(row["stock_equivalent"].get<double>(), 1.5);
  EXPECT_EQ(row["stock_lower_bound"], 2);
}
TEST_F(Quantities, SharedPlacementsAndSheetsAreNotDefinitionCounts) {
  const auto made = frame();
  const auto r = report();
  EXPECT_EQ(r["totals"]["count"].get<int>(), made["parts"].get<int>() - made["hardware"].get<int>());
  EXPECT_GT(r["totals"]["count"].get<int>(), static_cast<int>(m.document().definitions().size()));
  int sheets = 0;
  for (const auto& row : r["rows"])
    if (row["role"] == "floor_sheathing") {
      sheets += row["count"].get<int>();
      EXPECT_TRUE(row["stock_equivalent"].is_null());
      EXPECT_EQ(row["unit"], "枚");
      EXPECT_NEAR(row["design_volume_m3"].get<double>(), .910 * .910 * .028 * row["count"].get<int>(), 1e-12);
    }
  EXPECT_EQ(sheets, 4);
  EXPECT_TRUE(r["complete"]);
}
TEST_F(Quantities, UniformScaleAndMirroredSharedMembersKeepTheirOwnSizes) {
  const auto id = member();
  gp_Trsf tr;
  tr.SetScale(gp::Origin(), 2);
  ASSERT_TRUE(m.transform({id}, tr, "Scale"));
  const auto scaled = report();
  EXPECT_NEAR(scaled["rows"][0]["design_volume_m3"].get<double>(), .06 * 8, 1e-12);
  EXPECT_NEAR(scaled["rows"][0]["net_volume_m3"].get<double>(), .06 * 8, 1e-10);
  frame();
  ObjectId shared;
  for (const auto& o : m.document().objects())
    if (o.attributes.count("kind") && o.attributes.at("kind") == "rafter") {
      shared = o.id;
      break;
    }
  const auto before = report();
  ASSERT_TRUE(m.mirrorPlacement(shared, 0));
  const auto after = report();
  EXPECT_EQ(after["totals"]["count"], before["totals"]["count"]);
  EXPECT_NEAR(after["totals"]["known_design_volume_m3"].get<double>(),
              before["totals"]["known_design_volume_m3"].get<double>(), 1e-10);
}
TEST_F(Quantities, EditedAndUnknownSizesAreNotSilentZeros) {
  const auto id = member();
  auto edited = *m.document().find(id);
  edited.recipe.modifiers.push_back(ScaleOp{gp::Origin(), 2});
  ASSERT_TRUE(rebuild(edited));
  m.document().replace(edited);
  const auto r = report();
  EXPECT_FALSE(r["complete"]);
  EXPECT_EQ(r["totals"]["not_evaluated_count"], 1);
  EXPECT_TRUE(r["rows"][0]["design_volume_m3"].is_null());
  EXPECT_NEAR(r["rows"][0]["net_volume_m3"].get<double>(), .48, 1e-10);
}
TEST_F(Quantities, TwoSolidShapesInOneLogicalObjectCountOnce) {
  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);
  builder.Add(compound, BRepPrimAPI_MakeBox(100, 100, 100).Shape());
  builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(200, 0, 0), 100, 100, 100).Shape().Reversed());
  Object object;
  object.id = generateObjectId();
  object.name = "one split member";
  object.recipe.base = ImportedShape{compound, "test"};
  object.shape = compound;
  object.attributes = {{"kind", "floor_beam"}, {"material_class", "timber"}};
  m.document().insert(object);
  const auto r = report();
  EXPECT_EQ(r["totals"]["count"], 1);
  EXPECT_NEAR(r["rows"][0]["net_volume_m3"].get<double>(), .002, 1e-12);
  EXPECT_TRUE(r["rows"][0]["length_mm"].is_null());
}
TEST_F(Quantities, StockConversionIsALengthRatioNotACuttingPlan) {
  for (int i = 0; i < 5; ++i) member(500);
  const auto r = report({{"stock_length", 1000}});
  const auto& row = r["rows"][0];
  EXPECT_DOUBLE_EQ(row["stock_equivalent"].get<double>(), 2.5);
  EXPECT_EQ(row["stock_lower_bound"], 3);
  EXPECT_FALSE(row["joint_required"]);
  member(1500);
  bool joint = false;
  const auto longer = report({{"stock_length", 1000}});
  for (const auto& line : longer["rows"])
    if (line["length_mm"] == 1500) joint = line["joint_required"].get<bool>();
  EXPECT_TRUE(joint);
}
TEST_F(Quantities, ScopeTagsAndVisibilityDoNotChangeTheModel) {
  const auto made = frame();
  const auto outside = member();
  ASSERT_TRUE(m.setLayer(outside, "outside"));
  const auto bytes = serializeDocument(m.document());
  const auto filtered = report({{"within", made["frame"]}, {"tags", {"2階床組"}}});
  for (const auto& p : filtered["parts"]) EXPECT_EQ(p["tag"], "2階床組");
  EXPECT_EQ(serializeDocument(m.document()), bytes);
  m.setTagVisible("2階床組", false);
  EXPECT_EQ(
      report({{"within", made["frame"]}, {"tags", {"2階床組"}}, {"visible_only", true}})["totals"]["count"],
      0);
  EXPECT_EQ(report({{"within", made["frame"]}, {"tags", {"2階床組"}}})["totals"]["count"],
            filtered["totals"]["count"]);
}
TEST_F(Quantities, ReadOnlyApiCsvAndSaveReopenAgree) {
  EXPECT_FALSE(construction::takeOff(m.document(), R"({"stock_length":0})"));
  member();
  api::CommandApi api(m);
  const auto rev = m.history().revision();
  const std::string command = R"({"command":"take_off","args":{"include_csv":true}})";
  EXPECT_EQ(api.effectOf(command), api::CommandApi::Effect::Read);
  const auto result = json::parse(api.execute(command));
  ASSERT_TRUE(result["ok"]);
  EXPECT_EQ(result["result"]["revision"], rev);
  EXPECT_EQ(m.history().revision(), rev);
  const std::string csv = result["result"]["csv"];
  EXPECT_EQ(csv.substr(0, 3), "\xef\xbb\xbf");
  EXPECT_NE(csv.find("B01"), std::string::npos);
  Document restored;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), restored));
  const auto reopened = construction::takeOff(restored);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(json::parse(*reopened)["rows"], report()["rows"]);
  EXPECT_FALSE(construction::takeOff(m.document(), R"({"within":"missing"})"));
  EXPECT_FALSE(construction::takeOff(m.document(), R"({"stock_length":0})"));
}
TEST_F(Quantities, FramingPresetsFilterBodiesAndKeepMatchingMarksAndGrid) {
  const auto made = frame();
  const auto bytes = serializeDocument(m.document());
  for (const auto* preset : {"foundation", "first_floor", "second_floor", "roof"}) {
    const auto spec = draw::framingView(m.document(), preset, made["frame"]);
    ASSERT_TRUE(spec) << spec.error();
    const auto view = draw::makeView(m.document(), *spec);
    ASSERT_TRUE(view) << view.error();
    const auto quantities = report({{"within", made["frame"]}, {"tags", spec->tags}, {"visible_only", true}});
    // docs/307: a row at an even pitch is marked once ("... @455"): no more marks than parts.
    EXPECT_GE(view->members.size(), 1u);
    EXPECT_LE(view->members.size(), quantities["parts"].size());
    if (std::string(preset) == "first_floor" || std::string(preset) == "roof") {
      const bool pitched = std::any_of(view->members.begin(), view->members.end(), [](const auto& mark) { return mark.text.find(" @") != std::string::npos; });
      EXPECT_TRUE(pitched) << preset;  // the joists / rafters as one mark with their pitch
    }
    EXPECT_EQ(view->grids.size(), 6u);
    for (const auto& mark : view->members) {
      bool found = false;
      for (const auto& row : quantities["rows"])
        if (mark.text.rfind(row["mark"].get<std::string>() + " ", 0) == 0) found = true;
      EXPECT_TRUE(found) << mark.text;
    }
    draw::SheetSpec page;
    page.scale = 30;
    const auto sheet = draw::layoutSheet(*view, page);
    ASSERT_TRUE(sheet);
    bool notice = false;
    for (const auto& item : sheet->items)
      if (item.text.find("断面・接合未照査") != std::string::npos) notice = true;
    EXPECT_TRUE(notice);
  }
  EXPECT_EQ(serializeDocument(m.document()), bytes);
}
TEST_F(Quantities, FramingDxfAndJwwRoundTripTheirGeometryAndWriteMarks) {
  frame();
  const auto spec = draw::framingView(m.document(), "second_floor");
  ASSERT_TRUE(spec);
  const auto view = draw::makeView(m.document(), *spec);
  ASSERT_TRUE(view);
  const auto sheet = draw::layoutSheet(*view, {});
  ASSERT_TRUE(sheet);
  const auto base = (std::filesystem::temp_directory_path() / generateObjectId()).string();
  const auto dxf = base + ".dxf", jww = base + ".jww";
  ASSERT_TRUE(draw::writeDxf(*sheet, dxf));
  ASSERT_TRUE(draw::writeJww(*sheet, jww));
  geom::DxfImportOptions dx;
  dx.allowPartial = true;
  const auto dxRead = geom::readDxf(dxf, dx);
  ASSERT_TRUE(dxRead);
  EXPECT_GT(dxRead->size(), 0u);
  geom::JwwImportOptions jw;
  jw.allowPartial = true;
  geom::JwwImportReport jwReport;
  const auto jwRead = geom::readJww(jww, jw, &jwReport);
  ASSERT_TRUE(jwRead);
  EXPECT_GT(jwReport.imported, 0);
  EXPECT_GT(jwReport.omitted["text"], 0);
  std::ifstream input(dxf, std::ios::binary);
  const std::string content((std::istreambuf_iterator<char>(input)), {});
  EXPECT_NE(content.find("P01"), std::string::npos);
  input.close();
  std::filesystem::remove(dxf);
  std::filesystem::remove(jww);
}
TEST_F(Quantities, HiddenFramingTagsRequireExplicitInclusion) {
  frame();
  m.setTagVisible("小屋組", false);
  EXPECT_FALSE(draw::framingView(m.document(), "roof"));
  const auto spec = draw::framingView(m.document(), "roof", "", true);
  ASSERT_TRUE(spec);
  const auto view = draw::makeView(m.document(), *spec);
  ASSERT_TRUE(view);
  EXPECT_GT(view->objects, 0);
  EXPECT_FALSE(draw::framingView(m.document(), "unknown"));
}

TEST_F(Quantities, ManuallyCreatedComponentsKeepMaterialMeaningAndDoNotHideUnknownAssemblies) {
  const auto a = member();
  const auto single = m.makeComponent({a}, "one member");
  ASSERT_TRUE(single);
  auto r = report();
  EXPECT_EQ(r["totals"]["count"], 1);
  EXPECT_TRUE(r["complete"]);
  EXPECT_NEAR(r["totals"]["known_design_volume_m3"].get<double>(), .06, 1e-12);
  const auto b = member(500), c = member(600);
  ASSERT_TRUE(m.makeComponent({b, c}, "two members"));
  r = report();
  EXPECT_EQ(r["totals"]["count"], 2);
  EXPECT_FALSE(r["complete"]);
  EXPECT_EQ(r["totals"]["not_evaluated_count"], 1);
  bool assembly = false;
  for (const auto& row : r["rows"])
    if (row["role"] == "unclassified") {
      assembly = true;
      EXPECT_EQ(row["unit"], "個");
      EXPECT_TRUE(row["design_volume_m3"].is_null());
      EXPECT_TRUE(row["net_volume_m3"].is_null());
    }
  EXPECT_TRUE(assembly);
}

// docs/289: the drawing set carries the framing plans of a generated frame.
TEST_F(Quantities, DrawingSetCarriesTheFramingPlans) {
  frame();
  const auto set = doc::drawingSet(m.document(), {});
  ASSERT_TRUE(set) << set.error();
  for (const char* name : {"基礎・土台伏図", "1階床伏図", "2階床伏図", "小屋伏図"})
    EXPECT_NE(std::find(set->names.begin(), set->names.end(), std::string(name)), set->names.end()) << name;
  sbr::api::CommandApi api(m);
  const auto r = json::parse(api.execute(R"({"command":"permit_intake_review","args":{}})"));
  ASSERT_TRUE(r["ok"]);
  for (const auto& row : r["result"]["rows"])
    if (row["form"] == "様式1-2" && (row["kind"] == "基礎伏図" || row["kind"] == "各階床伏図" || row["kind"] == "小屋伏図"))
      EXPECT_EQ(row["state"], "図面あり") << row.dump();
  // docs/290: frame elevations along the outer grid lines, each with member marks of its line only.
  std::vector<std::size_t> elevations;
  for (std::size_t i = 0; i < set->names.size(); ++i)
    if (set->names[i].find("通り軸組図") != std::string::npos) elevations.push_back(i);
  EXPECT_EQ(elevations.size(), 4u);
  for (const auto& row : r["result"]["rows"])
    if (row["form"] == "様式2" && row["kind"] == "2面以上の軸組図") EXPECT_EQ(row["state"], "図面あり") << row.dump();
  for (std::size_t i : elevations) {
    std::size_t texts = 0;
    for (const auto& item : set->sheets[i].items)
      if (item.kind == draw::Item::Kind::Text) ++texts;
    EXPECT_GT(texts, 12u) << set->names[i];
  }
}

// docs/291: the structural materials per role, given by the person, in the 使用構造材料一覧表.
TEST_F(Quantities, StructuralMaterialTableTakesTheGivenMaterials) {
  frame();
  sbr::api::CommandApi api(m);
  const auto call = [&](const std::string& command, const json& args) {
    return json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
  };
  auto table = call("structural_material_table", json::object());
  ASSERT_TRUE(table["ok"]) << table.dump();
  ASSERT_FALSE(table["result"]["rows"].empty());
  for (const auto& row : table["result"]["rows"]) EXPECT_EQ(row[3], "未入力");
  EXPECT_FALSE(call("set_structural_materials", {{"items", {{{"role", "nonsense"}, {"material", "すぎ"}}}}})["ok"]);
  EXPECT_FALSE(call("set_structural_materials", {{"items", {{{"role", "column"}, {"material", ""}}}}})["ok"]);
  const auto set = call("set_structural_materials", {{"items", {
      {{"role", "through_column"}, {"material", "ひのき"}, {"grade", "E95 機械等級区分 JAS"}, {"source", "架空の仕様書"}},
      {{"role", "default"}, {"material", "すぎ"}, {"grade", "E70"}}}}});
  ASSERT_TRUE(set["ok"]) << set.dump();
  table = call("structural_material_table", json::object());
  bool column = false, other = false;
  for (const auto& row : table["result"]["rows"]) {
    if (row[0] == "通し柱") column = row[3] == "ひのき" && row[4] == "E95 機械等級区分 JAS";
    else other |= row[3] == "すぎ";
  }
  EXPECT_TRUE(column) << table.dump();
  EXPECT_TRUE(other);
  EXPECT_EQ(table["result"]["given"].size(), 2u);
  EXPECT_EQ(table["result"]["rows"][0][0], "土台");  // in the order of the roles
  // Kept through save and reopen; one undo step.
  Document restored;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), restored));
  EXPECT_EQ(doc::Modeler::structuralMaterialsOf(restored).size(), 2u);
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(doc::Modeler::structuralMaterialsOf(m.document()).empty());
  ASSERT_TRUE(m.redo());
  // In the drawing set, found by the intake review.
  const auto drawings = doc::drawingSet(m.document(), {});
  ASSERT_TRUE(drawings) << drawings.error();
  EXPECT_NE(std::find(drawings->names.begin(), drawings->names.end(), std::string("使用構造材料一覧表")), drawings->names.end());
  const auto r = call("permit_intake_review", json::object());
  for (const auto& row : r["result"]["rows"])
    if (row["kind"] == "使用構造材料一覧表") EXPECT_EQ(row["state"], "図面あり") << row.dump();
}

// docs/295: the drawing set's plans leave out the frame's floor and roof framing (the columns stay).
TEST_F(Quantities, SetPlansLeaveOutFloorFraming) {
  frame();
  draw::ViewSpec all = draw::planView(600 + 1100);
  all.depth = 1500;
  draw::ViewSpec plan = all;
  plan.excludeTags = {"基礎・土台", "1階床組", "2階床組", "小屋組"};
  const auto withFraming = draw::makeView(m.document(), all);
  const auto without = draw::makeView(m.document(), plan);
  ASSERT_TRUE(withFraming && without);
  EXPECT_LT(without->items.size(), withFraming->items.size());
  EXPECT_FALSE(without->items.empty());  // the 1st storey's columns, cut
}

// docs/310: the frame's columns by section and distance, with slenderness against 令43条6項; the ratio of
// 令43条1項 is left to the notice (未評価).
TEST_F(Quantities, ColumnTableGivesSlendernessOfTheFrameColumns) {
  frame();
  const auto rows = doc::columnRows(m.document());
  ASSERT_FALSE(rows.empty());
  int counted = 0;
  for (const auto& r : rows) {
    EXPECT_GT(r.distanceMm, 0);
    EXPECT_NEAR(r.slenderness, r.distanceMm * std::sqrt(12.0) / std::min(r.widthMm, r.depthMm), 1e-9);
    counted += r.count;
  }
  EXPECT_GT(counted, 0);
  const auto table = doc::columnTable(rows);
  EXPECT_EQ(table.title, "構造詳細図（柱の小径・有効細長比）");
  for (const auto& row : table.rows) {
    EXPECT_TRUE(row[7] == "規定内" || row[7] == "規定外");
    EXPECT_EQ(row[10], "未入力");  // docs/323: no wall calculation: no Wd
  }
  sbr::api::CommandApi api(m);
  const auto read = json::parse(api.execute(R"({"command":"column_table"})"));
  ASSERT_TRUE(read["ok"]) << read.dump();
  EXPECT_EQ(read["result"]["rows"].size(), table.rows.size());
  // Hand check: a 105 column 2700 between members has λ = 2700 √12 / 105 = 89.1.
  doc::ColumnRow one;
  one.widthMm = one.depthMm = 105;
  one.distanceMm = 2700;
  one.slenderness = 2700 / (105 / std::sqrt(12.0));
  one.count = 1;
  one.role = "柱";
  EXPECT_EQ(doc::columnTable({one}).rows[0][6], "89.1");
}

// docs/311: the 構造詳細図's specifications: given items, the columns' slenderness, the source required.
TEST_F(Quantities, StructuralSpecTableTakesTheGivenSpecifications) {
  frame();
  auto table = doc::structuralSpecTable(m.document());
  ASSERT_EQ(table.rows.size(), 7u);  // docs/329: and the corner columns of a two-storey frame
  EXPECT_EQ(table.rows[0][3], "未入力");
  EXPECT_EQ(table.rows[1][0], "柱の有効細長比");
  EXPECT_TRUE(table.rows[1][3] == "規定内" || table.rows[1][3] == "規定外");
  EXPECT_EQ(table.rows[2][0], "隅柱の通し柱");
  EXPECT_EQ(table.rows[2][3], "規定内") << table.rows[2][1];  // the generated frame's corners are through columns
  doc::StructuralSpecs s;
  s.roofing = "ガルバリウム鋼板 立平葺き";
  s.joints = "平成12年建設省告示第1460号による";
  EXPECT_FALSE(m.setStructuralSpecs(s));  // the source is required
  s.source = "架空の仕様書";
  ASSERT_TRUE(m.setStructuralSpecs(s));
  table = doc::structuralSpecTable(m.document());
  EXPECT_EQ(table.rows[0][1], "ガルバリウム鋼板 立平葺き");
  EXPECT_EQ(table.rows[0][3], "入力済み");
  EXPECT_EQ(table.rows[4][3], "入力済み");
  EXPECT_EQ(table.rows[5][3], "未入力");
  Document restored;
  ASSERT_TRUE(deserializeDocument(serializeDocument(m.document()), restored));
  EXPECT_EQ(doc::structuralSpecsOf(restored).roofing, "ガルバリウム鋼板 立平葺き");
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(doc::structuralSpecsOf(m.document()).empty());
  sbr::api::CommandApi api(m);
  const auto set = json::parse(api.execute(R"({"command":"set_structural_specs","args":{"decay":"土台ひのき、柱・筋かいは防腐防蟻剤塗布","source":"架空の仕様書"}})"));
  ASSERT_TRUE(set["ok"]) << set.dump();
  const auto read = json::parse(api.execute(R"({"command":"structural_spec_table"})"));
  ASSERT_TRUE(read["ok"]) << read.dump();
  EXPECT_EQ(read["result"]["rows"][6][3], "入力済み");
}

// docs/315: the columns' longest clear distance in a height range (第五面 ニ).
TEST_F(Quantities, ClearDistanceOfTheColumnsInAStorey) {
  frame();
  const auto rows = doc::columnRows(m.document());
  ASSERT_FALSE(rows.empty());
  double longest = 0;
  for (const auto& r : rows) longest = std::max(longest, r.distanceMm);
  const auto all = doc::clearDistanceIn(m.document(), -1e9, 1e9);
  ASSERT_TRUE(all);
  EXPECT_DOUBLE_EQ(*all, longest);
  EXPECT_FALSE(doc::clearDistanceIn(m.document(), 1e8, 1e9));  // no column up there
}

// docs/316: a foundation made from a frame draws that frame's sill in its section detail.
TEST_F(Quantities, FoundationDetailFindsTheFrameSill) {
  const auto made = frame();
  const auto r = m.generateFoundation(json{{"inputs", {{"type", "mat"}, {"frame", made["frame"]}}}}.dump());
  ASSERT_TRUE(r) << r.error();
  const auto view = doc::foundationDetailView(m.document());
  ASSERT_TRUE(view);
  bool sill = false;
  for (const auto& item : view->items) sill |= item.kind == draw::Item::Kind::Text && item.text.rfind("土台 ", 0) == 0;
  EXPECT_TRUE(sill);
}

// docs/323: 令43条1項 by de = 0.027 l + 22.5 Wd / l, Wd from the wall calculation's weight over area.
TEST_F(Quantities, ColumnTableComparesTheRequiredDiameter) {
  frame();
  sbr::api::CommandApi api(m);
  const auto set = json::parse(api.execute(json{{"command", "set_wall_calculation"}, {"args", {{"inputs", {{"height_m", 6.0},
      {"floors", {{{"id", "f1"}, {"name", "1F"}, {"area_m2", 40.0}, {"weight_kn", 160.0}},
                  {{"id", "f2"}, {"name", "2F"}, {"area_m2", 40.0}, {"weight_kn", 80.0}}}}}}}}}.dump()));
  ASSERT_TRUE(set["ok"]) << set.dump();
  const auto rows = doc::columnRows(m.document());
  ASSERT_FALSE(rows.empty());
  bool compared = false;
  for (const auto& r : rows) {
    if (!r.wdNm2) continue;
    compared = true;
    EXPECT_DOUBLE_EQ(*r.wdNm2, r.storey == 1 ? 4000.0 : 2000.0);
    EXPECT_NEAR(*r.requiredMm, 0.027 * r.distanceMm + 22.5 * *r.wdNm2 / r.distanceMm, 1e-9);
  }
  EXPECT_TRUE(compared);
  // Hand check: l 2,900, Wd 4,000: 78.3 + 31.0 = 109.3 mm, so a 105 column is short and a 120 one is not.
  doc::ColumnRow one;
  one.storey = 1;
  one.role = "柱";
  one.distanceMm = 2900;
  one.count = 1;
  one.wdNm2 = 4000;
  one.requiredMm = 0.027 * 2900 + 22.5 * 4000 / 2900.0;
  one.widthMm = one.depthMm = 105;
  one.slenderness = 2900 / (105 / std::sqrt(12.0));
  EXPECT_EQ(doc::columnTable({one}).rows[0][9], "109.3");
  EXPECT_EQ(doc::columnTable({one}).rows[0][10], "規定外");
  one.widthMm = one.depthMm = 120;
  EXPECT_EQ(doc::columnTable({one}).rows[0][10], "規定内");
}

// docs/327: a plan circles the through columns its cut passes, and says so for the legend.
TEST_F(Quantities, PlansCircleTheThroughColumns) {
  frame();
  int through = 0;
  for (const auto& o : m.document().objects())
    if (const auto k = o.attributes.find("kind"); k != o.attributes.end() && k->second == "through_column") ++through;
  ASSERT_GT(through, 0);
  draw::ViewSpec spec = draw::planView(1000.0);
  spec.paperScale = 100;
  const auto view = draw::makeView(m.document(), spec);
  ASSERT_TRUE(view) << view.error();
  int rings = 0;
  for (const auto& item : view->items)
    rings += item.kind == draw::Item::Kind::Arc && item.pen == draw::Pen::Visible && item.radius > 0 && item.radius < 120.0 &&  // docs/328: round the column (about its half diagonal), not larger
             std::abs(item.sweep - 2 * M_PI) < 1e-9;
  EXPECT_EQ(rings, through);
  EXPECT_TRUE(view->symbols.count("through_column"));
  // A framing plan (the members drawn as they are) does not ring them.
  spec.framingPreset = "first_floor";
  const auto framing = draw::makeView(m.document(), spec);
  ASSERT_TRUE(framing);
  EXPECT_FALSE(framing->symbols.count("through_column"));
}

// docs/329: the corners of the columns' plan positions; a corner without a through column is to be checked.
TEST_F(Quantities, CornerColumnsAreThroughColumns) {
  frame();
  auto corners = doc::cornerColumns(m.document());
  ASSERT_TRUE(corners.applies);
  EXPECT_EQ(corners.corners, 4);
  EXPECT_EQ(corners.through, 4);
  // One corner's through column changed into a plain column: 3 of 4.
  for (const auto& o : m.document().objects())
    if (const auto k = o.attributes.find("kind"); k != o.attributes.end() && k->second == "through_column") {
      const Bnd_Box b = m.document().worldBounds(o);
      double x0, y0, z0, x1, y1, z1;
      b.Get(x0, y0, z0, x1, y1, z1);
      if (x0 > 100 || y0 > 100) continue;  // the corner at the origin
      ASSERT_TRUE(m.setAttribute({o.id}, "kind", "column"));
      break;
    }
  corners = doc::cornerColumns(m.document());
  EXPECT_EQ(corners.corners, 4);
  EXPECT_EQ(corners.through, 3);
}

// docs/330, docs/331: a plan marks each single brace with an open right triangle and a crossed pair with one open
// isosceles triangle (the MLIT manual's legend); the legend names them; framing plans do not.
TEST_F(Quantities, PlansMarkTheBraces) {
  const auto made = m.generateFrame(
      R"({"inputs":{"x_bays":2,"y_bays":2,"braces":[{"storey":1,"from":[0,0],"to":[1,0]},{"storey":1,"from":[2,1],"to":[2,2]},)"
      R"({"storey":1,"from":[0,1],"to":[0,2]},{"storey":1,"from":[0,2],"to":[0,1]}]}})");
  ASSERT_TRUE(made) << made.error();
  int braces = 0;
  for (const auto& o : m.document().objects())
    if (const auto k = o.attributes.find("kind"); k != o.attributes.end() && k->second == "brace") ++braces;
  ASSERT_EQ(braces, 4);
  draw::ViewSpec spec = draw::planView(1000.0);
  spec.paperScale = 100;
  const auto view = draw::makeView(m.document(), spec);
  ASSERT_TRUE(view) << view.error();
  int right = 0, isosceles = 0, filled = 0;
  for (const auto& item : view->items) {
    if (item.kind == draw::Item::Kind::Fill && item.points.size() == 3) ++filled;
    if (item.kind != draw::Item::Kind::Polyline || item.pen != draw::Pen::Visible || !item.closed || item.points.size() != 3) continue;
    const auto& p = item.points;
    const double ab = p[0].Distance(p[1]), bc = p[1].Distance(p[2]), ca = p[2].Distance(p[0]);
    if (std::abs(bc - ca) < 1e-6) ++isosceles;
    else ++right;
    // Beside the wall, not on it: the base is off the wall line by the gap.
    EXPECT_NEAR(ab, 400.0, 1e-6);
  }
  EXPECT_EQ(right, 2);      // the two single braces
  EXPECT_EQ(isosceles, 1);  // the crossed pair
  EXPECT_EQ(filled, 0);     // open triangles
  EXPECT_TRUE(view->symbols.count("brace"));
  EXPECT_TRUE(view->symbols.count("brace_double"));
  spec.framingPreset = "first_floor";
  const auto framing = draw::makeView(m.document(), spec);
  ASSERT_TRUE(framing);
  EXPECT_FALSE(framing->symbols.count("brace"));
}
