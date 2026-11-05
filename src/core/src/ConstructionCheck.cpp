#include "sbr/ConstructionCheck.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

#include "sbr/ConstructionFrame.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/ConstructionWall.h"
#include "sbr/KernelError.h"

namespace sbr::construction {
namespace {
using namespace doc;
using json = nlohmann::json;
constexpr const char* kVersion = "construction-check-1";
const std::vector<std::string> kRules = {"COM-01", "COM-02", "COM-03", "MOD-01", "MOD-02", "TMB-01", "TMB-02",
                                         "TMB-03", "TMB-04", "LAT-01", "LAT-02", "LAT-03", "SEC-01"};
std::string attr(const Object& o, const std::string& key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? "" : it->second;
}
bool column(const std::string& role) { return role == "column" || role == "through_column"; }
bool beam(const std::string& role) {
  return std::set<std::string>{"floor_beam", "rim_beam", "eave_beam", "roof_beam", "floor_girder",
                               "lintel",     "beam",     "girder",    "purlin",    "ridge"}
      .count(role);
}
struct Member {
  const Object* object;
  std::optional<Object> expanded;
  const LinearMember* recipe = nullptr;
  gp_Pnt a, b, bottomA, bottomB;
  std::string role, frame;
  Bnd_Box bounds;
  TopoDS_Shape shape;
  bool edited = false;
  bool selected = true;
};
json point(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
double xyDistance(const gp_Pnt& a, const gp_Pnt& b) { return std::hypot(a.X() - b.X(), a.Y() - b.Y()); }
bool includesPoint(const TopoDS_Shape& shape, const gp_Pnt& p, double tol) {
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
    BRepClass3d_SolidClassifier classifier(ex.Current(), p, tol);
    if (classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON) return true;
    BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(p).Vertex(), ex.Current());
    if (distance.IsDone() && distance.Value() <= tol) return true;
  }
  return false;
}

bool localConnection(const Member& a, const Member& b, const Bnd_Box& overlap) {
  if (!a.recipe || !b.recipe || a.edited || b.edited) return false;
  gp_Vec da(a.a, a.b), db(b.a, b.b);
  if (da.Crossed(db).Magnitude() < 1e-6 * da.Magnitude() * db.Magnitude()) return false;
  // Exclude only a small end connection, never parallel duplicates or a
  // crossing at both members' middles. Detailed joint adequacy stays unknown.
  double x0, y0, z0, x1, y1, z1;
  overlap.Get(x0, y0, z0, x1, y1, z1);
  const double radius = std::max({a.recipe->section.width, a.recipe->section.depth, b.recipe->section.width,
                                  b.recipe->section.depth}) *
                        1.5;
  for (const auto& p : {a.a, a.b, b.a, b.b}) {
    const double dx = std::max(std::abs(x0 - p.X()), std::abs(x1 - p.X()));
    const double dy = std::max(std::abs(y0 - p.Y()), std::abs(y1 - p.Y()));
    const double dz = std::max(std::abs(z0 - p.Z()), std::abs(z1 - p.Z()));
    const double far = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (far <= radius) return true;
  }
  return false;
}
}  // namespace

Result<std::string> checkFrame(const doc::Document& d, const std::string& text,
                               const std::atomic<bool>* cancelled) {
  try {
    const auto started = std::chrono::steady_clock::now();
    const json options = json::parse(text);
    if (!options.is_object()) throw std::runtime_error("check_frame options must be an object");
    if (options.value("system", std::string("timber")) != "timber")
      throw std::runtime_error("Only the timber construction checker is implemented");
    const auto patch = d.settings().constructionOverrides.find("timber");
    const json guide = json::parse(
        guideData("timber", patch == d.settings().constructionOverrides.end() ? "{}" : patch->second));
    const json& table = guide.at("checks");
    const double tol = table.at("tolerance_mm");
    std::set<std::string> wanted;
    const auto rules = options.value("rules", json(kRules));
    if (!rules.is_array()) throw std::runtime_error("rules must be an array");
    for (const auto& r : rules) {
      const std::string id = r.get<std::string>();
      bool found = false;
      for (const auto& known : kRules)
        if (known == id ||
            (id.size() && id.back() == '*' && known.rfind(id.substr(0, id.size() - 1), 0) == 0)) {
          wanted.insert(known);
          found = true;
        }
      if (!found) throw std::runtime_error("Unknown construction rule: " + id);
    }
    const std::string scope = options.value("frame", std::string());
    if (!scope.empty()) {
      const auto* o = d.find(scope);
      const auto* g = o ? std::get_if<GroupNode>(&o->recipe.base) : nullptr;
      if (!g || g->constructionFrame.empty())
        throw std::runtime_error("frame must name a generated frame group");
    }
    auto stop = [&] {
      if (cancelled && *cancelled) throw std::runtime_error("Construction check cancelled");
    };
    std::map<std::string, json> manifests;
    for (const auto& o : d.objects()) {
      const auto* group = std::get_if<GroupNode>(&o.recipe.base);
      if (group && !group->constructionFrame.empty())
        manifests[o.id] = json::parse(group->constructionFrame, nullptr, false);
    }
    std::vector<Member> members;
    // Reserve: recipe pointers into expanded objects remain stable.
    members.reserve(d.objects().size());
    for (const auto& o : d.objects()) {
      stop();
      if (attr(o, "material_class") != "timber" || attr(o, "display_only") == "true" || o.isContainer())
        continue;
      members.push_back({&o});
      Member& m = members.back();
      m.selected = scope.empty() || d.isAncestor(scope, o.id);
      m.role = attr(o, "kind");
      for (auto parent = o.parent; !parent.empty();) {
        if (manifests.count(parent)) {
          m.frame = parent;
          break;
        }
        const auto* node = d.find(parent);
        if (!node) break;
        parent = node->parent;
      }
      m.expanded = frameMemberObject(d, o);
      const Object& own = m.expanded ? *m.expanded : o;
      m.recipe = std::get_if<LinearMember>(&own.recipe.base);
      m.edited = !own.recipe.modifiers.empty();
      m.shape = d.worldShape(o);
      m.bounds = d.worldBounds(o);
      if (m.recipe) {
        const auto world = d.parentTransform(o) * own.transform;
        m.a = m.recipe->start.Transformed(world);
        m.b = m.recipe->end.Transformed(world);
        const auto frame = geom::memberFrame(m.recipe->start, m.recipe->end, m.recipe->up, m.recipe->roll);
        if (frame) {
          const gp_Vec down =
              -gp_Vec(frame->YDirection()) * ((m.recipe->anchor.up + 1) * m.recipe->section.depth / 2) -
              gp_Vec(frame->XDirection()) * (m.recipe->anchor.across * m.recipe->section.width / 2);
          m.bottomA = m.recipe->start.Translated(down).Transformed(world);
          m.bottomB = m.recipe->end.Translated(down).Transformed(world);
        }
      }
    }
    const auto selectedCount =
        std::count_if(members.begin(), members.end(), [](const Member& m) { return m.selected; });
    json results = json::array();
    auto emit = [&](const std::string& rule, const std::vector<std::string>& ids, const std::string& status,
                    const std::string& reason, const std::string& message, json measured = json::object(),
                    json required = json::object()) {
      if (!wanted.count(rule)) return;
      results.push_back({{"rule", rule},
                         {"targets", ids},
                         {"status", status},
                         {"reason_code", reason},
                         {"message", message},
                         {"measured", measured},
                         {"required", required},
                         {"source", table.at("source")},
                         {"normative", table.at("normative")},
                         {"checker_version", kVersion}});
    };
    auto missing = [&](const std::string& rule, const Member& m, const std::string& why) {
      emit(rule, {m.object->id}, "not_evaluated", "insufficient_design_information", why);
    };
    auto level = [&](const Member& m, const char* key) -> std::optional<double> {
      const auto it = manifests.find(m.frame);
      if (it == manifests.end() || !it->second.is_object()) return {};
      const auto& ids = it->second.value("levels", json::object());
      if (!ids.contains(key)) return {};
      for (const auto& l : d.settings().levels)
        if (l.id == ids.at(key)) return l.elevation;
      return {};
    };
    auto gridFor = [&](const Member& m) -> const Object* {
      if (m.object->gridPin) return d.find(m.object->gridPin->grid);
      const auto it = manifests.find(m.frame);
      if (it != manifests.end() && it->second.is_object())
        return d.find(it->second.value("grid", std::string()));
      const Object* found = nullptr;
      for (const auto& o : d.objects()) {
        const auto* g = std::get_if<GroupNode>(&o.recipe.base);
        if (g && g->grid) {
          if (found) return nullptr;
          found = &o;
        }
      }
      return found;
    };
    struct Bearing {
      // side: the end sits inside the side of a beam at the same level (a framed-in joint).
      std::vector<std::string> known, unknown, side;
    };
    auto support = [&](const Member& m, const gp_Pnt& p, bool belowColumn) {
      Bearing supports;
      for (const auto& other : members) {
        if (other.object == m.object || other.shape.IsNull()) continue;
        const bool roleKnown = column(other.role) || beam(other.role) || other.role == "sill" ||
                               (!belowColumn && other.role == "roof_post");
        if (other.bounds.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        other.bounds.Get(x0, y0, z0, x1, y1, z1);
        if (p.X() < x0 - tol || p.X() > x1 + tol || p.Y() < y0 - tol || p.Y() > y1 + tol ||
            p.Z() < z0 - tol || p.Z() > z1 + tol)
          continue;
        if (z0 >= p.Z() - tol) {
          // Not bearing from below. A beam end framed into the side of another beam at the
          // same level is still a connection, whose joint (仕口) is not evaluated here.
          if (!belowColumn && beam(other.role) && z1 > p.Z() + tol && includesPoint(other.shape, p, tol))
            supports.side.push_back(other.object->id);
          continue;
        }
        if (includesPoint(other.shape, p, tol))
          (roleKnown ? supports.known : supports.unknown).push_back(other.object->id);
      }
      return supports;
    };
    for (const auto& m : members) {
      stop();
      if (!m.selected) continue;
      const auto ids = std::vector<std::string>{m.object->id};
      if (wanted.count("COM-03")) {
        if (!m.recipe || m.edited)
          missing("COM-03", m, "現在の一本材の長さをレシピから確定できません");
        else {
          double stock = guide.at("presets").at("entries").at("stock_length").at("value");
          std::string basis = "catalog_preset";
          const auto it = manifests.find(m.frame);
          if (it != manifests.end() && it->second.is_object() &&
              it->second.at("inputs").contains("stock_length")) {
            stock = it->second.at("inputs").at("stock_length");
            basis = "frame_input";
          }
          const double length = m.a.Distance(m.b);
          emit("COM-03", ids, length <= stock + tol ? "pass" : "fail",
               length <= stock + tol ? "within_stock" : "joint_required",
               length <= stock + tol ? "指定定尺内です"
                                     : "指定定尺を超えています。継手の目印だけでは仕様確定になりません",
               {{"length", length}, {"unit", "mm"}},
               {{"stock_length", stock},
                {"unit", "mm"},
                {"basis", basis},
                {"source", basis == "frame_input" ? json{{"type", "user_input"}, {"frame", m.frame}}
                                                  : guide.at("presets").at("source")},
                {"override_source",
                 guide.value("overrides", json::object()).value("source", json::object())}});
        }
      }
      if (column(m.role) && wanted.count("MOD-01")) {
        const auto* grid = gridFor(m);
        const auto* g = grid ? std::get_if<GroupNode>(&grid->recipe.base) : nullptr;
        if (!m.recipe || !g || !g->grid || g->grid->x.empty() || g->grid->y.empty())
          missing("MOD-01", m, "柱の設計芯、または対応する通り芯がありません");
        else if (!attr(*m.object, "module_exception").empty())
          emit("MOD-01", ids, "not_applicable", "explicit_exception", attr(*m.object, "module_exception"));
        else {
          const auto tr = d.worldTransform(*grid);
          if (std::abs(tr.ScaleFactor() - 1) > 1e-7 || std::abs(tr.Value(3, 3) - 1) > 1e-7)
            missing("MOD-01", m, "水平・等倍の通り芯以外は未対応です");
          else {
            double offset = 0;
            for (const auto& p : {m.a, m.b}) {
              const auto q = p.Transformed(tr.Inverted());
              double dx = 1e100, dy = 1e100;
              for (const auto& axis : g->grid->x) dx = std::min(dx, std::abs(q.X() - axis.offset));
              for (const auto& axis : g->grid->y) dy = std::min(dy, std::abs(q.Y() - axis.offset));
              offset = std::max(offset, std::hypot(dx, dy));
            }
            emit("MOD-01", ids, offset <= tol ? "pass" : "fail", offset <= tol ? "on_grid" : "off_grid",
                 offset <= tol ? "設計芯が通り芯交点に乗っています" : "設計芯が通り芯交点から外れています",
                 {{"max_offset", offset}, {"unit", "mm"}, {"basis", "design_axis"}},
                 {{"tolerance", tol}, {"unit", "mm"}});
          }
        }
      }
      if ((m.role == "joist" || m.role == "rafter") && wanted.count("MOD-02")) {
        const auto f = manifests.find(m.frame);
        const auto* grid = gridFor(m);
        if (!m.recipe || m.edited || !grid || f == manifests.end() || !f->second.is_object() ||
            !f->second.contains("allocation") || !f->second.at("allocation").contains(m.object->id))
          missing("MOD-02", m, "割付の設計位置の記録がありません。段階3以降に生成した架構が対象です");
        else if (std::abs(d.worldTransform(*grid).ScaleFactor() - 1) > 1e-7 ||
                 std::abs(d.worldTransform(*grid).Value(3, 3) - 1) > 1e-7)
          missing("MOD-02", m, "水平・等倍の通り芯以外は未対応です");
        else {
          const json expected = f->second.at("allocation").at(m.object->id);
          const auto inv = d.worldTransform(*grid).Inverted();
          const auto a = m.a.Transformed(inv), b = m.b.Transformed(inv);
          auto read = [](const json& p) { return gp_Pnt(p[0].get<double>(), p[1].get<double>(), 0); };
          const double delta =
              std::max(xyDistance(a, read(expected.at("start"))), xyDistance(b, read(expected.at("end"))));
          emit("MOD-02", ids, delta <= tol ? "pass" : "fail",
               delta <= tol ? "matches_allocation" : "allocation_changed",
               delta <= tol ? "指定した割付の平面位置に一致します" : "指定した割付の平面位置から外れています",
               {{"max_plan_offset", delta}, {"unit", "mm"}},
               {{"allocation", expected}, {"tolerance", tol}, {"unit", "mm"}});
        }
      }
      if (column(m.role) || beam(m.role) || m.role == "roof_post" || m.role == "floor_post") {
        const bool vertical = column(m.role) || m.role == "roof_post" || m.role == "floor_post";
        const auto base = m.recipe ? (m.a.Z() < m.b.Z() ? m.a : m.b) : gp::Origin();
        const auto upper = level(m, "second_fl");
        const bool startsUpper =
            m.object->gridPin && !m.frame.empty() && upper &&
            manifests.at(m.frame).at("levels").at("second_fl") == m.object->gridPin->startLevel;
        if (wanted.count("TMB-01") && column(m.role)) {
          if (!m.frame.empty() && manifests.at(m.frame).is_object() &&
              manifests.at(m.frame).at("inputs").value("storeys", 2) == 1)
            emit("TMB-01", ids, "not_applicable", "no_second_storey", "平屋のため2階の柱はありません");
          else if (!m.recipe || m.edited || !upper)
            missing("TMB-01", m, "柱芯・階の基準高さ・未編集の一本材が必要です");
          else if (!startsUpper && std::min(m.a.Z(), m.b.Z()) < *upper - tol &&
                   std::max(m.a.Z(), m.b.Z()) > *upper + tol)
            emit("TMB-01", ids, "pass", "continuous_column", "同じ一本材の柱が2階高さをまたいでいます");
          else if (base.Z() < *upper - tol && !startsUpper)
            emit("TMB-01", ids, "not_applicable", "not_upper_column", "2階の柱ではありません");
          else {
            const auto found = support(m, base, true);
            emit("TMB-01", ids,
                 !found.known.empty()     ? "pass"
                 : !found.unknown.empty() ? "not_evaluated"
                                          : "fail",
                 !found.known.empty()     ? "lower_bearing_found"
                 : !found.unknown.empty() ? "bearing_role_unknown"
                                          : "no_lower_bearing",
                 found.known.empty() ? "柱脚の受け材がないか、接する形の役割を確認できません"
                                     : "柱脚に接する下階の柱・受け梁があります",
                 {{"at", point(base)},
                  {"supports", found.known},
                  {"unclassified", found.unknown},
                  {"unit", "mm"}},
                 {{"contact_tolerance", tol}, {"unit", "mm"}});
          }
        }
        for (const auto& rule : {std::string("COM-02"), std::string("TMB-02")}) {
          if (!wanted.count(rule) || (rule == "TMB-02" && vertical)) continue;
          if (!m.recipe || m.edited || (vertical && xyDistance(m.a, m.b) > tol) ||
              (!vertical && std::abs(m.a.Z() - m.b.Z()) > tol)) {
            missing(rule, m, "編集後の形や斜材の支えは未対応です");
            continue;
          }
          for (const auto& p :
               vertical ? std::vector<gp_Pnt>{base} : std::vector<gp_Pnt>{m.bottomA, m.bottomB}) {
            const auto found = support(m, p, vertical);
            const bool side = found.known.empty() && found.unknown.empty() && !found.side.empty();
            emit(rule, ids,
                 !found.known.empty()     ? "pass"
                 : !found.unknown.empty() || side ? "not_evaluated"
                                          : "fail",
                 !found.known.empty()     ? "bearing_contact"
                 : !found.unknown.empty() ? "bearing_role_unknown"
                 : side                   ? "side_joint_specification_missing"
                                          : "bearing_not_found",
                 !found.known.empty() ? "端部の下側に受け材との接触があります（接合耐力は未評価）"
                 : side ? "端部が同じ高さの梁の側面に取り付いています。仕口の仕様・耐力は未評価です"
                        : "端部の受け材がないか、接する形の役割を確認できません",
                 {{"at", point(p)},
                  {"supports", found.known},
                  {"unclassified", found.unknown},
                  {"side_joints", found.side},
                  {"unit", "mm"}},
                 {{"contact_tolerance", tol}, {"unit", "mm"}});
          }
        }
      }
      if (m.role == "through_column" && wanted.count("TMB-03")) {
        const auto first = level(m, "first_fl"), second = level(m, "second_fl");
        if (!m.recipe || m.edited || !first || !second)
          missing("TMB-03", m, "一本材の連続性、1FL・2FLを確認できません");
        else {
          const bool continuous = std::min(m.a.Z(), m.b.Z()) <= *first + tol &&
                                  std::max(m.a.Z(), m.b.Z()) >= *second - tol && xyDistance(m.a, m.b) <= tol;
          emit("TMB-03", ids, continuous ? "pass" : "fail",
               continuous ? "spans_both_storeys" : "not_continuous",
               continuous ? "未編集の一本材が1FLから2FLまで続きます"
                          : "通し柱の芯が両階を連続して通っていません",
               {{"start_z", std::min(m.a.Z(), m.b.Z())},
                {"end_z", std::max(m.a.Z(), m.b.Z())},
                {"unit", "mm"}},
               {{"first_fl", *first}, {"second_fl", *second}, {"unit", "mm"}});
        }
      }
    }
    if (wanted.count("MOD-02"))
      for (const auto& [id, manifest] : manifests) {
        if ((!scope.empty() && scope != id) || !manifest.is_object() || !manifest.contains("allocation"))
          continue;
        for (const auto& [target, unused] : manifest.at("allocation").items())
          if (!d.find(target))
            emit("MOD-02", {id}, "fail", "allocation_member_missing", "割付に記録した部材が削除されています",
                 {{"missing_id", target}});
      }
    bool complete = true;
    if (wanted.count("COM-01") && selectedCount > 0) {
      int pairs = 0, hits = 0, exempt = 0, unknown = 0;
      for (const auto& m : members)
        if (m.selected && !TopExp_Explorer(m.shape, TopAbs_SOLID).More()) {
          ++unknown;
          emit("COM-01", {m.object->id}, "not_evaluated", "non_solid_geometry",
               "閉じた立体でないため、体積によるぶつかりを評価できません");
        }
      const int budget = options.value("pair_budget", 2000);
      if (budget < 1 || budget > 20000) throw std::runtime_error("pair_budget must be 1 to 20000");
      for (std::size_t i = 0; i < members.size(); ++i) {
        stop();
        for (std::size_t j = i + 1; j < members.size(); ++j) {
          const auto& a = members[i];
          const auto& b = members[j];
          if (!a.selected && !b.selected) continue;
          if (a.bounds.IsVoid() || b.bounds.IsVoid() || a.bounds.IsOut(b.bounds)) continue;
          double ax, ay, az, aX, aY, aZ, bx, by, bz, bX, bY, bZ;
          a.bounds.Get(ax, ay, az, aX, aY, aZ);
          b.bounds.Get(bx, by, bz, bX, bY, bZ);
          const double x = std::max(ax, bx), y = std::max(ay, by), z = std::max(az, bz), X = std::min(aX, bX),
                       Y = std::min(aY, bY), Z = std::min(aZ, bZ);
          if (X - x <= tolerance::kLinear || Y - y <= tolerance::kLinear || Z - z <= tolerance::kLinear)
            continue;
          if (++pairs > budget) {
            complete = false;
            break;
          }
          try {
            TopTools_ListOfShape arguments, tools;
            arguments.Append(a.shape);
            tools.Append(b.shape);
            BRepAlgoAPI_Common common;
            common.SetArguments(arguments);
            common.SetTools(tools);
            common.SetNonDestructive(true);
            common.Build();
            if (!common.IsDone() || common.HasErrors()) {
              ++unknown;
              continue;
            }
            GProp_GProps props;
            BRepGProp::VolumeProperties(common.Shape(), props);
            const double volume = std::abs(props.Mass());
            // Decide "local end connection" from the real shared solid, not the two members'
            // axis-aligned boxes: for sloped members (rafters meeting at the ridge) the boxes
            // overlap over the whole roof height even when the contact is at their ends.
            Bnd_Box overlap;
            BRepBndLib::AddOptimal(common.Shape(), overlap, false, false);
            if (volume > 1.0 && !overlap.IsVoid() && localConnection(a, b, overlap)) {
              ++exempt;
              continue;
            }
            if (volume > 1.0) {
              ++hits;
              emit("COM-01", {a.object->id, b.object->id}, "fail", "solid_intersection",
                   "局所的な端部接合を除いた木材どうしに体積の重なりがあります",
                   {{"intersection_volume", volume}, {"unit", "mm3"}},
                   {{"minimum_volume", 1.0}, {"unit", "mm3"}});
            }
          } catch (...) {
            ++unknown;
          }
        }
        if (!complete) break;
      }
      if (unknown) complete = false;
      emit("COM-01", {},
           !complete ? "not_evaluated"
           : hits    ? "fail"
           : exempt  ? "not_evaluated"
                     : "pass",
           !complete ? "collision_check_incomplete"
           : hits    ? "collisions_found"
           : exempt  ? "end_connection_specification_missing"
                     : "no_collision_found",
           !complete ? "ぶつかり検査が完了していません。上限または形状演算の未完了があります"
                     : "木材の実形状を検査しました。局所的な端部接合の範囲は除外し、その接合仕様・耐力は評価"
                       "しません",
           {{"candidate_pairs", std::min(pairs, budget)},
            {"collisions", hits},
            {"excluded_end_connections", exempt},
            {"geometry_failures", unknown}},
           {{"pair_budget", budget}});
    }
    emit("TMB-04", {}, "not_evaluated", "spacing_reference_missing", "柱間隔の目安と出典の表が未登録です");
    if (wanted.count("LAT-01")) {
      json args = json::object();
      if (!scope.empty()) args["frame"] = scope;
      const auto checked = checkWallQuantity(d, args.dump());
      if (!checked) {
        emit("LAT-01", {}, "not_evaluated", "wall_inputs_invalid", checked.error());
        complete = false;
      } else {
        const auto report = json::parse(*checked);
        for (const auto& row : report.at("rows")) {
          const auto status = row.at("status").get<std::string>();
          auto message = row.at("floor").get<std::string>() + " " + row.at("direction").get<std::string>();
          for (const auto& reason : row.at("reasons")) message += "／" + reason.get<std::string>();
          message += " " + report.at("notice").get<std::string>();
          emit("LAT-01", {}, status,
               status == "not_evaluated" ? "wall_inputs_incomplete" : "wall_quantity_comparison",
               message, row);
          results.back()["wall_table"] = report.at("table_snapshot");
          results.back()["notice"] = report.at("notice");
          results.back()["calculation_checker_version"] = report.at("checker_version");
          results.back()["calculation_complete"] = report.at("complete");
          if (!report.at("table_snapshot").is_null()) {
            results.back()["source"] = report.at("table_snapshot").at("source");
            results.back()["normative"] = report.at("table_snapshot").at("normative");
          }
        }
      }
    }
    for (const auto& id : {"LAT-02", "LAT-03"})
      emit(id, {}, "not_evaluated", "calculation_not_implemented",
           "四分割法・N値の検査器と根拠表は未実装です");
    emit("SEC-01", {}, "not_applicable", "outside_stage_scope", "断面照査は今回の対象外です");
    // An empty selection cannot earn a green rule result.
    if (selectedCount == 0)
      for (const auto& rule : wanted)
        if (rule.rfind("LAT-", 0) != 0 && rule != "SEC-01" && rule != "TMB-04")
          emit(rule, {}, "not_applicable", "no_timber_members", "対象となる木材がありません");
    for (const auto& rule : wanted) {
      const bool present =
          std::any_of(results.begin(), results.end(), [&](const json& r) { return r.at("rule") == rule; });
      if (!present)
        emit(rule, {}, "not_applicable", "no_applicable_members", "このルールの対象となる部材がありません");
    }
    json counts = {{"pass", 0}, {"fail", 0}, {"not_evaluated", 0}, {"not_applicable", 0}};
    for (const auto& r : results) {
      const auto status = r.at("status").get<std::string>();
      counts[status] = counts.at(status).get<int>() + 1;
    }
    json frameSettings = json::object();
    for (const auto& [id, manifest] : manifests)
      if (manifest.is_object() && (scope.empty() || scope == id))
        frameSettings[id] = {{"spacing", manifest.value("spacing", json::array())},
                             {"inputs", manifest.value("inputs", json::object())}};
    return json{
        {"system", "timber"},
        {"checker_version", kVersion},
        {"results", results},
        {"counts", counts},
        {"complete", complete},
        {"members", selectedCount},
        {"frame_settings", frameSettings},
        {"notice", guide.at("notice")},
        {"elapsed_ms",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()}}
        .dump();
  } catch (const Standard_Failure& e) {
    return Result<std::string>::failure(kernelMessage(e));
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}
}  // namespace sbr::construction
