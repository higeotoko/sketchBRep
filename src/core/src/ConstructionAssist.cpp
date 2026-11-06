#include "sbr/ConstructionAssist.h"

#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <nlohmann/json.hpp>

#include "sbr/display/DisplayMesh.h"

namespace sbr::construction {
namespace {
using json = nlohmann::json;
constexpr const char* kVersion = "structure-assist-1";
constexpr double kTol = 2.0;  // mm: contact between members' boxes

void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error("structure_assist: " + message);
}
std::string attribute(const doc::Document& d, const doc::Object& o, const std::string& key) {
  const auto own = o.attributes.find(key);
  if (own != o.attributes.end()) return own->second;
  const auto* instance = std::get_if<doc::ComponentInstance>(&o.recipe.base);
  const auto* definition = instance ? d.findDefinition(instance->definitionId) : nullptr;
  if (definition && definition->objects.size() == 1) {
    const auto inherited = definition->objects[0].attributes.find(key);
    if (inherited != definition->objects[0].attributes.end()) return inherited->second;
  }
  return {};
}
struct Box {
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
};
Box boxOf(const Bnd_Box& b) {
  Box r;
  b.Get(r.x0, r.y0, r.z0, r.x1, r.y1, r.z1);
  const double g = b.GetGap();
  r.x0 += g; r.y0 += g; r.z0 += g; r.x1 -= g; r.y1 -= g; r.z1 -= g;
  return r;
}
double round1(double v) { return std::round(v * 10) / 10; }
double round2(double v) { return std::round(v * 100) / 100; }

// A coverage raster on a plane (u, v): cells are filled where any triangle covers the cell centre.
struct Raster {
  double u0, v0, cell;
  int nu, nv;
  std::vector<std::uint8_t> bits;
  Raster(double uMin, double uMax, double vMin, double vMax, double c) : u0(uMin), v0(vMin), cell(c) {
    nu = std::max(1, static_cast<int>(std::ceil((uMax - uMin) / c)));
    nv = std::max(1, static_cast<int>(std::ceil((vMax - vMin) / c)));
    require(static_cast<double>(nu) * nv <= 4.0e7, "the raster is too large; use a larger cell_mm");
    bits.assign(static_cast<std::size_t>(nu) * nv, 0);
  }
  void triangle(double au, double av, double bu, double bv, double cu, double cv) {
    const double area = (bu - au) * (cv - av) - (bv - av) * (cu - au);
    if (std::abs(area) < 1e-9) return;
    const int i0 = std::max(0, static_cast<int>(std::floor((std::min({au, bu, cu}) - u0) / cell)));
    const int i1 = std::min(nu - 1, static_cast<int>(std::floor((std::max({au, bu, cu}) - u0) / cell)));
    const int j0 = std::max(0, static_cast<int>(std::floor((std::min({av, bv, cv}) - v0) / cell)));
    const int j1 = std::min(nv - 1, static_cast<int>(std::floor((std::max({av, bv, cv}) - v0) / cell)));
    const double s = area > 0 ? 1 : -1;
    for (int j = j0; j <= j1; ++j) {
      const double pv = v0 + (j + .5) * cell;
      for (int i = i0; i <= i1; ++i) {
        const double pu = u0 + (i + .5) * cell;
        const double e0 = s * ((bu - au) * (pv - av) - (bv - av) * (pu - au));
        const double e1 = s * ((cu - bu) * (pv - bv) - (cv - bv) * (pu - bu));
        const double e2 = s * ((au - cu) * (pv - cv) - (av - cv) * (pu - cu));
        if (e0 >= -1e-9 && e1 >= -1e-9 && e2 >= -1e-9) bits[static_cast<std::size_t>(j) * nu + i] = 1;
      }
    }
  }
  // Filled area (m2) of the cells whose centre is at or above v.
  double areaAbove(double v) const {
    std::size_t n = 0;
    for (int j = 0; j < nv; ++j) {
      if (v0 + (j + .5) * cell < v) continue;
      for (int i = 0; i < nu; ++i) n += bits[static_cast<std::size_t>(j) * nu + i];
    }
    return n * cell * cell / 1e6;
  }
  double area() const { return areaAbove(-1e300); }
  // Extent of the filled cells: [u0, u1, v0, v1] in model units, or nothing.
  std::optional<std::array<double, 4>> extent() const {
    int a = nu, b = -1, c = nv, e = -1;
    for (int j = 0; j < nv; ++j)
      for (int i = 0; i < nu; ++i)
        if (bits[static_cast<std::size_t>(j) * nu + i]) {
          a = std::min(a, i); b = std::max(b, i); c = std::min(c, j); e = std::max(e, j);
        }
    if (b < 0) return std::nullopt;
    return std::array<double, 4>{u0 + a * cell, u0 + (b + 1) * cell, v0 + c * cell, v0 + (e + 1) * cell};
  }
};

struct Mesh {
  std::vector<std::array<gp_Pnt, 3>> triangles;
};
Mesh meshOf(const TopoDS_Shape& shape, double deflection) {
  Mesh out;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  if (faces.Extent() == 0) return out;
  const TopoDS_Shape copy = display::makeDisplayCopy(shape);
  BRepMesh_IncrementalMesh mesher(copy, deflection, Standard_False, 0.5, Standard_True);
  TopTools_IndexedMapOfShape meshed;
  TopExp::MapShapes(copy, TopAbs_FACE, meshed);
  for (int i = 1; i <= meshed.Extent(); ++i) {
    TopLoc_Location location;
    const auto t = BRep_Tool::Triangulation(TopoDS::Face(meshed(i)), location);
    if (t.IsNull()) continue;
    const gp_Trsf trsf = location.Transformation();
    for (int k = 1; k <= t->NbTriangles(); ++k) {
      int a, b, c;
      t->Triangle(k).Get(a, b, c);
      out.triangles.push_back({t->Node(a).Transformed(trsf), t->Node(b).Transformed(trsf), t->Node(c).Transformed(trsf)});
    }
  }
  return out;
}

struct Member {
  std::string id, name, role;
  Box box;
  bool column = false, horizontal = false, alongX = false;
};
struct Storey {
  std::string id, name;
  double fl = 0;
};
}  // namespace

Result<std::string> structureAssist(const doc::Document& d, const std::string& options) {
  try {
    const json opt = json::parse(options);
    require(opt.is_object(), "options must be an object");
    for (const auto& [key, unused] : opt.items()) {
      (void)unused;
      require(key == "storeys" || key == "cell_mm" || key == "wind_cut_mm" || key == "span_depth_limit" ||
                  key == "limit_basis" || key == "floor_band_mm",
              "unknown option: " + key);
    }
    const double cell = opt.value("cell_mm", 25.0);
    require(cell >= 5 && cell <= 500, "cell_mm must be 5..500");
    const double cut = opt.value("wind_cut_mm", 1350.0);
    require(cut >= 0 && cut <= 10000, "wind_cut_mm must be 0..10000");
    const double band = opt.value("floor_band_mm", 400.0);
    std::optional<double> limit;
    if (opt.contains("span_depth_limit")) {
      require(opt.at("span_depth_limit").is_number() && opt.at("span_depth_limit").get<double>() > 0,
              "span_depth_limit must be a positive number");
      require(opt.contains("limit_basis") && opt.at("limit_basis").is_string() &&
                  !opt.at("limit_basis").get<std::string>().empty(),
              "limit_basis (where the limit comes from) is required with span_depth_limit");
      limit = opt.at("span_depth_limit").get<double>();
    }

    // --- storeys: given, or the levels named "<n>FL" (1FL, 2FL ...)
    std::vector<Storey> storeys;
    json notes = json::array();
    if (opt.contains("storeys")) {
      require(opt.at("storeys").is_array(), "storeys must be [{id, fl}]");
      for (const auto& s : opt.at("storeys")) {
        require(s.is_object() && s.contains("id") && s.at("id").is_string() && s.contains("fl") && s.at("fl").is_number(),
                "each storey needs id and fl (world z of the floor, mm)");
        storeys.push_back({s.at("id"), s.value("name", s.at("id").get<std::string>()), s.at("fl")});
      }
    } else {
      for (int n = 1; n <= 9; ++n) {
        const std::string token = std::to_string(n) + "FL";
        for (const auto& level : d.settings().levels) {
          const auto& name = level.name;
          if (name.size() < token.size() || name.compare(name.size() - token.size(), token.size(), token) != 0) continue;
          if (name.size() > token.size() && name[name.size() - token.size() - 1] != ' ') continue;
          storeys.push_back({std::to_string(n) + "F", std::to_string(n) + "階", level.elevation});
          break;
        }
      }
      if (storeys.empty()) notes.push_back("階の床の高さが分かりません（レベル「1FL」「2FL」… がないため）。storeys で指定してください");
      else notes.push_back("階の床の高さはレベル「<n>FL」から取りました");
    }
    std::sort(storeys.begin(), storeys.end(), [](const Storey& a, const Storey& b) { return a.fl < b.fl; });
    auto storeyOf = [&](double z) -> int {
      int k = -1;
      for (std::size_t i = 0; i < storeys.size(); ++i)
        if (z >= storeys[i].fl - 700) k = static_cast<int>(i);
      return k;
    };

    // --- collect shapes and members
    std::vector<Mesh> meshes;
    std::vector<std::pair<const doc::Object*, Box>> slabs;
    std::vector<Member> members;
    Bnd_Box all;
    std::size_t skipped = 0;
    for (const auto& o : d.objects()) {
      if (o.isContainer() || !d.isVisible(o.id) || o.layer == "Grid") continue;
      const auto shape = d.worldShape(o);
      if (shape.IsNull()) continue;
      const Bnd_Box bb = d.worldBounds(o);
      if (bb.IsVoid()) continue;
      const Box b = boxOf(bb);
      const auto kind = attribute(d, o, "kind");
      if (kind == "slab") slabs.emplace_back(&o, b);
      if (attribute(d, o, "material_class") == "timber" && attribute(d, o, "display_only") != "true") {
        Member m{o.id, o.name, kind, b};
        const double dx = b.x1 - b.x0, dy = b.y1 - b.y0, dz = b.z1 - b.z0;
        m.column = (kind == "column" || kind == "through_column" || kind == "roof_post") && dz > std::max(dx, dy);
        m.horizontal = !m.column && std::max(dx, dy) > 2 * dz && std::max(dx, dy) > 3 * std::min(dx, dy);
        m.alongX = dx >= dy;
        members.push_back(m);
      }
      meshes.push_back(meshOf(shape, std::max(1.0, cell / 5)));
      if (meshes.back().triangles.empty()) { meshes.pop_back(); ++skipped; continue; }
      all.Add(bb);
    }
    json out = {{"system", "timber"},
                {"version", kVersion},
                {"sequence", d.sequence()},
                {"notice", "モデルの形から読み取った数値です。設計者が確かめてから壁量計算などに入力してください。"
                           "荷重・接合・断面・法適合は評価していません。"},
                {"notes", notes},
                {"cell_mm", cell},
                {"storeys", json::array()}};
    for (const auto& s : storeys) out["storeys"].push_back({{"id", s.id}, {"name", s.name}, {"fl", s.fl}});

    // --- wind projected areas: X (force along X) = projection on the YZ plane, Y = on XZ
    json wind = json::array();
    if (!all.IsVoid() && !storeys.empty()) {
      const Box w = boxOf(all);
      Raster yz(w.y0 - cell, w.y1 + cell, w.z0 - cell, w.z1 + cell, cell);
      Raster xz(w.x0 - cell, w.x1 + cell, w.z0 - cell, w.z1 + cell, cell);
      for (const auto& m : meshes)
        for (const auto& t : m.triangles) {
          yz.triangle(t[0].Y(), t[0].Z(), t[1].Y(), t[1].Z(), t[2].Y(), t[2].Z());
          xz.triangle(t[0].X(), t[0].Z(), t[1].X(), t[1].Z(), t[2].X(), t[2].Z());
        }
      for (const auto& s : storeys)
        wind.push_back({{"floor", s.id}, {"cut_z", s.fl + cut}, {"wind_x_m2", round2(yz.areaAbove(s.fl + cut))},
                        {"wind_y_m2", round2(xz.areaAbove(s.fl + cut))}});
      out["building_top_z"] = round1(w.z1);
    }
    out["wind"] = wind;
    out["wind_basis"] = "見える面・立体を鉛直面に投影した外形の面積（セル" + std::to_string(static_cast<int>(cell)) +
                        "mm）。各階の床面＋" + std::to_string(static_cast<int>(cut)) +
                        "mm以下を除く。X＝X方向の力を受ける面（YZ面への投影）。通り芯は除外。";

    // --- floor slabs per storey (kind = slab, top within the band below the floor level)
    json floors = json::array();
    for (const auto& s : storeys) {
      std::vector<std::pair<const doc::Object*, Box>> here;
      for (const auto& [o, b] : slabs)
        if (b.z1 >= s.fl - band && b.z1 <= s.fl + 40) here.emplace_back(o, b);
      json row = {{"floor", s.id}, {"slabs", json::array()}, {"area_m2", nullptr}, {"plan", nullptr}};
      if (!here.empty()) {
        Box u = here[0].second;
        for (const auto& [o, b] : here) {
          u.x0 = std::min(u.x0, b.x0); u.y0 = std::min(u.y0, b.y0); u.x1 = std::max(u.x1, b.x1); u.y1 = std::max(u.y1, b.y1);
        }
        Raster plan(u.x0 - cell, u.x1 + cell, u.y0 - cell, u.y1 + cell, cell);
        for (const auto& [o, b] : here) {
          Raster one(b.x0 - cell, b.x1 + cell, b.y0 - cell, b.y1 + cell, cell);
          for (const auto& t : meshOf(d.worldShape(*o), std::max(1.0, cell / 5)).triangles) {
            plan.triangle(t[0].X(), t[0].Y(), t[1].X(), t[1].Y(), t[2].X(), t[2].Y());
            one.triangle(t[0].X(), t[0].Y(), t[1].X(), t[1].Y(), t[2].X(), t[2].Y());
          }
          row["slabs"].push_back({{"id", o->id}, {"name", o->name}, {"top", round1(b.z1)}, {"area_m2", round2(one.area())}});
        }
        row["area_m2"] = round2(plan.area());
        if (const auto e = plan.extent()) row["plan"] = {{"x", {round1((*e)[0]), round1((*e)[1])}}, {"y", {round1((*e)[2]), round1((*e)[3])}}};
      }
      floors.push_back(row);
    }
    out["floor_slabs"] = floors;
    out["floor_basis"] = "kind=slab の物（床合板・土間など）のうち、天端が各階の床面の" + std::to_string(static_cast<int>(band)) +
                         "mm下〜40mm上にあるものの平面の和。バルコニー・吹抜けなど床面積に入れない物が含まれていないか確認すること。";

    // --- column alignment (直下率) and posts standing on beams (陸立ち)
    std::vector<std::vector<const Member*>> posts(storeys.size());
    for (const auto& m : members)
      if (m.column && (m.role == "column" || m.role == "through_column")) {
        const int k = storeyOf(m.box.z0);
        if (k >= 0) posts[k].push_back(&m);
        // a through column also counts in the storeys it passes
        for (std::size_t i = k + 1; k >= 0 && i < storeys.size(); ++i)
          if (m.box.z1 > storeys[i].fl + 100) posts[i].push_back(&m);
      }
    auto cx = [](const Box& b) { return (b.x0 + b.x1) / 2; };
    auto cy = [](const Box& b) { return (b.y0 + b.y1) / 2; };
    json alignment = json::array();
    for (std::size_t k = 1; k < storeys.size(); ++k) {
      json floating = json::array();
      std::size_t below = 0;
      for (const auto* p : posts[k]) {
        if (p->box.z0 < storeys[k].fl - 700) { ++below; continue; }  // through column
        bool found = false;
        for (const auto* q : posts[k - 1])
          if (std::abs(cx(q->box) - cx(p->box)) <= 1 && std::abs(cy(q->box) - cy(p->box)) <= 1) found = true;
        if (found) { ++below; continue; }
        json on = json::array();
        for (const auto& m : members)
          if (m.horizontal && std::abs(m.box.z1 - p->box.z0) <= kTol && cx(p->box) >= m.box.x0 - kTol &&
              cx(p->box) <= m.box.x1 + kTol && cy(p->box) >= m.box.y0 - kTol && cy(p->box) <= m.box.y1 + kTol)
            on.push_back({{"id", m.id}, {"name", m.name}, {"role", m.role}});
        floating.push_back({{"id", p->id}, {"name", p->name}, {"xy", {round1(cx(p->box)), round1(cy(p->box))}},
                            {"base_z", round1(p->box.z0)}, {"stands_on", on},
                            {"status", on.empty() ? "no_support_found" : "on_beam"}});
      }
      alignment.push_back({{"floor", storeys[k].id}, {"posts", posts[k].size()}, {"with_post_below", below},
                           {"ratio", posts[k].empty() ? json() : json(round2(static_cast<double>(below) / posts[k].size()))},
                           {"floating", floating}});
    }
    out["column_alignment"] = alignment;
    out["column_alignment_basis"] = "上の階の柱のうち、真下（平面位置の差1mm以内）に下の階の柱がある本数の割合（柱直下率）。"
                                    "直下率は法令の基準ではなく設計の目安。陸立ちの柱は乗っている横架材とともに列挙。";

    // --- beam spans between supports
    const std::set<std::string> spanRoles{"beam", "girder", "rim_beam", "floor_beam", "eave_beam", "roof_beam", "purlin", "ridge"};
    json beams = json::array();
    for (const auto& b : members) {
      if (!b.horizontal || !spanRoles.count(b.role)) continue;
      const double a0 = b.alongX ? b.box.x0 : b.box.y0, a1 = b.alongX ? b.box.x1 : b.box.y1;
      const double c0 = b.alongX ? b.box.y0 : b.box.x0, c1 = b.alongX ? b.box.y1 : b.box.x1;
      const double depth = b.box.z1 - b.box.z0, length = a1 - a0;
      std::vector<double> at;
      json supports = json::array();
      auto along = [&](const Box& m) { return b.alongX ? std::pair{m.x0, m.x1} : std::pair{m.y0, m.y1}; };
      auto across = [&](const Box& m) { return b.alongX ? std::pair{m.y0, m.y1} : std::pair{m.x0, m.x1}; };
      for (const auto& m : members) {
        if (m.id == b.id) continue;
        const auto [m0, m1] = along(m.box);
        const auto [n0, n1] = across(m.box);
        const bool crossesLine = n0 <= c1 + kTol && n1 >= c0 - kTol;
        if (!crossesLine) continue;
        if ((m.column || m.horizontal) && std::abs(m.box.z1 - b.box.z0) <= kTol && m1 >= a0 - kTol && m0 <= a1 + kTol) {
          // under the beam: a post, or a member it rests on
          // a post bears at its centre line; a member below, at the middle of the overlap
          const double s = m.column ? std::clamp((m0 + m1) / 2, a0, a1)
                                    : std::clamp((std::max(m0, a0) + std::min(m1, a1)) / 2, a0, a1);
          at.push_back(s);
          supports.push_back({{"id", m.id}, {"name", m.name}, {"role", m.role}, {"at", round1(s - a0)}, {"kind", "under"}});
          continue;
        }
        // at an end: the beam butts into the side of a member that overlaps it in height
        const bool sideBySide = std::min(m.box.z1, b.box.z1) - std::max(m.box.z0, b.box.z0) > depth / 2;
        if ((m.column || m.horizontal) && sideBySide) {
          if (std::abs(m1 - a0) <= kTol || (m0 <= a0 + kTol && m1 >= a0 - kTol && m0 < a0 - kTol)) {
            at.push_back(a0);
            supports.push_back({{"id", m.id}, {"name", m.name}, {"role", m.role}, {"at", 0.0}, {"kind", "end_side"}});
          } else if (std::abs(m0 - a1) <= kTol || (m0 <= a1 + kTol && m1 >= a1 - kTol && m1 > a1 + kTol)) {
            at.push_back(a1);
            supports.push_back({{"id", m.id}, {"name", m.name}, {"role", m.role}, {"at", round1(length)}, {"kind", "end_side"}});
          }
        }
      }
      std::sort(at.begin(), at.end());
      std::vector<double> stations;
      for (double s : at)
        if (stations.empty() || s - stations.back() > 50) stations.push_back(s);
      double span = 0;
      for (std::size_t i = 1; i < stations.size(); ++i) span = std::max(span, stations[i] - stations[i - 1]);
      json row = {{"id", b.id}, {"name", b.name}, {"role", b.role}, {"axis", b.alongX ? "X" : "Y"},
                  {"length", round1(length)}, {"width", round1(c1 - c0)}, {"depth", round1(depth)},
                  {"supports", supports}, {"max_span", nullptr}, {"span_to_depth", nullptr},
                  {"cantilever_start", nullptr}, {"cantilever_end", nullptr}, {"status", "not_evaluated"}};
      if (stations.empty()) {
        row["status"] = "no_support_found";
      } else {
        if (stations.front() - a0 > 50) row["cantilever_start"] = round1(stations.front() - a0);
        if (a1 - stations.back() > 50) row["cantilever_end"] = round1(a1 - stations.back());
        if (stations.size() >= 2) {
          row["max_span"] = round1(span);
          row["span_to_depth"] = round2(span / depth);
          row["status"] = "measured";
          if (limit) row["status"] = span / depth > *limit + 1e-9 ? "over_limit" : "within_limit";
        } else {
          row["status"] = "single_support";
        }
      }
      beams.push_back(row);
    }
    std::sort(beams.begin(), beams.end(), [](const json& a, const json& b) {
      const double ra = a.at("span_to_depth").is_number() ? a.at("span_to_depth").get<double>() : -1;
      const double rb = b.at("span_to_depth").is_number() ? b.at("span_to_depth").get<double>() : -1;
      return ra > rb;
    });
    out["beams"] = beams;
    out["beam_basis"] = limit ? "支点間の最大距離÷梁せい。判定の上限 " + json(*limit).dump() + "（根拠: " +
                                    opt.at("limit_basis").get<std::string>() + "）。断面照査ではありません。"
                              : "支点間の最大距離÷梁せい（目安の比較用）。上限を指定していないため判定しません。断面照査ではありません。";
    out["beam_supports_basis"] = "支点＝梁の下面に接する柱・横架材、または梁の端が側面に取り付く部材（箱の接触、2mm以内）。"
                                 "接合の仕様・耐力は評価しません。";

    // --- inputs the designer can review and pass to set_wall_calculation
    json suggested = json::array();
    for (std::size_t i = 0; i < storeys.size(); ++i) {
      json f = {{"id", storeys[i].id}, {"name", storeys[i].name}};
      if (i < floors.size() && !floors[i].at("area_m2").is_null()) f["area_m2"] = floors[i].at("area_m2");
      if (i < floors.size() && !floors[i].at("plan").is_null()) f["plan"] = floors[i].at("plan");
      if (i < wind.size()) { f["wind_x_m2"] = wind[i].at("wind_x_m2"); f["wind_y_m2"] = wind[i].at("wind_y_m2"); }
      suggested.push_back(f);
    }
    out["suggested_wall_floors"] = suggested;
    out["skipped_without_faces"] = skipped;
    return out.dump();
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}
}  // namespace sbr::construction
