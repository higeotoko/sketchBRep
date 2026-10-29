// Linear members (docs/40, spec "部材生成・基本編集仕様案 v0.4"): made from a
// catalog or own section between two points, edited afterwards like any
// solid, and their basic sizes read and changed back and forth by people
// and the AI (§13.4).

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Section.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;

namespace {

TopoDS_Shape world(const Modeler& m, const ObjectId& id) { return m.document().worldShape(*m.document().find(id)); }

struct Box {
  double x0, y0, z0, x1, y1, z1;
};
Box boxOf(const Modeler& m, const ObjectId& id) {
  Bnd_Box b;
  BRepBndLib::AddOptimal(world(m, id), b, Standard_False, Standard_False);
  Box out;
  b.Get(out.x0, out.y0, out.z0, out.x1, out.y1, out.z1);
  return out;
}

double volumeOf(const Modeler& m, const ObjectId& id) { return geom::volume(world(m, id)); }
bool valid(const Modeler& m, const ObjectId& id) { return geom::validateShape(world(m, id), true).valid; }

geom::Section rectangle(double width, double depth) {
  geom::Section s;
  s.width = width;
  s.depth = depth;
  return s;
}

const geom::Section& h200() { return geom::findSection("H-200x100x5.5x8")->section; }
const geom::Section& h400() { return geom::findSection("H-400x200x8x13")->section; }

double hArea(const geom::Section& s) {
  return 2 * s.width * s.flange + (s.depth - 2 * s.flange) * s.web + 4 * s.radius * s.radius * (1 - M_PI / 4);
}

template <typename F>
Modeler::MemberChange change(F set) {
  Modeler::MemberChange c;
  set(c);
  return c;
}

double field(const Modeler::MemberInfo& info, const std::string& key) {
  for (const auto& f : info.fields) {
    if (f.key == key) return f.value;
  }
  ADD_FAILURE() << "no field " << key;
  return NAN;
}

std::string basis(const Modeler::MemberInfo& info, const std::string& key) {
  for (const auto& f : info.fields) {
    if (f.key == key) return f.basis;
  }
  return {};
}

// The face of the member whose outward normal is `n` and that reaches
// furthest that way (an end face, a top face).
TopoDS_Face faceFacing(const Modeler& m, const ObjectId& id, const gp_Dir& n) {
  TopoDS_Face best;
  double far = -1e300;
  for (TopExp_Explorer e(world(m, id), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const auto normal = geom::planarFaceNormal(f);
    if (!normal || normal->Angle(n) > 1e-6) continue;
    const double d = gp_Vec(geom::SurfaceRef::fromFace(f)->point.XYZ()).Dot(gp_Vec(n));
    if (d > far) far = d, best = f;
  }
  return best;
}

// A beam on its top edge from (0, 0, 3000) to (4000, 0, 3000).
ObjectId topBeam(Modeler& m, const geom::Section& section) {
  Modeler::MemberSpec spec;
  spec.start = gp_Pnt(0, 0, 3000);
  spec.end = gp_Pnt(4000, 0, 3000);
  spec.section = section;
  spec.anchor = {0, 1};
  spec.role = "beam";
  auto id = m.createMember(spec);
  EXPECT_TRUE(id) << id.error();
  return *id;
}

}  // namespace

TEST(Members, SectionsAreExactSolids) {
  Modeler m;
  const ObjectId h = topBeam(m, h200());
  EXPECT_TRUE(valid(m, h));
  EXPECT_NEAR(volumeOf(m, h), hArea(h200()) * 4000, 1e-6 * hArea(h200()) * 4000);
  // Upright on its top edge: flanges horizontal, 200 deep below z = 3000.
  const Box b = boxOf(m, h);
  EXPECT_NEAR(b.z1, 3000, 1e-6);
  EXPECT_NEAR(b.z0, 2800, 1e-6);
  EXPECT_NEAR(b.y1 - b.y0, 100, 1e-6);
  EXPECT_NEAR(b.x1 - b.x0, 4000, 1e-6);
  // The fillets are exact arcs, not polylines.
  int curved = 0;
  for (TopExp_Explorer e(world(m, h), TopAbs_FACE); e.More(); e.Next()) curved += !geom::planarFaceNormal(TopoDS::Face(e.Current()));
  EXPECT_EQ(curved, 4);
  const auto info = m.memberInfo(h);
  ASSERT_TRUE(info) << info.error();
  EXPECT_EQ(info->catalog, "H-200x100x5.5x8");
  EXPECT_EQ(info->origin, "H-200x100x5.5x8");
  EXPECT_EQ(info->role, "beam");
  EXPECT_FALSE(info->edited);
  EXPECT_NEAR(field(*info, "length"), 4000, 1e-9);
}

TEST(Members, ColumnsStandUpWithAStableSection) {
  Modeler m;
  Modeler::MemberSpec spec;
  spec.start = gp_Pnt(1000, 1000, 0);
  spec.end = gp_Pnt(1000, 1000, 3000);
  spec.section = rectangle(120, 300);
  spec.role = "column";
  spec.materialClass = "rc";
  auto id = m.createMember(spec);
  ASSERT_TRUE(id) << id.error();
  // Along world up: the section's up is +Y, its width along X.
  const Box b = boxOf(m, *id);
  EXPECT_NEAR(b.x1 - b.x0, 120, 1e-6);
  EXPECT_NEAR(b.y1 - b.y0, 300, 1e-6);
  EXPECT_NEAR(b.z0, 0, 1e-6);
  // Slightly off vertical gives the same section direction, not a flip.
  spec.end = gp_Pnt(1000.5, 1000, 3000);
  auto leaning = m.createMember(spec);
  ASSERT_TRUE(leaning);
  const Box l = boxOf(m, *leaning);
  EXPECT_NEAR(l.y1 - l.y0, 300, 1);
  // The height is the member's length; the foot stays.
  auto changed = m.changeMember(*id, change([](Modeler::MemberChange& c) { c.length = 2400.0; }));
  ASSERT_TRUE(changed) << changed.error();
  EXPECT_NEAR(boxOf(m, *id).z0, 0, 1e-6);
  EXPECT_NEAR(boxOf(m, *id).z1, 2400, 1e-6);
  EXPECT_EQ(changed->materialClass, "rc");
}

// §13.4-1: made on its top edge, moved and turned by hand, then swapped by
// the AI to another size: the top line, direction and length stay.
TEST(Members, SwappedSectionKeepsTopLineAfterMoveAndRotate) {
  Modeler m;
  const ObjectId h = topBeam(m, h200());
  gp_Trsf up;
  up.SetTranslation(gp_Vec(0, 0, 500));
  ASSERT_TRUE(m.transform({h}, up));
  ASSERT_TRUE(m.rotate({h}, gp_Ax1(gp_Pnt(0, 0, 0), gp::DZ()), M_PI / 2));
  auto swapped = m.changeMember(h, change([](Modeler::MemberChange& c) { c.catalog = std::string("H-400x200x8x13"); }));
  ASSERT_TRUE(swapped) << swapped.error();
  EXPECT_EQ(swapped->id, h);
  EXPECT_EQ(swapped->catalog, "H-400x200x8x13");
  const Box b = boxOf(m, h);
  EXPECT_NEAR(b.z1, 3500, 1e-6);  // the top line where the person put it
  EXPECT_NEAR(b.z0, 3100, 1e-6);
  EXPECT_NEAR(b.y1 - b.y0, 4000, 1e-6);  // along Y after the turn, as long
  EXPECT_NEAR(b.x1 - b.x0, 200, 1e-6);
  EXPECT_NEAR(volumeOf(m, h), hArea(h400()) * 4000, 1e-6 * hArea(h400()) * 4000);
  // One undo takes the swap back.
  ASSERT_TRUE(m.undo());
  EXPECT_NEAR(boxOf(m, h).z0, 3300, 1e-6);
}

// §13.4-2: the end face pushed by hand, the AI reads the new length (not
// the one it was made with) and sets another; direct edits carry on.
TEST(Members, PushedEndIsTheLengthTheAiReadsAndSets) {
  Modeler m;
  const ObjectId beam = topBeam(m, rectangle(105, 240));
  auto pulled = m.pushPull(beam, faceFacing(m, beam, gp::DX()), 600);
  ASSERT_TRUE(pulled) << pulled.error();
  EXPECT_EQ(pulled->parameterKey, "length");
  auto info = m.memberInfo(beam);
  ASSERT_TRUE(info);
  EXPECT_NEAR(field(*info, "length"), 4600, 1e-9);
  EXPECT_FALSE(info->edited);  // still a plain member: the push became its length
  auto set = m.changeMember(beam, change([](Modeler::MemberChange& c) { c.length = 3000.0; }));
  ASSERT_TRUE(set) << set.error();
  EXPECT_NEAR(boxOf(m, beam).x0, 0, 1e-6);
  EXPECT_NEAR(boxOf(m, beam).x1, 3000, 1e-6);
  // Pulled at the start end too, then pushed on its side: still editable.
  ASSERT_TRUE(m.pushPull(beam, faceFacing(m, beam, -gp::DX()), 200));
  EXPECT_NEAR(boxOf(m, beam).x0, -200, 1e-6);
  ASSERT_TRUE(m.pushPull(beam, faceFacing(m, beam, gp::DZ()), 60));
  info = m.memberInfo(beam);
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->edited);
  EXPECT_EQ(basis(*info, "length"), "shape");
  EXPECT_NEAR(field(*info, "length"), 3200, 1e-6);
  set = m.changeMember(beam, change([](Modeler::MemberChange& c) { c.length = 2500.0; }));
  ASSERT_TRUE(set) << set.error();
  EXPECT_NEAR(field(*set, "length"), 2500, 1e-6);
  EXPECT_NEAR(boxOf(m, beam).z1, 3060, 1e-6);  // the side push stayed
  EXPECT_TRUE(valid(m, beam));
}

// §13.4-3: the AI changes a column's section and height; the person then
// edits it directly, without unlocking anything.
TEST(Members, AiChangedColumnStaysDirectlyEditable) {
  Modeler m;
  Modeler::MemberSpec spec;
  spec.start = gp_Pnt(0, 0, 0);
  spec.end = gp_Pnt(0, 0, 2800);
  spec.section = rectangle(105, 105);
  spec.role = "column";
  spec.materialClass = "timber";
  const ObjectId c = *m.createMember(spec);
  auto changed = m.changeMember(c, change([](Modeler::MemberChange& c) { c.width = 120.0; c.depth = 150.0; c.length = 3000.0; }));
  ASSERT_TRUE(changed) << changed.error();
  EXPECT_NEAR(volumeOf(m, c), 120.0 * 150 * 3000, 1e-3);
  // Its own dimensions: no catalog entry claimed.
  EXPECT_TRUE(changed->catalog.empty());
  ASSERT_TRUE(m.pushPull(c, faceFacing(m, c, gp::DX()), 30));  // a side: an ordinary push/pull
  EXPECT_NEAR(volumeOf(m, c), 150.0 * 150 * 3000, 1e-3);
  ASSERT_TRUE(m.pushPull(c, faceFacing(m, c, gp::DZ()), 200));  // the top: its height
  EXPECT_NEAR(boxOf(m, c).z1, 3200, 1e-6);
  EXPECT_TRUE(valid(m, c));
}

// §13.4-5: a swap that the edits made since cannot follow leaves the
// member (and its edits) as they are, with the reason; direct edits work.
TEST(Members, SwapThatCannotKeepTheEditsChangesNothing) {
  Modeler m;
  const ObjectId h = topBeam(m, h400());
  // A flange's outer side pushed: an edit tied to that face's place.
  ASSERT_TRUE(m.pushPull(h, faceFacing(m, h, gp::DY()), 40));
  const double before = volumeOf(m, h);
  auto swapped = m.changeMember(h, change([](Modeler::MemberChange& c) { c.catalog = std::string("H-200x100x5.5x8"); }));
  if (swapped) {
    // Redone on the smaller section: the edit is still there.
    EXPECT_GT(volumeOf(m, h), hArea(h200()) * 4000);
  } else {
    EXPECT_NE(swapped.error().find("not changed"), std::string::npos) << swapped.error();
    EXPECT_NEAR(volumeOf(m, h), before, 1e-6 * before);
  }
  EXPECT_TRUE(valid(m, h));
  ASSERT_TRUE(m.pushPull(h, faceFacing(m, h, gp::DX()), 100));
  // A name that is not in the catalog is never replaced by another size.
  auto missing = m.changeMember(h, change([](Modeler::MemberChange& c) { c.catalog = std::string("H-300x150"); }));
  EXPECT_FALSE(missing);
}

TEST(Members, AnchorMovesOnlyTheAxisLine) {
  Modeler m;
  Modeler::MemberSpec spec;
  spec.start = gp_Pnt(0, 0, 3000);
  spec.end = gp_Pnt(4000, 0, 3000);
  spec.section = rectangle(105, 240);
  const ObjectId beam = *m.createMember(spec);  // centred: 2880..3120
  const Box before = boxOf(m, beam);
  auto moved = m.changeMember(beam, change([](Modeler::MemberChange& c) { c.anchor = geom::SectionAnchor{0, 1}; }));
  ASSERT_TRUE(moved) << moved.error();
  const Box after = boxOf(m, beam);
  EXPECT_NEAR(after.z0, before.z0, 1e-6);
  EXPECT_NEAR(after.z1, before.z1, 1e-6);
  // Now on its top edge: deeper goes down from there.
  ASSERT_TRUE(m.changeMember(beam, change([](Modeler::MemberChange& c) { c.depth = 300.0; })));
  EXPECT_NEAR(boxOf(m, beam).z1, 3120, 1e-6);
  EXPECT_NEAR(boxOf(m, beam).z0, 2820, 1e-6);
}

TEST(Members, BadRequestsChangeNothing) {
  Modeler m;
  Modeler::MemberSpec spec;
  spec.start = spec.end = gp_Pnt(0, 0, 0);
  spec.section = rectangle(100, 100);
  EXPECT_FALSE(m.createMember(spec));  // one point
  spec.end = gp_Pnt(NAN, 0, 0);
  EXPECT_FALSE(m.createMember(spec));
  spec.end = gp_Pnt(1000, 0, 0);
  spec.section = rectangle(0, 100);
  EXPECT_FALSE(m.createMember(spec));
  geom::Section bad = h200();
  bad.web = 120;  // wider than the flanges
  spec.section = bad;
  EXPECT_FALSE(m.createMember(spec));
  EXPECT_TRUE(m.document().objects().empty());
  EXPECT_EQ(m.history().undoCount(), 0u);
  spec.section = h200();
  const ObjectId h = *m.createMember(spec);
  const double v = volumeOf(m, h);
  EXPECT_FALSE(m.changeMember(h, change([](Modeler::MemberChange& c) { c.length = -5.0; })));
  EXPECT_FALSE(m.changeMember(h, change([](Modeler::MemberChange& c) { c.web = 150.0; })));
  EXPECT_FALSE(m.changeMember(h, change([](Modeler::MemberChange& c) { c.width = 150.0; c.flange = 120.0; })));  // checked together: refused as a whole
  EXPECT_NEAR(volumeOf(m, h), v, 1e-9);
  EXPECT_NEAR(field(*m.memberInfo(h), "width"), 100, 1e-12);
}

// §13.4-6: saved and opened again, the same basic edits work; GUI and AI go
// through the same Modeler call.
TEST(Members, SavedMembersStayEditable) {
  Modeler m;
  const ObjectId h = topBeam(m, h200());
  ASSERT_TRUE(m.changeMember(h, change([](Modeler::MemberChange& c) { c.length = 5000.0; c.roll = M_PI; })));
  const std::string file = ::testing::TempDir() + "members.sbr";
  ASSERT_TRUE(m.save(file));
  Modeler loaded;
  ASSERT_TRUE(loaded.load(file));
  auto info = loaded.memberInfo(h);
  ASSERT_TRUE(info) << info.error();
  EXPECT_EQ(info->catalog, "H-200x100x5.5x8");
  EXPECT_NEAR(field(*info, "length"), 5000, 1e-9);
  EXPECT_NEAR(field(*info, "roll"), 180, 1e-9);
  EXPECT_EQ(info->role, "beam");
  // Turned over on its top line: now hanging up from it.
  EXPECT_NEAR(boxOf(loaded, h).z0, 3000, 1e-6);
  ASSERT_TRUE(loaded.changeMember(h, change([](Modeler::MemberChange& c) { c.catalog = std::string("H-400x200x8x13"); })));
  EXPECT_NEAR(boxOf(loaded, h).z1, 3400, 1e-6);
}

TEST(Members, AiCommandsGoTheSameWay) {
  Modeler m;
  api::CommandApi api(m);
  auto run = [&](json command) { return json::parse(api.execute(command.dump())); };
  const json list = run({{"command", "list_sections"}, {"args", {{"query", "H-400x200x8x13"}}}});
  ASSERT_TRUE(list.value("ok", false)) << list.dump();
  EXPECT_EQ(list["result"]["sections"][0]["id"], "H-400x200x8x13");
  const json made = run({{"command", "create_member"},
                         {"args",
                          {{"section", "H-400x200x8x13"},
                           {"start", {0, 0, 3000}},
                           {"end", {6000, 0, 3000}},
                           {"anchor", {{"up", "top"}}},
                           {"role", "beam"}}}});
  ASSERT_TRUE(made.value("ok", false)) << made.dump();
  const std::string id = made["result"]["id"];
  EXPECT_EQ(made["result"]["material_class"], "steel");
  EXPECT_EQ(made["result"]["anchor"]["up"], "top");
  const json changed = run({{"command", "change_member"}, {"args", {{"id", id}, {"catalog", "H-200x100x5.5x8"}, {"length", 5000}}}});
  ASSERT_TRUE(changed.value("ok", false)) << changed.dump();
  EXPECT_EQ(changed["result"]["catalog"], "H-200x100x5.5x8");
  EXPECT_NEAR(boxOf(m, id).z1, 3000, 1e-6);
  EXPECT_NEAR(boxOf(m, id).x1, 5000, 1e-6);
  const json own = run({{"command", "create_member"},
                        {"args", {{"section", {{"kind", "rectangle"}, {"width", 120}, {"depth", 240}}},
                                  {"start", {0, 2000, 0}}, {"length", 2800}, {"role", "column"}}}});
  ASSERT_TRUE(own.value("ok", false)) << own.dump();
  EXPECT_TRUE(own["result"]["catalog"].is_null());
  const json missing = run({{"command", "create_member"},
                            {"args", {{"section", "H-300x150x6.5x9"}, {"start", {0, 0, 0}}, {"end", {1, 0, 0}}}}});
  EXPECT_FALSE(missing.value("ok", true));
  // The role is the ordinary kind attribute: found by the ordinary query.
  const json beams = run({{"command", "query_objects"}, {"args", {{"where", {{"kind", "beam"}}}}}});
  ASSERT_TRUE(beams.value("ok", false)) << beams.dump();
}

// §9.4 / §13.2: optional, SBR_MEMBER_BENCH_N members: making them, one
// change among them, saving and opening, with and without their meaning.
TEST(Members, Bench) {
  const char* env = std::getenv("SBR_MEMBER_BENCH_N");
  const int n = env ? std::atoi(env) : 0;
  if (n <= 0) GTEST_SKIP() << "set SBR_MEMBER_BENCH_N to run";
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point s) { return std::chrono::duration<double, std::milli>(Clock::now() - s).count(); };
  for (const int mode : {0, 1, 2}) {
    const bool meaning = mode == 1, plain = mode == 2;
    Modeler m;
    auto s = Clock::now();
    m.history().beginGroup("Bench");
    std::vector<ObjectId> ids;
    for (int i = 0; i < n && plain; ++i) {
      // The same rectangles as ordinary extruded boxes, for comparison.
      geom::SketchPlane plane = geom::SketchPlane::ground();
      plane.origin = gp_Pnt((i % 40) * 1000.0, (i / 40) * 1000.0, 2760);
      const ObjectId id = *m.createRectangle(plane, 900, 105);
      ASSERT_TRUE(m.extrude(id, 240));
      ids.push_back(id);
    }
    for (int i = 0; i < n && !plain; ++i) {
      Modeler::MemberSpec spec;
      spec.start = gp_Pnt((i % 40) * 1000.0, (i / 40) * 1000.0, 3000);
      spec.end = spec.start.Translated(gp_Vec(900, 0, 0));
      spec.section = i % 2 ? h200() : rectangle(105, 240);
      if (meaning) {
        spec.role = "beam";
        spec.materialClass = i % 2 ? "steel" : "timber";
      }
      ids.push_back(*m.createMember(spec));
    }
    m.history().endGroup();
    const double made = ms(s);
    s = Clock::now();
    if (plain) {
      ASSERT_TRUE(m.setParameter(ids[n / 2], "width", 800.0));
    } else {
      ASSERT_TRUE(m.changeMember(ids[n / 2], change([](Modeler::MemberChange& c) { c.length = 800.0; })));
    }
    const double one = ms(s);
    s = Clock::now();
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, 10));
    ASSERT_TRUE(m.transform({ids[n / 3]}, t));
    const double moved = ms(s);
    const std::string file = ::testing::TempDir() + "members_bench.sbr";
    s = Clock::now();
    ASSERT_TRUE(m.save(file));
    const double saved = ms(s);
    Modeler loaded;
    s = Clock::now();
    ASSERT_TRUE(loaded.load(file));
    const double opened = ms(s);
    std::cout << n << (plain ? " plain boxes" : meaning ? " members with role/material" : " members without meaning")
              << ": made " << made
              << " ms, one change " << one << " ms, one move " << moved << " ms, save " << saved << " ms, open "
              << opened << " ms\n";
  }
}
