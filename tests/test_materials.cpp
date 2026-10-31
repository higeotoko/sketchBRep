// Materials (docs/11_materials.md): library with stable ids, object and face
// assignments with inheritance, undo / redo, save and reload, colours from
// older files, and what happens to painted faces when the shape changes.

#include <BRepClass_FaceClassifier.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <nlohmann/json.hpp>

#include "TestHelpers.h"
#include "sbr/Codec.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/geom/Operations.h"

using namespace sbr;
using namespace sbr::doc;

namespace {

ObjectId box(Modeler& m, double x, double y, double w, double d, double h) {
  geom::SketchPlane plane = geom::SketchPlane::ground();
  plane.origin = gp_Pnt(x, y, 0);
  auto id = m.createRectangle(plane, w, d);
  EXPECT_TRUE(m.extrude(*id, h).ok());
  return *id;
}

// The face of the object's world shape through `p` facing `normal`.
TopoDS_Face faceAt(const Modeler& m, const ObjectId& id, const gp_Dir& normal, const gp_Pnt& p) {
  for (TopExp_Explorer e(m.document().worldShape(id), TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face f = TopoDS::Face(e.Current());
    const auto n = geom::planarFaceNormal(f);
    if (!n || n->Angle(normal) > 1e-6) continue;
    BRepClass_FaceClassifier c(f, p, 1e-6);
    if (c.State() == TopAbs_IN) return f;
  }
  return {};
}

std::string shown(const Modeler& m, const ObjectId& id, const gp_Dir& normal, const gp_Pnt& p) {
  const TopoDS_Face f = faceAt(m, id, normal, p);
  if (f.IsNull()) return "<no face>";
  auto at = m.materialAt(id, f);
  return at ? at->material : "<" + at.error() + ">";
}

}  // namespace

TEST(Materials, FaceBeatsObjectBeatsContainerAndInheritRestores) {
  Modeler m;
  const std::string wood = *m.createMaterial("木", {0.7, 0.5, 0.3});
  const std::string plaster = *m.createMaterial("漆喰", {0.95, 0.95, 0.9});
  const std::string tile = *m.createMaterial("タイル", {0.3, 0.4, 0.6});
  const ObjectId a = box(m, 0, 0, 1000, 1000, 1000);
  const ObjectId b = box(m, 2000, 0, 1000, 1000, 1000);
  auto g = m.group({a, b});
  ASSERT_TRUE(g);
  const gp_Pnt top(500, 500, 1000);

  EXPECT_EQ(shown(m, a, gp::DZ(), top), "");  // nothing yet
  ASSERT_TRUE(m.assignMaterial({*g}, plaster).ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), plaster);  // from the group
  EXPECT_EQ(m.materialAt(a, faceAt(m, a, gp::DZ(), top))->source, "container");
  ASSERT_TRUE(m.assignMaterial({a}, wood).ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), wood);
  EXPECT_EQ(shown(m, b, gp::DZ(), gp_Pnt(2500, 500, 1000)), plaster);
  ASSERT_TRUE(m.paintFaces(a, {faceAt(m, a, gp::DZ(), top)}, tile).ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), tile);
  EXPECT_EQ(m.materialAt(a, faceAt(m, a, gp::DZ(), top))->source, "face");
  EXPECT_EQ(shown(m, a, -gp::DY(), gp_Pnt(500, 0, 500)), wood);  // other faces: the object's
  EXPECT_NEAR(m.document().displayColor(*m.document().find(a)).r, 0.7, 1e-12);

  // Inherit again: face -> object -> group.
  ASSERT_TRUE(m.paintFaces(a, {faceAt(m, a, gp::DZ(), top)}, "").ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), wood);
  ASSERT_TRUE(m.assignMaterial({a}, "").ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), plaster);

  // Every step undoes and redoes exactly.
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), wood);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), tile);
  ASSERT_TRUE(m.redo());
  ASSERT_TRUE(m.redo());
  EXPECT_EQ(shown(m, a, gp::DZ(), top), plaster);

  // Unknown materials are refused.
  EXPECT_FALSE(m.assignMaterial({a}, "mat-nope").ok());
  EXPECT_FALSE(m.paintFaces(a, {faceAt(m, a, gp::DZ(), top)}, "mat-nope").ok());
}

TEST(Materials, LibraryEditsUndoAndDeleteFallsBack) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 1000, 1000, 1000);
  const std::string wood = *m.createMaterial("木", {0.7, 0.5, 0.3});
  ASSERT_TRUE(m.assignMaterial({a}, wood).ok());
  ASSERT_TRUE(m.paintFaces(a, {faceAt(m, a, gp::DZ(), gp_Pnt(500, 500, 1000))}, wood).ok());
  ASSERT_TRUE(m.updateMaterial(wood, std::string("オーク"), Color{0.8, 0.6, 0.4}).ok());
  EXPECT_EQ(m.document().findMaterial(wood)->name, "オーク");
  EXPECT_NEAR(m.document().displayColor(*m.document().find(a)).r, 0.8, 1e-12);
  ASSERT_TRUE(m.undo());
  EXPECT_EQ(m.document().findMaterial(wood)->name, "木");

  ASSERT_TRUE(m.deleteMaterial(wood).ok());
  EXPECT_FALSE(m.document().findMaterial(wood));
  EXPECT_TRUE(m.document().find(a)->material.empty());
  EXPECT_TRUE(m.document().find(a)->faceMaterials.empty());
  ASSERT_TRUE(m.undo());  // library entry and its users come back together
  EXPECT_TRUE(m.document().findMaterial(wood));
  EXPECT_EQ(m.document().find(a)->material, wood);
  EXPECT_EQ(m.document().find(a)->faceMaterials.size(), 1u);
  // Ids are stable and unique; names may repeat.
  const std::string other = *m.createMaterial("木", {0.1, 0.1, 0.1});
  EXPECT_NE(other, wood);
}

TEST(Materials, SaveReloadAndColoursFromOlderFiles) {
  Modeler m;
  const ObjectId a = box(m, 0, 0, 1000, 1000, 1000);
  const ObjectId b = box(m, 2000, 0, 1000, 1000, 1000);
  const std::string wood = *m.createMaterial("木", {0.7, 0.5, 0.3});
  ASSERT_TRUE(m.assignMaterial({a}, wood).ok());
  ASSERT_TRUE(m.paintFaces(b, {faceAt(m, b, gp::DZ(), gp_Pnt(2500, 500, 1000))}, wood).ok());
  const std::string path = test::tempPath("materials.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler r;
  ASSERT_TRUE(r.load(path).ok());
  ASSERT_TRUE(r.document().findMaterial(wood));
  EXPECT_EQ(r.document().findMaterial(wood)->name, "木");
  EXPECT_EQ(shown(r, a, -gp::DY(), gp_Pnt(500, 0, 500)), wood);
  EXPECT_EQ(shown(r, b, gp::DZ(), gp_Pnt(2500, 500, 1000)), wood);
  EXPECT_EQ(shown(r, b, -gp::DY(), gp_Pnt(2500, 0, 500)), "");
  // The painted face still follows edits after reloading.
  ASSERT_TRUE(r.setParameter(b, "op0.distance", 2000).ok());
  EXPECT_EQ(shown(r, b, gp::DZ(), gp_Pnt(2500, 500, 2000)), wood);

  // A file from before materials: plain colours. Each becomes the material
  // made from it, shown the same.
  nlohmann::json old = nlohmann::json::parse(serializeDocument(m.document()));
  old.erase("materials");
  for (auto& o : old["objects"]) {
    o.erase("material");
    o.erase("faceMaterials");
  }
  old["objects"][0]["color"] = {0.9, 0.1, 0.1};
  old["objects"][1]["color"] = {0.9, 0.1, 0.1};
  Document legacy;
  ASSERT_TRUE(deserializeDocument(old.dump(), legacy).ok());
  ASSERT_EQ(legacy.materials().size(), 1u);  // same colour, one material
  const Material& red = legacy.materials().front();
  EXPECT_EQ(red.id, colorMaterialId({0.9, 0.1, 0.1}));
  EXPECT_EQ(legacy.objects()[0].material, red.id);
  EXPECT_EQ(legacy.displayColor(legacy.objects()[1]), (Color{0.9, 0.1, 0.1}));
  // Older versions still see colours in new files.
  const nlohmann::json written = nlohmann::json::parse(serializeDocument(m.document()));
  EXPECT_NEAR(written["objects"][0]["color"][0].get<double>(), 0.7, 1e-12);
}

TEST(Materials, PaintedFacesFollowShapeEdits) {
  Modeler m;
  const std::string red = *m.createMaterial("赤", {0.9, 0.1, 0.1});
  const std::string blue = *m.createMaterial("青", {0.1, 0.2, 0.9});
  const ObjectId a = box(m, 0, 0, 1000, 1000, 1000);
  ASSERT_TRUE(m.paintFaces(a, {faceAt(m, a, gp::DZ(), gp_Pnt(500, 500, 1000))}, red).ok());
  ASSERT_TRUE(m.paintFaces(a, {faceAt(m, a, gp::DX(), gp_Pnt(1000, 500, 500))}, blue).ok());

  // Push/pull the painted top: it keeps its paint at the new height.
  ASSERT_TRUE(m.pushPull(a, faceAt(m, a, gp::DZ(), gp_Pnt(500, 500, 1000)), 500).ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), gp_Pnt(500, 500, 1500)), red);
  // A parameter moves the +X face: still blue; the sides stay unpainted.
  ASSERT_TRUE(m.setParameter(a, "width", 3000).ok());
  EXPECT_EQ(shown(m, a, gp::DX(), gp_Pnt(3000, 500, 500)), blue);
  EXPECT_EQ(shown(m, a, gp::DZ(), gp_Pnt(2500, 500, 1500)), red);
  EXPECT_EQ(shown(m, a, -gp::DY(), gp_Pnt(1500, 0, 500)), "");
  // Splitting the painted top: both parts keep it; pushing one moves its paint along.
  ASSERT_TRUE(m.splitWithLine({gp_Pnt(1500, 0, 1500), gp_Pnt(1500, 1000, 1500)}).ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), gp_Pnt(500, 500, 1500)), red);
  EXPECT_EQ(shown(m, a, gp::DZ(), gp_Pnt(2500, 500, 1500)), red);
  ASSERT_TRUE(m.pushPull(a, faceAt(m, a, gp::DZ(), gp_Pnt(2500, 500, 1500)), 300).ok());
  EXPECT_EQ(shown(m, a, gp::DZ(), gp_Pnt(2500, 500, 1800)), red);
  EXPECT_EQ(shown(m, a, gp::DZ(), gp_Pnt(500, 500, 1500)), red);
  EXPECT_EQ(resolveFaceMaterials(*m.document().find(a)).unresolved, 0u);
  // Scaling keeps them too; copies carry them.
  ASSERT_TRUE(m.scale({a}, gp::Origin(), 2.0).ok());
  EXPECT_EQ(shown(m, a, gp::DX(), gp_Pnt(6000, 1000, 1000)), blue);
  gp_Trsf shift;
  shift.SetTranslation(gp_Vec(0, 5000, 0));
  auto copies = m.copy({a}, shift);
  ASSERT_TRUE(copies);
  EXPECT_EQ(shown(m, copies->front(), gp::DX(), gp_Pnt(6000, 6000, 1000)), blue);
}

// When the face cannot be told apart any more, the paint goes nowhere
// rather than onto another face.
TEST(Materials, UnidentifiableFaceIsNotRepainted) {
  const TopoDS_Shape one = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 1000, 1000, 1000).Shape();
  TopoDS_Face plusX;
  for (TopExp_Explorer e(one, TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->IsEqual(gp::DX(), 1e-9)) plusX = TopoDS::Face(e.Current());
  }
  const auto fm = FaceMaterial::of(plusX, "x");
  ASSERT_TRUE(fm && fm->plane);
  // Same face, moved: found.
  const TopoDS_Shape wider = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 2000, 1000, 1000).Shape();
  EXPECT_EQ(fm->facesIn(wider).size(), 1u);
  // Now two faces facing +X (a step), both where the painted one was: it
  // could be either, so neither gets it.
  TopoDS_Compound two;
  BRep_Builder b;
  b.MakeCompound(two);
  b.Add(two, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 1500, 1000, 500).Shape());
  b.Add(two, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 500), 1800, 1000, 500).Shape());
  EXPECT_TRUE(fm->facesIn(two).empty());
}

TEST(Materials, BooleansKeepEachInputsLook) {
  Modeler m;
  const std::string red = *m.createMaterial("赤", {0.9, 0.1, 0.1});
  const std::string blue = *m.createMaterial("青", {0.1, 0.2, 0.9});
  const ObjectId a = box(m, 0, 0, 1000, 1000, 1000);
  const ObjectId b = box(m, 500, 0, 1000, 1000, 1500);
  ASSERT_TRUE(m.assignMaterial({a}, red).ok());
  ASSERT_TRUE(m.assignMaterial({b}, blue).ok());
  auto u = m.boolean(BooleanKind::Union, {a, b});
  ASSERT_TRUE(u) << u.error();
  EXPECT_EQ(m.document().find(*u)->material, red);
  EXPECT_EQ(shown(m, *u, -gp::DX(), gp_Pnt(0, 500, 500)), red);
  EXPECT_EQ(shown(m, *u, gp::DX(), gp_Pnt(1500, 500, 500)), blue);
  EXPECT_EQ(shown(m, *u, gp::DZ(), gp_Pnt(1200, 500, 1500)), blue);

  // A window cut into a painted wall: the wall keeps its paint (also on the
  // face around the hole); the reveal inherits the wall's material.
  Modeler w;
  const std::string plaster = *w.createMaterial("漆喰", {0.95, 0.95, 0.9});
  const std::string paint = *w.createMaterial("塗装", {0.2, 0.5, 0.3});
  const ObjectId wall = box(w, 0, 0, 4000, 200, 3000);
  ASSERT_TRUE(w.assignMaterial({wall}, plaster).ok());
  ASSERT_TRUE(w.paintFaces(wall, {faceAt(w, wall, -gp::DY(), gp_Pnt(2000, 0, 1500))}, paint).ok());
  auto host = w.placeOpenings(wall, -gp::DY(), 1, 900, 1200, 900);
  ASSERT_TRUE(host) << host.error();
  EXPECT_EQ(shown(w, *host, -gp::DY(), gp_Pnt(500, 0, 500)), paint);
  EXPECT_EQ(shown(w, *host, gp::DY(), gp_Pnt(500, 200, 500)), plaster);
  EXPECT_EQ(shown(w, *host, gp::DZ(), gp_Pnt(2000, 100, 900)), plaster);  // sill reveal
}

// Image underlays (docs/13): embedded by content hash, calibrated by scale,
// locked against accidental edits, carried by the file.
TEST(Underlay, CodecBasics) {
  EXPECT_EQ(sbr::codec::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sbr::codec::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string bytes("\x89PNG\r\n\x1a\n\0\x01\xff", 11);
  EXPECT_EQ(sbr::codec::base64Decode(sbr::codec::base64Encode(bytes)), bytes);
  EXPECT_EQ(sbr::codec::base64Encode("Man"), "TWFu");
}

TEST(Underlay, PlaceCalibrateLockSaveAndReload) {
  Modeler m;
  const std::string bytes = "pretend-png-bytes";
  auto id = m.addImageUnderlay(bytes, "plan.png", 2000, 1000, geom::SketchPlane::ground(), 10000);
  ASSERT_TRUE(id) << id.error();
  const Object* o = m.document().find(*id);
  EXPECT_EQ(o->kind(), ObjectKind::Image);
  const auto& u = std::get<ImageUnderlay>(o->recipe.base);
  EXPECT_NEAR(u.height, 5000, 1e-9);
  ASSERT_TRUE(m.document().findImage(u.image));
  EXPECT_EQ(m.document().findImage(u.image)->bytes, bytes);
  // Not a profile: push/pull and painting leave it alone.
  EXPECT_FALSE(m.extrude(*id, 100).ok());

  // Calibrate: the distance between two marks drawn 4 m apart on the plan
  // measures 2 m on screen, so scale x2 about the first mark.
  ASSERT_TRUE(m.scale({*id}, gp_Pnt(1000, 1000, 0), 2.0).ok());
  const auto& scaled = std::get<ImageUnderlay>(m.document().find(*id)->recipe.base);
  EXPECT_NEAR(scaled.width, 20000, 1e-9);
  EXPECT_NEAR(scaled.height, 10000, 1e-9);
  EXPECT_NEAR(scaled.plane.origin.X(), -1000, 1e-9);  // the mark stays put
  // The width parameter keeps the proportions.
  ASSERT_TRUE(m.setParameter(*id, "width", 30000).ok());
  EXPECT_NEAR(std::get<ImageUnderlay>(m.document().find(*id)->recipe.base).height, 15000, 1e-9);

  // Locked: no moving, scaling or erasing (also from the API).
  ASSERT_TRUE(m.setLocked({*id}, true).ok());
  gp_Trsf t;
  t.SetTranslation(gp_Vec(100, 0, 0));
  EXPECT_FALSE(m.transform({*id}, t, "Move").ok());
  EXPECT_FALSE(m.scale({*id}, gp::Origin(), 2.0).ok());
  EXPECT_FALSE(m.remove({*id}).ok());

  // Saved with the image; reloaded intact; an image nothing shows is dropped.
  const std::string path = test::tempPath("underlay.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler r;
  ASSERT_TRUE(r.load(path).ok());
  const Object* back = r.document().find(*id);
  ASSERT_TRUE(back);
  EXPECT_TRUE(back->locked);
  EXPECT_EQ(r.document().findImage(std::get<ImageUnderlay>(back->recipe.base).image)->bytes, bytes);
  ASSERT_TRUE(r.setLocked({*id}, false).ok());
  ASSERT_TRUE(r.remove({*id}).ok());
  const nlohmann::json saved = nlohmann::json::parse(serializeDocument(r.document()));
  EXPECT_TRUE(saved["images"].empty());

  // A damaged embedded image is reported, not shown as something else.
  nlohmann::json broken = nlohmann::json::parse(serializeDocument(m.document()));
  for (auto it = broken["images"].begin(); it != broken["images"].end(); ++it) {
    it.value()["data"] = sbr::codec::base64Encode("other bytes");
  }
  Document d;
  std::vector<std::string> warnings;
  ASSERT_TRUE(deserializeDocument(broken.dump(), d, &warnings).ok());
  EXPECT_TRUE(d.images().empty());
  EXPECT_FALSE(warnings.empty());
}

// Stage 2 (docs/11_materials.md): editing keeps the id and name, slider
// drags are one undo step, copies are separate, textures / opacity /
// rendering values are saved.
TEST(Materials, EditDuplicateTextureAndSave) {
  Modeler m;
  auto plaster = m.createMaterial("Plaster", {0.9, 0.88, 0.85});
  ASSERT_TRUE(plaster.ok());
  const ObjectId b = box(m, 0, 0, 1000, 1000, 1000);
  ASSERT_TRUE(m.assignMaterial({b}, *plaster).ok());

  // A colour dragged in three steps: one undo step, same id and name.
  const std::size_t steps = m.history().undoCount();
  Material edit = *m.document().findMaterial(*plaster);
  for (double r : {0.8, 0.7, 0.6}) {
    edit.color.r = r;
    ASSERT_TRUE(m.editMaterial(edit, true).ok());
  }
  EXPECT_EQ(m.history().undoCount(), steps + 1);
  EXPECT_EQ(m.document().findMaterial(*plaster)->name, "Plaster");
  EXPECT_DOUBLE_EQ(m.document().displayColor(*m.document().find(b)).r, 0.6);
  ASSERT_TRUE(m.undo());
  EXPECT_DOUBLE_EQ(m.document().findMaterial(*plaster)->color.r, 0.9);

  // Opacity, a texture at real size, rendering values.
  const std::string png = "\\x89PNG fake bytes for the content hash";
  const std::string hash = m.addTextureImage(png, "plaster.png");
  edit = *m.document().findMaterial(*plaster);
  edit.opacity = 0.4;
  edit.texture = MaterialTexture{hash, 600, 300, true};
  edit.metallic = 0.1;
  edit.roughness = 0.8;
  edit.reflectance = 0.3;
  ASSERT_TRUE(m.editMaterial(edit).ok());
  EXPECT_DOUBLE_EQ(m.document().displayOpacity(*m.document().find(b)), 0.4);
  Material bad = edit;
  bad.texture->image = "no-such-picture";
  EXPECT_FALSE(m.editMaterial(bad).ok());
  bad = edit;
  bad.opacity = 1.5;
  EXPECT_FALSE(m.editMaterial(bad).ok());

  // A copy to change separately.
  auto copy = m.duplicateMaterial(*plaster);
  ASSERT_TRUE(copy.ok());
  EXPECT_NE(*copy, *plaster);
  EXPECT_EQ(m.document().findMaterial(*copy)->name, "Plaster 2");
  EXPECT_EQ(m.document().findMaterial(*copy)->texture, edit.texture);
  Material other = *m.document().findMaterial(*copy);
  other.color = {0.2, 0.2, 0.2};
  ASSERT_TRUE(m.editMaterial(other).ok());
  EXPECT_DOUBLE_EQ(m.document().findMaterial(*plaster)->color.g, 0.88);

  const std::string path = test::tempPath("materials_stage2.sbr");
  ASSERT_TRUE(m.save(path).ok());
  Modeler loaded;
  std::vector<std::string> warnings;
  ASSERT_TRUE(loaded.load(path, &warnings).ok());
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  const Material* back = loaded.document().findMaterial(*plaster);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, *m.document().findMaterial(*plaster));
  ASSERT_TRUE(loaded.document().findImage(hash));
  EXPECT_EQ(loaded.document().findImage(hash)->bytes, png);
}
