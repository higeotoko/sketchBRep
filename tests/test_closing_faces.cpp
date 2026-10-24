// Faces drawn one by one round a volume (report: six rectangles drawn edge
// to edge round a cube stayed six loose faces): once the last one closes it
// without gaps, they are one solid, like a box pushed up from a rectangle.

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

    // Undo takes back the last face: open again, five faces; redo closes.
    ASSERT_TRUE(m.undo());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_EQ(count(m, ObjectKind::Face), 5);
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

// What does not close: five sides, a gap, a side twice, a side in another
// group - the faces stay faces.
TEST(ClosingFaces, OpenGapsTwiceAndOtherGroupsStayFaces) {
  {
    Modeler m;
    const auto sides = cubeSides();
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(m.createPolygon(sides[i]).ok());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_EQ(count(m, ObjectKind::Face), 5);
  }
  {
    Modeler m;  // the right side 0.5 mm off: a gap, kept
    auto sides = cubeSides();
    for (gp_Pnt& p : sides[5]) p.SetX(p.X() + 0.5);
    for (const auto& side : sides) ASSERT_TRUE(m.createPolygon(side).ok());
    EXPECT_EQ(count(m, ObjectKind::Solid), 0);
    EXPECT_EQ(count(m, ObjectKind::Face), 6);
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
    EXPECT_EQ(count(m, ObjectKind::Face), 7);
  }
}

// A face taken out: open again (the rest kept); a new face there closes it.
// Painted faces keep their material.
TEST(ClosingFaces, DeleteAFaceThenCloseAgainKeepsMaterials) {
  Modeler m;
  auto red = m.createMaterial("Red", Color{1, 0, 0});
  ASSERT_TRUE(red.ok());
  const auto sides = cubeSides();
  std::vector<ObjectId> ids;
  for (const auto& side : sides) {
    if (ids.size() == 2) {  // the front, painted before the box closes
      auto front = m.createPolygon(side);
      ASSERT_TRUE(front.ok());
      ASSERT_TRUE(m.assignMaterial({*front}, *red).ok());
      ids.push_back(*front);
      continue;
    }
    auto id = m.createPolygon(side);
    ASSERT_TRUE(id.ok());
    ids.push_back(*id);
  }
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
