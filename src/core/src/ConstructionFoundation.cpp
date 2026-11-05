#include "sbr/ConstructionFoundation.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/Codec.h"
#include "sbr/KernelError.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include <TopExp_Explorer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <TopTools_ListOfShape.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace {
using namespace sbr;
using namespace sbr::doc;
using json = nlohmann::json;
constexpr const char* kManifest = "foundation_plan";
void need(bool yes, const std::string& why) { if (!yes) throw std::runtime_error(why); }
void allowed(const json& j, const std::set<std::string>& keys) {
  need(j.is_object(), "objectが必要です");
  for (const auto& [key, unused] : j.items()) need(keys.count(key), "不明な項目: " + key);
}
double dimension(const json& j, const char* key, double fallback, bool zero = false) {
  if (!j.contains(key)) return fallback;
  need(j[key].is_number(), std::string(key) + "は数値です");
  const double value = j[key].get<double>();
  need(std::isfinite(value) && value <= 1e6 && (zero ? value >= 0 : value > 0.01), std::string(key) + "が範囲外です");
  return value;
}
json point(const gp_Pnt& p) {
  return {std::round(p.X()*1e6)/1e6, std::round(p.Y()*1e6)/1e6, std::round(p.Z()*1e6)/1e6};
}
std::string attr(const Object& o, const Document& d, const std::string& key) {
  if (const auto it = o.attributes.find(key); it != o.attributes.end()) return it->second;
  const auto* instance = std::get_if<ComponentInstance>(&o.recipe.base);
  const auto* def = instance ? d.findDefinition(instance->definitionId) : nullptr;
  if (def && def->objects.size() == 1) {
    const auto it = def->objects[0].attributes.find(key);
    if (it != def->objects[0].attributes.end()) return it->second;
  }
  return {};
}
struct Sill { ObjectId id; gp_Pnt a, b; double bottom; };
struct PadSite { ObjectId id; gp_Pnt center; double bottom; };
// An opening through an interior stem of a mat foundation (人通口).
struct Passage { double x=0, y=0, width=600, height=330; };
struct Sources {
  json inputs, rows = json::array();
  std::vector<Sill> lines;
  std::vector<PadSite> sites;
  std::vector<Passage> passages;
  std::string type="strip";
  bool pads=false, mat=false;
  double padWidth=0,padDepth=0,padHeight=0;
  std::string signature, key;
  double top=0, stemWidth=0, stemHeight=0, footWidth=0, footHeight=0, packing=0, slabThickness=0, edgeWidth=0, edgeDepth=0;
  Sources(const Document& d, const json& requested) {
    inputs = requested;
    type=inputs.value("type",std::string("strip"));
    need(type=="strip" || type=="pads" || type=="mat","typeはstrip/pads/matです");pads=type=="pads";mat=type=="mat";
    if(pads) {
      allowed(inputs,{"type","sources","width","depth","height","top_z"});
      need(inputs.contains("sources") && inputs["sources"].is_array(),"実在する床束・柱のsources配列を指定してください");
      std::set<ObjectId> ids;for(const auto& id:inputs["sources"]) ids.insert(id.get<std::string>());
      need(!ids.empty() && ids.size()<=200,"床束・柱を1〜200本選択してください。未選定の目印は対象にできません");
      padWidth=dimension(inputs,"width",300);padDepth=dimension(inputs,"depth",300);padHeight=dimension(inputs,"height",150);
      inputs["sources"]=ids;inputs["width"]=padWidth;inputs["depth"]=padDepth;inputs["height"]=padHeight;
      if(inputs.contains("top_z")) {
        need(inputs["top_z"].is_number(),"top_zは高さmmです");top=inputs["top_z"].get<double>();
        need(std::isfinite(top) && std::abs(top)<1e9,"束石天端が範囲外です");
      }
      for(const auto& id:ids) {
        const auto* original=d.find(id);need(original,"参照部材がありません: "+id);
        const auto role=attr(*original,d,"kind");
        need(role=="floor_post" || role=="column" || role=="through_column","床束・柱を選択してください: "+id);
        need(attr(*original,d,"display_only")!="true","未選定の目印ではなく実部材を指定してください: "+original->name);
        const auto expanded=construction::frameMemberObject(d,*original);const auto& own=expanded?*expanded:*original;
        const auto* member=std::get_if<LinearMember>(&own.recipe.base);
        need(member && own.recipe.modifiers.empty() && member->section.kind==geom::Section::Kind::Rectangle,
             "加工・任意形状・非矩形の部材は未対応です: "+original->name);
        const auto world=d.parentTransform(*original)*own.transform;
        const auto axis=gp_Vec(member->start,member->end).Transformed(world);
        need(axis.Magnitude()>1 && gp_Dir(axis).IsParallel(gp::DZ(),1e-6),"鉛直な床束・柱を指定してください: "+original->name);
        const auto bounds=geom::exactBounds(d.worldShape(*original));need(!bounds.IsVoid(),"部材形状がありません");
        const auto lo=bounds.CornerMin(),hi=bounds.CornerMax();
        need(padWidth+1e-4>=hi.X()-lo.X() && padDepth+1e-4>=hi.Y()-lo.Y(),"束石の幅・奥行が部材下面を覆っていません: "+original->name);
        const double bottom=std::round(lo.Z()*1e6)/1e6;
        gp_Pnt center((lo.X()+hi.X())/2,(lo.Y()+hi.Y())/2,bottom);
        sites.push_back({id,center,bottom});
        rows.push_back({{"id",id},{"name",original->name},{"base_z",bottom},{"center",point(center)},
                        {"stamp",construction::frameFingerprint(own)}});
      }
      key="pads:sources:"+inputs["sources"].dump();signature=codec::sha256Hex(rows.dump());
      if(!inputs.contains("top_z")) top=sites.front().bottom;
      return;
    }
    if (mat) allowed(inputs, {"type", "frame", "sources", "stem_width", "stem_height", "slab_thickness", "edge_width", "edge_depth",
                              "packing_mm", "top_z", "passages"});
    else allowed(inputs, {"type", "frame", "sources", "stem_width", "stem_height", "footing_width", "footing_thickness", "packing_mm", "top_z"});
    need(inputs.contains("frame") != inputs.contains("sources"), "frameまたはsources（土台ID）のどちらか一つを指定してください");
    std::set<ObjectId> ids;
    if (inputs.contains("frame")) {
      const std::string frame = inputs["frame"].get<std::string>();
      const auto* root = d.find(frame);
      need(root && root->isContainer(), "土台の対象グループがありません");
      for (const auto& id : d.subtree(frame)) {
        const auto* o = d.find(id);
        if (o && attr(*o, d, "kind") == "sill" && attr(*o, d, "display_only") != "true") ids.insert(id);
      }
      key = "frame:" + frame;
    } else {
      need(inputs["sources"].is_array(), "sourcesは土台IDの配列です");
      for (const auto& id : inputs["sources"]) ids.insert(id.get<std::string>());
      inputs["sources"] = ids;
      key = "sources:" + inputs["sources"].dump();
    }
    need(!ids.empty() && ids.size() <= 200, "水平な土台を1〜200本指定してください");
    double lowest = 1e300, highest = -1e300;
    for (const auto& id : ids) {
      const auto* original = d.find(id);
      need(original && attr(*original, d, "kind") == "sill" && attr(*original, d, "material_class") == "timber" &&
           attr(*original, d, "display_only") != "true", "土台を参照できません: " + id);
      auto expanded = construction::frameMemberObject(d, *original);
      const auto& own = expanded ? *expanded : *original;
      const auto* member = std::get_if<LinearMember>(&own.recipe.base);
      need(member && own.recipe.modifiers.empty(), "手編集・任意形状の土台は未対応です: " + id);
      need(member->section.kind==geom::Section::Kind::Rectangle,"矩形断面の土台を指定してください: "+id);
      const auto frame = geom::memberFrame(member->start, member->end, member->up, member->roll);
      need(frame.ok(), frame.error());
      const auto world = d.parentTransform(*original) * own.transform;
      need(frame->XDirection().Transformed(world).IsParallel(gp::DZ(),1e-6) ||
           frame->YDirection().Transformed(world).IsParallel(gp::DZ(),1e-6),"断面が傾いた土台は未対応です: "+id);
      const gp_Vec centre = -gp_Vec(frame->XDirection()) * (member->anchor.across * member->section.width/2) -
                           gp_Vec(frame->YDirection()) * (member->anchor.up * member->section.depth/2);
      const auto a = member->start.Translated(centre).Transformed(world), b = member->end.Translated(centre).Transformed(world);
      need(a.Distance(b) > 1 && std::abs(a.Z()-b.Z()) < 1e-6, "傾いた土台・短すぎる土台は未対応です: " + id);
      const auto bounds = geom::exactBounds(d.worldShape(*original));
      need(!bounds.IsVoid(), "土台の形状がありません");
      const double bottom = std::round(bounds.CornerMin().Z()*1e6)/1e6;
      lowest = std::min(lowest, bottom); highest = std::max(highest, bottom);
      lines.push_back({id, a, b, bottom});
      rows.push_back({{"id", id}, {"name", original->name}, {"start", point(a)}, {"end", point(b)},
                     {"sill_bottom_z", bottom}, {"stamp", construction::frameFingerprint(own)}});
    }
    need(highest-lowest < .001, "高さの異なる土台が混在しています。段差基礎は未対応です。対象を分けてください");
    // Mat: the stem stands on the slab, so its default height is the rise above the slab top.
    stemWidth = dimension(inputs, "stem_width", 150); stemHeight = dimension(inputs, "stem_height", mat ? 400 : 600);
    packing = dimension(inputs, "packing_mm", 20, true);
    if (mat) {
      slabThickness = dimension(inputs, "slab_thickness", 150);
      if (inputs.contains("passages")) {
        need(inputs["passages"].is_array() && inputs["passages"].size() <= 20, "passagesは20個までの配列です");
        json normalized = json::array();
        for (const auto& p : inputs["passages"]) {
          allowed(p, {"at", "width", "height"});
          need(p.contains("at") && p["at"].is_array() && p["at"].size() == 2 && p["at"][0].is_number() && p["at"][1].is_number(),
               "人通口のatは平面の点[x,y]（mm）です");
          Passage passage{p["at"][0].get<double>(), p["at"][1].get<double>(), dimension(p, "width", 600), dimension(p, "height", 330)};
          need(std::isfinite(passage.x) && std::isfinite(passage.y) && std::abs(passage.x) < 1e9 && std::abs(passage.y) < 1e9, "人通口の位置が範囲外です");
          need(passage.height < stemHeight, "人通口の高さは立上り高さ未満にしてください");
          passages.push_back(passage);
          normalized.push_back({{"at", {passage.x, passage.y}}, {"width", passage.width}, {"height", passage.height}});
        }
        inputs["passages"] = normalized;
      }
      inputs["slab_thickness"]=slabThickness;
      // 根入れ部: a band under the outer stems below the slab bottom (0 = none).
      edgeWidth = dimension(inputs, "edge_width", 300); edgeDepth = dimension(inputs, "edge_depth", 150, true);
      need(edgeWidth >= stemWidth, "根入れ部の幅は立上り幅以上にしてください");
      inputs["edge_width"]=edgeWidth; inputs["edge_depth"]=edgeDepth;
    } else {
      footWidth = dimension(inputs, "footing_width", 450); footHeight = dimension(inputs, "footing_thickness", 150);
      need(footWidth >= stemWidth, "底盤幅は立上り幅以上にしてください");
      inputs["footing_width"]=footWidth; inputs["footing_thickness"]=footHeight;
    }
    top = lowest-packing;
    if (inputs.contains("top_z")) {
      need(inputs["top_z"].is_number(), "top_zは高さmmです"); top = inputs["top_z"].get<double>();
      need(std::isfinite(top) && std::abs(top)<1e9, "基礎天端が範囲外です");
    }
    inputs["stem_width"]=stemWidth; inputs["stem_height"]=stemHeight; inputs["packing_mm"]=packing;
    // The type is part of the stored inputs so a later merge-patch cannot change it unnoticed.
    if (mat) inputs["type"]="mat";
    signature = codec::sha256Hex(rows.dump());
  }
  double siteTop(const PadSite& site) const {return inputs.contains("top_z")?top:site.bottom;}
};
std::string rootStamp(const Object& root, const Document& d) {
  Object world = root; world.attributes.erase(kManifest); world.transform = d.worldTransform(root);
  return construction::frameFingerprint(world);
}
bool modified(const Object& root, const Document& d, const json& manifest) {
  if (rootStamp(root,d) != manifest.at("root_stamp")) return true;
  const auto children = d.subtree(root.id);
  if (children.size() != manifest.at("parts").size()+1) return true;
  for (const auto& [id, stamp] : manifest.at("parts").items()) {
    const auto* o = d.find(id);
    if (!o || o->parent != root.id || construction::frameFingerprint(*o) != stamp) return true;
  }
  return false;
}
double overlapVolume(const TopoDS_Shape& a,const TopoDS_Shape& b) {
  if(geom::exactBounds(a).IsOut(geom::exactBounds(b))) return 0;
  TopTools_ListOfShape arguments,tools;arguments.Append(a);tools.Append(b);
  BRepAlgoAPI_Common common;common.SetArguments(arguments);common.SetTools(tools);common.SetNonDestructive(true);common.Build();
  need(common.IsDone(),"基礎どうしの重なりを確認できません");return std::abs(geom::volume(common.Shape()));
}
// A straight run of the given width along a sill axis, its ends extended by half the width.
Recipe band(const Sill& line, double width, double base, double height) {
  const gp_Dir along(gp_Vec(line.a, line.b)), normal = gp::DZ().Crossed(along);
  auto origin = line.a.Translated(-gp_Vec(along)*(width/2)-gp_Vec(normal)*(width/2)); origin.SetZ(base);
  geom::SketchPlane plane; plane.origin=origin; plane.normal=gp::DZ(); plane.xDir=along;
  Recipe recipe; recipe.base=RectangleProfile{plane, line.a.Distance(line.b)+width, width};
  recipe.modifiers.push_back(ExtrudeOp{height});
  return recipe;
}
BooleanOperand operand(Recipe recipe) {
  BooleanOperand o; o.recipe=std::make_shared<Recipe>(std::move(recipe)); return o;
}
Recipe unionOf(const std::vector<Sill>& lines, double width, double base, double height) {
  BooleanFeature unioned; unioned.kind = BooleanKind::Union;
  for (const auto& line : lines) unioned.operands.push_back(operand(band(line, width, base, height)));
  return unioned.operands.size()==1 ? *unioned.operands[0].recipe : Recipe{unioned,{}};
}
Recipe unionOf(const Sources& plan, double width, double base, double height) { return unionOf(plan.lines, width, base, height); }
double stemBase(const Sources& plan) { return plan.top-plan.stemHeight; }
Object part(const Document& d, const ObjectId& root, const std::string& kind, const std::string& name, Recipe recipe) {
  Object o; o.id=generateObjectId(); o.parent=root; o.layer="基礎・土台"; o.name=name;
  o.attributes={{"kind", kind}, {"material_class", "rc"}, {"foundation_owner",root}};
  o.recipe=std::move(recipe);
  const auto built = rebuild(o,d.regenContext()); need(built.ok(),built.message());
  return o;
}
Object network(const Document& d, const Sources& plan, const ObjectId& root, bool footing) {
  const double width = footing ? plan.footWidth : plan.stemWidth;
  const double height = footing ? plan.footHeight : plan.stemHeight;
  const double base = stemBase(plan)-(footing ? plan.footHeight : 0);
  return part(d, root, footing ? "foundation_footing" : "foundation_stem",
              footing ? "基礎底盤（計画）" : "基礎立上り（計画）", unionOf(plan, width, base, height));
}

// --- Mat foundation (べた基礎) --------------------------------------------------
// The slab fills the outer outline of the stem network: the outer wire of the
// stems' joined bottom face. Inner walls only divide it, so they are inside.
std::vector<gp_Pnt> outline(const Document& d, const Sources& plan) {
  Object probe; probe.recipe=unionOf(plan, plan.stemWidth, stemBase(plan), plan.stemHeight);
  const auto built=rebuild(probe,d.regenContext()); need(built.ok(),built.message());
  std::vector<TopoDS_Face> bottoms;
  for (TopExp_Explorer e(probe.shape,TopAbs_FACE);e.More();e.Next()) {
    const auto face=TopoDS::Face(e.Current());
    BRepAdaptor_Surface surface(face);
    if (surface.GetType()!=GeomAbs_Plane) continue;
    gp_Dir n=surface.Plane().Axis().Direction(); if (face.Orientation()==TopAbs_REVERSED) n.Reverse();
    if (n.IsParallel(gp::DZ(),1e-9) && std::abs(surface.Plane().Location().Z()-stemBase(plan))<1e-6 && n.Z()<0) bottoms.push_back(face);
  }
  need(bottoms.size()==1,"土台が一つにつながっていません。外周が一続きの土台を対象にしてください");
  int wires=0; for (TopExp_Explorer w(bottoms[0],TopAbs_WIRE);w.More();w.Next()) ++wires;
  need(wires>=2,"土台が閉じた外周を作っていません。べた基礎は外周の土台が一周つながっている必要があります");
  std::vector<gp_Pnt> points;
  for (BRepTools_WireExplorer w(BRepTools::OuterWire(bottoms[0]),bottoms[0]);w.More();w.Next()) {
    gp_Pnt p=BRep_Tool::Pnt(w.CurrentVertex()); p.SetZ(0);
    need(BRepAdaptor_Curve(w.Current()).GetType()==GeomAbs_Line,"曲線を含む外周は未対応です");
    points.push_back(p);
  }
  // Drop points on a straight run, then turn the loop counter-clockwise (slab normal +Z).
  std::vector<gp_Pnt> corners;
  for (std::size_t i=0;i<points.size();++i) {
    const auto& prev=points[(i+points.size()-1)%points.size()], &here=points[i], &next=points[(i+1)%points.size()];
    if (gp_Vec(prev,here).Crossed(gp_Vec(here,next)).Magnitude()>1e-6*gp_Vec(prev,here).Magnitude()*gp_Vec(here,next).Magnitude()) corners.push_back(here);
  }
  need(corners.size()>=3,"外周の形を読み取れません");
  double area=0; for (std::size_t i=0;i<corners.size();++i) { const auto& a=corners[i], &b=corners[(i+1)%corners.size()]; area+=a.X()*b.Y()-b.X()*a.Y(); }
  if (area<0) std::reverse(corners.begin(),corners.end());
  return corners;
}
bool insideOutline(const std::vector<gp_Pnt>& loop, double x, double y) {
  bool in=false;
  for (std::size_t i=0,j=loop.size()-1;i<loop.size();j=i++) {
    const auto& a=loop[i], &b=loop[j];
    if ((a.Y()>y)!=(b.Y()>y) && x<(b.X()-a.X())*(y-a.Y())/(b.Y()-a.Y())+a.X()) in=!in;
  }
  return in;
}
Object slab(const Document& d, const Sources& plan, const ObjectId& root, const std::vector<gp_Pnt>& loop) {
  std::vector<gp_Pnt> points; const double bottom=stemBase(plan)-plan.slabThickness;
  for (auto p : loop) { p.SetZ(bottom); points.push_back(p); }
  Recipe recipe; recipe.base=PolygonProfile{points}; recipe.modifiers.push_back(ExtrudeOp{plan.slabThickness});
  return part(d, root, "foundation_slab", "べた基礎 耐圧盤（計画）", std::move(recipe));
}
// A stem is outer when the ground just beside it, at its middle, is outside the outline.
bool outerRun(const Sources& plan, const Sill& line, const std::vector<gp_Pnt>& loop) {
  const gp_Dir along(gp_Vec(line.a,line.b)), normal=gp::DZ().Crossed(along);
  const gp_Pnt middle((line.a.X()+line.b.X())/2,(line.a.Y()+line.b.Y())/2,0);
  for (double side : {-1.0, 1.0}) {
    const gp_Pnt probe=middle.Translated(gp_Vec(normal)*(side*(plan.stemWidth/2+1)));
    if (!insideOutline(loop,probe.X(),probe.Y())) return true;
  }
  return false;
}
// 根入れ部 (edge footing): a band centred under each outer stem, below the slab
// bottom, joined at the corners. Its depth is below the slab, not below a ground level.
std::optional<Object> edges(const Document& d, const Sources& plan, const ObjectId& root, const std::vector<gp_Pnt>& loop) {
  if (plan.edgeDepth <= 0) return std::nullopt;
  std::vector<Sill> outer;
  for (const auto& line : plan.lines) if (outerRun(plan, line, loop)) outer.push_back(line);
  need(!outer.empty(), "外周の立上りを読み取れません");
  const double base = stemBase(plan)-plan.slabThickness-plan.edgeDepth;
  return part(d, root, "foundation_edge_footing", "べた基礎 外周の根入れ部（計画）",
              unionOf(outer, plan.edgeWidth, base, plan.edgeDepth));
}
// The stems with each passage cut through one interior run. A passage on an
// outer run, on a junction or off every run is refused (nothing is moved).
Object matStems(const Document& d, const Sources& plan, const ObjectId& root, const std::vector<gp_Pnt>& loop) {
  Recipe stems=unionOf(plan, plan.stemWidth, stemBase(plan), plan.stemHeight);
  if (plan.passages.empty()) return part(d, root, "foundation_stem", "基礎立上り（計画）", std::move(stems));
  BooleanFeature cut; cut.kind=BooleanKind::Subtract; cut.operands.push_back(operand(std::move(stems)));
  std::vector<TopoDS_Shape> boxes;
  for (const auto& p : plan.passages) {
    const Sill* best=nullptr; double bestDistance=1e300, at=0;
    for (const auto& line : plan.lines) {
      const gp_Vec axis(line.a,line.b); const double length=axis.Magnitude();
      const double s=gp_Vec(line.a,gp_Pnt(p.x,p.y,line.a.Z())).Dot(axis)/length;
      if (s<-1e-6 || s>length+1e-6) continue;
      const double distance=gp_Pnt(p.x,p.y,line.a.Z()).Distance(line.a.Translated(axis*(s/length)));
      if (distance<=plan.stemWidth/2+1e-6 && distance<bestDistance) { best=&line; bestDistance=distance; at=s; }
    }
    need(best,"人通口の位置に立上りがありません。内部の立上りの上を指定してください");
    const gp_Dir along(gp_Vec(best->a,best->b)), normal=gp::DZ().Crossed(along);
    const gp_Pnt centre=best->a.Translated(gp_Vec(along)*at);
    for (double side : {-1.0, 1.0}) {
      const gp_Pnt probe=centre.Translated(gp_Vec(normal)*(side*(plan.stemWidth/2+1)));
      need(insideOutline(loop,probe.X(),probe.Y()),"外周の立上りには人通口を開けられません。内部の立上りを指定してください");
    }
    Sill run{best->id, centre.Translated(-gp_Vec(along)*(p.width/2)), centre.Translated(gp_Vec(along)*(p.width/2)), best->bottom};
    // The cutter is one stem width plus 1 mm each side, from the slab top up.
    Recipe cutter; {
      auto origin=run.a.Translated(-gp_Vec(normal)*(plan.stemWidth/2+1)); origin.SetZ(stemBase(plan));
      geom::SketchPlane plane; plane.origin=origin; plane.normal=gp::DZ(); plane.xDir=along;
      cutter.base=RectangleProfile{plane, p.width, plan.stemWidth+2}; cutter.modifiers.push_back(ExtrudeOp{p.height});
    }
    Object probe; probe.recipe=cutter; const auto built=rebuild(probe,d.regenContext()); need(built.ok(),built.message());
    for (const auto& line : plan.lines) {
      if (&line==best) continue;
      const gp_Vec other(line.a,line.b);
      const bool collinear=other.IsParallel(gp_Vec(best->a,best->b),1e-9) &&
                           gp_Vec(best->a,line.a).Crossed(gp_Vec(along)).Magnitude()<1e-6;
      if (collinear) continue;
      Object crossing; crossing.recipe=band(line, plan.stemWidth, stemBase(plan), plan.stemHeight);
      const auto made=rebuild(crossing,d.regenContext()); need(made.ok(),made.message());
      need(overlapVolume(probe.shape,crossing.shape)<=.001,"人通口が立上りの交差部にかかります。位置か幅を変えてください");
    }
    for (const auto& other : boxes) need(overlapVolume(probe.shape,other)<=.001,"人通口どうしが重なります");
    boxes.push_back(probe.shape);
    cut.operands.push_back(operand(std::move(cutter)));
  }
  return part(d, root, "foundation_stem", "基礎立上り（計画）", Recipe{cut,{}});
}
std::vector<Object> padObjects(const Document& d,const Sources& plan,const ObjectId& root,json& sourceTops) {
  std::vector<Object> out;std::map<std::string,std::size_t> positions;
  for(const auto& site:plan.sites) {
    const double top=plan.siteTop(site);sourceTops[site.id]=top;
    const auto position=point(gp_Pnt(site.center.X(),site.center.Y(),top)).dump();
    if(positions.count(position)) {
      auto& part=out.at(positions.at(position));auto refs=json::parse(part.attributes.at("source_ids"));refs.push_back(site.id);
      part.attributes["source_ids"]=refs.dump();continue;
    }
    Object o;o.id=generateObjectId();o.parent=root;o.layer="基礎・土台";o.name="束石・独立基礎（計画）";
    o.attributes={{"kind","foundation_pad"},{"material_class","rc"},{"foundation_owner",root},{"source_ids",json::array({site.id}).dump()}};
    geom::SketchPlane plane;plane.origin={site.center.X()-plan.padWidth/2,site.center.Y()-plan.padDepth/2,top-plan.padHeight};
    o.recipe.base=RectangleProfile{plane,plan.padWidth,plan.padDepth};o.recipe.modifiers.push_back(ExtrudeOp{plan.padHeight});
    const auto status=rebuild(o,d.regenContext());need(status.ok(),status.message());
    for(const auto& other:out) need(overlapVolume(o.shape,other.shape)<=.001,"束石どうしが重なります。対象・寸法を分けて計画してください");
    for(const auto& other:d.objects()) {
      if(d.isAncestor(root,other.id)) continue;
      if(other.isContainer() || attr(other,d,"display_only")=="true") continue;
      const auto shape=d.worldShape(other);if(shape.IsNull() || !TopExp_Explorer(shape,TopAbs_SOLID).More()) continue;
      need(overlapVolume(o.shape,shape)<=.001,"既存の形状と束石が重なります: "+other.name+"。支持先一覧で未検出の部材を選んでください");
    }
    positions[position]=out.size();out.push_back(std::move(o));
  }
  return out;
}
json report(const Document& d, const Object& root) {
  const auto manifest=json::parse(root.attributes.at(kManifest));
  json result={{"foundation",root.id},{"name",root.name},{"inputs",manifest.at("inputs")},
               {"status","current"},{"reasons",json::array()},{"sources",json::array()},
               {"quantities",json::object()}, {"verified",false},
               {"unassessed",{"地盤・支持力・沈下","基礎断面の構造計算","配筋・かぶり・定着","耐力壁と柱の荷重伝達","凍結深度・地形","人通口・配管・換気"}}};
  const bool mat=manifest.at("inputs").value("type",std::string("strip"))=="mat";
  if (mat) result["unassessed"]={"地盤・支持力・沈下","耐圧盤・立上りの構造計算","配筋・かぶり・定着","耐力壁と柱の荷重伝達",
                                  "地盤面からの根入れ深さ・凍結深度・地形","防湿・断熱・床下換気","人通口の補強・床下の点検経路","配管の貫通"};
  if (modified(root,d,manifest)) { result["status"]="modified"; result["reasons"].push_back("基礎の形状・配置・構成が手編集されています"); }
  try {
    Sources source(d,manifest.at("inputs"));
    if (source.signature!=manifest.at("source_signature")) {
      if (result["status"]=="current") result["status"]="needs_regeneration";
      result["reasons"].push_back("参照土台が変更されています。基礎を作り直してください");
    }
    for (const auto& line : source.rows) {
      auto row=line; row.erase("stamp");
      const double plannedTop=source.pads?manifest.at("source_tops").at(line["id"].get<std::string>()).get<double>():manifest.at("top_z").get<double>();
      const double gap=line[source.pads?"base_z":"sill_bottom_z"].get<double>()-plannedTop;
      row["planned_top_z"]=plannedTop;
      row["packing_gap_mm"]=gap;
      row["packing_gap_matches"] = result["status"]=="current" ? json(std::abs(gap-source.packing)<=1) : json();
      result["sources"].push_back(row);
    }
  } catch (const std::exception& e) { result["status"]="source_missing_or_unsupported"; result["reasons"].push_back(e.what()); }
  double total=0; bool complete=true;
  for (const auto& [id, stamp] : manifest.at("parts").items()) {
    (void)stamp;
    const auto* part=d.find(id); if (!part) { complete=false; continue; }
    const auto shape=d.worldShape(*part);
    const double volume=geom::volume(shape)/1e9;
    int solids=0; for (TopExp_Explorer s(shape,TopAbs_SOLID);s.More();s.Next()) ++solids;
    const std::string role=manifest.at("roles").at(id);
    auto& q=result["quantities"][role];if(q.is_null()) q={{"volume_m3",0.0},{"solids",0},{"ids",json::array()}};
    q["volume_m3"]=q["volume_m3"].get<double>()+volume;q["solids"]=q["solids"].get<int>()+solids;q["ids"].push_back(id);
    if(q["ids"].size()==1) q["id"]=id;else q.erase("id");
    total+=volume;
  }
  result["known_parts_volume_m3"]=total;
  result["generated_parts_volume_m3"]=complete?json(total):json();
  result["top_z"]=manifest.at("top_z");
  result["notice"]=mat?"指定した土台の下の立上りと、その外周の内側の耐圧盤の計画です。地盤・構造・配筋・防湿・換気等は別途確認してください。数量は生成部材のみです。":
                       "指定部材下の基礎計画です。対象外の支持先・べた基礎・地盤・構造・配筋等は別途確認してください。数量は生成部材のみです。";
  return result;
}
}

namespace sbr::construction {
Result<std::string> checkFoundation(const doc::Document& document, const std::string& options) {
  try {
    const auto opt=json::parse(options); allowed(opt,{"foundation"});
    json plans=json::array();
    const std::string id=opt.value("foundation",std::string());
    if (!id.empty()) need(document.find(id) && document.find(id)->attributes.count(kManifest),"基礎計画がありません");
    for (const auto& root : document.objects())
      if (root.attributes.count(kManifest) && (id.empty() || id==root.id)) plans.push_back(report(document,root));
    return json{{"version","foundation-plan-1"},{"sequence",document.sequence()},{"plans",plans}}.dump();
  } catch (const Standard_Failure& e) { return Result<std::string>::failure(kernelMessage(e)); }
    catch (const std::exception& e) { return Result<std::string>::failure(e.what()); }
}
}

namespace sbr::doc {
Result<std::string> Modeler::generateFoundation(const std::string& text) {
  try {
    const auto args=json::parse(text);
    allowed(args,{"inputs","replace","on_modified","expected_revision","preview","name"});
    if (args.contains("expected_revision")) need(args["expected_revision"]==history_.revision(),"モデルが変わりました。再プレビューしてください");
    const std::string replace=args.value("replace",std::string());
    const Object* old=replace.empty()?nullptr:document_.find(replace);
    json previous=json::object();
    if (!replace.empty()) {
      need(old && old->isContainer() && old->attributes.count(kManifest),"置換する基礎計画がありません");
      previous=json::parse(old->attributes.at(kManifest));
      need(!modified(*old,document_,previous) || args.value("on_modified",std::string("ask"))=="replace",
           "requires_decision: 手編集された基礎です。保持するか、on_modified:replaceを明示してください");
    }
    need(!args.contains("on_modified") || args["on_modified"]=="ask" || args["on_modified"]=="replace","on_modifiedはask/replaceです");
    json inputs=previous.empty()?json::object():previous.at("inputs");
    if (args.contains("inputs")) inputs.merge_patch(args["inputs"]);
    Sources plan(document_,inputs);
    if(old) need(plan.type==previous.at("inputs").value("type",std::string("strip")),"基礎種別は置換で変更できません。新規計画を作ってください");
    if(old) for(const auto& line:plan.lines)
      need(!document_.isAncestor(old->id,line.id),"置換対象の基礎内にある土台は参照できません。先に外へ移してください");
    if (replace.empty()) for (const auto& candidate:document_.objects()) {
      const auto stored=candidate.attributes.find(kManifest); if(stored==candidate.attributes.end()) continue;
      const auto existing=json::parse(stored->second);
      if(existing.at("source_key")!=plan.key && existing.at("source_signature")!=plan.signature) continue;
      if(existing.at("source_signature")==plan.signature && existing.at("inputs")==plan.inputs && !modified(candidate,document_,existing)) {
        auto r=report(document_,candidate); r["unchanged"]=true; return r.dump();
      }
      const auto existingType=existing.at("inputs").value("type",std::string("strip"));
      if(existingType!=plan.type)
        throw std::runtime_error("requires_decision: 同じ土台に別の種別の基礎計画があります。使わない方を削除してから作ってください: "+candidate.id);
      throw std::runtime_error("requires_decision: 同じ土台の基礎計画があります。replaceで作り直してください: "+candidate.id);
    }
    if(old) for(const auto& site:plan.sites) need(!document_.isAncestor(old->id,site.id),"基礎内の床束・柱は置換前に外へ移してください");
    Object root; root.id=old?old->id:generateObjectId();
    root.name=args.value("name",old?old->name:std::string(plan.pads?"束石・独立基礎計画":plan.mat?"べた基礎計画":"布基礎計画"));
    root.recipe.base=GroupNode{}; root.layer="基礎・土台"; root.attributes["kind"]="foundation_plan";
    json sourceTops=json::object();std::vector<Object> parts;
    if(plan.pads) parts=padObjects(document_,plan,root.id,sourceTops);
    else if(plan.mat) {
      const auto loop=outline(document_,plan);
      parts.push_back(matStems(document_,plan,root.id,loop));parts.push_back(slab(document_,plan,root.id,loop));
      if(auto edge=edges(document_,plan,root.id,loop)) parts.push_back(std::move(*edge));
    }
    else {parts.push_back(network(document_,plan,root.id,false));parts.push_back(network(document_,plan,root.id,true));}
    json manifest={{"version",1},{"inputs",plan.inputs},{"source_key",plan.key},{"source_signature",plan.signature},
                   {"roles",json::object()},{"top_z",plan.top},{"parts",json::object()}};
    if(plan.pads) {manifest["source_tops"]=sourceTops;for(const auto& value:sourceTops) if(std::abs(value.get<double>()-plan.top)>.000001) manifest["top_z"]=nullptr;}
    for(const auto& part:parts) {manifest["roles"][part.id]=part.attributes.at("kind");manifest["parts"][part.id]=construction::frameFingerprint(part);}
    manifest["root_stamp"]=construction::frameFingerprint(root);
    root.attributes[kManifest]=manifest.dump();
    Command command; command.name="Generate Foundation Plan";
    if(old) {
      auto ids=document_.subtree(old->id);
      std::sort(ids.begin(),ids.end(),[&](const auto& a,const auto& b){return document_.indexOf(a)>document_.indexOf(b);});
      for(const auto& id:ids) if(id!=root.id) command.deltas.push_back({id,*document_.find(id),std::nullopt,document_.indexOf(id)});
    }
    command.deltas.push_back({root.id,old?std::optional<Object>(*old):std::nullopt,root,old?document_.indexOf(root.id):std::nullopt});
    for(const auto& part:parts) command.deltas.push_back({part.id,std::nullopt,part,std::nullopt});
    // Finish measurements and the result before the one model mutation.
    // A geometry/measurement failure must leave no partial foundation behind.
    json r={{"foundation",root.id},{"name",root.name},{"inputs",plan.inputs},{"status","current"},
            {"verified",false},{"top_z",manifest["top_z"]},{"generated_parts_volume_m3",0.0},{"quantities",json::object()}};
    for(const auto& part:parts) {
      const double volume=geom::volume(part.shape)/1e9;r["generated_parts_volume_m3"]=r["generated_parts_volume_m3"].get<double>()+volume;
      auto& q=r["quantities"][part.attributes.at("kind")];if(q.is_null()) q={{"volume_m3",0.0},{"ids",json::array()}};
      q["volume_m3"]=q["volume_m3"].get<double>()+volume;q["ids"].push_back(part.id);
      if(q["ids"].size()==1) q["id"]=part.id;else q.erase("id");
    }
    r["warnings"]={"布基礎の計画形状です。地盤・構造・配筋・開口等は未照査。寸法の初期値は設計値ではありません。",
      "対象は指定した土台下のみ。床束下の束石・土台のない耐力壁下・べた基礎は別途計画してください。"};
    if(plan.pads) r["warnings"]={"束石・独立基礎の計画形状です。寸法・地盤・支持力・凍結・配筋・アンカー等は未照査です。",
      "選択部材の下面を参照します。地盤や荷重から基礎底・必要断面を決める機能ではありません。"};
    if(plan.mat) r["warnings"]={"べた基礎の計画形状です。耐圧盤・立上り・根入れ部の寸法、地盤・構造・配筋・防湿・換気は未照査。寸法の初期値は設計値ではありません。",
      "外周の根入れ部の深さは耐圧盤の下面からの寸法です。地盤面からの根入れ深さ・凍結深度は確認していません。",
      "耐圧盤は指定した土台の外周の内側だけ。土台のない耐力壁の下や、外周の外の玄関・ポーチは含みません。",
      "人通口は指定した位置の内部立上りだけに開けます。補強や床下の点検経路は確認していません。"};
    const auto result=r.dump();
    history_.execute(std::move(command));
    return result;
  } catch(const Standard_Failure& e) { return Result<std::string>::failure(kernelMessage(e)); }
    catch(const std::exception& e) { return Result<std::string>::failure(e.what()); }
}
}
