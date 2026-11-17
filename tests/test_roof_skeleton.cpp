// The faces of a roof worked out by moving every side inwards (docs/164). Checked against what
// is known another way: over a convex outline a point is roofed by the side whose plane is
// lowest there; over an L the ridges, hips and valleys are where hand calculation puts them; and
// everywhere the faces must meet at one height (the roof has no steps).
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Roof.h"
#include "sbr/geom/RoofSkeleton.h"

using sbr::geom::roofSkeletonFaces;
using sbr::geom::SkeletonPoint;
using sbr::geom::SkeletonSide;

namespace {
using Faces = std::vector<std::vector<SkeletonPoint>>;

std::vector<SkeletonSide> sidesOf(const std::vector<SkeletonPoint>& outline, double slope = 1.0, double out = 0.0) {
  std::vector<SkeletonSide> sides;
  for (std::size_t i = 0; i < outline.size(); ++i) {
    SkeletonSide s;
    s.a = outline[i];
    s.b = outline[(i + 1) % outline.size()];
    s.slope = slope;
    s.out = out;
    s.start = -slope * out;  // its plane passes through the side at height 0
    sides.push_back(s);
  }
  return sides;
}

double areaOf(const std::vector<SkeletonPoint>& p) {
  double a = 0.0;
  for (std::size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
  return a / 2;
}

// The height of side k's plane over a point: 0 on the side, rising inwards.
double heightAt(const SkeletonSide& s, const SkeletonPoint& p) {
  const double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y, len = std::hypot(dx, dy);
  return s.slope * ((p.y - s.a.y) * dx - (p.x - s.a.x) * dy) / len;
}

bool has(const std::vector<SkeletonPoint>& face, double x, double y) {
  for (const SkeletonPoint& p : face)
    if (std::hypot(p.x - x, p.y - y) < 0.01) return true;
  return false;
}

// Where faces share a corner their planes are at one height there.
void expectNoSteps(const std::vector<SkeletonSide>& sides, const Faces& faces) {
  for (std::size_t i = 0; i < faces.size(); ++i)
    for (const SkeletonPoint& p : faces[i])
      for (std::size_t j = i + 1; j < faces.size(); ++j)
        for (const SkeletonPoint& q : faces[j])
          if (std::hypot(p.x - q.x, p.y - q.y) < 0.01 && sides[i].moving && sides[j].moving) {
            EXPECT_NEAR(heightAt(sides[i], p), heightAt(sides[j], q), 0.01) << "sides " << i << " and " << j << " at " << p.x << ", " << p.y;
          }
}
}  // namespace

TEST(RoofSkeleton, OverAConvexOutlineEachPointIsUnderTheLowestPlane) {
  // A rectangle, a pentagon with a slanted side, a hexagon; pitches of their own, a gable, overhangs.
  const std::vector<std::vector<SkeletonPoint>> outlines = {
      {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}},
      {{0, 0}, {6000, 0}, {8000, 2000}, {8000, 6000}, {0, 6000}},
      {{4000, 0}, {2000, 3464.1}, {-2000, 3464.1}, {-4000, 0}, {-2000, -3464.1}, {2000, -3464.1}},
  };
  for (int variant = 0; variant < 4; ++variant)
    for (const auto& outline : outlines) {
      std::vector<SkeletonSide> sides = sidesOf(outline, 0.4, variant == 3 ? 600.0 : 0.0);
      if (variant == 1) sides[0].slope = 0.8, sides[2].slope = 0.25;
      if (variant == 2) sides[1].moving = false, sides[1].out = 0.0;
      if (variant == 3)
        for (SkeletonSide& s : sides) s.start = -s.slope * s.out;
      if (variant == 1)
        for (SkeletonSide& s : sides) s.start = 0.0;
      std::string why;
      const auto faces = roofSkeletonFaces(sides, &why);
      ASSERT_TRUE(faces) << "variant " << variant << ": " << why;
      double total = 0.0;
      for (std::size_t k = 0; k < sides.size(); ++k) {
        const auto& face = (*faces)[k];
        if (!sides[k].moving) {
          EXPECT_TRUE(face.empty());
          continue;
        }
        ASSERT_GE(face.size(), 3u) << "variant " << variant << " side " << k;
        total += areaOf(face);
        // At its corners and in its middle no other plane is lower.
        SkeletonPoint mid{0, 0};
        for (const SkeletonPoint& p : face) mid.x += p.x / face.size(), mid.y += p.y / face.size();
        std::vector<SkeletonPoint> probes = face;
        probes.push_back(mid);
        for (const SkeletonPoint& p : probes)
          for (std::size_t j = 0; j < sides.size(); ++j)
            if (sides[j].moving) {
              EXPECT_LE(heightAt(sides[k], p), heightAt(sides[j], p) + 0.01) << "variant " << variant << " side " << k << " against " << j;
            }
      }
      // Together they cover the outline grown by the overhangs.
      double grown = areaOf(outline);
      if (variant == 3) {
        double around = 0.0, corners = 0.0;
        for (std::size_t i = 0; i < outline.size(); ++i) {
          const auto &a = outline[i], &b = outline[(i + 1) % outline.size()], &c = outline[(i + 2) % outline.size()];
          around += std::hypot(b.x - a.x, b.y - a.y);
          const double turn = std::atan2((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x), (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y));
          corners += std::tan(turn / 2);
        }
        grown += around * 600 + 600.0 * 600 * corners;
      }
      EXPECT_NEAR(total, grown, 1e-6 * grown + 2.0) << "variant " << variant;
      expectNoSteps(sides, *faces);
    }
}

TEST(RoofSkeleton, AnLHasAHipOutsideAndAValleyInside) {
  // Wings 4000 deep: the ridges over y = 2000 and x = 2000 meet the hip and the valley at one point.
  const std::vector<SkeletonPoint> ell = {{0, 0}, {10000, 0}, {10000, 4000}, {4000, 4000}, {4000, 9000}, {0, 9000}};
  auto sides = sidesOf(ell);
  std::string why;
  auto faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  EXPECT_NEAR(areaOf((*faces)[0]), (10000.0 + 6000) / 2 * 2000, 1.0);  // south: a trapezium up to the ridge
  EXPECT_TRUE(has((*faces)[0], 2000, 2000) && has((*faces)[0], 8000, 2000));
  EXPECT_NEAR(areaOf((*faces)[1]), 4000.0 * 2000 / 2, 1.0);  // the east end: a triangle
  EXPECT_NEAR(areaOf((*faces)[2]), (6000.0 + 6000) / 2 * 2000, 1.0);  // north of wing A: from the valley to the hip end
  EXPECT_TRUE(has((*faces)[2], 4000, 4000) && has((*faces)[2], 2000, 2000));
  EXPECT_NEAR(areaOf((*faces)[5]), (9000.0 + 5000) / 2 * 2000, 1.0);  // west
  expectNoSteps(sides, *faces);
  // Drawn from another corner: the same faces.
  std::vector<SkeletonPoint> turned(ell.begin() + 2, ell.end());
  turned.insert(turned.end(), ell.begin(), ell.begin() + 2);
  const auto again = roofSkeletonFaces(sidesOf(turned), &why);
  ASSERT_TRUE(again) << why;
  for (std::size_t k = 0; k < 6; ++k) EXPECT_NEAR(areaOf((*again)[k]), areaOf((*faces)[(k + 2) % 6]), 1.0);

  // A narrow wing (2000 wide) on a deep one (4000): its ridge over x = 1000 ends where the valley
  // from the inward corner meets it, at (1000, 3000); from there a line runs on to (2000, 2000).
  const std::vector<SkeletonPoint> uneven = {{0, 0}, {10000, 0}, {10000, 4000}, {2000, 4000}, {2000, 9000}, {0, 9000}};
  sides = sidesOf(uneven);
  faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  EXPECT_NEAR(areaOf((*faces)[5]), 9.0e6, 1.0);  // west: (0,9000) (0,0) (2000,2000) (1000,3000) (1000,8000)
  EXPECT_TRUE(has((*faces)[5], 1000, 3000) && has((*faces)[5], 2000, 2000) && has((*faces)[5], 1000, 8000));
  EXPECT_TRUE(has((*faces)[3], 2000, 4000) && has((*faces)[3], 1000, 3000));  // east of the narrow wing: down to the valley
  EXPECT_NEAR(areaOf((*faces)[3]), 5000.0 * 1000, 1.0);  // (2000,4000) (2000,9000) (1000,8000) (1000,3000)
  expectNoSteps(sides, *faces);
}

TEST(RoofSkeleton, AnInwardCornerThatReachesTheSideAcrossSplitsTheRoofInTwo) {
  // A 12000 x 6000 block with a V cut in from the north down to (6000, 2000): its point runs
  // south and meets the south side's front at height t where t = 2000 - t * sqrt(17).
  const std::vector<SkeletonPoint> cut = {{0, 0}, {12000, 0}, {12000, 6000}, {7000, 6000}, {6000, 2000}, {5000, 6000}, {0, 6000}};
  const auto sides = sidesOf(cut);
  std::string why;
  const auto faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  const double meet = 2000.0 / (1.0 + std::sqrt(17.0));
  EXPECT_TRUE(has((*faces)[0], 6000, meet)) << "the south face has the meeting point";
  EXPECT_EQ((*faces)[0].size(), 5u);  // two lobes, each up to its own top, meeting at that point
  double total = 0.0;
  for (const auto& face : *faces) total += areaOf(face);
  EXPECT_NEAR(total, areaOf(cut), 2.0);
  expectNoSteps(sides, *faces);
  // Mirror-symmetric outline: mirror-symmetric faces.
  EXPECT_NEAR(areaOf((*faces)[1]), areaOf((*faces)[6]), 1.0);
  EXPECT_NEAR(areaOf((*faces)[2]), areaOf((*faces)[5]), 1.0);
  EXPECT_NEAR(areaOf((*faces)[3]), areaOf((*faces)[4]), 1.0);
}

TEST(RoofSkeleton, ShapesWithManyInwardCornersAndWhatIsRefused) {
  std::string why;
  // A cross (four inward corners), a U, a T, a Z: worked out, no steps, symmetric where they are.
  const std::vector<SkeletonPoint> cross = {{3000, 0}, {6000, 0}, {6000, 3000}, {9000, 3000}, {9000, 6000}, {6000, 6000},
                                            {6000, 9000}, {3000, 9000}, {3000, 6000}, {0, 6000}, {0, 3000}, {3000, 3000}};
  auto sides = sidesOf(cross);
  auto faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  expectNoSteps(sides, *faces);
  for (std::size_t k = 3; k < 12; ++k) EXPECT_NEAR(areaOf((*faces)[k]), areaOf((*faces)[k % 3]), 1.0) << k;  // four-fold
  const std::vector<SkeletonPoint> u = {{0, 0}, {9000, 0}, {9000, 7000}, {6000, 7000}, {6000, 3000}, {3000, 3000}, {3000, 7000}, {0, 7000}};
  sides = sidesOf(u, 0.5, 450.0);
  faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  expectNoSteps(sides, *faces);
  EXPECT_NEAR(areaOf((*faces)[1]), areaOf((*faces)[7]), 1.0);
  const std::vector<SkeletonPoint> z = {{0, 0}, {5000, 0}, {5000, 3000}, {9000, 3000}, {9000, 8000}, {4000, 8000}, {4000, 5000}, {0, 5000}};
  sides = sidesOf(z);
  faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  expectNoSteps(sides, *faces);
  // A side in two parts in one line (a corner on a straight run): one plane, shared between them.
  const std::vector<SkeletonPoint> straight = {{0, 0}, {4000, 0}, {8000, 0}, {8000, 6000}, {0, 6000}};
  sides = sidesOf(straight);
  faces = roofSkeletonFaces(sides, &why);
  ASSERT_TRUE(faces) << why;
  EXPECT_NEAR(areaOf((*faces)[0]) + areaOf((*faces)[1]), (8000.0 + 2000) / 2 * 3000, 1.0);
  // Refused, with the reason: the two parts with different pitches; no side that moves.
  sides[0].slope = 2.0;
  EXPECT_FALSE(roofSkeletonFaces(sides, &why));
  EXPECT_NE(why.find("one line"), std::string::npos) << why;
  sides = sidesOf(straight);
  for (auto& s : sides) s.moving = false;
  EXPECT_FALSE(roofSkeletonFaces(sides, &why));
}

// The roof as a solid: where the old way (the largest convex parts, each roofed, together) was
// right the new one gives the same roof; a narrow wing on a deep one no longer shows the narrow
// wing's hip end inside the deep wing.
TEST(RoofSkeleton, TheSolidOverInwardCorners) {
  using sbr::geom::makeRoof;
  using sbr::geom::RoofSpec;
  RoofSpec ell;
  ell.kind = "hip";
  ell.height = 3000;
  ell.outline = {gp_Pnt(0, 0, 0), gp_Pnt(10000, 0, 0), gp_Pnt(10000, 4000, 0), gp_Pnt(4000, 4000, 0), gp_Pnt(4000, 9000, 0), gp_Pnt(0, 9000, 0)};
  const auto rects = makeRoof(ell);  // right-angled: the largest rectangles (docs/99)
  ASSERT_TRUE(rects) << rects.error();
  RoofSpec same = ell;
  same.sideEave = {{0, 600.0}};  // the same overhang, said for one side: worked out side by side
  const auto skeleton = makeRoof(same);
  ASSERT_TRUE(skeleton) << skeleton.error();
  EXPECT_EQ(skeleton->parts, 6);  // a face for every side
  EXPECT_NEAR(skeleton->planArea, rects->planArea, 1.0);
  EXPECT_NEAR(sbr::geom::volume(skeleton->solid), sbr::geom::volume(rects->solid), 1e-6 * sbr::geom::volume(rects->solid));
  EXPECT_NEAR(skeleton->ridgeHeight, rects->ridgeHeight, 1e-3);
  // Sides of their own pitch over inward corners: the same thickness on every slope.
  RoofSpec steep = same;
  steep.sidePitch = {{0, 8.0}, {5, 6.0}};
  const auto mixed = makeRoof(steep);
  ASSERT_TRUE(mixed) << mixed.error();
  EXPECT_EQ(mixed->parts, 6);  // worked out side by side, not by the convex parts
  EXPECT_NEAR(mixed->planArea, rects->planArea, 1.0);
  EXPECT_NEAR(mixed->eaveHeight, 3000 - 0.8 * 600, 1e-3);
  // Put together from its faces: the upper ones, the lower ones and one upright face a side.
  int count = 0, solids = 0;
  for (TopExp_Explorer e(mixed->solid, TopAbs_FACE); e.More(); e.Next()) ++count;
  for (TopExp_Explorer e(mixed->solid, TopAbs_SOLID); e.More(); e.Next()) ++solids;
  EXPECT_EQ(count, 18);
  EXPECT_EQ(solids, 1);
  EXPECT_TRUE(BRepCheck_Analyzer(mixed->solid).IsValid());
  // The west eave (pitch 6) is higher than the south one (pitch 8): the roof's lowest edge is the south one.
  EXPECT_GT(mixed->ridgeHeight, rects->ridgeHeight);
  EXPECT_NEAR(sbr::geom::volume(mixed->solid), mixed->slopeArea * 100.0, mixed->slopeArea * 100.0 * 0.03);
}
