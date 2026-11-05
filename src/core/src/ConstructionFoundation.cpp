#include "sbr/ConstructionFoundation.h"
#include "sbr/ConstructionFrame.h"
#include "sbr/Codec.h"
#include "sbr/KernelError.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include <TopExp_Explorer.hxx>
#include <algorithm>
#include <cmath>
#include <map>
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
struct Sources {
  json inputs, rows = json::array();
  std::vector<Sill> lines;
  std::string signature, key;
  double top, stemWidth, stemHeight, footWidth, footHeight, packing;
  Sources(const Document& d, const json& requested) {
    inputs = requested;
    allowed(inputs, {"frame", "sources", "stem_width", "stem_height", "footing_width", "footing_thickness", "packing_mm", "top_z"});
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
    stemWidth = dimension(inputs, "stem_width", 150); stemHeight = dimension(inputs, "stem_height", 600);
    footWidth = dimension(inputs, "footing_width", 450); footHeight = dimension(inputs, "footing_thickness", 150);
    packing = dimension(inputs, "packing_mm", 20, true);
    need(footWidth >= stemWidth, "底盤幅は立上り幅以上にしてください");
    top = lowest-packing;
    if (inputs.contains("top_z")) {
      need(inputs["top_z"].is_number(), "top_zは高さmmです"); top = inputs["top_z"].get<double>();
      need(std::isfinite(top) && std::abs(top)<1e9, "基礎天端が範囲外です");
    }
    inputs["stem_width"]=stemWidth; inputs["stem_height"]=stemHeight;
    inputs["footing_width"]=footWidth; inputs["footing_thickness"]=footHeight; inputs["packing_mm"]=packing;
    signature = codec::sha256Hex(rows.dump());
  }
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
Object network(const Document& d, const Sources& plan, const ObjectId& root, bool footing) {
  BooleanFeature unioned; unioned.kind = BooleanKind::Union;
  const double width = footing ? plan.footWidth : plan.stemWidth;
  const double height = footing ? plan.footHeight : plan.stemHeight;
  const double base = plan.top-plan.stemHeight-(footing ? plan.footHeight : 0);
  for (const auto& line : plan.lines) {
    const gp_Dir along(gp_Vec(line.a, line.b)), normal = gp::DZ().Crossed(along);
    auto origin = line.a.Translated(-gp_Vec(along)*(width/2)-gp_Vec(normal)*(width/2)); origin.SetZ(base);
    geom::SketchPlane plane; plane.origin=origin; plane.normal=gp::DZ(); plane.xDir=along;
    Recipe recipe; recipe.base=RectangleProfile{plane, line.a.Distance(line.b)+width, width};
    recipe.modifiers.push_back(ExtrudeOp{height});
    BooleanOperand operand; operand.recipe=std::make_shared<Recipe>(std::move(recipe));
    unioned.operands.push_back(std::move(operand));
  }
  Object o; o.id=generateObjectId(); o.parent=root; o.layer="基礎・土台";
  o.name=footing ? "基礎底盤（計画）" : "基礎立上り（計画）";
  o.attributes={{"kind", footing ? "foundation_footing" : "foundation_stem"}, {"material_class", "rc"}, {"foundation_owner",root}};
  o.recipe = unioned.operands.size()==1 ? *unioned.operands[0].recipe : Recipe{unioned,{}};
  const auto built = rebuild(o,d.regenContext()); need(built.ok(),built.message());
  return o;
}
json report(const Document& d, const Object& root) {
  const auto manifest=json::parse(root.attributes.at(kManifest));
  json result={{"foundation",root.id},{"name",root.name},{"inputs",manifest.at("inputs")},
               {"status","current"},{"reasons",json::array()},{"sources",json::array()},
               {"quantities",json::object()}, {"verified",false},
               {"unassessed",{"地盤・支持力・沈下","基礎断面の構造計算","配筋・かぶり・定着","耐力壁と柱の荷重伝達","凍結深度・地形","人通口・配管・換気"}}};
  if (modified(root,d,manifest)) { result["status"]="modified"; result["reasons"].push_back("基礎の形状・配置・構成が手編集されています"); }
  try {
    Sources source(d,manifest.at("inputs"));
    if (source.signature!=manifest.at("source_signature")) {
      if (result["status"]=="current") result["status"]="needs_regeneration";
      result["reasons"].push_back("参照土台が変更されています。基礎を作り直してください");
    }
    for (const auto& line : source.rows) {
      auto row=line; row.erase("stamp");
      const double gap=line["sill_bottom_z"].get<double>()-manifest.at("top_z").get<double>();
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
    result["quantities"][role]={{"volume_m3",volume},{"solids",solids},{"id",id}};
    total+=volume;
  }
  result["known_parts_volume_m3"]=total;
  result["generated_parts_volume_m3"]=complete?json(total):json();
  result["top_z"]=manifest.at("top_z");
  result["notice"]="指定した土台下のみの布基礎計画です。床束下・土台のない耐力壁下・べた基礎は別途計画が必要です。交点は各層で結合しています。地盤・構造・配筋等は未照査です。";
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
    if(old) for(const auto& line:plan.lines)
      need(!document_.isAncestor(old->id,line.id),"置換対象の基礎内にある土台は参照できません。先に外へ移してください");
    if (replace.empty()) for (const auto& candidate:document_.objects()) {
      const auto stored=candidate.attributes.find(kManifest); if(stored==candidate.attributes.end()) continue;
      const auto existing=json::parse(stored->second);
      if(existing.at("source_key")!=plan.key && existing.at("source_signature")!=plan.signature) continue;
      if(existing.at("source_signature")==plan.signature && existing.at("inputs")==plan.inputs && !modified(candidate,document_,existing)) {
        auto r=report(document_,candidate); r["unchanged"]=true; return r.dump();
      }
      throw std::runtime_error("requires_decision: 同じ土台の基礎計画があります。replaceで作り直してください: "+candidate.id);
    }
    Object root; root.id=old?old->id:generateObjectId(); root.name=args.value("name",old?old->name:std::string("布基礎計画"));
    root.recipe.base=GroupNode{}; root.layer="基礎・土台"; root.attributes["kind"]="foundation_plan";
    auto stem=network(document_,plan,root.id,false), foot=network(document_,plan,root.id,true);
    json manifest={{"version",1},{"inputs",plan.inputs},{"source_key",plan.key},{"source_signature",plan.signature},
                   {"roles",{{stem.id,"foundation_stem"},{foot.id,"foundation_footing"}}},
                   {"top_z",plan.top},{"parts",{{stem.id,construction::frameFingerprint(stem)},{foot.id,construction::frameFingerprint(foot)}}}};
    manifest["root_stamp"]=construction::frameFingerprint(root);
    root.attributes[kManifest]=manifest.dump();
    Command command; command.name="Generate Foundation Plan";
    if(old) {
      auto ids=document_.subtree(old->id);
      std::sort(ids.begin(),ids.end(),[&](const auto& a,const auto& b){return document_.indexOf(a)>document_.indexOf(b);});
      for(const auto& id:ids) if(id!=root.id) command.deltas.push_back({id,*document_.find(id),std::nullopt,document_.indexOf(id)});
    }
    command.deltas.push_back({root.id,old?std::optional<Object>(*old):std::nullopt,root,old?document_.indexOf(root.id):std::nullopt});
    command.deltas.push_back({stem.id,std::nullopt,stem,std::nullopt}); command.deltas.push_back({foot.id,std::nullopt,foot,std::nullopt});
    // Finish measurements and the result before the one model mutation.
    // A geometry/measurement failure must leave no partial foundation behind.
    const double stemVolume=geom::volume(stem.shape)/1e9, footVolume=geom::volume(foot.shape)/1e9;
    json r={{"foundation",root.id},{"name",root.name},{"inputs",plan.inputs},{"status","current"},
            {"verified",false},{"top_z",plan.top},{"generated_parts_volume_m3",stemVolume+footVolume},
            {"quantities",{{"foundation_stem",{{"id",stem.id},{"volume_m3",stemVolume}}},
                            {"foundation_footing",{{"id",foot.id},{"volume_m3",footVolume}}}}}};
    r["warnings"]={"布基礎の計画形状です。地盤・構造・配筋・開口等は未照査。寸法の初期値は設計値ではありません。",
      "対象は指定した土台下のみ。床束下の束石・土台のない耐力壁下・べた基礎は別途計画してください。"};
    const auto result=r.dump();
    history_.execute(std::move(command));
    return result;
  } catch(const Standard_Failure& e) { return Result<std::string>::failure(kernelMessage(e)); }
    catch(const std::exception& e) { return Result<std::string>::failure(e.what()); }
}
}
