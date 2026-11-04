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
    if (i.pen == Pen::GridLabel && i.kind == Item::Kind::Arc) bubbles += std::abs(i.radius - 500) < 1e-6;
    if (i.kind != Item::Kind::Text) continue;
    x2 |= i.text == "X2";
    span |= i.pen == Pen::Dimension && i.text == "6000";
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
