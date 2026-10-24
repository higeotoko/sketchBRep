// Follow Me as reported (a rectangle standing at the start of a polyline of
// several segments on the ground): the profile runs along the whole path,
// joined at the corners, where it stands (not moved onto the path).

#include <algorithm>
#include <optional>

#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

std::vector<TopoDS_Edge> edgesOf(const Modeler& m, const ObjectId& id) {
  std::vector<TopoDS_Edge> out;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_EDGE); e.More(); e.Next()) {
    out.push_back(TopoDS::Edge(e.Current()));
  }
  return out;
}

int faceCount(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
  return n;
}

double lengthOf(const std::vector<gp_Pnt>& points) {
  double l = 0;
  for (std::size_t i = 1; i < points.size(); ++i) l += points[i - 1].Distance(points[i]);
  return l;
}

// The 2000 x 1500 rectangle standing on y = 0 (as in the report).
ObjectId standingProfile(Modeler& m) {
  return *m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 1500), gp_Pnt(0, 0, 1500)});
}

// A face of `shape` exactly where the profile stood (it was not moved).
bool hasStartCap(const TopoDS_Shape& shape) {
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const Bnd_Box b = geom::exactBounds(e.Current());
    if (std::abs(b.CornerMin().Y()) < 1e-6 && std::abs(b.CornerMax().Y()) < 1e-6 && std::abs(b.CornerMin().X()) < 1e-6 &&
        std::abs(b.CornerMax().X() - 2000) < 1e-6 && std::abs(b.CornerMax().Z() - 1500) < 1e-6) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Straight, one corner, several corners (the report: 3 segments); the line
// as one polyline or one line per segment, given in reverse order. Joined
// at the corners: one valid solid, 4 sides per segment and the two ends (no
// face left inside), the start where the profile stood, the volume the
// profile's area times the path's length (its centre stays above the path).
TEST(FollowMe, StraightOneCornerSeveralCorners) {
  const std::vector<std::vector<gp_Pnt>> paths{
      {gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0)},
      {gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0)},
      {gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0), gp_Pnt(6000, 3500, 0)},
  };
  for (const auto& points : paths) {
    for (bool separate : {false, true}) {
      Modeler m;
      const ObjectId profile = standingProfile(m);
      std::vector<TopoDS_Edge> edges;
      if (separate) {
        for (std::size_t i = 1; i < points.size(); ++i) {
          const auto line = m.createPolyline({points[i - 1], points[i]});
          ASSERT_TRUE(line.ok());
          const auto mine = edgesOf(m, *line);
          edges.insert(edges.begin(), mine.begin(), mine.end());  // last first
        }
      } else {
        const auto line = m.createPolyline(points);
        ASSERT_TRUE(line.ok());
        edges = edgesOf(m, *line);
        std::reverse(edges.begin(), edges.end());
      }
      const std::size_t steps = m.history().undoCount();
      auto r = m.followMe(profile, edges);
      ASSERT_TRUE(r.ok()) << r.error();
      EXPECT_EQ(m.history().undoCount(), steps + 1);
      EXPECT_FALSE(m.document().find(profile));  // consumed, as in SketchUp
      const TopoDS_Shape shape = m.document().worldShape(*m.document().find(*r));
      const int segments = int(points.size()) - 1;
      EXPECT_TRUE(geom::validateShape(shape, true).valid);
      EXPECT_EQ(faceCount(shape), 4 * segments + 2) << segments;
      EXPECT_TRUE(hasStartCap(shape)) << segments;
      EXPECT_NEAR(geom::volume(shape), 2000.0 * 1500 * lengthOf(points), 1.0) << segments;
      // Undo: profile back, result gone; redo; saved and reloaded.
      ASSERT_TRUE(m.undo());
      EXPECT_TRUE(m.document().find(profile));
      EXPECT_FALSE(m.document().find(*r));
      ASSERT_TRUE(m.redo());
      const std::string file = ::testing::TempDir() + "follow_me.sbr";
      ASSERT_TRUE(m.save(file).ok());
      Modeler back;
      ASSERT_TRUE(back.load(file).ok());
      EXPECT_NEAR(geom::volume(back.document().worldShape(*back.document().find(*r))), geom::volume(shape),
                  1e-9 * geom::volume(shape));
    }
  }
}

// The AI's follow_me with path_edges gives the same shape as the GUI's call.
TEST(FollowMe, ApiMatches) {
  const std::vector<gp_Pnt> points{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0),
                                   gp_Pnt(6000, 3500, 0)};
  Modeler a;
  const ObjectId pa = standingProfile(a);
  const ObjectId la = *a.createPolyline(points);
  auto ra = a.followMe(pa, edgesOf(a, la));
  ASSERT_TRUE(ra.ok());
  Modeler b;
  api::CommandApi api(b);
  const ObjectId pb = standingProfile(b);
  const ObjectId lb = *b.createPolyline(points);
  nlohmann::json args{{"profile", pb}, {"path_edges", nlohmann::json::array({nlohmann::json{{"id", lb}}})}};
  const auto r = nlohmann::json::parse(api.execute(nlohmann::json{{"command", "follow_me"}, {"args", args}}.dump()));
  ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
  const TopoDS_Shape sa = a.document().worldShape(*a.document().find(*ra));
  const TopoDS_Shape sb = b.document().worldShape(*b.document().find(r["result"]["id"].get<std::string>()));
  EXPECT_NEAR(geom::volume(sa), geom::volume(sb), 1e-6);
  EXPECT_EQ(faceCount(sa), faceCount(sb));
  const Bnd_Box ba = geom::exactBounds(sa), bb = geom::exactBounds(sb);
  EXPECT_LT(ba.CornerMin().Distance(bb.CornerMin()) + ba.CornerMax().Distance(bb.CornerMax()), 1e-6);
}

// An arc in the path, and a hollow profile (a frame with a hole).
TEST(FollowMe, ArcInThePathAndHollowProfile) {
  {
    Modeler m;
    const ObjectId profile = standingProfile(m);
    const ObjectId line = *m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0)});
    auto arc = m.createArc(gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 6000, 0), gp::DZ(), -800);
    ASSERT_TRUE(arc.ok());
    std::vector<TopoDS_Edge> edges = edgesOf(m, *arc);
    const auto first = edgesOf(m, line);
    edges.insert(edges.end(), first.begin(), first.end());
    auto r = m.followMe(profile, edges);
    ASSERT_TRUE(r.ok()) << r.error();
    const TopoDS_Shape shape = m.document().worldShape(*m.document().find(*r));
    EXPECT_TRUE(geom::validateShape(shape, true).valid);
    EXPECT_TRUE(hasStartCap(shape));
  }
  {
    Modeler m;
    standingProfile(m);
    // A 200 frame: the inner rectangle cuts a hole in the profile.
    ASSERT_TRUE(m.createPolygon({gp_Pnt(200, 0, 200), gp_Pnt(1800, 0, 200), gp_Pnt(1800, 0, 1300), gp_Pnt(200, 0, 1300)}).ok());
    const double frameArea = 2000.0 * 1500 - 1600.0 * 1100;
    std::optional<ObjectId> frame;
    for (const Object& o : m.document().objects()) {
      if (o.kind() == ObjectKind::Face && std::abs(geom::area(m.document().worldShape(o)) - frameArea) < 1.0) frame = o.id;
    }
    ASSERT_TRUE(frame.has_value());
    const std::vector<gp_Pnt> points{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0),
                                     gp_Pnt(6000, 3500, 0)};
    const ObjectId line = *m.createPolyline(points);
    auto r = m.followMe(*frame, edgesOf(m, line));
    ASSERT_TRUE(r.ok()) << r.error();
    const TopoDS_Shape shape = m.document().worldShape(*m.document().find(*r));
    EXPECT_TRUE(geom::validateShape(shape, true).valid);
    EXPECT_NEAR(geom::volume(shape), frameArea * lengthOf(points), 1.0);
  }
}

// What cannot be swept says why and where; profile and path stay as they
// were (nothing recorded).
TEST(FollowMe, BranchGapAndSelfCrossingRefusedWithTheReason) {
  auto check = [](const std::vector<std::vector<gp_Pnt>>& lines, const std::string& expect) {
    Modeler m;
    const ObjectId profile = standingProfile(m);
    std::vector<TopoDS_Edge> edges;
    for (const auto& l : lines) {
      const auto id = m.createPolyline(l);
      ASSERT_TRUE(id.ok());
      const auto mine = edgesOf(m, *id);
      edges.insert(edges.end(), mine.begin(), mine.end());
    }
    const std::size_t objects = m.document().objects().size(), steps = m.history().undoCount();
    auto r = m.followMe(profile, edges);
    ASSERT_FALSE(r.ok()) << expect;
    EXPECT_NE(r.error().find(expect), std::string::npos) << r.error();
    EXPECT_EQ(m.document().objects().size(), objects);
    EXPECT_EQ(m.history().undoCount(), steps);
    EXPECT_TRUE(m.document().find(profile));
  };
  // A branch at the corner (1000, 3000).
  check({{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0)}, {gp_Pnt(1000, 3000, 0), gp_Pnt(-2000, 4000, 0)}},
        "(1000.0, 3000.0, 0.0)");
  // A gap between (1000, 3000) and (1500, 3000).
  check({{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0)}, {gp_Pnt(1500, 3000, 0), gp_Pnt(4000, 4500, 0)}},
        "(1000.0, 3000.0, 0.0)");
  // Turning straight back within the profile's width: the shape would cross itself.
  check({{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(1500, 0, 0)}}, "cross");
}
