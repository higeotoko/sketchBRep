#include "sbr/ConstructionFoundation.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/Codec.h"
#include "sbr/KernelError.h"
#include "sbr/geom/Measure.h"
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopTools_ListOfShape.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <nlohmann/json.hpp>

namespace sbr::construction {
namespace {
using namespace doc;
using json=nlohmann::json;
std::string attribute(const Object& o,const Document& d,const char* key) {
  const auto it=o.attributes.find(key);if(it!=o.attributes.end()) return it->second;
  const auto* ref=std::get_if<ComponentInstance>(&o.recipe.base);
  const auto* def=ref?d.findDefinition(ref->definitionId):nullptr;
  if(def && def->objects.size()==1) {
    const auto value=def->objects[0].attributes.find(key);
    if(value!=def->objects[0].attributes.end()) return value->second;
  }
  return {};
}
bool concrete(const std::string& role) {
  return role=="foundation_stem" || role=="foundation_footing" || role=="foundation_pad";
}
bool subjectRole(const std::string& role) {
  return role=="sill" || role=="column" || role=="through_column" || role=="floor_post";
}
struct Member {
  const Object* object;
  std::string role,reason;
  bool rc=false;
  double low=0,high=0,area=0;
  Bnd_Box bounds;
  TopoDS_Shape bottom,top;
};
TopoDS_Shape horizontalFaces(const TopoDS_Shape& shape,double z) {
  BRep_Builder builder;TopoDS_Compound out;builder.MakeCompound(out);
  for(TopExp_Explorer e(shape,TopAbs_FACE);e.More();e.Next()) {
    const auto face=TopoDS::Face(e.Current());BRepAdaptor_Surface surface(face);
    if(surface.GetType()!=GeomAbs_Plane || !surface.Plane().Axis().Direction().IsParallel(gp::DZ(),1e-6)) continue;
    const auto box=geom::exactBounds(face);
    if(!box.IsVoid() && std::abs(box.CornerMin().Z()-z)<1e-4 && std::abs(box.CornerMax().Z()-z)<1e-4)
      builder.Add(out,face);
  }
  return out;
}
Member member(const Object& o,const Document& d) {
  Member out;out.object=&o;out.role=attribute(o,d,"kind");out.rc=concrete(out.role);
  if(attribute(o,d,"display_only")=="true" || !attribute(o,d,"hardware").empty() || attribute(o,d,"catalog")=="STEEL-POST-UNSELECTED") {
    out.reason="目印・製品未選定の床束は実部材として評価しません";return out;
  }
  if(out.rc) {
    if(attribute(o,d,"material_class")!="rc") {out.reason="基礎の材料属性がrcではありません";return out;}
  } else {
    const auto expanded=frameMemberObject(d,o);const auto& own=expanded?*expanded:o;
    const auto* linear=std::get_if<LinearMember>(&own.recipe.base);
    if(!linear || !own.recipe.modifiers.empty() || linear->section.kind!=geom::Section::Kind::Rectangle) {
      out.reason="加工・任意形状・非矩形の部材は未対応です";return out;
    }
    const auto axis=gp_Vec(linear->start,linear->end).Transformed(d.parentTransform(o)*own.transform);
    if(axis.Magnitude()<1e-6 || (out.role=="sill"?std::abs(axis.Z())>1e-5:!gp_Dir(axis).IsParallel(gp::DZ(),1e-6))) {
      out.reason="水平な土台・鉛直な柱と床束のみが対象です";return out;
    }
  }
  const auto shape=d.worldShape(o);if(shape.IsNull()) {out.reason="実形状がありません";return out;}
  out.bounds=geom::exactBounds(shape);if(out.bounds.IsVoid()) {out.reason="実形状がありません";return out;}
  out.low=out.bounds.CornerMin().Z();out.high=out.bounds.CornerMax().Z();
  if(out.rc) for(TopExp_Explorer e(shape,TopAbs_FACE);e.More();e.Next()) {
    const auto face=TopoDS::Face(e.Current());BRepAdaptor_Surface surface(face);
    if(surface.GetType()!=GeomAbs_Plane) continue;
    auto normal=surface.Plane().Axis().Direction();if(face.Orientation()==TopAbs_REVERSED) normal.Reverse();
    if(normal.IsEqual(gp::DZ(),1e-6) && std::abs(surface.Plane().Location().Z()-out.high)>1e-4) {
      out.reason="段差・複数天端の基礎は未対応です";return out;
    }
  }
  out.bottom=horizontalFaces(shape,out.low);out.top=horizontalFaces(shape,out.high);
  out.area=geom::area(out.bottom);
  if(out.area<=1e-4 || geom::area(out.top)<=1e-4) out.reason="水平な下面・上面を評価できません";
  return out;
}
bool xyOverlap(const Bnd_Box& a,const Bnd_Box& b) {
  return a.CornerMin().X()<b.CornerMax().X()-1e-5 && a.CornerMax().X()>b.CornerMin().X()+1e-5 &&
         a.CornerMin().Y()<b.CornerMax().Y()-1e-5 && a.CornerMax().Y()>b.CornerMin().Y()+1e-5;
}
json reference(const Member& m) {
  return {{"id",m.object->id},{"name",m.object->name},{"role",m.role}};
}
}

Result<std::string> foundationSupports(const doc::Document& d,const std::string& options) {
  try {
    const auto opt=json::parse(options);
    if(!opt.is_object()) throw std::runtime_error("objectが必要です");
    for(const auto& [key,value]:opt.items())
      if(!std::set<std::string>{"within","ids","tolerance_mm","search_depth_mm","packing_mm","pair_budget"}.count(key))
        throw std::runtime_error("不明な項目: "+key);
    const double tolerance=opt.value("tolerance_mm",1.0),depth=opt.value("search_depth_mm",1000.0),packing=opt.value("packing_mm",20.0);
    const int budget=opt.value("pair_budget",20000);
    if(!std::isfinite(tolerance) || tolerance<0 || tolerance>10 || !std::isfinite(depth) || depth<1 || depth>10000 ||
       !std::isfinite(packing) || packing<0 || packing>1000 || budget<1 || budget>100000)
      throw std::runtime_error("許容差・探索深さ・パッキン・計算上限が範囲外です");
    const std::string within=opt.value("within",std::string());
    if(!within.empty() && (!d.find(within) || !d.find(within)->isContainer())) throw std::runtime_error("対象グループがありません");
    std::set<std::string> ids;
    if(opt.contains("ids")) {
      if(!opt["ids"].is_array()) throw std::runtime_error("idsは部材IDの配列です");
      for(const auto& id:opt["ids"]) {
        const std::string value=id.get<std::string>();
        if(!d.find(value) || !subjectRole(attribute(*d.find(value),d,"kind"))) throw std::runtime_error("対象の柱・土台・床束がありません: "+value);
        ids.insert(value);
      }
    }
    std::vector<Member> members;std::vector<std::size_t> subjects;
    json stamps=json::array(),plans=json::object();
    const auto planReport=checkFoundation(d);
    if(!planReport) return Result<std::string>::failure(planReport.error());
    const auto planData=json::parse(*planReport);
    for(const auto& p:planData["plans"]) plans[p["foundation"].get<std::string>()]=p;
    double sillTop=-1e300;
    for(const auto& o:d.objects()) {
      const auto role=attribute(o,d,"kind");if(!subjectRole(role) && !concrete(role)) continue;
      if(members.size()>=4000) throw std::runtime_error("対象部材が多すぎます（上限4000）。モデルを分けて確認してください");
      members.push_back(member(o,d));const auto& own=members.back();
      // Also stamp unsupported/hidden members: changing these invalidates a displayed report.
      Object world=o;world.transform=d.worldTransform(o);
      std::string definitionStamp;
      if(const auto* instance=std::get_if<ComponentInstance>(&o.recipe.base))
        if(const auto* definition=d.findDefinition(instance->definitionId)) definitionStamp=frameDefinitionFingerprint(*definition);
      stamps.push_back({o.id,frameFingerprint(world),definitionStamp,own.reason});
      if(own.reason.empty() && role=="sill") sillTop=std::max(sillTop,own.high);
      if(subjectRole(role) && (within.empty() || o.id==within || d.isAncestor(within,o.id)) &&
         (!opt.contains("ids") || ids.count(o.id))) subjects.push_back(members.size()-1);
    }
    json unassessed=json::array();
    for(const auto& own:members) if(own.rc && !own.reason.empty()) {
      auto row=reference(own);row["reason"]=own.reason;unassessed.push_back(row);
    }
    int tested=0;bool exhausted=false;
    std::map<std::size_t,json> cache;
    const auto inspect=[&](std::size_t index) {
      const auto& own=members[index];json row=reference(own);
      row["verified"]=false;row["candidates"]=json::array();row["bottom_z"]=nullptr;
      row["status"]="not_evaluated";row["reason"]=own.reason;
      if(!own.reason.empty()) return row;
      row["bottom_z"]=own.low;row["base_area_mm2"]=own.area;
      if((own.role=="column" || own.role=="through_column") && sillTop>-1e299 && own.low>sillTop+depth) {
        row["reason"]="土台天端から探索深さを超えた柱脚です。上階・梁上から基礎までの経路は未対応です";return row;
      }
      double nearest=1e300;json candidates=json::array();
      for(std::size_t i=0;i<members.size();++i) {
        const auto& target=members[i];if(i==index || !target.reason.empty()) continue;
        if(!(target.rc || (own.role!="sill" && target.role=="sill"))) continue;
        const double gap=own.low-target.high;
        if(gap < -depth || gap>depth || !xyOverlap(own.bounds,target.bounds)) continue;
        if(tested>=budget) {exhausted=true;row["reason"]="形状比較の上限に達しました。対象を絞って再確認してください";return row;}
        ++tested;
        gp_Trsf shift;shift.SetTranslation(gp_Vec(0,0,gap));
        const auto aligned=BRepBuilderAPI_Transform(target.top,shift,true).Shape();
        TopTools_ListOfShape arguments,tools;arguments.Append(own.bottom);tools.Append(aligned);
        BRepAlgoAPI_Common common;common.SetArguments(arguments);common.SetTools(tools);
        // These faces share topology with the document. Boolean defaults can add
        // pcurves to input edges; inspection must leave native B-reps unchanged.
        common.SetNonDestructive(true);common.Build();
        if(!common.IsDone()) {row["reason"]="下面と支持面の交差を計算できません";return row;}
        const double overlap=geom::area(common.Shape());if(overlap<=1e-4) continue;
        nearest=std::min(nearest,gap);
        auto c=reference(target);c["gap_mm"]=gap;c["overlap_mm2"]=overlap;c["coverage_ratio"]=std::min(1.0,overlap/own.area);
        double expected=own.role=="sill"?packing:0;
        const auto owner=attribute(*target.object,d,"foundation_owner");
        c["foundation"]=owner.empty()?json():json(owner);c["plan_status"]=nullptr;
        if(!owner.empty()) {
          c["plan_status"]=plans.contains(owner)?plans[owner]["status"]:json("missing");
          if(own.role=="sill" && plans.contains(owner)) expected=plans[owner]["inputs"]["packing_mm"].get<double>();
        }
        c["expected_gap_mm"]=expected;c["gap_matches"]=std::abs(gap-expected)<=tolerance;
        candidates.push_back(c);
      }
      json closest=json::array();
      for(const auto& c:candidates) if(c["gap_mm"].get<double>()<=nearest+tolerance) closest.push_back(c);
      candidates=std::move(closest);
      row["candidates"]=candidates;row["reason"]="";
      int full=0,matching=0;
      for(const auto& c:candidates) if(c["coverage_ratio"].get<double>()>=1-1e-6) {++full;if(c["gap_matches"]==true) ++matching;}
      row["status"]=candidates.empty()?"missing":full==0?"partial":full>1?"ambiguous":matching==1?"detected":nearest < -tolerance?"interference":"gap";
      return row;
    };
    // Compute sills first so a column can refer to their independent concrete support diagnostics.
    for(std::size_t i=0;i<members.size();++i) if(members[i].role=="sill") cache[i]=inspect(i);
    json rows=json::array(),counts=json::object();
    for(const auto index:subjects) {
      auto row=cache.count(index)?cache[index]:inspect(index);
      row["path_status"]="not_evaluated";row["via"]=json::array();
      if(row["status"]!="not_evaluated") {
        row["path_status"]="needs_review";
        if(row["status"]=="detected") {
          bool fresh=true;
          for(const auto& c:row["candidates"]) if(c["coverage_ratio"].get<double>()>=1-1e-6) {
            if(c["role"]=="sill") {
              const auto it=std::find_if(members.begin(),members.end(),[&](const auto& m){return m.object->id==c["id"];});
              const auto s=cache.at(static_cast<std::size_t>(std::distance(members.begin(),it)));
              row["via"].push_back(s);if(s["status"]!="detected") fresh=false;
              for(const auto& f:s["candidates"]) if(!f["plan_status"].is_null() && f["plan_status"]!="current") fresh=false;
            } else if(!c["plan_status"].is_null() && c["plan_status"]!="current") fresh=false;
          }
          row["path_status"]=fresh?"geometry_detected":"needs_review";
        }
      }
      const std::string status=row["status"];counts[status]=counts.value(status,0)+1;rows.push_back(row);
    }
    return json{{"version","foundation-supports-1"},{"sequence",d.sequence()},
      {"source_signature",codec::sha256Hex(stamps.dump()+plans.dump()+opt.dump())},
      {"tolerance_mm",tolerance},{"search_depth_mm",depth},{"pair_budget",budget},{"tested_pairs",tested},
      {"incomplete",exhausted},{"rows",rows},{"counts",counts},{"unassessed_supports",unassessed},{"verified",false},
      {"notice","実形状の下面と支持面の重なり・高さの診断です。接合・アンカー・パッキン実体・支持耐力・地盤は未照査です。部分支持を合算して全面支持とは扱いません。"}}.dump();
  } catch(const Standard_Failure& e) {return Result<std::string>::failure(kernelMessage(e));}
    catch(const std::exception& e) {return Result<std::string>::failure(e.what());}
}
}
