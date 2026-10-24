// Faces drawn one by one round a volume (report: six rectangles drawn edge
// to edge round a cube stayed six loose faces): once the last one closes it
// without gaps, they are one solid, like a box pushed up from a rectangle.

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>

#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

int qEnvInt(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::atoi(v) : 0;
}

int count(const Modeler& m, ObjectKind k) {
  int n = 0;
  for (const Object& o : m.document().objects()) n += o.kind() == k;
  return n;
}

// The six sides of the cube [0, s]^3, as corner lists.
std::vector<std::vector<gp_Pnt>> cubeSides(double s = 1000) {
  return {
      {gp_Pnt(0, 0, 0), gp_Pnt(s, 0, 0), gp_Pnt(s, s, 0), gp_Pnt(0, s, 0)},  // bottom
      {gp_Pnt(0, 0, s), gp_Pnt(s, 0, s), gp_Pnt(s, s, s), gp_Pnt(0, s, s)},  // top
      {gp_Pnt(0, 0, 0), gp_Pnt(s, 0, 0), gp_Pnt(s, 0, s), gp_Pnt(0, 0, s)},  // front
      {gp_Pnt(0, s, 0), gp_Pnt(s, s, 0), gp_Pnt(s, s, s), gp_Pnt(0, s, s)},  // back
      {gp_Pnt(0, 0, 0), gp_Pnt(0, s, 0), gp_Pnt(0, s, s), gp_Pnt(0, 0, s)},  // left
      {gp_Pnt(s, 0, 0), gp_Pnt(s, s, 0), gp_Pnt(s, s, s), gp_Pnt(s, 0, s)},  // right
  };
}

}  // namespace

TEST(ClosingFaces, SixSidesBecomeASolid) {
  Modeler m;
  for (const auto& side : cubeSides()) ASSERT_TRUE(m.createPolygon(side).ok());
  EXPECT_EQ(count(m, ObjectKind::Solid), 1);
  EXPECT_EQ(count(m, ObjectKind::Face), 0);
}

namespace {

std::optional<ObjectId> onlySolid(const Modeler& m) {
  std::optional<ObjectId> out;
  for (const Object& o : m.document().objects()) {
    if (o.kind() == ObjectKind::Solid) {
      if (out) return std::nullopt;
      out = o.id;
    }
  }
  return out;
}

TopoDS_Face faceThrough(const Modeler& m, const ObjectId& id, const gp_Pnt& p) {
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    if (geom::pointOnFace(f, p) && !geom::pointOnFaceBoundary(f, p)) return f;
  }
  return {};
}

}  // namespace

// 1000 mm sides in any order: one valid solid of 1e9 mm3, six faces each
// picked and pushed / pulled on its own, kept through undo, redo, files.
TEST(ClosingFaces, AnyOrderVolumePushPullUndoAndFiles) {
  for (const std::vector<int>& order : {std::vector<int>{0, 1, 2, 3, 4, 5}, std::vector<int>{5, 2, 0, 4, 1, 3},
                                        std::vector<int>{3, 4, 1, 5, 0, 2}}) {
    Modeler m;
    const auto sides = cubeSides();
    const std::size_t before = m.history().undoCount();
    for (int i : order) ASSERT_TRUE(m.createPolygon(sides[i]).ok());
    const auto solid = onlySolid(m);
    ASSERT_TRUE(solid.has_value());
    EXPECT_EQ(m.document().objects().size(), 1u);
    const TopoDS_Shape shape = m.document().worldShape(*m.document().find(*solid));
    EXPECT_NEAR(geom::volume(shape), 1e9, 1e-3);
    EXPECT_TRUE(geom::validateShape(shape, true).valid);
    int faces = 0;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) ++faces;
    EXPECT_EQ(faces, 6);
    EXPECT_EQ(m.history().undoCount(), before + 6);  // one step per face drawn

    // Undo takes back the last face: open again (the five as one open
    // shape, or in pieces where only side by side); redo closes.
    ASSERT_TRUE(m.undo());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_GE(count(m, ObjectKind::Shell), 1);
    ASSERT_TRUE(m.redo());
    ASSERT_TRUE(onlySolid(m).has_value());

    // Saved and reloaded: the same solid.
    const std::string path = ::testing::TempDir() + "closing_faces.sbr";
    ASSERT_TRUE(m.save(path).ok());
    Modeler back;
    std::vector<std::string> warnings;
    ASSERT_TRUE(back.load(path, &warnings).ok());
    ASSERT_TRUE(onlySolid(back).has_value());
    EXPECT_NEAR(geom::volume(back.document().worldShape(*back.document().find(*onlySolid(back)))), 1e9, 1e-3);

    // The top on its own, up by 500.
    const TopoDS_Face top = faceThrough(m, *solid, gp_Pnt(500, 500, 1000));
    ASSERT_FALSE(top.IsNull());
    ASSERT_TRUE(m.pushPull(*solid, top, 500).ok());
    EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*solid))), 1.5e9, 1e-3);
  }
}

// What does not close: five sides, a gap, a fin, a side in another group -
// no solid (the faces meeting at an angle are one open shape meanwhile).
TEST(ClosingFaces, OpenGapsTwiceAndOtherGroupsStayFaces) {
  {
    Modeler m;
    const auto sides = cubeSides();
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(m.createPolygon(sides[i]).ok());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_EQ(count(m, ObjectKind::Shell), 1);
    EXPECT_EQ(count(m, ObjectKind::Face), 0);
  }
  {
    Modeler m;  // the right side 0.5 mm off: a gap, kept
    auto sides = cubeSides();
    for (gp_Pnt& p : sides[5]) p.SetX(p.X() + 0.5);
    for (const auto& side : sides) ASSERT_TRUE(m.createPolygon(side).ok());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_EQ(count(m, ObjectKind::Shell), 1);  // the five that meet
    EXPECT_EQ(count(m, ObjectKind::Face), 1);   // the one 0.5 mm off
  }
  {
    Modeler m;  // a fin on the top edge: three faces on one edge
    const auto sides = cubeSides();
    ASSERT_TRUE(m.createPolygon({gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000), gp_Pnt(1000, -800, 1600),
                                 gp_Pnt(0, -800, 1600)})
                    .ok());
    for (const auto& side : sides) ASSERT_TRUE(m.createPolygon(side).ok());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
  }
  {
    Modeler m;  // the last side in a group of its own
    const auto sides = cubeSides();
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(m.createPolygon(sides[i]).ok());
    auto lone = m.createPolygon({gp_Pnt(5000, 0, 0), gp_Pnt(6000, 0, 0), gp_Pnt(6000, 1000, 0)});
    auto group = m.group({*lone});
    ASSERT_TRUE(group.ok());
    ASSERT_TRUE(m.setActiveGroup(*group).ok());
    ASSERT_TRUE(m.createPolygon(sides[5]).ok());
    ASSERT_TRUE(m.setActiveGroup({}).ok());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_EQ(count(m, ObjectKind::Shell), 1);  // the five outside
    EXPECT_EQ(count(m, ObjectKind::Face), 2);   // in the group: apart
  }
}

// A face taken out: open again (the rest kept); a new face there closes it.
// Painted faces keep their material.
TEST(ClosingFaces, DeleteAFaceThenCloseAgainKeepsMaterials) {
  Modeler m;
  auto red = m.createMaterial("Red", Color{1, 0, 0});
  ASSERT_TRUE(red.ok());
  const auto sides = cubeSides();
  // The front first, painted while it is a face of its own.
  auto front = m.createPolygon(sides[2]);
  ASSERT_TRUE(front.ok());
  ASSERT_TRUE(m.assignMaterial({*front}, *red).ok());
  for (int i : {0, 1, 3, 4, 5}) ASSERT_TRUE(m.createPolygon(sides[i]).ok());
  const auto solid = onlySolid(m);
  ASSERT_TRUE(solid.has_value());
  auto materialAt = [&](const ObjectId& id, const gp_Pnt& p) {
    const Object& o = *m.document().find(id);
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(o.shape, TopAbs_FACE, faces);
    for (const auto& [i, mat] : resolveFaceMaterials(o).faces) {
      const TopoDS_Face f = TopoDS::Face(faces(i).Moved(TopLoc_Location(m.document().worldTransform(o))));
      if (geom::pointOnFace(f, p)) return mat;
    }
    return o.material;
  };
  EXPECT_EQ(materialAt(*solid, gp_Pnt(500, 0, 500)), *red);
  EXPECT_NE(materialAt(*solid, gp_Pnt(500, 1000, 500)), *red);

  // The top deleted: an open shape with the other five faces.
  const TopoDS_Face top = faceThrough(m, *solid, gp_Pnt(500, 500, 1000));
  ASSERT_TRUE(m.removeFaces(*solid, {top}).ok());
  EXPECT_EQ(m.document().find(*solid)->kind(), ObjectKind::Shell);
  // Closed again by drawing the top.
  ASSERT_TRUE(m.createPolygon(sides[1]).ok());
  const auto again = onlySolid(m);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*again))), 1e9, 1e-3);
  EXPECT_EQ(materialAt(*again, gp_Pnt(500, 0, 500)), *red);
}

namespace {

int edgesOf(const TopoDS_Shape& s) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  return edges.Extent();
}

}  // namespace

// Stage 1 of sharing edges while still open (SketchUp): faces meeting at an
// angle are one open shape at once, the edge between them one edge; faces
// side by side on one plane stay apart for now.
TEST(ClosingFaces, AngledFacesShareTheirEdgeWhileOpen) {
  Modeler m;
  const auto sides = cubeSides();
  ASSERT_TRUE(m.createPolygon(sides[0]).ok());  // bottom
  ASSERT_TRUE(m.createPolygon(sides[2]).ok());  // front, at right angles
  ASSERT_EQ(m.document().objects().size(), 1u);
  const Object& surface = m.document().objects().front();
  EXPECT_EQ(surface.kind(), ObjectKind::Shell);
  EXPECT_EQ(edgesOf(surface.shape), 7);  // 4 + 4 - the shared one
  // Two more sides: still one open shape; Undo takes back one face at a time.
  ASSERT_TRUE(m.createPolygon(sides[4]).ok());
  ASSERT_TRUE(m.createPolygon(sides[3]).ok());
  ASSERT_EQ(m.document().objects().size(), 1u);
  EXPECT_EQ(edgesOf(m.document().objects().front().shape), 4 * 4 - 5);  // 5 shared
  ASSERT_TRUE(m.undo());
  ASSERT_EQ(m.document().objects().size(), 1u);  // the three before, the fourth gone
  EXPECT_EQ(edgesOf(m.document().objects().front().shape), 4 * 3 - 3);
  ASSERT_TRUE(m.redo());

  // The shared top corner of front and left moved up: both faces follow.
  const ObjectId id = m.document().objects().front().id;
  ASSERT_TRUE(m.moveVertices(id, {gp_Pnt(0, 0, 1000)}, gp_Vec(0, 0, 200)).ok());
  bool front = false, left = false;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    if (!geom::pointOnFace(f, gp_Pnt(0, 0, 1200))) continue;
    const Bnd_Box b = geom::exactBounds(f);
    front = front || std::abs(b.CornerMax().Y() - b.CornerMin().Y()) < 1e-6;
    left = left || std::abs(b.CornerMax().X() - b.CornerMin().X()) < 1e-6;
  }
  EXPECT_TRUE(front && left);

  // Side by side on one plane: two faces of their own (stage 2).
  Modeler flat;
  ASSERT_TRUE(flat.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0), gp_Pnt(0, 1000, 0)}).ok());
  ASSERT_TRUE(flat.createPolygon({gp_Pnt(1000, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 1000, 0), gp_Pnt(1000, 1000, 0)}).ok());
  EXPECT_EQ(count(flat, ObjectKind::Face), 2);
  EXPECT_EQ(count(flat, ObjectKind::Shell), 0);
}

// An open box drawn face by face, then a side pulled out: the surface
// grows like a SketchUp one; the last face closes it into a solid of the
// right volume.
TEST(ClosingFaces, OpenBoxPulledThenClosed) {
  Modeler m;
  const auto sides = cubeSides();
  for (int i : {0, 2, 3, 4, 5}) ASSERT_TRUE(m.createPolygon(sides[i]).ok());  // no top
  ASSERT_EQ(m.document().objects().size(), 1u);
  const ObjectId id = m.document().objects().front().id;
  TopoDS_Face right;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) {
    const Bnd_Box b = geom::exactBounds(e.Current());
    if (std::abs(b.CornerMin().X() - 1000) < 1e-6 && std::abs(b.CornerMax().X() - 1000) < 1e-6) {
      right = TopoDS::Face(e.Current());
    }
  }
  ASSERT_FALSE(right.IsNull());
  // Its normal may face in or out (drawn faces): pulled out to x = 1500 either way.
  const auto n = geom::planarFaceNormal(right);
  ASSERT_TRUE(n.has_value());
  ASSERT_TRUE(m.pushPull(id, right, n->X() > 0 ? 500 : -500).ok());
  EXPECT_EQ(m.document().find(id)->kind(), ObjectKind::Shell);
  EXPECT_NEAR(geom::exactBounds(m.document().worldShape(*m.document().find(id))).CornerMax().X(), 1500, 1e-6);
  // The rest of the top (the pull made the strip over x 1000..1500).
  ASSERT_TRUE(m.createPolygon(sides[1]).ok());
  const auto solid = onlySolid(m);
  ASSERT_TRUE(solid.has_value());
  EXPECT_EQ(m.document().objects().size(), 1u);
  EXPECT_NEAR(geom::volume(m.document().worldShape(*m.document().find(*solid))), 1.5e9, 1e-3);
}

// Cost of drawing face by face (SBR_BENCH_N=20: a 20 x 20 floor of tiles,
// then 20 boxes drawn side by side face by face). Skipped otherwise.
TEST(ClosingFaces, Bench) {
  const int n = qEnvInt("SBR_BENCH_N");
  if (n <= 0) GTEST_SKIP() << "set SBR_BENCH_N to run";
  Modeler m;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      const double x = i * 1000, y = j * 1000;
      ASSERT_TRUE(m.createPolygon({gp_Pnt(x, y, 0), gp_Pnt(x + 1000, y, 0), gp_Pnt(x + 1000, y + 1000, 0),
                                   gp_Pnt(x, y + 1000, 0)})
                      .ok());
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  for (int b = 0; b < n; ++b) {
    for (auto side : cubeSides()) {
      for (gp_Pnt& p : side) p.SetY(p.Y() - 3000.0 - 1500.0 * b);
      ASSERT_TRUE(m.createPolygon(side).ok());
    }
  }
  auto t2 = std::chrono::steady_clock::now();
  using ms = std::chrono::milliseconds;
  std::printf("floor %d tiles: %lld ms (%.2f ms each); %d boxes face by face: %lld ms (%.2f ms per face)\n", n * n,
              (long long)std::chrono::duration_cast<ms>(t1 - t0).count(),
              double(std::chrono::duration_cast<ms>(t1 - t0).count()) / (n * n), n,
              (long long)std::chrono::duration_cast<ms>(t2 - t1).count(),
              double(std::chrono::duration_cast<ms>(t2 - t1).count()) / (n * 6));
  EXPECT_EQ(count(m, ObjectKind::Solid), n);
}

// PERF-15: boxes closed face by face (each last face closes a solid), and
// one open shape growing a face at a time (each new face joins the whole
// shape again). Skipped unless SBR_BENCH_N is set; prints timings.
TEST(ClosingFaces, BenchJoiningOneByOne) {
  const int n = qEnvInt("SBR_BENCH_N");
  if (n <= 0) GTEST_SKIP() << "set SBR_BENCH_N to run";
  Modeler m;
  auto quad = [&](gp_Pnt a, gp_Pnt b, gp_Pnt c, gp_Pnt d) { ASSERT_TRUE(m.createPolygon({a, b, c, d}).ok()); };
  auto t0 = std::chrono::steady_clock::now();
  const int boxes = std::max(1, n / 10);
  for (int k = 0; k < boxes; ++k) {
    const double x = k * 3000.0, s = 1000.0;
    quad(gp_Pnt(x, 0, 0), gp_Pnt(x + s, 0, 0), gp_Pnt(x + s, s, 0), gp_Pnt(x, s, 0));
    quad(gp_Pnt(x, 0, s), gp_Pnt(x + s, 0, s), gp_Pnt(x + s, s, s), gp_Pnt(x, s, s));
    quad(gp_Pnt(x, 0, 0), gp_Pnt(x + s, 0, 0), gp_Pnt(x + s, 0, s), gp_Pnt(x, 0, s));
    quad(gp_Pnt(x, s, 0), gp_Pnt(x + s, s, 0), gp_Pnt(x + s, s, s), gp_Pnt(x, s, s));
    quad(gp_Pnt(x, 0, 0), gp_Pnt(x, s, 0), gp_Pnt(x, s, s), gp_Pnt(x, 0, s));
    quad(gp_Pnt(x + s, 0, 0), gp_Pnt(x + s, s, 0), gp_Pnt(x + s, s, s), gp_Pnt(x + s, 0, s));
  }
  const double closing = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int solids = 0;
  for (const auto& o : m.document().objects()) solids += o.kind() == ObjectKind::Solid;
  EXPECT_EQ(solids, boxes);
  // A zigzag wall: n upright faces, each sharing an edge with the last.
  std::vector<double> perFace;
  for (int k = 0; k < n; ++k) {
    const double x0 = k * 1000.0, y0 = (k % 2) * 1000.0, x1 = (k + 1) * 1000.0, y1 = ((k + 1) % 2) * 1000.0;
    const auto t = std::chrono::steady_clock::now();
    quad(gp_Pnt(x0, y0 + 50000, 0), gp_Pnt(x1, y1 + 50000, 0), gp_Pnt(x1, y1 + 50000, 3000), gp_Pnt(x0, y0 + 50000, 3000));
    perFace.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count() * 1000);
  }
  double total = 0;
  for (double ms : perFace) total += ms;
  std::printf("bench joining n=%d boxes=%d closing_s=%.2f per_box_ms=%.1f zigzag_total_s=%.2f first_ms=%.1f last_ms=%.1f\n", n,
              boxes, closing, closing * 1000 / boxes, total / 1000, perFace.front(), perFace.back());
}
