// 2D drawings (docs/50): plans, sections and elevations made from the
// model, laid out on a sheet, written as DXF and JWW.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sbr/TextEncoding.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/Schedules.h"
#include "sbr/doc/GroundLevel.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/draw/Drawing.h"
#include "sbr/geom/Dxf.h"
#include "sbr/geom/Jww.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <cstdlib>

using namespace sbr;
using namespace sbr::draw;

namespace {

doc::ObjectId box(doc::Modeler& m, double x, double y, double w, double d, double h, double z = 0) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, z);
  const doc::ObjectId id = *m.createRectangle(plane, w, d);
  EXPECT_TRUE(m.extrude(id, h));
  return id;
}

int count(const View& v, Pen pen, Item::Kind kind) {
  int n = 0;
  for (const Item& i : v.items) n += i.pen == pen && i.kind == kind;
  return n;
}
int count(const Sheet& s, Pen pen, Item::Kind kind) {
  int n = 0;
  for (const Item& i : s.items) n += i.pen == pen && i.kind == kind;
  return n;
}

double polygonArea(const std::vector<gp_Pnt2d>& p) {
  double a = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const gp_Pnt2d& q = p[i];
    const gp_Pnt2d& r = p[(i + 1) % p.size()];
    a += q.X() * r.Y() - r.X() * q.Y();
  }
  return std::abs(a) / 2;
}

std::string tempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("sbr_drawing_" + name)).string();
}

// ---- a JWW reader for the checks (the format as Jw_cad writes it; this
// reading was checked against files saved by Jw_cad 8.25a) ---------------
struct JwwFile {
  std::uint32_t version = 0;
  std::uint32_t paper = 0;
  double scale = 0;
  std::string memo;
  std::vector<std::string> layerNames;
  std::map<std::string, int> counts;
  struct Line { double x0, y0, x1, y1; std::uint16_t color; std::uint8_t type; std::uint16_t layer; };
  std::vector<Line> lines;
  struct Text { double x, y, size; std::string text; };
  std::vector<Text> texts;
  std::size_t end = 0, size = 0;
};

class JwwReader {
 public:
  explicit JwwReader(std::vector<std::uint8_t> d) : d_(std::move(d)) {}
  std::uint8_t u8() { return d_.at(p_++); }
  std::uint16_t u16() { std::uint16_t v; std::memcpy(&v, &d_.at(p_), 2); p_ += 2; return v; }
  std::uint32_t u32() { std::uint32_t v; std::memcpy(&v, &d_.at(p_), 4); p_ += 4; return v; }
  double f64() { double v; std::memcpy(&v, &d_.at(p_), 8); p_ += 8; return v; }
  std::string str() {
    std::size_t n = u8();
    if (n == 0xFF) {
      n = u16();
      if (n == 0xFFFF) n = u32();
    }
    std::string s(reinterpret_cast<const char*>(&d_.at(p_)), n);
    p_ += n;
    return s;
  }
  JwwFile read() {
    JwwFile f;
    f.size = d_.size();
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(d_.data()), 8), "JwwData.");
    p_ = 8;
    f.version = u32();
    f.memo = str();
    f.paper = u32();
    u32();
    for (int g = 0; g < 16; ++g) {
      u32(); u32();
      const double s = f64();
      if (g == 0) f.scale = s;
      u32();
      for (int l = 0; l < 32; ++l) u32();
    }
    for (int i = 0; i < 14 + 5 + 1 + 1; ++i) u32();
    f64(); f64(); f64(); u32();
    u32(); for (int i = 0; i < 5; ++i) f64();
    for (int i = 0; i < 256; ++i) f.layerNames.push_back(str());
    for (int i = 0; i < 16; ++i) str();
    f64(); f64(); u32(); f64();
    f64(); f64();
    u32();
    for (int i = 0; i < 6; ++i) f64();
    for (int n = 0; n < 8; ++n) { f64(); f64(); f64(); u32(); }
    for (int i = 0; i < 3; ++i) f64();
    u32(); f64(); f64(); f64(); u32();
    for (int n = 0; n < 11; ++n) f64();
    for (int n = 0; n < 10; ++n) { u32(); u32(); }
    for (int n = 0; n < 10; ++n) { u32(); u32(); f64(); }
    for (int n = 0; n < 8 * 4 + 5 * 5 + 4 * 4; ++n) u32();
    for (int i = 0; i < 10 + 1 + 1 + 4; ++i) u32();
    for (int i = 0; i < 9; ++i) f64();
    u32(); u32();
    for (int n = 0; n <= 256; ++n) { u32(); u32(); }
    for (int n = 0; n <= 256; ++n) { str(); u32(); u32(); f64(); }
    for (int n = 0; n <= 32; ++n) { u32(); u32(); u32(); u32(); }
    for (int n = 0; n <= 32; ++n) { str(); u32(); for (int j = 0; j < 10; ++j) f64(); }
    for (int i = 0; i < 10; ++i) { f64(); f64(); f64(); u32(); }
    f64(); f64(); f64(); u32(); u32();
    f64(); f64(); u32();
    for (int i = 0; i < 6; ++i) f64();
    list(f);
    list(f);
    f.end = p_;
    return f;
  }

 private:
  struct Base { std::uint8_t type; std::uint16_t color, layer; };
  Base base(std::uint32_t version) {
    Base b;
    u32();
    b.type = u8();
    b.color = u16();
    if (version >= 351) u16();
    b.layer = u16();
    u16(); u16();
    return b;
  }
  void list(JwwFile& f) {
    std::size_t n = u16();
    if (n == 0xFFFF) n = u32();
    for (std::size_t i = 0; i < n; ++i) object(f);
  }
  void object(JwwFile& f) {
    const std::uint16_t tag = u16();
    std::string name;
    if (tag == 0xFFFF) {
      u16();
      const std::size_t len = u16();
      name.assign(reinterpret_cast<const char*>(&d_.at(p_)), len);
      p_ += len;
      classes_[count_] = name;
      ++count_;
    } else if (tag == 0x7FFF) {
      name = classes_.at(u32() & 0x7FFFFFFF);
    } else {
      ASSERT_TRUE(tag & 0x8000) << "tag " << tag << " at " << p_;
      name = classes_.at(tag & 0x7FFF);
    }
    ++count_;
    ++f.counts[name];
    if (name == "CDataSen") {
      const Base b = base(f.version);
      JwwFile::Line l{f64(), f64(), f64(), f64(), b.color, b.type, b.layer};
      f.lines.push_back(l);
    } else if (name == "CDataEnko") {
      base(f.version);
      for (int i = 0; i < 7; ++i) f64();
      u32();
    } else if (name == "CDataMoji") {
      base(f.version);
      const double x = f64(), y = f64();
      f64(); f64();
      u32();
      const double size = f64();
      f64(); f64(); f64();
      str();
      f.texts.push_back({x, y, size, str()});
    } else if (name == "CDataSolid") {
      const std::size_t at = p_;
      u32(); u8();
      const std::uint16_t color = u16();
      p_ = at;
      base(f.version);
      for (int i = 0; i < 8; ++i) f64();
      if (color == 10) u32();
    } else {
      FAIL() << "unexpected class " << name;
    }
  }
  std::vector<std::uint8_t> d_;
  std::size_t p_ = 0;
  std::map<std::uint32_t, std::string> classes_;
  std::uint32_t count_ = 1;
};

JwwFile readJww(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return JwwReader(std::move(d)).read();
}

}  // namespace

// A plan through a wall: its outline cut (thick, exactly 4 lines) and
// filled; a low box seen below (thin); a box above the cut not drawn.
TEST(Drawing, PlanCutsAndSeesBelow) {
  doc::Modeler m;
  box(m, 0, 0, 4000, 200, 3000);           // wall, cut at 1200
  box(m, 1000, 1000, 1000, 1000, 500);     // low box: seen below
  box(m, 0, 3000, 1000, 1000, 500, 2000);  // up high: above the cut
  const auto v = makeView(m.document(), planView(1200));
  ASSERT_TRUE(v) << v.error();
  EXPECT_EQ(count(*v, Pen::Cut, Item::Kind::Line), 4);
  ASSERT_EQ(count(*v, Pen::Fill, Item::Kind::Fill), 1);
  for (const Item& i : v->items) {
    if (i.kind == Item::Kind::Fill) EXPECT_NEAR(polygonArea(i.points), 4000.0 * 200, 1e-6);
  }
  // The low box: its top outline (4 lines), and the wall's lower edges are
  // hidden under the cut (none of the seen lines lie on the cut).
  EXPECT_EQ(count(*v, Pen::Visible, Item::Kind::Line), 4);
  for (const Item& i : v->items) {
    if (i.pen != Pen::Visible) continue;
    for (const gp_Pnt2d& p : i.points) {
      EXPECT_TRUE(p.X() >= 1000 - 1e-6 && p.X() <= 2000 + 1e-6 && p.Y() >= 1000 - 1e-6 && p.Y() <= 2000 + 1e-6);
    }
  }
  EXPECT_LT(v->maxV, 2000 + 1e-6);  // nothing of the box above
}

TEST(Drawing, SiteNorthPersistsAndFollowsThePlanFrame) {
  doc::Modeler m;
  box(m, 0, 0, 3000, 5000, 2700);
  auto sun = m.document().settings().sun;
  sun.northAngle = 30.0;
  ASSERT_TRUE(m.setSun(sun));
  ViewSpec plan = planView(1200);
  const auto legacy = makeView(m.document(), plan);
  ASSERT_TRUE(legacy);
  EXPECT_FALSE(legacy->trueNorth);
  const auto steps = m.history().undoCount();
  EXPECT_FALSE(m.setSiteNorthFromPoints(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 100), "Survey", "2026-11-19"));
  EXPECT_EQ(m.history().undoCount(), steps);
  ASSERT_TRUE(m.setSiteNorthFromPoints(gp_Pnt(0, 0, 0), gp_Pnt(100, 0, 0), "Survey", "2026-11-19"));
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  const auto east = makeView(m.document(), plan);
  ASSERT_TRUE(east && east->trueNorth);
  EXPECT_NEAR(east->trueNorth->X(), 1.0, 1e-10);
  EXPECT_NEAR(east->trueNorth->Y(), 0.0, 1e-10);
  EXPECT_EQ(east->objects, legacy->objects);
  EXPECT_DOUBLE_EQ(east->minU, legacy->minU);
  EXPECT_DOUBLE_EQ(east->maxV, legacy->maxV);

  auto checkMark = [](const Sheet& sheet) {
    const Item* arrow = nullptr;
    for (const Item& item : sheet.items) {
      if (item.kind == Item::Kind::Fill && item.pen == Pen::Text) arrow = &item;
      EXPECT_NE(item.text, "仮");
    }
    ASSERT_TRUE(arrow);
    ASSERT_EQ(arrow->points.size(), 4u);
    EXPECT_GT(arrow->points[0].X(), arrow->points[2].X());
    EXPECT_NEAR(arrow->points[0].Y(), arrow->points[2].Y(), 1e-10);
  };
  const auto single = layoutSheet(*east, {});
  ASSERT_TRUE(single);
  checkMark(*single);
  Table table;
  table.title = "Area";
  table.headers = {"Room"};
  table.rows = {{"1"}};
  const auto withTable = layoutSheetWithTables(*east, {table}, {});
  ASSERT_TRUE(withTable);
  checkMark(*withTable);

  plan.frame.SetXDirection(gp_Dir(0, 1, 0));
  const auto rotated = makeView(m.document(), plan);
  ASSERT_TRUE(rotated && rotated->trueNorth);
  EXPECT_NEAR(rotated->trueNorth->X(), 0.0, 1e-10);
  EXPECT_NEAR(rotated->trueNorth->Y(), -1.0, 1e-10);

  const std::string path = ::testing::TempDir() + "site-north.sbr";
  ASSERT_TRUE(m.save(path));
  doc::Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  EXPECT_EQ(loaded.document().settings().siteNorth, m.document().settings().siteNorth);
  EXPECT_DOUBLE_EQ(loaded.document().settings().sun.northAngle, 90.0);
  ASSERT_TRUE(loaded.setSun(sun));
  EXPECT_DOUBLE_EQ(loaded.document().settings().sun.northAngle, 90.0);
  ASSERT_TRUE(m.undo());
  EXPECT_FALSE(m.document().settings().siteNorth);
  EXPECT_DOUBLE_EQ(m.document().settings().sun.northAngle, 30.0);
}

TEST(Drawing, SitePlanUsesTheLiveBoundaryAndBuildingProjection) {
  doc::Modeler m;
  const auto site = sitePlanView();
  EXPECT_FALSE(makeView(m.document(), site));
  const auto open = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(10000, 0, 0), gp_Pnt(10000, 12000, 0)});
  ASSERT_TRUE(open);
  EXPECT_FALSE(m.setSiteBoundary(doc::SiteBoundary{*open, "Survey"}));
  const auto crossing = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(6000, 8000, 0), gp_Pnt(0, 8000, 0),
                                         gp_Pnt(6000, 0, 0), gp_Pnt(0, 0, 0)});
  if (crossing) EXPECT_FALSE(m.setSiteBoundary(doc::SiteBoundary{*crossing, "Survey"}));
  const auto boundary = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(10000, 0, 0), gp_Pnt(10000, 12000, 0),
                                          gp_Pnt(0, 12000, 0), gp_Pnt(0, 0, 0)});
  ASSERT_TRUE(boundary);
  ASSERT_TRUE(m.setSiteBoundary(doc::SiteBoundary{*boundary, "Survey"}));
  const auto outline = m.document().siteBoundaryGeometry(*boundary);
  ASSERT_TRUE(outline);
  EXPECT_NEAR(outline->areaMm2 / 1e6, 120.0, 1e-8);
  const auto vacant = makeView(m.document(), site);
  ASSERT_TRUE(vacant);
  ASSERT_EQ(vacant->items.size(), 1u);
  EXPECT_TRUE(vacant->items.front().closed);
  EXPECT_EQ(vacant->items.front().pen, Pen::Boundary);
  EXPECT_FALSE(vacant->trueNorth);

  const auto building = box(m, 1500, 2500, 5200, 6500, 4000, 100000);
  ASSERT_TRUE(m.setSiteNorth({75, "Survey", "2026-11-19"}));
  const auto withBuilding = makeView(m.document(), site);
  ASSERT_TRUE(withBuilding && withBuilding->trueNorth);
  EXPECT_GT(withBuilding->trueNorth->X(), 0.9);
  EXPECT_GT(count(*withBuilding, Pen::Visible, Item::Kind::Line), 0);
  gp_Trsf move;
  move.SetTranslation(gp_Vec(5000, 0, 0));
  ASSERT_TRUE(m.transform({building}, move));
  const auto moved = makeView(m.document(), site);
  ASSERT_TRUE(moved);
  EXPECT_GT(moved->maxU, withBuilding->maxU);
  EXPECT_EQ(moved->calculationNotes.front(), withBuilding->calculationNotes.front());

  const auto group = doc::drawingSet(m.document(), {});
  ASSERT_TRUE(group);
  // docs/209: the sheet list, the outline of the project, then the site plan.
  EXPECT_EQ(group->names.at(2), site.title);
  const std::string path = ::testing::TempDir() + "site-plan.sbr";
  ASSERT_TRUE(m.save(path));
  doc::Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  EXPECT_EQ(loaded.document().settings().siteBoundary, m.document().settings().siteBoundary);
  ASSERT_TRUE(makeView(loaded.document(), site));
  ASSERT_TRUE(loaded.setSiteBoundary(std::nullopt));
  EXPECT_FALSE(makeView(loaded.document(), site));
  ASSERT_TRUE(loaded.undo());
  EXPECT_TRUE(makeView(loaded.document(), site));
}

TEST(Drawing, SiteRoadUsesTheSelectedEdgeAndSavedInput) {
  doc::Modeler m;
  const auto outline = m.createRectangle(geom::SketchPlane::ground(), 10000, 12000);
  ASSERT_TRUE(outline);
  const auto building = box(m, 1500, 2500, 5200, 6500, 3500);
  ASSERT_TRUE(m.setSiteBoundary(doc::SiteBoundary{*outline, "Survey"}));
  doc::SiteRoad road;
  road.kind = "Test road";
  road.source = "Synthetic road register";
  road.widthMm = 5200;
  road.frontageMm = 8800;
  EXPECT_FALSE(m.setSiteRoad({gp_Pnt(0, 0, 0), gp_Pnt(4000, 0, 0)}, road));
  ASSERT_TRUE(m.setSiteRoad({gp_Pnt(10000, 0, 0), gp_Pnt(0, 0, 0)}, road));
  const auto plan = makeView(m.document(), sitePlanView());
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->dimensions.size(), 1u);
  EXPECT_EQ(plan->dimensions[0].text, "2.500 m");
  EXPECT_NEAR(plan->dimensions[0].a.Y(), 0.0, 1e-6);
  EXPECT_NEAR(plan->dimensions[0].b.Y(), 2500.0, 1e-6);
  EXPECT_NE(plan->calculationNotes[2].find("8.800 m"), std::string::npos);

  gp_Trsf move;
  move.SetTranslation(gp_Vec(0, -500, 0));
  ASSERT_TRUE(m.transform({building}, move));
  const auto updated = makeView(m.document(), sitePlanView());
  ASSERT_TRUE(updated);
  ASSERT_EQ(updated->dimensions.size(), 1u);
  EXPECT_EQ(updated->dimensions[0].text, "2.000 m");
  move.SetTranslation(gp_Vec(0, 400, 0));
  ASSERT_TRUE(m.transform({*outline}, move));
  const auto movedSite = makeView(m.document(), sitePlanView());
  ASSERT_TRUE(movedSite);
  ASSERT_EQ(movedSite->dimensions.size(), 1u);
  EXPECT_EQ(movedSite->dimensions[0].text, "1.600 m");

  const std::string path = ::testing::TempDir() + "site-road.sbr";
  ASSERT_TRUE(m.save(path));
  doc::Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  EXPECT_EQ(loaded.document().settings().siteBoundary, m.document().settings().siteBoundary);
  loaded.clearSiteRoad();
  EXPECT_FALSE(loaded.document().settings().siteBoundary->road);
  ASSERT_TRUE(loaded.undo());
  EXPECT_TRUE(loaded.document().settings().siteBoundary->road);
}

TEST(Drawing, SiteRoadClearanceUsesTheVisibleCircularArc) {
  doc::Modeler m;
  const auto outline = m.createRectangle(geom::SketchPlane::ground(), 10000, 12000);
  ASSERT_TRUE(outline);
  ASSERT_TRUE(m.setSiteBoundary(doc::SiteBoundary{*outline, "Survey"}));
  doc::SiteRoad road;
  road.source = "Synthetic road register";
  ASSERT_TRUE(m.setSiteRoad({gp_Pnt(0, 0, 0), gp_Pnt(10000, 0, 0)}, road));

  auto plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(4000, 3500, 0);
  const auto cylinder = m.createCircle(plane, 1100);
  ASSERT_TRUE(cylinder && m.extrude(*cylinder, 2800));
  const auto cutter = box(m, 2500, 2900, 3000, 2000, 3200, -100);
  ASSERT_TRUE(m.boolean(doc::BooleanKind::Subtract, {*cylinder, cutter}));

  const auto plan = makeView(m.document(), sitePlanView());
  ASSERT_TRUE(plan);
  ASSERT_GT(count(*plan, Pen::Visible, Item::Kind::Arc), 0);
  ASSERT_EQ(plan->dimensions.size(), 1u);
  EXPECT_EQ(plan->dimensions[0].text, "2.400 m");
  EXPECT_NEAR(plan->dimensions[0].b.X(), 4000.0, 1e-6);
  EXPECT_NEAR(plan->dimensions[0].b.Y(), 2400.0, 1e-6);
}

TEST(Drawing, SiteGroundWeightsTheMeasuredPerimeterAndDetectsStalePoints) {
  doc::Modeler m;
  const auto site = m.createRectangle(geom::SketchPlane::ground(), 10000, 12000);
  ASSERT_TRUE(site);
  ASSERT_TRUE(m.setSiteBoundary(doc::SiteBoundary{*site, "Survey"}));
  const auto contact = m.createPolyline({gp_Pnt(1000, 1000, 0), gp_Pnt(3000, 1000, 0), gp_Pnt(5000, 1000, 0),
      gp_Pnt(5000, 3000, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(1000, 1000, 0)});
  ASSERT_TRUE(contact);
  // Reversed order with a different starting vertex. P2 is a point along a straight edge.
  doc::SiteGround input{*contact, "BM", "Synthetic survey", {
      {5000, 1000, 0}, {3000, 1000, 600}, {1000, 1000, 0}, {1000, 3000, -100}, {5000, 3000, 0}}};
  auto bad = input;
  std::swap(bad.samples[2], bad.samples[3]);
  EXPECT_FALSE(m.setSiteGround(bad));
  ASSERT_TRUE(m.setSiteGround(input));
  const auto measured = m.document().siteGroundReport(input);
  ASSERT_TRUE(measured);
  EXPECT_NEAR(measured->lengthMm, 12000, 1e-6);
  EXPECT_NEAR(measured->averageMm(), 75, 1e-6);
  const auto view = makeView(m.document(), sitePlanView());
  ASSERT_TRUE(view && view->siteGroundTable);
  EXPECT_EQ(view->members.size(), 5u);
  EXPECT_EQ(view->siteGroundTable->rows.front()[2], "0.000");
  EXPECT_EQ(view->siteGroundTable->rows.front()[3], "0.600");
  EXPECT_EQ(view->siteGroundTable->rows.back()[4], "0.900");
  const auto sheet = layoutSheet(*view, {});
  ASSERT_TRUE(sheet);
  EXPECT_TRUE(std::any_of(sheet->items.begin(), sheet->items.end(),
      [](const Item& item) { return item.text.find("0.075 m") != std::string::npos; }));
  const auto drawingSet = doc::drawingSet(m.document(), {});
  // (docs/209: the third sheet, after the list and the outline of the project.)
  ASSERT_TRUE(drawingSet);
  EXPECT_TRUE(std::any_of(drawingSet->sheets.at(2).items.begin(), drawingSet->sheets.at(2).items.end(),
      [](const Item& item) { return item.text.find("0.075 m") != std::string::npos; }));

  const auto path = ::testing::TempDir() + "site-ground.sbr";
  ASSERT_TRUE(m.save(path));
  doc::Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  EXPECT_EQ(loaded.document().settings().siteBoundary->ground, std::optional<doc::SiteGround>(input));
  gp_Trsf move;
  move.SetTranslation(gp_Vec(100, 0, 0));
  ASSERT_TRUE(loaded.transform({*contact}, move));
  const auto stale = makeView(loaded.document(), sitePlanView());
  ASSERT_TRUE(stale);
  EXPECT_FALSE(stale->siteGroundTable);
  ASSERT_TRUE(loaded.undo());
  EXPECT_TRUE(loaded.document().siteGroundReport(input));
  loaded.clearSiteGround();
  EXPECT_FALSE(loaded.document().settings().siteBoundary->ground);
  ASSERT_TRUE(loaded.undo());
  EXPECT_TRUE(loaded.document().settings().siteBoundary->ground);
}

TEST(Drawing, SiteGroundHoldsAveragesAfterTheAssociatedBuildingMoves) {
  doc::Modeler m;
  const auto site = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(10000, 0, 0), gp_Pnt(10000, 12000, 0),
      gp_Pnt(0, 12000, 0), gp_Pnt(0, 0, 0)});
  ASSERT_TRUE(site);
  ASSERT_TRUE(m.setSiteBoundary(doc::SiteBoundary{*site, "Synthetic survey"}));
  const auto outline = m.createPolyline({gp_Pnt(1000, 1000, 0), gp_Pnt(4000, 1000, 0),
      gp_Pnt(4000, 5000, 0), gp_Pnt(1000, 5000, 0), gp_Pnt(1000, 1000, 0)});
  ASSERT_TRUE(outline);
  const auto building = box(m, 1000, 1000, 3000, 4000, 3500, 50);
  const auto parent = m.group({building}, "House group");
  ASSERT_TRUE(parent);
  const doc::SiteGround input{*outline, "BM", "Synthetic elevation survey",
      {{1000, 1000, -100}, {4000, 1000, 0}, {4000, 5000, 100}, {1000, 5000, 400}}};
  EXPECT_FALSE(m.setSiteGround(input, *outline));
  ASSERT_TRUE(m.setSiteGround(input, building));
  const auto check = [](const doc::Modeler& model) {
    const auto view = makeView(model.document(), sitePlanView());
    return view && view->siteGroundTable.has_value();
  };
  EXPECT_TRUE(check(m));

  // Move the parent, so the associated solid's local transform does not change.
  gp_Trsf move;
  move.SetTranslation(gp_Vec(100, 0, 0));
  ASSERT_TRUE(m.transform({*parent}, move));
  EXPECT_FALSE(check(m));
  EXPECT_FALSE(m.setSiteGround(input));
  ASSERT_TRUE(m.undo());
  EXPECT_TRUE(check(m));
  ASSERT_TRUE(m.redo());
  EXPECT_FALSE(check(m));

  const auto path = ::testing::TempDir() + "site-ground-placement.sbr";
  ASSERT_TRUE(m.save(path));
  doc::Modeler loaded;
  ASSERT_TRUE(loaded.load(path));
  EXPECT_FALSE(check(loaded));
  ASSERT_TRUE(loaded.setSiteGround(input, {}, true));
  EXPECT_TRUE(check(loaded));

  gp_Trsf rotation;
  rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.1);
  ASSERT_TRUE(loaded.transform({*parent}, rotation));
  EXPECT_FALSE(check(loaded));
  ASSERT_TRUE(loaded.remove({*parent}));
  EXPECT_FALSE(check(loaded));
  const auto replacement = box(loaded, 1000, 1000, 3000, 4000, 3500, 50);
  EXPECT_FALSE(loaded.setSiteGround(input, replacement));
  ASSERT_TRUE(loaded.setSiteGround(input, replacement, true));
  EXPECT_TRUE(check(loaded));
}

TEST(Drawing, CylinderCutIsACircle) {
  doc::Modeler m;
  const doc::ObjectId c = *m.createCircle(geom::SketchPlane::ground(), 300);
  ASSERT_TRUE(m.extrude(c, 3000));
  const auto v = makeView(m.document(), planView(1200));
  ASSERT_TRUE(v) << v.error();
  int circles = 0;
  for (const Item& i : v->items) {
    if (i.pen == Pen::Cut && i.kind == Item::Kind::Arc) {
      circles += std::abs(i.sweep - 2 * M_PI) < 1e-9;
      EXPECT_NEAR(i.radius, 300, 1e-6);
    }
  }
  EXPECT_EQ(circles, 1);
}

// A section across a slab on two walls; an elevation of a box behind
// another, hidden lines asked for.
TEST(Drawing, SectionAndElevation) {
  doc::Modeler m;
  box(m, 0, 0, 200, 3000, 3000);
  box(m, 3800, 0, 200, 3000, 3000);
  box(m, 0, 0, 4000, 3000, 200, 3000);
  const auto s = makeView(m.document(), sectionView(gp_Pnt(0, 1500, 0), gp_Dir(0, -1, 0)));
  ASSERT_TRUE(s) << s.error();
  // Three cut rectangles: 12 lines; the sheet's right is +X, up is +Z.
  EXPECT_EQ(count(*s, Pen::Cut, Item::Kind::Line), 12);
  EXPECT_NEAR(s->minU, 0, 1e-6);
  EXPECT_NEAR(s->maxU, 4000, 1e-6);
  EXPECT_NEAR(s->maxV, 3200, 1e-6);
  EXPECT_EQ(count(*s, Pen::Fill, Item::Kind::Fill), 3);

  doc::Modeler e;
  box(e, 0, 0, 1000, 1000, 1000);
  box(e, 500, 2000, 1000, 1000, 2000);  // behind (north of) the first, taller
  ViewSpec south = elevationView(e.document(), 0);
  south.hidden = true;
  const auto el = makeView(e.document(), south);
  ASSERT_TRUE(el) << el.error();
  EXPECT_EQ(count(*el, Pen::Cut, Item::Kind::Line), 0);
  EXPECT_GT(count(*el, Pen::Visible, Item::Kind::Line), 4);
  EXPECT_GT(count(*el, Pen::Hidden, Item::Kind::Line), 0);  // the back box's edges behind the front one
  EXPECT_NEAR(el->maxV, 2000, 1e-6);
}

// Grids and levels become bubbles, spans and level lines on the sheet;
// the paper is chosen; the frame and title block are filled in.
TEST(Drawing, GridLevelsSheetAndTitleBlock) {
  doc::Modeler m;
  const auto grid = m.createGridLines(gp_Pnt(0, 0, 0), {6000, 6000}, {5000});
  ASSERT_TRUE(grid) << grid.error();
  box(m, -100, -100, 200, 200, 3000);
  box(m, 5900, -100, 200, 200, 3000);
  ASSERT_TRUE(m.setLevels({{"", "GL", 0.0}, {"", "1FL", 500.0}, {"", "2FL", 3500.0}}));
  ViewSpec planSpec = planView(1700);
  planSpec.title = "1階平面図";
  const auto plan = makeView(m.document(), planSpec);
  ASSERT_TRUE(plan) << plan.error();
  EXPECT_EQ(plan->grids.size(), 5u);  // X1-X3, Y1-Y2
  SheetSpec spec;
  spec.scale = 100;
  spec.title = {"テスト邸", "", "A-01", "設計者", "2026-11-02"};
  const auto sheet = layoutSheet(*plan, spec);
  ASSERT_TRUE(sheet) << sheet.error();
  EXPECT_EQ(sheet->paper, "A4");  // 12 m x 5 m at 1:100 fits A4
  int bubbles = 0;
  bool x2 = false, span = false, project = false, number = false, caption = false;
  for (const Item& i : sheet->items) {
    if (i.pen == Pen::GridLabel && i.kind == Item::Kind::Arc) bubbles += std::abs(i.radius - 300) < 1e-6;  // docs/296: 6 mm across at 1:100
    if (i.kind != Item::Kind::Text) continue;
    x2 |= i.text == "X2";
    span |= i.pen == Pen::Dimension && i.text == "6,000";  // docs/296: grouped digits
    project |= i.text == "テスト邸";
    number |= i.text == "A-01";
    caption |= i.text == "1階平面図  S=1:100";
  }
  EXPECT_EQ(bubbles, 5);
  EXPECT_TRUE(x2 && span && project && number && caption);
  EXPECT_EQ(count(*sheet, Pen::Frame, Item::Kind::Polyline), 1);
  // Everything on the paper.
  for (const Item& i : sheet->items) {
    for (const gp_Pnt2d& p : i.points) {
      EXPECT_TRUE(p.X() >= 0 && p.X() <= 297 * 100 && p.Y() >= 0 && p.Y() <= 210 * 100) << i.text;
    }
  }
  // At 1:50 it needs more paper; at 1:20 it does not fit even A1.
  EXPECT_EQ(layoutSheet(*plan, SheetSpec{50, "auto", true, {}, true})->paper, "A3");
  const auto huge = layoutSheet(*plan, SheetSpec{10, "auto", true, {}, true});
  ASSERT_TRUE(huge);
  EXPECT_EQ(huge->paper, "A1");
  EXPECT_FALSE(huge->notes.empty());

  // A section across the grid: the X axes stand up, the levels run across.
  const auto section = makeView(m.document(), sectionView(gp_Pnt(0, 2500, 0), gp_Dir(0, -1, 0)));
  ASSERT_TRUE(section) << section.error();
  EXPECT_EQ(section->grids.size(), 3u);
  EXPECT_EQ(section->levels.size(), 3u);
  const auto sectionSheet = layoutSheet(*section, spec);
  ASSERT_TRUE(sectionSheet);
  EXPECT_EQ(count(*sectionSheet, Pen::Level, Item::Kind::Line), 3);
  // Two levels at one height share a line and a name.
  ASSERT_TRUE(m.setLevels({{"", "GL", 0.0}, {"", "1FL", 0.0}, {"", "2FL", 3500.0}}));
  const auto shared = layoutSheet(*makeView(m.document(), sectionView(gp_Pnt(0, 2500, 0), gp_Dir(0, -1, 0))), spec);
  ASSERT_TRUE(shared);
  EXPECT_EQ(count(*shared, Pen::Level, Item::Kind::Line), 2);
  bool both = false;
  for (const Item& i : shared->items) both |= i.text == "GL・1FL";
  EXPECT_TRUE(both);
}

// DXF: R12 with layers and Japanese text in Shift_JIS.
TEST(Drawing, WritesDxf) {
  doc::Modeler m;
  box(m, 0, 0, 4000, 200, 3000);
  ViewSpec spec = planView(1200);
  spec.title = "平面図";
  const auto sheet = layoutSheet(*makeView(m.document(), spec), SheetSpec{});
  ASSERT_TRUE(sheet);
  const std::string path = tempPath("plan.dxf");
  ASSERT_TRUE(writeDxf(*sheet, path));
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("AC1009"), std::string::npos);
  EXPECT_NE(text.find("ANSI_932"), std::string::npos);
  EXPECT_NE(text.find("CUT"), std::string::npos);
  EXPECT_NE(text.find(toShiftJis("平面図")), std::string::npos);
  EXPECT_EQ(text.find("平面図"), std::string::npos);  // not left in UTF-8
  int lines = 0;
  for (std::size_t at = text.find("\nLINE\n"); at != std::string::npos; at = text.find("\nLINE\n", at + 1)) ++lines;
  EXPECT_GE(lines, 4);
  EXPECT_NE(text.find("\nSOLID\n"), std::string::npos);
  EXPECT_EQ(text.substr(text.size() - 4), "EOF\n");
  // And read back by our own DXF import (Shift_JIS text, R12).
  geom::DxfImportReport report;
  const auto back = geom::readDxf(path, {}, &report);
  ASSERT_TRUE(back) << back.error();
  EXPECT_FALSE(back->empty());
  EXPECT_EQ(report.convertedFrom, "Shift_JIS");
  std::remove(path.c_str());
}

// JWW: read back to the end; paper coordinates, scale on the layer group.
TEST(Drawing, WritesJww) {
  doc::Modeler m;
  box(m, 0, 0, 4000, 200, 3000);
  const doc::ObjectId c = *m.createCircle(geom::SketchPlane::ground(), 300);
  ASSERT_TRUE(m.extrude(c, 3000));
  ViewSpec spec = planView(1200);
  spec.title = "1階平面図";
  SheetSpec sheetSpec;
  sheetSpec.scale = 50;
  sheetSpec.title.project = "テスト邸";
  const auto sheet = layoutSheet(*makeView(m.document(), spec), sheetSpec);
  ASSERT_TRUE(sheet);
  const std::string path = tempPath("plan.jww");
  ASSERT_TRUE(writeJww(*sheet, path));
  const JwwFile f = readJww(path);
  EXPECT_EQ(f.end, f.size);  // read to the very end
  EXPECT_EQ(f.version, 600u);
  EXPECT_DOUBLE_EQ(f.scale, 50.0);
  EXPECT_EQ(f.memo, toShiftJis("1階平面図"));
  EXPECT_EQ(f.layerNames[0], toShiftJis("断面"));
  EXPECT_GT(f.counts.at("CDataSen"), 4);
  EXPECT_GE(f.counts.at("CDataEnko"), 1);
  EXPECT_GE(f.counts.at("CDataSolid"), 2);
  EXPECT_GE(f.counts.at("CDataMoji"), 3);
  // The wall's cut line along y = 0 runs 4000 mm = 80 mm on paper, on the
  // cut layer with Jw line colour 5.
  bool found = false;
  for (const JwwFile::Line& l : f.lines) {
    if (l.layer == 0 && std::abs(std::abs(l.x1 - l.x0) - 80.0) < 1e-6 && std::abs(l.y1 - l.y0) < 1e-9) {
      found = true;
      EXPECT_EQ(l.color, 5);
    }
    // On the paper (centre at 0, 0).
    EXPECT_LE(std::abs(l.x0), sheet->paperWidth / 2 + 1e-6);
    EXPECT_LE(std::abs(l.y0), sheet->paperHeight / 2 + 1e-6);
  }
  EXPECT_TRUE(found);
  bool project = false;
  for (const auto& t : f.texts) project |= t.text == toShiftJis("テスト邸");
  EXPECT_TRUE(project);
  std::remove(path.c_str());
}

TEST(Drawing, ShiftJis) {
  EXPECT_EQ(toShiftJis("abc"), "abc");
  EXPECT_EQ(toShiftJis("平面図"), std::string("\x95\xbd\x96\xca\x90\x7d"));
  EXPECT_EQ(toShiftJis("±0"), std::string("\x81\x7d") + "0");
}

TEST(Drawing, NothingToDraw) {
  doc::Modeler m;
  EXPECT_FALSE(makeView(m.document(), planView(1200)));
  box(m, 0, 0, 1000, 1000, 1000, 5000);
  EXPECT_FALSE(makeView(m.document(), planView(1200)));  // all of it above the cut
}

TEST(Drawing, Api) {
  doc::Modeler m;
  box(m, 0, 0, 4000, 200, 3000);
  api::CommandApi commands(m);
  auto run = [&](const nlohmann::json& args) {
    return nlohmann::json::parse(commands.execute(nlohmann::json{{"command", "export_drawing"}, {"args", args}}.dump()));
  };
  const std::string jww = tempPath("api.jww"), dxf = tempPath("api.dxf");
  const auto plan = run({{"path", jww}, {"kind", "plan"}, {"cut_z", 1000}, {"scale", 50}, {"title", "平面図"}});
  ASSERT_TRUE(plan.value("ok", false)) << plan.dump();
  EXPECT_EQ(plan["result"]["paper"], "A4");
  EXPECT_EQ(readJww(jww).end, readJww(jww).size);
  const auto section = run({{"path", dxf}, {"kind", "section"}, {"point", {2000, 100, 0}}, {"look", {1, 0, 0}}});
  ASSERT_TRUE(section.value("ok", false)) << section.dump();
  EXPECT_TRUE(std::filesystem::file_size(dxf) > 100);
  EXPECT_FALSE(run({{"path", tempPath("api.pdf")}, {"kind", "plan"}}).value("ok", true));
  EXPECT_FALSE(run({{"path", jww}, {"kind", "perspective"}}).value("ok", true));
  std::remove(jww.c_str());
  std::remove(dxf.c_str());
}

// JWW read back (docs/50): what we write comes in at real size, on layers
// named as Jw shows them.
TEST(Jww, ReadsWhatWeWrite) {
  doc::Modeler m;
  box(m, 0, 0, 4000, 200, 3000);
  const doc::ObjectId c = *m.createCircle(geom::SketchPlane::ground(), 300);
  ASSERT_TRUE(m.extrude(c, 3000));
  SheetSpec spec;
  spec.scale = 50;
  const auto sheet = layoutSheet(*makeView(m.document(), planView(1200)), spec);
  ASSERT_TRUE(sheet);
  const std::string path = tempPath("roundtrip.jww");
  ASSERT_TRUE(writeJww(*sheet, path));
  geom::JwwImportReport report;
  EXPECT_FALSE(geom::readJww(path, {}, &report));  // text and fills: not read, so refused unless asked
  const auto parts = geom::readJww(path, {true, false}, &report);
  ASSERT_TRUE(parts) << parts.error();
  EXPECT_EQ(report.version, 600u);
  EXPECT_GT(report.omitted["text"], 0);
  EXPECT_GT(report.omitted["solid fills"], 0);
  // The cut wall: a 4000 mm line on the "断面" layer; the column: a circle r 300.
  bool wall = false, column = false;
  for (const auto& p : *parts) {
    Bnd_Box b;
    BRepBndLib::AddOptimal(p.shape, b, false, false);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    if (p.layer.find("/断面") != std::string::npos) {
      wall |= std::abs((x1 - x0) - 4000) < 1e-3 && std::abs(y1 - y0) < 1e-3;
      column |= std::abs((x1 - x0) - 600) < 1e-2 && std::abs((y1 - y0) - 600) < 1e-2;
    }
  }
  EXPECT_TRUE(wall);
  EXPECT_TRUE(column);
  // One object per layer, if asked.
  const auto grouped = geom::readJww(path, {true, true});
  ASSERT_TRUE(grouped);
  EXPECT_LT(grouped->size(), parts->size());
  // Through the model and the AI command: one undo step.
  doc::Modeler target;
  api::CommandApi commands(target);
  const auto refused = nlohmann::json::parse(
      commands.execute(nlohmann::json{{"command", "import_jww"}, {"args", {{"path", path}}}}.dump()));
  EXPECT_FALSE(refused.value("ok", true));  // text and fills are not read: asked for explicitly
  const auto in = nlohmann::json::parse(commands.execute(
      nlohmann::json{{"command", "import_jww"}, {"args", {{"path", path}, {"allow_partial", true}, {"group_by_layer", true}}}}
          .dump()));
  ASSERT_TRUE(in.value("ok", false)) << in.dump();
  EXPECT_EQ(in["result"]["report"]["version"], 600);
  EXPECT_EQ(target.history().undoCount(), 1u);
  std::remove(path.c_str());
  EXPECT_FALSE(geom::readJww(tempPath("missing.jww")));
}

// Jw_cad's own drawings (not in the repository): SBR_JWW_SAMPLES=<folder>.
TEST(Jww, ReadsJwCadDrawings) {
  const char* dir = std::getenv("SBR_JWW_SAMPLES");
  if (!dir) GTEST_SKIP() << "SBR_JWW_SAMPLES not set";
  int files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(dir))) {
    if (entry.path().extension() != ".jww") continue;
    geom::JwwImportReport report;
    const auto parts = geom::readJww(entry.path().u8string(), {true, false}, &report);
    EXPECT_TRUE(parts) << entry.path().u8string() << ": " << (parts ? "" : parts.error());
    if (parts) std::printf("%s: %s\n", entry.path().filename().u8string().c_str(), report.summary().c_str());
    ++files;
  }
  EXPECT_GT(files, 0);
}

// docs/288: the ground level by 令 2 条 2 項 from the measured contact perimeter, as its own sheet.
TEST(Drawing, GroundLevelTableFollowsTheOrder) {
  doc::Modeler m;
  EXPECT_EQ(doc::groundLevelStudy(m.document()).state, "未入力");
  const auto site = m.createRectangle(geom::SketchPlane::ground(), 10000, 12000);
  ASSERT_TRUE(site);
  ASSERT_TRUE(m.setSiteBoundary(doc::SiteBoundary{*site, "Survey"}));
  const auto contact = m.createPolyline({gp_Pnt(1000, 1000, 0), gp_Pnt(3000, 1000, 0), gp_Pnt(5000, 1000, 0),
      gp_Pnt(5000, 3000, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(1000, 1000, 0)});
  ASSERT_TRUE(contact);
  doc::SiteGround input{*contact, "BM", "Synthetic survey", {
      {5000, 1000, 0}, {3000, 1000, 600}, {1000, 1000, 0}, {1000, 3000, -100}, {5000, 3000, 0}}};
  ASSERT_TRUE(m.setSiteGround(input));
  auto study = doc::groundLevelStudy(m.document());
  EXPECT_EQ(study.state, "算出");
  EXPECT_NEAR(study.levelMm, 75.0, 1e-6);  // (2*0.3 + 2*0.3 + 2*-0.05 + 4*-0.05 + 2*0) / 12 = 0.9/12
  EXPECT_NEAR(study.rangeMm(), 700.0, 1e-6);
  auto table = doc::groundLevelTable(study);
  ASSERT_EQ(table.rows.size(), 6u);
  EXPECT_EQ(table.rows[0][4], "0.300");
  EXPECT_EQ(table.rows.back()[5], "0.900");
  EXPECT_NE(table.notes.front().find("+0.075 m"), std::string::npos);
  EXPECT_NE(table.notes.front().find("地盤面"), std::string::npos);
  // The drawing set: right after the site plan (list, outline, site plan, ground level).
  const auto set = doc::drawingSet(m.document(), {});
  ASSERT_TRUE(set) << set.error();
  ASSERT_GE(set->names.size(), 4u);
  EXPECT_EQ(set->names[3], "地盤面算出表");
  EXPECT_TRUE(std::any_of(set->sheets[3].items.begin(), set->sheets[3].items.end(),
      [](const Item& item) { return item.text.find("令2条") != std::string::npos ||
                                    item.text.find("第2条第2項") != std::string::npos; }));
  // docs/289: the site area sheet (10 m x 12 m).
  const auto siteArea = std::find(set->names.begin(), set->names.end(), std::string("敷地面積求積図"));
  ASSERT_NE(siteArea, set->names.end());
  const auto& siteSheet = set->sheets[static_cast<std::size_t>(siteArea - set->names.begin())];
  EXPECT_TRUE(std::any_of(siteSheet.items.begin(), siteSheet.items.end(),
      [](const Item& item) { return item.text.find("120.000") != std::string::npos; }));
  EXPECT_TRUE(std::any_of(siteSheet.items.begin(), siteSheet.items.end(),
      [](const Item& item) { return item.text.find("10000 × 12000") != std::string::npos; }));
  // The API.
  api::CommandApi api(m);
  const auto r = nlohmann::json::parse(api.execute(R"({"command":"ground_level_table","args":{}})"));
  ASSERT_TRUE(r.value("ok", false)) << r.dump();
  EXPECT_EQ(r["result"]["state"], "算出");
  EXPECT_NEAR(r["result"]["level_m"].get<double>(), 0.075, 1e-9);
  EXPECT_NE(r["result"]["csv"].get<std::string>().find("地盤面"), std::string::npos);
  // docs/293: the datum's model height gives the ground level a Z to hold against the level GL; kept on save.
  ASSERT_TRUE(m.setLevels({{"", "GL", 0}, {"", "1FL", 500}}));
  input.datumZ = -500.0;
  ASSERT_TRUE(m.setSiteGround(input));
  study = doc::groundLevelStudy(m.document());
  ASSERT_TRUE(study.levelZ && study.glZ);
  EXPECT_NEAR(*study.levelZ, -425.0, 1e-6);
  EXPECT_NEAR(*study.glZ, 0.0, 1e-9);
  table = doc::groundLevelTable(study);
  EXPECT_TRUE(std::any_of(table.notes.begin(), table.notes.end(),
      [](const std::string& n) { return n.find("Z -425") != std::string::npos && n.find("+425") != std::string::npos; }));
  const auto withZ = nlohmann::json::parse(api.execute(R"({"command":"ground_level_table","args":{}})"));
  EXPECT_NEAR(withZ["result"]["gl_minus_level_mm"].get<double>(), 425.0, 1e-6);
  {
    doc::Document restored;
    ASSERT_TRUE(doc::deserializeDocument(doc::serializeDocument(m.document()), restored));
    ASSERT_TRUE(restored.settings().siteBoundary && restored.settings().siteBoundary->ground);
    EXPECT_EQ(restored.settings().siteBoundary->ground->datumZ, std::optional<double>(-500.0));
  }
  // More than 3 m between the lowest and highest point: the division is the designer's.
  input.samples[1][2] = 3200;
  ASSERT_TRUE(m.setSiteGround(input));
  study = doc::groundLevelStudy(m.document());
  EXPECT_EQ(study.state, "区分が必要");
  EXPECT_NEAR(study.rangeMm(), 3300.0, 1e-6);
  table = doc::groundLevelTable(study);
  EXPECT_NE(table.notes.front().find("参考"), std::string::npos);
  EXPECT_TRUE(std::any_of(table.notes.begin(), table.notes.end(),
      [](const std::string& n) { return n.find("3 m を超える") != std::string::npos; }));
  // Exactly 3 m: one level.
  input.samples[1][2] = 2900;
  ASSERT_TRUE(m.setSiteGround(input));
  EXPECT_EQ(doc::groundLevelStudy(m.document()).state, "算出");
}
