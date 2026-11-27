#include <ctime>
#include "sbr/doc/Schedules.h"
#include "sbr/doc/FireSpread.h"
#include "sbr/doc/PermitIntake.h"
#include "sbr/doc/GroundLevel.h"
#include "sbr/doc/FoundationGround.h"
#include "sbr/doc/ColumnCheck.h"
#include "sbr/doc/FireCompartments.h"
#include "sbr/ConstructionQuantity.h"
#include "sbr/ConstructionWall.h"
#include "sbr/ConstructionFoundation.h"
#include "sbr/ConstructionDiagnostics.h"
#include "sbr/ConstructionAssist.h"
#include "sbr/ConstructionCheck.h"
#include "sbr/api/CommandApi.h"
#include "sbr/OpeningSymbols.h"
#include "sbr/doc/IfcExport.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/KernelError.h"
#include "sbr/draw/Drawing.h"
#include "sbr/draw/SheetCheck.h"
#include "sbr/geom/Surfaces.h"
#include <limits>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <BRepTools.hxx>
#include <Standard_Failure.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include "sbr/doc/ChangeSummary.h"
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include "sbr/geom/FaceRef.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::api {
namespace {

using json = nlohmann::json;
using doc::ObjectId;

// Thrown for invalid requests; turned into {"ok": false, "error": ...}.
struct ApiError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

constexpr double kDegree = M_PI / 180.0;

// --- argument helpers -------------------------------------------------------------

const json& arg(const json& args, const char* key) {
  if (!args.contains(key)) throw ApiError(std::string("missing argument '") + key + "'");
  return args.at(key);
}

double number(const json& args, const char* key) {
  const json& v = arg(args, key);
  if (!v.is_number()) throw ApiError(std::string("'") + key + "' must be a number");
  return v.get<double>();
}

double numberOr(const json& args, const char* key, double fallback) {
  return args.contains(key) ? number(args, key) : fallback;
}

std::vector<double> numbersOr(const json& args, const char* key) {
  std::vector<double> out;
  if (!args.contains(key)) return out;
  const json& v = args.at(key);
  if (!v.is_array()) throw ApiError(std::string("'") + key + "' must be an array of numbers");
  for (const json& n : v) {
    if (!n.is_number()) throw ApiError(std::string("'") + key + "' must be an array of numbers");
    out.push_back(n.get<double>());
  }
  return out;
}

std::string text(const json& args, const char* key) {
  const json& v = arg(args, key);
  if (!v.is_string()) throw ApiError(std::string("'") + key + "' must be a string");
  return v.get<std::string>();
}

std::string textOr(const json& args, const char* key, const std::string& fallback) {
  return args.contains(key) ? text(args, key) : fallback;
}

gp_XYZ xyz(const json& v, const char* key) {
  if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) {
    throw ApiError(std::string("'") + key + "' must be [x, y, z]");
  }
  return gp_XYZ(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

gp_Pnt point(const json& args, const char* key) { return gp_Pnt(xyz(arg(args, key), key)); }

// The roof sizes given in `a` put into `s` (the others stay).
void roofArgs(const json& a, geom::RoofSpec& s) {
  if (a.contains("kind")) s.kind = text(a, "kind");
  if (a.contains("ridge")) s.ridge = text(a, "ridge");
  if (a.contains("height")) s.height = number(a, "height");
  if (a.contains("pitch")) s.pitch = number(a, "pitch");
  if (a.contains("eave")) s.eave = number(a, "eave");
  if (a.contains("gable_eave")) s.gableEave = number(a, "gable_eave");
  if (a.contains("thickness")) s.thickness = number(a, "thickness");
  if (a.contains("fall")) {
    const json& v = a.at("fall");
    if (!v.is_array() || v.size() < 2 || !v[0].is_number() || !v[1].is_number()) throw ApiError("'fall' is [x, y]");
    s.fallX = v[0].get<double>();
    s.fallY = v[1].get<double>();
  }
  // docs/138: the gable sides of an outline with slanted sides (0-based); a new outline without
  // them: chosen again.
  if (a.contains("gable_sides")) {
    const json& v = a.at("gable_sides");
    if (!v.is_array()) throw ApiError("'gable_sides' is a list of side numbers (0: from corner 0 to corner 1)");
    s.gableSides.clear();
    for (const json& g : v) {
      if (!g.is_number_integer()) throw ApiError("'gable_sides' is a list of side numbers (0: from corner 0 to corner 1)");
      s.gableSides.push_back(g.get<int>());
    }
  } else if (a.contains("outline")) {
    s.gableSides.clear();
  }
  // docs/161: sides against a higher wall ("auto" is worked out by the command itself).
  if (a.contains("wall_sides") && !a.at("wall_sides").is_string()) {
    const json& v = a.at("wall_sides");
    const char* shape = "'wall_sides' is a list of side numbers (0: from corner 0 to corner 1) or \"auto\"";
    if (!v.is_array()) throw ApiError(shape);
    s.wallSides.clear();
    for (const json& g : v) {
      if (!g.is_number_integer()) throw ApiError(shape);
      s.wallSides.push_back(g.get<int>());
    }
  } else if (a.contains("outline") && !a.contains("wall_sides")) {
    s.wallSides.clear();
  }
  // docs/160: sides with a pitch / an overhang of their own; the list replaces the one before.
  if (a.contains("sides")) {
    const json& v = a.at("sides");
    const char* shape = "'sides' is a list of {side, pitch, eave} (side 0: from corner 0 to corner 1; pitch and eave may be left out)";
    if (!v.is_array()) throw ApiError(shape);
    s.sidePitch.clear();
    s.sideEave.clear();
    for (const json& one : v) {
      if (!one.is_object() || !one.contains("side") || !one.at("side").is_number_integer()) throw ApiError(shape);
      for (const auto& [key, value] : one.items())
        if (key != "side" && key != "pitch" && key != "eave") throw ApiError(shape);
      const int side = one.at("side").get<int>();
      if (one.contains("pitch")) {
        if (!one.at("pitch").is_number()) throw ApiError(shape);
        s.sidePitch[side] = one.at("pitch").get<double>();
      }
      if (one.contains("eave")) {
        if (!one.at("eave").is_number()) throw ApiError(shape);
        s.sideEave[side] = one.at("eave").get<double>();
      }
    }
  } else if (a.contains("outline")) {
    s.sidePitch.clear();
    s.sideEave.clear();
  }
}

// docs/161: 'wall_sides': "auto": the sides lying along walls that stand higher than the roof.
void wallSidesAuto(const doc::Modeler& m, const json& a, geom::RoofSpec& s) {
  if (!a.contains("wall_sides") || !a.at("wall_sides").is_string()) return;
  if (a.at("wall_sides").get<std::string>() != "auto") throw ApiError("'wall_sides' is a list of side numbers (0: from corner 0 to corner 1) or \"auto\"");
  s.wallSides = m.roofSidesAtWalls(s);
}

// docs/163: the balcony sizes given in `a` put into `s` (the others stay).
void balconyArgs(const doc::Modeler& m, const json& a, doc::Modeler::BalconySpec& s) {
  if (a.contains("at")) {
    const json& v = a.at("at");
    if (!v.is_array() || v.size() < 2 || !v[0].is_number() || !v[1].is_number()) throw ApiError("'at' is [x, y]");
    const auto near = m.balconyNear(s.host, gp_Pnt(v[0].get<double>(), v[1].get<double>(), 0.0));
    if (!near) throw ApiError(near.error());
    s.along = near->first;
    s.side = near->second;
  }
  if (a.contains("along")) s.along = number(a, "along");
  if (a.contains("side")) {
    if (!a.at("side").is_number_integer() || (a.at("side").get<int>() != 1 && a.at("side").get<int>() != -1)) throw ApiError("'side' is 1 or -1");
    s.side = a.at("side").get<int>();
  }
  if (a.contains("width")) s.width = number(a, "width");
  if (a.contains("depth")) s.depth = number(a, "depth");
  if (a.contains("top")) s.top = number(a, "top");
  if (a.contains("thickness")) s.thickness = number(a, "thickness");
  if (a.contains("rail")) s.rail = text(a, "rail");
  if (a.contains("rail_height")) s.railHeight = number(a, "rail_height");
  if (a.contains("floor_area")) s.floorArea = text(a, "floor_area");
  if (a.contains("posts")) {
    if (!a.at("posts").is_number_integer()) throw ApiError("'posts' is 0 or 2 to 6");
    s.posts = a.at("posts").get<int>();
  }
  if (a.contains("post_size")) s.postSize = number(a, "post_size");
  if (a.contains("wrap")) s.wrap = text(a, "wrap");
  if (a.contains("wrap_length")) s.wrapLength = number(a, "wrap_length");
}

json balconyJson(const std::string& id, const doc::Modeler::BalconySpec& s) {
  return json{{"id", id}, {"host", s.host}, {"along", s.along}, {"side", s.side}, {"width", s.width}, {"depth", s.depth}, {"top", s.top},
              {"thickness", s.thickness}, {"rail", s.rail}, {"rail_height", s.railHeight}, {"floor_area", s.floorArea},
              {"posts", s.posts}, {"post_size", s.postSize}, {"wrap", s.wrap}, {"wrap_length", s.wrapLength}};
}

// The stair sizes given in `a` put into `s` (the others stay).
void stairArgs(const json& a, geom::StairSpec& s) {
  if (a.contains("base")) s.base = number(a, "base");
  if (a.contains("rise")) s.rise = number(a, "rise");
  if (a.contains("top")) s.rise = number(a, "top") - s.base;
  if (a.contains("width")) s.width = number(a, "width");
  if (a.contains("risers")) s.risers = static_cast<int>(number(a, "risers"));
  if (a.contains("riser_limit")) s.riserLimit = number(a, "riser_limit");
  if (a.contains("style")) s.style = text(a, "style");
  if (a.contains("thickness")) s.thickness = number(a, "thickness");
  if (a.contains("handrail")) s.handrail = text(a, "handrail");
  if (a.contains("handrail_height")) s.handrailHeight = number(a, "handrail_height");
  if (a.contains("stringer")) s.stringer = text(a, "stringer");
  if (a.contains("stringer_thickness")) s.stringerThickness = number(a, "stringer_thickness");
  if (a.contains("stringer_style")) s.stringerStyle = text(a, "stringer_style");
  if (a.contains("riser_boards")) s.riserBoards = a.at("riser_boards").get<bool>();
  if (a.contains("riser_thickness")) s.riserThickness = number(a, "riser_thickness");
  if (a.contains("newels")) s.newels = a.at("newels").get<bool>();
  if (a.contains("newel_size")) s.newelSize = number(a, "newel_size");
  if (a.contains("nosing")) s.nosing = number(a, "nosing");
  if (a.contains("shape")) s.shape = text(a, "shape");
  if (a.contains("sweep")) s.sweep = number(a, "sweep");
  if (a.contains("turns")) {
    const json& v = a.at("turns");
    s.turns.clear();
    auto one = [](const json& t) {
      if (t.is_number_integer()) return t.get<int>();
      if (t.is_string() && t.get<std::string>() == "landing") return 1;
      throw ApiError("'turns': per turn 1 or \"landing\" (a flat landing), or 2 to 6 (winders: treads its corner is cut into)");
    };
    if (v.is_array()) for (const json& t : v) s.turns.push_back(one(t));
    else s.turns.push_back(one(v));
  }
}

json stairJson(const doc::StairInfo& r) {
  json path = json::array(), checks = json::array();
  for (const gp_Pnt& p : r.spec.path) path.push_back({p.X(), p.Y()});
  bool allOk = true;
  for (const auto& c : r.checks) {
    checks.push_back({{"item", c.item}, {"value", c.value}, {"limit", c.limit}, {"state", c.state}});
    allOk &= c.state == "ok";
  }
  return json{{"path", path}, {"base", r.spec.base}, {"rise", r.spec.rise}, {"width", r.spec.width},
              {"risers", r.risers}, {"risers_automatic", r.spec.risers == 0}, {"riser_limit", r.spec.riserLimit},
              {"riser", r.riser}, {"treads", r.treads}, {"going_min", r.goingMin}, {"going_max", r.goingMax},
              {"winder_going", r.winderGoing}, {"walk_length", r.walkLength}, {"flights", r.flights},
              {"landings", r.landings}, {"winders", r.winders}, {"turns", r.spec.turns}, {"style", r.spec.style},
              {"thickness", r.spec.thickness}, {"handrail", r.spec.handrail}, {"handrail_height", r.spec.handrailHeight},
              {"stringer", r.spec.stringer}, {"stringer_thickness", r.spec.stringerThickness},
              {"stringer_style", r.spec.stringerStyle}, {"riser_boards", r.spec.riserBoards}, {"riser_thickness", r.spec.riserThickness},
              {"newels", r.spec.newels}, {"newel_size", r.spec.newelSize}, {"nosing", r.spec.nosing}, {"shape", r.spec.shape}, {"sweep", r.spec.sweep}, {"base_level", r.baseLevel}, {"top_level", r.topLevel},
              {"opened_slabs", r.openedSlabs}, {"checks", checks}, {"checks_all_ok", allOk},
              {"checks_basis",
               "Sizes for a house's own stair (not a shared stair of flats), Building Standards Act Enforcement Order "
               "arts. 23-25 as recorded in docs/103: riser <= 230, going >= 150 (winders: measured 300 from the narrow "
               "end, here as the chord of the tread's angle), width >= 750, a landing within every 4000 of height, a "
               "handrail on at least one side (none made here: not_evaluated, it may be made otherwise). "
               "not_evaluated = not measured here (clear width between finishes, landings of taller stairs): "
               "never a pass. Other uses of building have other sizes."}};
}

json roofJson(const doc::RoofInfo& r) {
  json outline = json::array();
  for (const gp_Pnt& p : r.spec.outline) outline.push_back({p.X(), p.Y()});
  json sides = json::array();
  for (const auto& [side, pitch] : r.spec.sidePitch) {
    json one{{"side", side}, {"pitch", pitch}};
    if (const auto it = r.spec.sideEave.find(side); it != r.spec.sideEave.end()) one["eave"] = it->second;
    sides.push_back(one);
  }
  for (const auto& [side, eave] : r.spec.sideEave)
    if (!r.spec.sidePitch.count(side)) sides.push_back({{"side", side}, {"eave", eave}});
  return json{{"kind", r.spec.kind}, {"outline", outline}, {"height", r.spec.height}, {"pitch", r.spec.pitch},
              {"eave", r.spec.eave}, {"gable_eave", r.spec.gableEave}, {"thickness", r.spec.thickness},
              {"ridge", r.spec.ridge}, {"fall", {r.spec.fallX, r.spec.fallY}}, {"gable_sides", r.spec.gableSides}, {"sides", sides}, {"wall_sides", r.spec.wallSides}, {"plan_area", r.planArea / 1e6},
              {"slope_area", r.slopeArea / 1e6}, {"ridge_height", r.ridgeHeight}, {"eave_height", r.eaveHeight},
              {"parts", r.parts}, {"level", r.level}};
}

json roomJson(const doc::RoomInfo& r) {
  auto outline = [](const std::vector<gp_Pnt>& points) {
    json out = json::array();
    for (const gp_Pnt& p : points) out.push_back({p.X(), p.Y()});
    return out;
  };
  json j{{"name", r.name}, {"level", r.level}, {"at", {r.at.X(), r.at.Y(), r.at.Z()}}, {"closed", r.closed},
         {"problem", r.problem}, {"unreadable_walls", r.unreadableWalls}};
  if (!r.id.empty()) j["id"] = r.id;
  if (r.closed) {
    j["area_centre"] = r.centreArea / 1e6;
    j["area_inner"] = r.innerKnown ? json(r.innerArea / 1e6) : json(nullptr);
    j["inner_perimeter"] = r.innerKnown ? json(r.innerPerimeter) : json(nullptr);
    j["outline_centre"] = outline(r.centre);
    j["outline_inner"] = outline(r.inner);
    j["walls"] = r.walls;
  } else {
    json ends = json::array();
    for (const gp_Pnt& p : r.looseEnds) ends.push_back({p.X(), p.Y()});
    j["loose_ends"] = ends;
  }
  return j;
}

gp_Dir direction(const json& args, const char* key, const gp_Dir& fallback) {
  if (!args.contains(key)) return fallback;
  const gp_XYZ v = xyz(args.at(key), key);
  if (v.Modulus() < 1e-12) throw ApiError(std::string("'") + key + "' must not be zero");
  return gp_Dir(v);
}

std::vector<std::array<gp_Pnt, 2>> confirmedNeighborEdges(const json& args) {
  if (!args.at("boundaries").is_array()) throw ApiError("boundaries must be an array");
  std::vector<std::array<gp_Pnt, 2>> edges;
  for (const auto& edge : args.at("boundaries")) edges.push_back({point(edge, "start"), point(edge, "end")});
  return edges;
}

std::vector<gp_Pnt> points(const json& args, const char* key) {
  const json& v = arg(args, key);
  if (!v.is_array()) throw ApiError(std::string("'") + key + "' must be an array of points");
  std::vector<gp_Pnt> out;
  for (const json& p : v) out.emplace_back(xyz(p, key));
  return out;
}

std::vector<ObjectId> ids(const json& args, const char* key) {
  const json& v = arg(args, key);
  if (v.is_string()) return {v.get<std::string>()};
  if (!v.is_array()) throw ApiError(std::string("'") + key + "' must be an id or an array of ids");
  std::vector<ObjectId> out;
  for (const json& id : v) out.push_back(id.get<std::string>());
  return out;
}

doc::Color color(const json& args, const char* key) {
  const json& v = arg(args, key);
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s.size() == 7 && s[0] == '#') {
      auto channel = [&](int i) { return std::stoi(s.substr(1 + 2 * i, 2), nullptr, 16) / 255.0; };
      return doc::Color{channel(0), channel(1), channel(2)};
    }
    throw ApiError("colour must be [r, g, b] (0..1) or \"#rrggbb\"");
  }
  const gp_XYZ c = xyz(v, key);
  return doc::Color{c.X(), c.Y(), c.Z()};
}

json toJson(const gp_Pnt& p) { return json::array({p.X(), p.Y(), p.Z()}); }

// A material named by its id or its (unique) name; "" = inherit.
std::string materialRef(const doc::Document& d, const json& args, const char* key) {
  const std::string v = text(args, key);
  if (v.empty() || d.findMaterial(v)) return v;
  std::string found;
  for (const doc::Material& m : d.materials()) {
    if (m.name != v) continue;
    if (!found.empty()) throw ApiError("several materials are named '" + v + "'; use the id");
    found = m.id;
  }
  if (found.empty()) throw ApiError("no material '" + v + "' (see list_materials)");
  return found;
}

json materialJson(const doc::Document& d, const std::string& id) {
  const doc::Material* m = d.findMaterial(id);
  if (!m) return nullptr;
  json j = {{"id", m->id},
            {"name", m->name},
            {"color", {m->color.r, m->color.g, m->color.b}},
            {"opacity", m->opacity},
            {"metallic", m->metallic},
            {"roughness", m->roughness},
            {"reflectance", m->reflectance}};
  if (m->texture) {
    j["texture"] = {{"image", m->texture->image},
                    {"width", m->texture->width},
                    {"height", m->texture->height},
                    {"colorize", m->texture->colorize}};
  }
  return j;
}
json toJson(const gp_XYZ& p) { return json::array({p.X(), p.Y(), p.Z()}); }

template <class T>
T must(Result<T> r) {
  if (!r) throw ApiError(r.error());
  return r.value();
}

void must(const Status& s) {
  if (!s) throw ApiError(s.message());
}

// 'openings' of an opening command on 'host': ids (text) or numbers from list_openings.
// Empty when the argument is absent (the commands then take every opening).
std::vector<std::size_t> openingNumbers(doc::Modeler& m, const json& a) {
  std::vector<std::size_t> which;
  if (!a.contains("openings")) return which;
  if (!a.at("openings").is_array()) throw ApiError("'openings' must be an array");
  for (const json& i : a.at("openings")) {
    if (i.is_string())
      which.push_back(must(m.openingIndices(text(a, "host"), {i.get<std::string>()})).front());
    else if (i.is_number_integer() && i.get<long long>() >= 1)
      which.push_back(i.get<std::size_t>());
    else
      throw ApiError("'openings' must be ids or numbers >= 1 from list_openings");
  }
  return which;
}

// --- object descriptions ---------------------------------------------------------------

json bounds(const TopoDS_Shape& shape) {
  const Bnd_Box box = geom::exactBounds(shape);
  if (box.IsVoid()) return nullptr;
  return json{{"min", toJson(box.CornerMin())}, {"max", toJson(box.CornerMax())}};
}

// Volume, area and bounds of local canonical shapes, computed once per
// shape: every placement of a component shares its shape, and unchanged
// objects keep theirs (the cache holds the shape, so entries never refer
// to a freed one). Bounded: cleared when it grows too large.
struct Measures {
  TopoDS_Shape shape;
  std::optional<double> volume, area;
  std::optional<Bnd_Box> box;  // exact, local coordinates

  double volumeValue() {
    if (!volume) volume = geom::volume(shape);
    return *volume;
  }
  double areaValue() {
    if (!area) area = geom::area(shape);
    return *area;
  }
  const Bnd_Box& boxValue() {
    if (!box) box = geom::exactBounds(shape);
    return *box;
  }
};

Measures& measuresOf(const TopoDS_Shape& local) {
  struct Key {
    const void* tshape;
    int orientation;
    bool operator==(const Key& o) const { return tshape == o.tshape && orientation == o.orientation; }
  };
  struct Hash {
    std::size_t operator()(const Key& k) const { return std::hash<const void*>()(k.tshape) ^ k.orientation; }
  };
  static std::unordered_map<Key, Measures, Hash> cache;
  const Key key{local.TShape().get(), static_cast<int>(local.Orientation())};
  auto it = cache.find(key);
  if (it != cache.end() && it->second.shape.Location().IsEqual(local.Location())) return it->second;
  if (cache.size() > 50000) cache.clear();
  Measures& m = cache[key];
  m = Measures{};
  m.shape = local;
  return m;
}

// Exact world bounds: shifted from the cached local box when the placement
// is a pure translation, recomputed otherwise.
json worldBounds(const doc::Document& d, const doc::Object& o) {
  if (o.shape.IsNull()) return nullptr;
  const gp_Trsf t = d.worldTransform(o);
  if (t.Form() == gp_Identity || t.Form() == gp_Translation) {
    const Bnd_Box& box = measuresOf(o.shape).boxValue();
    if (box.IsVoid()) return nullptr;
    const gp_Vec v = t.TranslationPart();
    return json{{"min", toJson(box.CornerMin().Translated(v))}, {"max", toJson(box.CornerMax().Translated(v))}};
  }
  return bounds(d.worldShape(o));
}

// A grid's lines (docs/43): by direction, in order of position, each with
// its id, name, position (from the grid's origin) and spacing from the one
// before.
json gridJson(const doc::GridSpec& spec) {
  auto axes = [](std::vector<doc::GridAxis> list) {
    std::sort(list.begin(), list.end(), [](const doc::GridAxis& a, const doc::GridAxis& b) { return a.offset < b.offset; });
    json out = json::array();
    for (std::size_t i = 0; i < list.size(); ++i) {
      json a = {{"id", list[i].id}, {"name", list[i].name}, {"position", list[i].offset}};
      if (i > 0) a["spacing"] = list[i].offset - list[i - 1].offset;
      out.push_back(a);
    }
    return out;
  };
  return json{{"x", axes(spec.x)}, {"y", axes(spec.y)}, {"label_offset", spec.labelOffset}};
}

json sunJson(const geom::SunSettings& s) {
  const geom::SunPosition p = geom::sunPosition(s);
  return {{"month", s.month},       {"day", s.day},
          {"hour", s.hour},         {"latitude", s.latitude},
          {"longitude", s.longitude}, {"time_zone", s.timeZone},
          {"north_angle", s.northAngle}, {"altitude", p.altitude},
          {"azimuth", p.azimuth},   {"toward_sun", p.towardSun},
          {"up", p.up()}};
}

json levelsJson(const doc::Modeler& m) {
  json out = json::array();
  for (const doc::FloorLevel& l : m.levels()) out.push_back({{"id", l.id}, {"name", l.name}, {"elevation", l.elevation}});
  return out;
}

json describeObject(const doc::Modeler& m, const doc::Object& o, bool withFaces) {
  const doc::Document& d = m.document();
  const TopoDS_Shape world = d.worldShape(o);
  json j = {{"id", o.id},
            {"name", o.name},
            {"kind", doc::toString(o.kind())},
            {"tag", o.layer},
            {"visible", o.visible},
            {"color", [&] {
               const doc::Color c = d.displayColor(o);
               return json::array({c.r, c.g, c.b});
             }()},
            {"history", doc::describe(o.recipe)},
            {"bounds", worldBounds(d, o)}};
  if (!o.parent.empty()) j["parent"] = o.parent;
  if (!o.attributes.empty()) j["attributes"] = o.attributes;
  if (const doc::GridSpec* grid = m.gridSpec(o.id)) j["grid"] = gridJson(*grid);
  if (o.gridPin) {
    j["grid_pin"] = {{"grid", o.gridPin->grid},
                     {"start", {o.gridPin->startX, o.gridPin->startY}},
                     {"end", {o.gridPin->endX, o.gridPin->endY}}};
    if (!o.gridPin->startLevel.empty() || !o.gridPin->endLevel.empty()) {
      j["grid_pin"]["levels"] = {o.gridPin->startLevel, o.gridPin->endLevel};
    }
  }
  if (o.follow) {
    j["follow"] = {{"host", o.follow->host},
                   {"state", m.followState(o.id) == doc::Modeler::FollowState::Active ? "active" : "suspended"}};
  }
  j["material"] = o.material.empty() ? json(nullptr) : json(o.material);
  if (const std::string shown = d.inheritedMaterial(o); shown != o.material) j["inherited_material"] = shown;
  if (!o.faceMaterials.empty()) {
    const doc::FaceMaterialMap faces = doc::resolveFaceMaterials(o);
    json list = json::array();
    for (const auto& [index, material] : faces.faces) list.push_back({{"face", index}, {"material", material}});
    j["face_materials"] = list;
    if (faces.unresolved) j["unresolved_face_materials"] = faces.unresolved;
  }
  // Rigid placements keep volume and area: measured on the local shape.
  if (o.kind() == doc::ObjectKind::Solid) j["volume"] = measuresOf(o.shape).volumeValue();
  if (o.kind() == doc::ObjectKind::Face) j["area"] = measuresOf(o.shape).areaValue();
  if (const auto label = doc::annotationLabel(o.recipe)) j["text"] = label->text;
  json params = json::array();
  for (const doc::Parameter& p : m.parameters(o.id)) {
    params.push_back({{"key", p.key}, {"label", p.label}, {"value", p.value},
                      {"unit", p.isLength ? "mm" : "deg"}});
  }
  j["parameters"] = params;
  if (withFaces && !world.IsNull()) {
    // Faces are numbered like the viewport does (TopExp map order, 1-based).
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(world, TopAbs_FACE, faces);
    json list = json::array();
    for (int i = 1; i <= faces.Extent(); ++i) {
      const TopoDS_Face face = TopoDS::Face(faces(i));
      json f = {{"index", i}, {"area", geom::area(face)}};
      if (const auto ref = geom::FaceRef::fromFace(face)) {
        f["normal"] = toJson(ref->normal.XYZ());
        f["centroid"] = toJson(ref->centroid);
      } else {
        f["planar"] = false;
      }
      list.push_back(f);
    }
    j["faces"] = list;
  }
  return j;
}

const doc::Object& object(const doc::Modeler& m, const ObjectId& id) {
  const doc::Object* o = m.document().find(id);
  if (!o) throw ApiError("no object with id '" + id + "'");
  return *o;
}

// Face selection: {"face": 3} (index from get_object),
// {"face": {"normal": [0,0,1]}} (the outermost planar face with that
// normal) or {"face": {"normal": [...], "near": [x,y,z]}} (of those facing
// that way, the one closest to the point). Ties are refused with the
// candidates' indices rather than one being picked arbitrarily.
TopoDS_Face selectFace(const doc::Modeler& m, const ObjectId& id, const json& selector) {
  const TopoDS_Shape world = m.document().worldShape(object(m, id));
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(world, TopAbs_FACE, faces);
  if (selector.is_number_integer()) {
    const int index = selector.get<int>();
    if (index < 1 || index > faces.Extent()) throw ApiError("face index out of range");
    return TopoDS::Face(faces(index));
  }
  if (selector.is_object() && selector.contains("normal")) {
    const gp_Dir wanted(xyz(selector.at("normal"), "normal"));
    const std::optional<gp_Pnt> near =
        selector.contains("near") ? std::optional<gp_Pnt>(gp_Pnt(xyz(selector.at("near"), "near"))) : std::nullopt;
    // Score: outermost (largest offset along the normal) or closest to the point.
    std::vector<std::pair<double, int>> scored;
    for (int i = 1; i <= faces.Extent(); ++i) {
      const TopoDS_Face face = TopoDS::Face(faces(i));
      const auto ref = geom::FaceRef::fromFace(face);
      if (!ref || ref->normal.Angle(wanted) > 1e-6) continue;
      double score = -gp_Vec(ref->centroid.XYZ()).Dot(gp_Vec(wanted));
      if (near) {
        BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(*near).Vertex(), face);
        if (!dist.IsDone()) continue;
        score = dist.Value();
      }
      scored.emplace_back(score, i);
    }
    if (scored.empty()) throw ApiError("no planar face with that normal");
    std::sort(scored.begin(), scored.end());
    std::vector<int> tied;
    for (const auto& [score, index] : scored) {
      if (score - scored.front().first < 1e-3) tied.push_back(index);
    }
    if (tied.size() > 1) {
      std::string list;
      for (int i : tied) list += (list.empty() ? "" : ", ") + std::to_string(i);
      throw ApiError("ambiguous face: faces " + list + (near ? " are equally close to 'near'" : " are equally outermost") +
                     "; give the face index or add 'near': [x, y, z] (a point on the face you mean)");
    }
    return TopoDS::Face(faces(scored.front().second));
  }
  throw ApiError("'face' must be an index or {\"normal\": [x, y, z], \"near\": [x, y, z]}");
}

// Edge selection: {"edge": 3} (index in the object's edge map, 1-based) or
// {"edge": {"near": [x, y, z]}} (the edge closest to the point; ties refused).
TopoDS_Edge selectEdge(const doc::Modeler& m, const ObjectId& id, const json& selector) {
  const TopoDS_Shape world = m.document().worldShape(object(m, id));
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(world, TopAbs_EDGE, edges);
  if (selector.is_number_integer()) {
    const int index = selector.get<int>();
    if (index < 1 || index > edges.Extent()) throw ApiError("edge index out of range");
    return TopoDS::Edge(edges(index));
  }
  if (!selector.is_object() || !selector.contains("near")) {
    throw ApiError("'edge' must be an index or {\"near\": [x, y, z]}");
  }
  const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(gp_Pnt(xyz(selector.at("near"), "near"))).Vertex();
  std::vector<std::pair<double, int>> scored;
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepExtrema_DistShapeShape d(v, edges(i));
    if (d.IsDone()) scored.emplace_back(d.Value(), i);
  }
  if (scored.empty()) throw ApiError("the object has no edges");
  std::sort(scored.begin(), scored.end());
  if (scored.size() > 1 && scored[1].first - scored[0].first < 1e-3) {
    throw ApiError("ambiguous edge: edges " + std::to_string(scored[0].second) + " and " +
                   std::to_string(scored[1].second) + " are equally close; move 'near' onto the edge you mean");
  }
  return TopoDS::Edge(edges(scored.front().second));
}

// A curve input: an object id (the whole curve) or {"id", "edge"} for one edge of it.
doc::Modeler::CurvePick curvePick(const doc::Modeler& m, const json& item) {
  if (item.is_string()) return {item.get<std::string>(), std::nullopt};
  const ObjectId id = text(item, "id");
  if (!item.contains("edge")) return {id, std::nullopt};
  return {id, selectEdge(m, id, item.at("edge"))};
}

// The selection as the GUI holds it ('ids' whole, 'elements' = faces /
// edges picked on an object), shared by delete and copy.
std::vector<doc::DeleteTarget> selectionTargets(const doc::Modeler& m, const json& a) {
  std::vector<doc::DeleteTarget> targets;
  if (a.contains("ids")) {
    for (const ObjectId& id : ids(a, "ids")) targets.push_back(doc::DeleteTarget{id, true, {}, {}});
  }
  if (a.contains("elements")) {
    for (const json& item : arg(a, "elements")) {
      doc::DeleteTarget t;
      t.id = text(item, "id");
      if (item.contains("faces")) {
        for (const json& f : item.at("faces")) t.faces.push_back(selectFace(m, t.id, f));
      }
      if (item.contains("edges")) {
        for (const json& e : item.at("edges")) t.edges.push_back(selectEdge(m, t.id, e));
      }
      targets.push_back(std::move(t));
    }
  }
  if (targets.empty()) throw ApiError("give ids or elements");
  return targets;
}

doc::BooleanKind booleanKind(const std::string& s) {
  if (s == "union") return doc::BooleanKind::Union;
  if (s == "subtract") return doc::BooleanKind::Subtract;
  if (s == "intersect") return doc::BooleanKind::Intersect;
  throw ApiError("operation must be union, subtract or intersect");
}

json idResult(const ObjectId& id) { return json{{"id", id}}; }

// A member section: a catalog id, or {kind, width, depth[, web, flange, radius]}.
std::string constructionPatch(const doc::Modeler& m, const std::string& system) {
  const auto& patches = m.document().settings().constructionOverrides;
  const auto it = patches.find(system);
  return it == patches.end() ? "{}" : it->second;
}

geom::Section sectionFrom(const json& v, const doc::Modeler& m) {
  if (v.is_string() && (v.get<std::string>().rfind("TIMBER-", 0) == 0 ||
                        v.get<std::string>().rfind("PLYWOOD-", 0) == 0 ||
                        v.get<std::string>().rfind("STEEL-", 0) == 0)) {
    return construction::section(v.get<std::string>(), constructionPatch(m, "timber"));
  }
  if (v.is_string()) {
    const geom::CatalogSection* entry = geom::findSection(v.get<std::string>());
    if (!entry) throw ApiError("no section '" + v.get<std::string>() + "' in the catalog (see list_sections)");
    return entry->section;
  }
  if (!v.is_object()) throw ApiError("'section' must be a catalog id or {kind, width, depth, ...}");
  geom::Section s;
  const std::string kind = textOr(v, "kind", "rectangle");
  if (kind == "circle") {
    s.kind = geom::Section::Kind::Circle;
    s.width = s.depth = number(v, "diameter");
    return s;
  }
  if (kind == "h") s.kind = geom::Section::Kind::H;
  else if (kind != "rectangle") throw ApiError("section kind: rectangle, circle or h (catalog sections by id)");
  s.width = number(v, "width");
  s.depth = number(v, "depth");
  if (s.kind == geom::Section::Kind::H) {
    s.web = number(v, "web");
    s.flange = number(v, "flange");
    s.radius = numberOr(v, "radius", 0.0);
  }
  return s;
}

geom::SectionAnchor anchorFrom(const json& v) {
  if (!v.is_object()) throw ApiError("'anchor' must be {across: left|center|right, up: top|center|bottom}");
  geom::SectionAnchor anchor;
  const std::string across = textOr(v, "across", "center"), up = textOr(v, "up", "center");
  if (across == "left") anchor.across = 1;
  else if (across == "right") anchor.across = -1;
  else if (across != "center") throw ApiError("anchor across: left, center or right");
  if (up == "top") anchor.up = 1;
  else if (up == "bottom") anchor.up = -1;
  else if (up != "center") throw ApiError("anchor up: top, center or bottom");
  return anchor;
}

json memberInfoJson(const doc::Modeler::MemberInfo& info) {
  json fields = json::array();
  for (const auto& f : info.fields) {
    json field{{"key", f.key}, {"meaning", f.meaning}, {"unit", f.unit}, {"value", f.value},
               {"basis", f.basis}, {"editable", f.editable}};
    if (!f.note.empty()) field["note"] = f.note;
    fields.push_back(field);
  }
  static const char* across[] = {"right", "center", "left"};
  static const char* up[] = {"bottom", "center", "top"};
  if (info.element != "member") {
    json out{{"id", info.id}, {"element", info.element}, {"edited", info.edited}, {"fields", fields}};
    if (!info.materialClass.empty()) out["material_class"] = info.materialClass;
    return out;
  }
  json out{{"id", info.id},
           {"element", info.element},
           {"section", info.section},
           {"kind", geom::toString(info.kind)},
           {"catalog", info.catalog.empty() ? json(nullptr) : json(info.catalog)},
           {"made_from", info.origin.empty() ? json(nullptr) : json(info.origin)},
           {"anchor", {{"across", across[info.anchor.across + 1]}, {"up", up[info.anchor.up + 1]}}},
           {"edited", info.edited},
           {"fields", fields}};
  if (!info.role.empty()) out["role"] = info.role;
  if (!info.materialClass.empty()) out["material_class"] = info.materialClass;
  return out;
}
json idsResult(const std::vector<ObjectId>& v) { return json{{"ids", v}}; }

// --- queries for agents on large models -------------------------------------------------

// Filter shared by query_objects and aggregate. Cheap tests run first;
// bounds are only computed for objects that pass them.
struct ObjectFilter {
  std::set<std::string> kinds, tags, ids;
  std::optional<std::string> name, parent, within, definition, material;
  std::optional<bool> visible;
  std::optional<Bnd_Box> region;
  std::map<std::string, std::string> attributes;  // all must match ("*" = has the key)

  static std::set<std::string> strings(const json& a, const char* key) {
    std::set<std::string> out;
    if (!a.contains(key)) return out;
    const json& v = a.at(key);
    if (v.is_string()) {
      out.insert(v.get<std::string>());
    } else if (v.is_array()) {
      for (const json& s : v) {
        if (!s.is_string()) throw ApiError(std::string("'") + key + "' must be a string or strings");
        out.insert(s.get<std::string>());
      }
    } else {
      throw ApiError(std::string("'") + key + "' must be a string or strings");
    }
    return out;
  }

  explicit ObjectFilter(const json& a) {
    kinds = strings(a, "kind");
    tags = strings(a, "tag");
    ids = strings(a, "ids");
    if (a.contains("name")) name = text(a, "name");
    if (a.contains("parent")) parent = text(a, "parent");
    if (a.contains("within")) within = text(a, "within");
    if (a.contains("definition")) definition = text(a, "definition");
    if (a.contains("material")) material = text(a, "material");
    if (a.contains("visible")) {
      if (!a.at("visible").is_boolean()) throw ApiError("'visible' must be true or false");
      visible = a.at("visible").get<bool>();
    }
    if (a.contains("attributes")) {
      const json& at = a.at("attributes");
      if (!at.is_object()) throw ApiError("'attributes' must be an object of strings");
      for (auto it = at.begin(); it != at.end(); ++it) {
        if (!it.value().is_string()) throw ApiError("'attributes' must be an object of strings");
        attributes[it.key()] = it.value().get<std::string>();
      }
    }
    if (a.contains("region")) {
      const json& r = a.at("region");
      Bnd_Box box;
      box.Add(gp_Pnt(xyz(arg(r, "min"), "min")));
      box.Add(gp_Pnt(xyz(arg(r, "max"), "max")));
      region = box;
    }
  }

  bool matches(const doc::Document& d, const doc::Object& o) const {
    if (!ids.empty() && !ids.count(o.id)) return false;
    if (!kinds.empty() && !kinds.count(doc::toString(o.kind()))) return false;
    if (!tags.empty() && !tags.count(o.layer)) return false;
    if (name && o.name.find(*name) == std::string::npos) return false;
    if (parent && o.parent != *parent) return false;
    if (within && !d.isAncestor(*within, o.id)) return false;
    if (definition) {
      const auto* c = std::get_if<doc::ComponentInstance>(&o.recipe.base);
      if (!c || c->definitionId != *definition) return false;
    }
    if (visible && d.isVisible(o.id) != *visible) return false;
    if (material) {  // what the object shows as a whole (own or inherited); id or name
      const std::string shown = d.inheritedMaterial(o);
      const doc::Material* mat = d.findMaterial(shown);
      if (shown != *material && !(mat && mat->name == *material)) return false;
    }
    for (const auto& [key, value] : attributes) {
      const auto it = o.attributes.find(key);
      if (it == o.attributes.end() || (value != "*" && it->second != value)) return false;
    }
    if (region) {
      const json b = worldBounds(d, o);
      if (b.is_null()) return false;
      Bnd_Box box;
      box.Add(gp_Pnt(xyz(b["min"], "min")));
      box.Add(gp_Pnt(xyz(b["max"], "max")));
      if (box.IsOut(*region)) return false;
    }
    return true;
  }
};

// Only the requested fields: an agent listing 10,000 windows by name does
// not pay for volumes and parameters.
json objectFields(const doc::Modeler& m, const doc::Object& o, const std::set<std::string>& fields) {
  if (fields.count("all")) return describeObject(m, o, false);
  const doc::Document& d = m.document();
  json j = json::object();
  for (const std::string& f : fields) {
    if (f == "id") {
      j["id"] = o.id;
    } else if (f == "name") {
      j["name"] = o.name;
    } else if (f == "kind") {
      j["kind"] = doc::toString(o.kind());
    } else if (f == "tag") {
      j["tag"] = o.layer;
    } else if (f == "visible") {
      j["visible"] = d.isVisible(o.id);
    } else if (f == "parent") {
      j["parent"] = o.parent;
    } else if (f == "color") {
      const doc::Color c = d.displayColor(o);
      j["color"] = {c.r, c.g, c.b};
    } else if (f == "material") {
      j["material"] = d.inheritedMaterial(o);
    } else if (f == "bounds") {
      j["bounds"] = worldBounds(d, o);
    } else if (f == "volume") {
      if (o.kind() == doc::ObjectKind::Solid || o.kind() == doc::ObjectKind::Instance) {
        j["volume"] = measuresOf(o.shape).volumeValue();
      }
    } else if (f == "area") {
      if (o.kind() == doc::ObjectKind::Face) j["area"] = measuresOf(o.shape).areaValue();
    } else if (f == "definition") {
      if (const auto* c = std::get_if<doc::ComponentInstance>(&o.recipe.base)) {
        j["definition"] = c->definitionId;
        if (c->mirrored) j["mirrored"] = true;
        if (!c->overrides.empty()) j["own_parameters"] = c->overrides.size();
      }
    } else if (f == "history") {
      j["history"] = doc::describe(o.recipe);
    } else if (f == "parameters") {
      json params = json::array();
      for (const doc::Parameter& p : m.parameters(o.id)) params.push_back({{"key", p.key}, {"value", p.value}});
      j["parameters"] = params;
    } else if (f == "attributes") {
      j["attributes"] = o.attributes;
    } else if (f == "text") {
      if (const auto label = doc::annotationLabel(o.recipe)) j["text"] = label->text;
    } else {
      throw ApiError("unknown field '" + f + "'");
    }
  }
  return j;
}

json queryObjects(const doc::Modeler& m, const json& a) {
  const ObjectFilter filter(a);
  std::set<std::string> fields = ObjectFilter::strings(a, "fields");
  if (fields.empty()) fields = {"id", "name", "kind", "tag"};
  const std::size_t limit = static_cast<std::size_t>(std::clamp(numberOr(a, "limit", 200), 1.0, 5000.0));
  std::size_t start = 0;
  if (a.contains("cursor")) {
    const std::string c = text(a, "cursor");
    try {
      start = std::stoul(c);
    } catch (...) {
      throw ApiError("bad cursor");
    }
  }
  json objects = json::array();
  std::size_t matched = 0;
  std::optional<std::size_t> next;
  for (const doc::Object& o : m.document().objects()) {
    if (!filter.matches(m.document(), o)) continue;
    if (matched++ < start) continue;
    if (objects.size() == limit) {
      if (!next) next = start + limit;
      continue;  // keep counting the total
    }
    objects.push_back(objectFields(m, o, fields));
  }
  json result = {{"objects", objects}, {"total", matched}, {"sequence", m.document().sequence()}};
  if (next) result["next_cursor"] = std::to_string(*next);
  return result;
}

json aggregateObjects(const doc::Modeler& m, const json& a) {
  const ObjectFilter filter(a);
  const std::string by = a.contains("group_by") ? text(a, "group_by") : std::string();
  if (!by.empty() && by != "kind" && by != "tag" && by != "definition" && by != "name" && by != "parent" &&
      by != "material" && by.rfind("attribute:", 0) != 0) {
    throw ApiError("group_by must be kind, tag, definition, name, parent, material or attribute:<key>");
  }
  struct Sum {
    std::size_t count = 0;
    double volume = 0.0, area = 0.0;
  };
  std::map<std::string, Sum> groups;
  Sum total;
  for (const doc::Object& o : m.document().objects()) {
    if (!filter.matches(m.document(), o)) continue;
    std::string key;
    if (by == "kind") key = doc::toString(o.kind());
    if (by == "tag") key = o.layer;
    if (by == "name") key = o.name;
    if (by == "parent") key = o.parent;
    if (by == "material") key = m.document().inheritedMaterial(o);
    if (by.rfind("attribute:", 0) == 0) {
      const auto it = o.attributes.find(by.substr(10));
      key = it == o.attributes.end() ? std::string() : it->second;
    }
    if (by == "definition") {
      const auto* c = std::get_if<doc::ComponentInstance>(&o.recipe.base);
      key = c ? c->definitionId : std::string();
    }
    Sum add;
    add.count = 1;
    // Component placements count with their definition's volume.
    if (o.kind() == doc::ObjectKind::Solid || o.kind() == doc::ObjectKind::Instance) {
      add.volume = measuresOf(o.shape).volumeValue();
    }
    if (o.kind() == doc::ObjectKind::Face) add.area = measuresOf(o.shape).areaValue();
    for (Sum* s : {&groups[key], &total}) {
      s->count += add.count;
      s->volume += add.volume;
      s->area += add.area;
    }
  }
  json result = {{"count", total.count}, {"volume", total.volume}, {"area", total.area}};
  if (!by.empty()) {
    json list = json::array();
    for (const auto& [key, s] : groups) {
      list.push_back({{"key", key}, {"count", s.count}, {"volume", s.volume}, {"area", s.area}});
    }
    result["groups"] = list;
  }
  return result;
}

json changesSince(const doc::Modeler& m, const json& a) {
  const json& since = arg(a, "since");
  if (!since.is_number_integer() || since.get<long long>() < 0) throw ApiError("'since' must be a sequence number");
  const doc::Document& d = m.document();
  const auto changed = d.changedSince(since.get<std::uint64_t>());
  json result = {{"sequence", d.sequence()}};
  if (!changed) {
    result["full"] = true;  // re-read what you need (model reloaded, tags changed, ...)
    return result;
  }
  json present = json::array(), removed = json::array();
  for (const doc::ObjectId& id : *changed) (d.find(id) ? present : removed).push_back(id);
  result["full"] = false;
  result["changed"] = present;
  result["removed"] = removed;
  return result;
}

json runPlaceOnGrid(doc::Modeler& m, const json& args);

// Named grid lines (from create_grid): lines and their crossings. The same
// name twice is ambiguous and refused rather than guessed.
struct GridLookup {
  const doc::Document& d;
  std::map<std::string, const doc::Object*> lines;
  std::set<std::string> duplicated;

  explicit GridLookup(const doc::Document& document) : d(document) {
    for (const doc::Object& o : d.objects()) {
      if (o.kind() != doc::ObjectKind::Guide) continue;
      if (!lines.emplace(o.name, &o).second) duplicated.insert(o.name);
    }
  }
  std::vector<std::string> named(char prefix) const {
    std::vector<std::string> names;
    for (const auto& [name, o] : lines) {
      if (!name.empty() && name[0] == prefix && o->layer == "Grid") names.push_back(name);
    }
    if (names.empty()) throw ApiError(std::string("no grid lines ") + prefix + "1.. (create_grid first)");
    return names;
  }
  gp_Lin line(const std::string& name) const {
    if (duplicated.count(name)) throw ApiError("grid line name '" + name + "' is ambiguous (used twice)");
    const auto it = lines.find(name);
    if (it == lines.end()) throw ApiError("no grid line named '" + name + "'");
    const auto& g = std::get<doc::GuideLine>(it->second->recipe.base);
    const gp_Trsf t = d.worldTransform(*it->second);
    return gp_Lin(g.point.Transformed(t), g.direction.Transformed(t));
  }
  // Crossing in plan, at the height of the first line.
  gp_Pnt crossing(const std::string& a, const std::string& b) const {
    const gp_Lin xl = line(a), yl = line(b);
    const gp_Dir u = xl.Direction(), v = yl.Direction();
    const double det = u.X() * v.Y() - u.Y() * v.X();
    if (std::abs(det) < 1e-9) throw ApiError("grid lines '" + a + "' and '" + b + "' are parallel");
    const gp_Vec w(xl.Location(), yl.Location());
    const double s = (w.X() * v.Y() - w.Y() * v.X()) / det;
    return xl.Location().Translated(gp_Vec(u) * s);
  }
  // A plan point: [x, y(, z)] or a crossing name "X1/Y2"; z = base.
  gp_Pnt point(const json& v, double base) const {
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      const auto slash = s.find('/');
      if (slash == std::string::npos) throw ApiError("a grid point is written like \"X1/Y2\"");
      const gp_Pnt p = crossing(s.substr(0, slash), s.substr(slash + 1));
      return gp_Pnt(p.X(), p.Y(), base);
    }
    if (!v.is_array() || v.size() < 2) throw ApiError("a point is [x, y] or a grid crossing \"X1/Y2\"");
    return gp_Pnt(v.at(0).get<double>(), v.at(1).get<double>(), base);
  }
};

// --- command table ---------------------------------------------------------------------

using Handler = std::function<json(doc::Modeler&, const json&)>;

struct CommandDef {
  const char* name;
  const char* description;
  const char* schema;  // JSON Schema of "args"
  Handler run;
};

#define SCHEMA(...) #__VA_ARGS__

const std::vector<CommandDef>& commands() {
  static const std::vector<CommandDef> table = {
      // ---- inspection
      {"list_objects", "List all objects (id, name, kind, bounds, parameters). Units: mm.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json list = json::array();
         for (const doc::Object& o : m.document().objects()) list.push_back(describeObject(m, o, false));
         return json{{"objects", list}};
       }},
      {"get_object",
       "Describe one object including its numbered faces (index, normal, centroid, area) for push_pull.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) { return describeObject(m, object(m, text(a, "id")), true); }},

      // ---- creation
      {"create_rectangle",
       "Create a rectangular profile. origin = first corner; normal = plane normal (default +Z); "
       "width along the plane's x direction (world X for horizontal planes), height along y.",
       SCHEMA({"type": "object", "properties": {
         "origin": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "normal": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "width": {"type": "number"}, "height": {"type": "number"}, "name": {"type": "string"}},
         "required": ["origin", "width", "height"]}),
       [](doc::Modeler& m, const json& a) {
         const auto plane = geom::SketchPlane::fromNormal(point(a, "origin"), direction(a, "normal", gp::DZ()));
         return idResult(must(m.createRectangle(plane, number(a, "width"), number(a, "height"),
                                                textOr(a, "name", {}))));
       }},
      {"create_circle", "Create a circular profile (centre, radius, optional plane normal).",
       SCHEMA({"type": "object", "properties": {
         "center": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "normal": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "radius": {"type": "number"}, "name": {"type": "string"}},
         "required": ["center", "radius"]}),
       [](doc::Modeler& m, const json& a) {
         const auto plane = geom::SketchPlane::fromNormal(point(a, "center"), direction(a, "normal", gp::DZ()));
         return idResult(must(m.createCircle(plane, number(a, "radius"), textOr(a, "name", {}))));
       }},
      {"create_regular_polygon",
       "Create a regular polygon profile: 'sides' corners on a circle of 'radius' around 'center' (first corner "
       "along 'x_direction', default +X or the plane's horizontal). Radius and sides stay editable parameters.",
       SCHEMA({"type": "object", "properties": {
         "center": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "normal": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "x_direction": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "radius": {"type": "number"}, "sides": {"type": "integer"}, "name": {"type": "string"}},
         "required": ["center", "radius", "sides"]}),
       [](doc::Modeler& m, const json& a) {
         auto plane = geom::SketchPlane::fromNormal(point(a, "center"), direction(a, "normal", gp::DZ()));
         if (a.contains("x_direction")) {
           gp_Vec x(direction(a, "x_direction", plane.xDir));
           x -= gp_Vec(plane.normal) * x.Dot(gp_Vec(plane.normal));
           if (x.Magnitude() < 1e-9) throw ApiError("'x_direction' must not be along the normal");
           plane.xDir = gp_Dir(x);
         }
         const json& sides = arg(a, "sides");
         if (!sides.is_number_integer()) throw ApiError("'sides' must be an integer");
         return idResult(must(m.createRegularPolygon(plane, number(a, "radius"), sides.get<int>(),
                                                     textOr(a, "name", {}))));
       }},
      {"create_arc",
       "Create an exact circular arc from start to end. bulge = signed distance of the arc's apex from "
       "the chord midpoint (half the chord = semicircle), on the side normal x (end - start). normal = "
       "plane normal (default +Z). closed = segment face (arc + chord) that can be extruded.",
       SCHEMA({"type": "object", "properties": {
         "start": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "end": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "normal": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "bulge": {"type": "number"}, "closed": {"type": "boolean"}, "name": {"type": "string"}},
         "required": ["start", "end", "bulge"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.createArc(point(a, "start"), point(a, "end"), direction(a, "normal", gp::DZ()),
                                          number(a, "bulge"), a.value("closed", false), textOr(a, "name", {}))));
       }},
      {"create_dimension",
       "Add a linear dimension between two points. offset = vector moving the dimension line away from "
       "the measured points (e.g. [0, -600, 0]). text overrides the measured value (optional).",
       SCHEMA({"type": "object", "properties": {
         "start": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "end": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "offset": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "text": {"type": "string"}}, "required": ["start", "end"]}),
       [](doc::Modeler& m, const json& a) {
         const gp_XYZ offset = a.contains("offset") ? xyz(a.at("offset"), "offset") : gp_XYZ(0, 0, 0);
         return idResult(must(m.createDimension(point(a, "start"), point(a, "end"), gp_Vec(offset),
                                                textOr(a, "text", {}))));
       }},
      {"create_guide",
       "Add a construction line (guide) through 'point' along 'direction': drawing snaps to it. Guides are "
       "not exported and can be hidden with the 'Guides' tag.",
       SCHEMA({"type": "object", "properties": {
         "point": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "direction": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}},
         "required": ["point", "direction"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.createGuide(point(a, "point"), direction(a, "direction", gp::DX()))));
       }},
      {"create_grid",
       "Structural grid (通り芯) on the ground: lines X1.. (along Y) at the cumulative 'x_spacings' from "
       "'origin', lines Y1.. (along X) at 'y_spacings', with labels and chained spacing dimensions, all on the "
       "'Grid' tag, in a group (the grid) that remembers them: edit with set_grid, place members with "
       "place_members_on_grid. Drawing snaps to the lines and their crossings. Returns the grid's id and "
       "line ids by name.",
       SCHEMA({"type": "object", "properties": {
         "origin": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "x_spacings": {"type": "array", "items": {"type": "number"}},
         "y_spacings": {"type": "array", "items": {"type": "number"}},
         "label_offset": {"type": "number"}}}),
       [](doc::Modeler& m, const json& a) {
         const gp_Pnt origin = a.contains("origin") ? point(a, "origin") : gp::Origin();
         const auto grid = must(m.createGridLines(origin, numbersOr(a, "x_spacings"), numbersOr(a, "y_spacings"),
                                                  numberOr(a, "label_offset", 1500.0)));
         json lines = json::object();
         for (const ObjectId& id : grid.lines) lines[m.document().find(id)->name] = id;
         return json{{"grid", grid.grid}, {"lines", lines}, {"labels", grid.labels}, {"dimensions", grid.dimensions}};
       }},
      {"hide", "Hide objects (one undo step). Hidden objects are not drawn, snapped to or exported.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.hide(ids(a, "ids")));
         return json::object();
       }},
      {"unhide_all", "Show every hidden object again (one undo step).", SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         must(m.unhideAll());
         return json::object();
       }},
      {"clear_guides", "Delete all guides (one undo step).", SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         must(m.clearGuides());
         return json::object();
       }},
      {"create_text",
       "Add a text note at 'position' (e.g. a room name or area), with a leader line from 'target' if given.",
       SCHEMA({"type": "object", "properties": {
         "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "target": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "text": {"type": "string"}}, "required": ["position", "text"]}),
       [](doc::Modeler& m, const json& a) {
         std::optional<gp_Pnt> target;
         if (a.contains("target")) target = point(a, "target");
         return idResult(must(m.createText(point(a, "position"), target, text(a, "text"))));
       }},
      {"place_device",
       "docs/334: Place a device the plan shows where it is (規則1条の3 表1: 給気口・排気機の位置、住宅用防災機器の位置及び種類): "
       "kind smoke_detector / heat_detector / exhaust_fan / range_hood / supply_vent / water_heater / floor_hatch / undercut, "
       "'position' [x,y,z] in mm (z its height, e.g. the storey's FL + 2100), 'airflow' [dx,dy] the way its air goes "
       "(fans and range hoods: out; supply vents: in; undercuts: through the door) and an optional one-line 'note' for the "
       "legend (\"100φ・防火覆い付 FL+2100\"). Drawn on the plan of its storey with the MLIT manual's symbols.",
       SCHEMA({"type": "object", "properties": {
         "kind": {"type": "string", "enum": ["smoke_detector", "heat_detector", "exhaust_fan", "range_hood", "supply_vent", "water_heater", "floor_hatch", "undercut"]},
         "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "airflow": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2},
         "note": {"type": "string"}}, "required": ["kind", "position"]}),
       [](doc::Modeler& m, const json& a) {
         std::optional<gp_Dir> airflow;
         if (a.contains("airflow")) {
           const auto& v = a.at("airflow");
           const double x = v.at(0).get<double>(), y = v.at(1).get<double>();
           if (std::hypot(x, y) < 1e-9) throw ApiError("'airflow' must not be zero");
           airflow = gp_Dir(x, y, 0);
         }
         return idResult(must(m.placeDevice(text(a, "kind"), point(a, "position"), airflow, textOr(a, "note", {}))));
       }},
      {"set_text", "Change the text of a dimension (empty = measured value) or a text note.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "text": {"type": "string"}},
               "required": ["id", "text"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setAnnotationText(text(a, "id"), text(a, "text")));
         return idResult(text(a, "id"));
       }},
      {"create_polygon", "Create a planar polygon profile from 3+ coplanar points (closed implicitly).",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}},
         "name": {"type": "string"}}, "required": ["points"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.createPolygon(points(a, "points"), textOr(a, "name", {}))));
       }},
      {"create_curve", "Create a NURBS curve through points (closed = periodic).",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}},
         "closed": {"type": "boolean"}, "name": {"type": "string"}}, "required": ["points"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.createCurve(points(a, "points"), a.value("closed", false),
                                            textOr(a, "name", {}))));
       }},
      {"create_box",
       "Create a parametric box: rectangle at origin (min corner) extruded upwards. One undo step.",
       SCHEMA({"type": "object", "properties": {
         "origin": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "width": {"type": "number"}, "depth": {"type": "number"}, "height": {"type": "number"},
         "name": {"type": "string"}}, "required": ["origin", "width", "depth", "height"]}),
       [](doc::Modeler& m, const json& a) {
         geom::SketchPlane plane = geom::SketchPlane::ground();
         plane.origin = point(a, "origin");
         m.history().beginGroup("Box");
         try {
           const ObjectId id = must(m.createRectangle(plane, number(a, "width"), number(a, "depth"),
                                                      textOr(a, "name", {})));
           must(m.extrude(id, number(a, "height")));
           m.history().endGroup();
           return idResult(id);
         } catch (...) {
           m.history().abortGroup();
           throw;
         }
       }},

      // ---- editing
      {"extrude", "Extrude a profile object along its normal (distance may be negative).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "distance": {"type": "number"}},
               "required": ["id", "distance"]}),
       [](doc::Modeler& m, const json& a) {
         const auto r = must(m.extrude(text(a, "id"), number(a, "distance")));
         return json{{"id", r.id}, {"parameter", r.parameterKey}};
       }},
      {"push_pull",
       "Push/pull a planar face. face = index from get_object, or {\"normal\": [x,y,z]} for the outermost "
       "face facing that way, plus \"near\": [x,y,z] for the one of those closest to a point (several "
       "equally outermost / close are refused, listing them). Planar faces only; curved faces by index, "
       "e.g. a cylinder side changes its radius. Positive distance moves the face outwards. A profile lying on another "
       "solid's face cuts into it (negative) or joins it (positive); the merged object gets a new "
       "id, so always continue with the returned id. A flat face of an open shape (faces deleted, or faces "
       "drawn edge to edge) moves out as on a SketchUp surface: side faces join it back to its edges, a face "
       "beyond an edge on the same plane grows or is divided; closing it makes a solid. With keep_start_face "
       "(SketchUp's Ctrl) the face stays and gives a new solid of its own instead (returned id), the face "
       "taken out of the open shape, which stays open.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"},
         "face": {"oneOf": [{"type": "integer"}, {"type": "object", "properties": {"normal": {"type": "array"},
                                                                                   "near": {"type": "array"}}}]},
         "distance": {"type": "number"}, "keep_start_face": {"type": "boolean"}}, "required": ["id", "face", "distance"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         const bool keep = a.contains("keep_start_face") && a.at("keep_start_face").get<bool>();
         const auto r = must(m.pushPull(id, selectFace(m, id, arg(a, "face")), number(a, "distance"), keep));
         return json{{"id", r.id}, {"parameter", r.parameterKey}};
       }},
      {"face_from_loop",
       "Turn loose open curves (lines from create_polyline-like drawing, open arcs, NURBS) that chain into a "
       "closed planar loop with the given curve into one face (e.g. an arched window outline). The curves' "
       "parameters stay editable as in<i>.<key>. Returns the face id.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) { return idResult(must(m.faceFromLoop(text(a, "id")))); }},
      {"create_line",
       "Draw an open polyline (2+ points), like the line tool: if it closes a planar loop with other loose "
       "lines/arcs it becomes a face (auto_face, default true; one undo step) and the result says so; lines "
       "crossing so that areas close make those faces too. Pieces lying on an edge already there (same "
       "group) are not drawn twice ('ids': the lines left; empty when all was there). A line from edge to "
       "edge across a face splits it (use split_face).",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}},
         "auto_face": {"type": "boolean"}, "name": {"type": "string"}}, "required": ["points"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<gp_Pnt> pts = points(a, "points");
         if (!a.value("auto_face", true)) return idResult(must(m.createPolyline(pts, textOr(a, "name", {}))));
         m.history().beginGroup("Line");
         auto line = m.createPolyline(pts, textOr(a, "name", {}));
         if (!line) {
           m.history().abortGroup();
           throw ApiError(line.error());
         }
         bool loopFound = false;
         auto face = m.faceFromLoop(*line, &loopFound);
         // As the line tool: faces from crossings, then nothing stacked twice.
         std::vector<doc::ObjectId> made{face ? *face : *line};
         if (!face && !loopFound) {
           if (auto crossed = m.facesFromCrossings(*line); crossed) {
             made.insert(made.end(), crossed->begin(), crossed->end());
           }
         }
         auto settled = m.settleOverlaps(made);
         if (!settled) {
           m.history().abortGroup();
           throw ApiError(settled.error());
         }
         m.history().endGroup();
         const std::vector<doc::ObjectId> now = doc::Modeler::settledIds(made, *settled);
         if (face) return json{{"id", now.empty() ? *face : now.front()}, {"face", true}};
         json r = {{"id", now.empty() || !m.document().find(now.front()) ? std::string() : now.front()},
                   {"face", false}, {"ids", now}};
         if (loopFound) r["warning"] = "closed loop could not become a face: " + face.error();
         return r;
       }},
      {"split_face",
       "Draw a line (2+ points) across planar faces to split them, so each part can be pushed/pulled "
       "separately (e.g. a band at 1 m on a wall). The line may start and end outside the faces and run "
       "across several faces side by side; each part running across a face from edge to edge divides it "
       "(a part stopping inside a face does not). Returns the number of faces divided.",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}},
         "required": ["points"]}),
       [](doc::Modeler& m, const json& a) {
         // As drawing it: a line across several faces side by side, or
         // starting / ending outside them, divides each face it runs across.
         const doc::Modeler::Divided d = must(m.splitAcross(points(a, "points")));
         if (d.faces == 0) throw ApiError("The line does not cross a face from edge to edge");
         return json{{"faces", d.faces}};
       }},
      {"delete_faces",
       "Delete faces on their own, like selecting faces and pressing Delete (SketchUp): the faces go, their "
       "edges stay as lines unless still used by a face that is left. A loose profile leaves its outline; a "
       "solid becomes an open shape (kind Shell) that can still be drawn on, painted and have more faces "
       "deleted (push/pull and Booleans need it closed again: fill_holes); all faces of an object leave all "
       "its edges as lines. 'edges' (optional) are taken away too, with the faces they bound. faces = list of "
       "selectors (index, {\"normal\": [...], \"near\": [...]}); edges = edge selectors as for erase_line. "
       "Returns the object's id (absent when nothing of it is left) and the lines made. One undo step.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "faces": {"type": "array"},
               "edges": {"type": "array"}},
               "required": ["id", "faces"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         const json& list = arg(a, "faces");
         if (!list.is_array()) throw ApiError("'faces' must be a list");
         std::vector<TopoDS_Face> faces;
         for (const json& f : list) faces.push_back(selectFace(m, id, f));
         std::vector<TopoDS_Edge> edges;
         if (a.contains("edges")) {
           for (const json& e : arg(a, "edges")) edges.push_back(selectEdge(m, id, e));
         }
         const auto lines = must(m.deleteFaces(id, faces, edges));
         json out{{"lines", lines}};
         if (m.document().find(id)) out["id"] = id;
         return out;
       }},
      {"fill_holes", "Fill the flat holes of open shapes with faces; closed again they are solids. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.fillHoles(ids(a, "ids")));
         return idsResult(ids(a, "ids"));
       }},
      {"reverse_faces", "Turn the faces of open shapes / single faces over (front <-> back). Solids always face out.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.reverseFaces(ids(a, "ids")));
         return idsResult(ids(a, "ids"));
       }},
      {"set_locked",
       "Lock objects (true) so that nobody moves, scales or deletes them by accident - e.g. an image underlay "
       "traced over (kind Image) - or unlock them (false). Locked objects are still shown and snapped to.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "locked": {"type": "boolean"}}, "required": ["ids", "locked"]}),
       [](doc::Modeler& m, const json& a) {
         const json& locked = arg(a, "locked");
         if (!locked.is_boolean()) throw ApiError("'locked' must be true or false");
         must(m.setLocked(ids(a, "ids"), locked.get<bool>()));
         return idsResult(ids(a, "ids"));
       }},
      {"erase_edge",
       "The eraser on a line: a line dividing a flat face (made by split_face, or a line drawn across a "
       "loose profile) is taken away and the face is one again; the shape itself does not change. A real "
       "corner of the shape is refused. edge = index or {\"near\": [x,y,z]} (a point on the line). Returns "
       "the object holding the merged face (a loose profile's parts become one new object).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "edge": {}}, "required": ["id", "edge"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         return idResult(must(m.eraseEdge(id, selectEdge(m, id, arg(a, "edge")))));
       }},
      {"move_vertices",
       "Reshape a solid, face or open shape by moving corners (world points on its vertices) by delta, "
       "keeping every face flat: two corners = an edge (e.g. lift the top edge of a box into a roof slope), "
       "all corners of a face = the face (any direction, unlike push/pull). Faces beside it in the same group "
       "that share an edge with it (the other half of a split face) follow. A face that cannot stay flat is "
       "folded into flat pieces along new edges (SketchUp Autofold; only where needed, convex faces without "
       "holes) unless autofold is false, then the move is refused. Refused when a face would turn over or "
       "collapse, or the solid would cross itself; nothing changes then. Or pass edge (index or "
       "{\"near\": [x,y,z]}) instead of points to move that edge. One undo step; the distance becomes a "
       "parameter (opN.distance).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "points": {"type": "array"},
               "edge": {}, "delta": {"type": "array"}, "autofold": {"type": "boolean"}},
               "required": ["id", "delta"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         std::vector<gp_Pnt> corners;
         if (a.contains("edge")) {
           const TopoDS_Edge edge = selectEdge(m, id, a.at("edge"));
           TopoDS_Vertex v1, v2;
           TopExp::Vertices(edge, v1, v2);
           corners = {BRep_Tool::Pnt(v1), BRep_Tool::Pnt(v2)};
         }
         if (a.contains("points")) {
           const std::vector<gp_Pnt> more = points(a, "points");
           corners.insert(corners.end(), more.begin(), more.end());
         }
         if (corners.empty()) throw ApiError("give points or edge");
         must(m.moveVertices(id, corners, gp_Vec(xyz(arg(a, "delta"), "delta")), a.value("autofold", true)));
         return idResult(id);
       }},
      {"set_edges_hidden",
       "Hide (hidden=true, default) or show again the lines of edges of a solid, face or open shape. Display "
       "only: the faces on both sides stay split (e.g. keep a wall divided for two materials without the "
       "line showing). edges = list of index or {\"near\": [x,y,z]}. One undo step; saved.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "edges": {"type": "array"},
               "hidden": {"type": "boolean"}}, "required": ["id", "edges"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         std::vector<TopoDS_Edge> edges;
         for (const json& e : arg(a, "edges")) edges.push_back(selectEdge(m, id, e));
         must(m.setEdgesHidden(id, edges, a.value("hidden", true)));
         return json{{"id", id}, {"hidden_edges", m.document().find(id)->hiddenEdges.size()}};
       }},
      {"show_hidden_edges", "Show every hidden edge line of the objects (and of what they contain).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.showHiddenEdges(ids(a, "ids")));
         return idsResult(ids(a, "ids"));
       }},
      {"erase_line",
       "The eraser on a line, as in SketchUp: on a loose line / arc only the piece between its neighbouring "
       "crossings or touching ends goes (the rest stays as lines); on the outline of a flat face the face goes "
       "and its other edges stay as lines; on a line dividing a flat face of a solid the faces merge; on a "
       "corner edge of a solid the faces meeting there go and the shape stays open. edge = index or "
       "{\"near\": [x,y,z]} (a point on the line, also choosing the piece). Returns the lines left.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "edge": {}}, "required": ["id", "edge"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         const json& selector = arg(a, "edge");
         const TopoDS_Edge edge = selectEdge(m, id, selector);
         gp_Pnt at;
         if (selector.is_object() && selector.contains("near")) {
           at = gp_Pnt(xyz(selector.at("near"), "near"));
         } else {
           BRepAdaptor_Curve c(edge);
           at = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
         }
         const doc::ObjectKind kind = object(m, id).kind();
         if (kind == doc::ObjectKind::Curve) return json{{"lines", must(m.eraseLinePiece(id, edge, at))}};
         if (kind == doc::ObjectKind::Face) {
           if (m.eraseEdge(id, edge)) return json{{"id", id}};
           return json{{"lines", must(m.eraseLinePiece(id, edge, at))}};
         }
         if (auto merged = m.eraseEdge(id, edge)) return json{{"id", *merged}};
         must(m.eraseCornerEdge(id, edge));
         return json{{"id", id}, {"open", true}};
       }},
      {"set_parameter", "Change a parameter (see get_object.parameters), e.g. op0.distance or width.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "key": {"type": "string"},
               "value": {"type": "number"}}, "required": ["id", "key", "value"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setParameter(text(a, "id"), text(a, "key"), number(a, "value")));
         return idResult(text(a, "id"));
       }},
      {"offset",
       "Offset a profile's outline (new object; positive = outwards). The two outlines divide the plane as in "
       "SketchUp: inwards the original becomes the frame around the new face, outwards the new face is the "
       "frame; either can be pushed/pulled alone. With 'face' (a solid's planar face) and a negative distance "
       "the outline is drawn onto that face, dividing it into frame and inner face of the solid (returns the "
       "solid; the distance is its last opN.distance parameter); outwards it is a new profile on that plane.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "distance": {"type": "number"},
               "face": {"oneOf": [{"type": "integer"}, {"type": "object"}]}}, "required": ["id", "distance"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         if (a.contains("face")) {
           return idResult(must(m.offsetSolidFace(id, selectFace(m, id, a.at("face")), number(a, "distance"))));
         }
         return idResult(must(m.offset(id, number(a, "distance"))).id);
       }},
      {"move",
       "Move objects by a vector. Walls joined to a moved wall (corners, T joints made by create_wall) are made "
       "longer or shorter so the joints stay: 'joined_walls' {followed, left: [names that could not follow, e.g. "
       "an opening would be left outside]}. keep_joins: false moves the objects alone. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "vector": {"type": "array", "items": {"type": "number"}}, "keep_joins": {"type": "boolean"}},
               "required": ["ids", "vector"]}),
       [](doc::Modeler& m, const json& a) {
         const bool was = m.keepWallJoins();
         m.setKeepWallJoins(a.value("keep_joins", true));
         const Status moved = m.move(ids(a, "ids"), gp_Vec(xyz(arg(a, "vector"), "vector")));
         m.setKeepWallJoins(was);
         must(moved);
         json result = idsResult(ids(a, "ids"));
         const doc::JoinFollow& joins = m.lastJoinFollow();
         if (joins.followed > 0 || joins.carried > 0 || joins.slabs > 0 || joins.members > 0 || joins.roofs > 0 || !joins.left.empty())
           result["joined_walls"] = {{"followed", joins.followed}, {"carried", joins.carried}, {"slabs", joins.slabs},
                                     {"members", joins.members}, {"roofs", joins.roofs}, {"left", joins.left}};
         return result;
       }},
      {"rotate", "Rotate objects about an axis (degrees).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "axis_origin": {"type": "array"}, "axis_direction": {"type": "array"}, "angle": {"type": "number"}},
               "required": ["ids", "axis_origin", "angle"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.rotate(ids(a, "ids"), gp_Ax1(point(a, "axis_origin"), direction(a, "axis_direction", gp::DZ())),
                       number(a, "angle") * kDegree));
         return idsResult(ids(a, "ids"));
       }},
      {"scale",
       "Scale objects about the world point 'center'. Uniformly by 'factor' (parametric: stays editable as "
       "op<N>.scale), or per direction with 'factors': [fx, fy, fz] along 'x_axis' / 'y_axis' (world, "
       "perpendicular; default the world axes; z = x cross y), e.g. a 1000 box twice as wide: factors [2,1,1] "
       "about its min corner. Exact B-rep (circles become ellipses). Factors must be positive. Groups scale "
       "their contents; components and images (per direction) are refused. One undo step.",
       SCHEMA({"type": "object", "properties": {
         "ids": {"type": "array", "items": {"type": "string"}},
         "center": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "factor": {"type": "number"},
         "factors": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "x_axis": {"type": "array"}, "y_axis": {"type": "array"}}, "required": ["ids", "center"]}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("factors")) {
           const gp_XYZ f = xyz(a.at("factors"), "factors");
           must(m.stretch(ids(a, "ids"), point(a, "center"), direction(a, "x_axis", gp::DX()),
                          direction(a, "y_axis", gp::DY()), f.X(), f.Y(), f.Z()));
           return json::object();
         }
         must(m.scale(ids(a, "ids"), point(a, "center"), number(a, "factor")));
         return json::object();
       }},
      {"copy_array",
       "Copy objects ('ids') or only some edges ('edges': [{\"id\": object, \"edge\": index or "
       "{\"near\": [x,y,z]}}], copied as lines; a straight copy lying across a face from edge to edge divides "
       "it, a copy on an existing edge is not made twice) 'count' times. Linear: give 'vector'; it is the step "
       "between copies, or with 'divide': true the whole distance divided into 'count' steps (SketchUp '/3'). "
       "Polar: give axis_origin, axis_direction and 'angle' (step, degrees; 'divide' divides it too). "
       "'elements' (as in delete: faces / edges picked on an object) copies just those faces (each a face of "
       "its own) and edges (lines), never the object they belong to. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "edges": {"type": "array"}, "elements": {"type": "array"}, "count": {"type": "integer"},
               "vector": {"type": "array"},
               "divide": {"type": "boolean"}, "axis_origin": {"type": "array"},
               "axis_direction": {"type": "array"}, "angle": {"type": "number"}}}),
       [](doc::Modeler& m, const json& a) {
         const int count = a.value("count", 1);
         if (count < 1 || count > 10000) throw ApiError("count must be between 1 and 10000");
         const double share = a.value("divide", false) ? 1.0 / count : 1.0;
         gp_Trsf step;
         if (a.contains("vector")) {
           step.SetTranslation(gp_Vec(xyz(a.at("vector"), "vector")) * share);
         } else {
           step.SetRotation(gp_Ax1(point(a, "axis_origin"), direction(a, "axis_direction", gp::DZ())),
                            number(a, "angle") * kDegree * share);
         }
         if (a.contains("edges")) {
           std::vector<TopoDS_Edge> edges;
           for (const json& item : arg(a, "edges")) edges.push_back(selectEdge(m, text(item, "id"), arg(item, "edge")));
           return idsResult(must(m.copyEdges(edges, step, count)));
         }
         if (a.contains("elements")) return idsResult(must(m.copyElements(selectionTargets(m, a), step, count)));
         return idsResult(must(m.copy(ids(a, "ids"), step, count)));
       }},
      {"boolean", "Solid union / subtract (first minus the others) / intersect. Parametric.",
       SCHEMA({"type": "object", "properties": {"operation": {"enum": ["union", "subtract", "intersect"]},
               "ids": {"type": "array", "items": {"type": "string"}}}, "required": ["operation", "ids"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.boolean(booleanKind(text(a, "operation")), ids(a, "ids"))));
       }},
      {"revolve", "Revolve a profile around an axis (degrees, default 360). Parametric angle.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "axis_origin": {"type": "array"},
               "axis_direction": {"type": "array"}, "angle": {"type": "number"}}, "required": ["id", "axis_origin"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.revolveObject(
             text(a, "id"), gp_Ax1(point(a, "axis_origin"), direction(a, "axis_direction", gp::DZ())),
             numberOr(a, "angle", 360.0) * kDegree)));
       }},
      {"loft", "Loft through section profiles/curves in order (solid by default). Parametric.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "solid": {"type": "boolean"}}, "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.loftObjects(ids(a, "ids"), a.value("solid", true))));
       }},
      {"follow_me",
       "SketchUp's Follow Me: sweep the profile face along 'path' (a curve object), along 'path_edges' (edges "
       "selected first, possibly of several objects: a list of {\"id\": object} for all its edges or "
       "{\"id\": object, \"edge\": index or {\"near\": [x,y,z]}}; joined into one path whatever their order; "
       "branches, gaps and duplicates are refused with the reason), or around the outline of a solid's face "
       "('around': {\"id\": solid, \"face\": index or {\"normal\": [...]}}). The profile is consumed; its "
       "dimensions stay editable on the result (in0.<key>).",
       SCHEMA({"type": "object", "properties": {"profile": {"type": "string"}, "path": {"type": "string"},
               "path_edges": {"type": "array"}, "around": {"type": "object"}}, "required": ["profile"]}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("path")) return idResult(must(m.followMe(text(a, "profile"), text(a, "path"))));
         if (a.contains("path_edges")) {
           std::vector<TopoDS_Edge> edges;
           for (const json& item : arg(a, "path_edges")) {
             const ObjectId owner = text(item, "id");
             if (item.contains("edge")) {
               edges.push_back(selectEdge(m, owner, item.at("edge")));
               continue;
             }
             TopTools_IndexedMapOfShape all;
             TopExp::MapShapes(m.document().worldShape(object(m, owner)), TopAbs_EDGE, all);
             for (int i = 1; i <= all.Extent(); ++i) edges.push_back(TopoDS::Edge(all(i)));
           }
           return idResult(must(m.followMe(text(a, "profile"), edges)));
         }
         const json& around = arg(a, "around");
         const ObjectId solid = text(around, "id");
         const TopoDS_Face face = selectFace(m, solid, arg(around, "face"));
         return idResult(must(m.followMe(text(a, "profile"), BRepTools::OuterWire(face))));
       }},
      {"sweep", "Sweep a profile along a path curve. Parametric.",
       SCHEMA({"type": "object", "properties": {"profile": {"type": "string"}, "path": {"type": "string"}},
               "required": ["profile", "path"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.sweepObject(text(a, "profile"), text(a, "path"))));
       }},
      {"sweep2",
       "Rhino's Sweep2: 'profiles' (curves or faces) swept along 'rail1' while kept touching 'rail2', each "
       "turned and stretched to span the rails. Each of rail1 / rail2 / profiles items is an object id "
       "(a curve) or {\"id\": object, \"edge\": index or {\"near\": [x,y,z]}} for one edge of it. The inputs "
       "stay; the result is a new object (closed profiles give a solid).",
       SCHEMA({"type": "object", "properties": {"rail1": {}, "rail2": {}, "profiles": {"type": "array"}},
               "required": ["rail1", "rail2", "profiles"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::CurvePick> profiles;
         for (const json& p : arg(a, "profiles")) profiles.push_back(curvePick(m, p));
         return idResult(must(m.sweep2(curvePick(m, arg(a, "rail1")), curvePick(m, arg(a, "rail2")), profiles)));
       }},
      {"edge_surface",
       "Rhino's EdgeSrf: a surface from 2, 3 or 4 boundary curves ('curves': ids or {\"id\", \"edge\"} picks). "
       "Two are joined straight across; three or four must meet end to end. The curves stay.",
       SCHEMA({"type": "object", "properties": {"curves": {"type": "array"}}, "required": ["curves"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::CurvePick> curves;
         for (const json& p : arg(a, "curves")) curves.push_back(curvePick(m, p));
         return idResult(must(m.surfaceFromCurves(curves, false)));
       }},
      {"patch",
       "Rhino's Patch: a smooth surface over one closed outline of any number of curves ('curves': ids or "
       "{\"id\", \"edge\"} picks); a flat outline gives a flat face. The curves stay.",
       SCHEMA({"type": "object", "properties": {"curves": {"type": "array"}}, "required": ["curves"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::CurvePick> curves;
         for (const json& p : arg(a, "curves")) curves.push_back(curvePick(m, p));
         return idResult(must(m.surfaceFromCurves(curves, true)));
       }},
      {"fillet_edges",
       "Round edges of a solid: 'edges' are edge indices or {\"near\": [x,y,z]} selectors, 'radius' in mm. "
       "Returns the parameter key; set_parameter changes the radius later.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "edges": {"type": "array"},
               "radius": {"type": "number"}}, "required": ["id", "edges", "radius"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         std::vector<TopoDS_Edge> edges;
         for (const json& e : arg(a, "edges")) edges.push_back(selectEdge(m, id, e));
         return json{{"id", id}, {"parameter", must(m.roundEdges(id, edges, number(a, "radius"), false))}};
       }},
      {"chamfer_edges",
       "Bevel edges of a solid by equal distances: 'edges' as fillet_edges, 'size' in mm. Returns the "
       "parameter key.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "edges": {"type": "array"},
               "size": {"type": "number"}}, "required": ["id", "edges", "size"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         std::vector<TopoDS_Edge> edges;
         for (const json& e : arg(a, "edges")) edges.push_back(selectEdge(m, id, e));
         return json{{"id", id}, {"parameter", must(m.roundEdges(id, edges, number(a, "size"), true))}};
       }},
      {"fillet_corners",
       "Round (or with 'chamfer': true, cut) corners of a flat face object: 'corners' are points [x,y,z] at "
       "the corners, or omitted for every corner; 'size' in mm. Returns the parameter key.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "corners": {"type": "array"},
               "size": {"type": "number"}, "chamfer": {"type": "boolean"}}, "required": ["id", "size"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         std::vector<gp_Pnt> corners;
         if (a.contains("corners")) {
           for (const json& c : arg(a, "corners")) corners.push_back(gp_Pnt(c.at(0), c.at(1), c.at(2)));
         } else {
           TopExp_Explorer f(m.document().worldShape(object(m, id)), TopAbs_FACE);
           if (f.More()) corners = geom::faceCorners(TopoDS::Face(f.Current()));
         }
         return json{{"id", id},
                     {"parameter", must(m.roundCorners(id, corners, number(a, "size"), a.value("chamfer", false)))}};
       }},
      {"fillet_lines",
       "AutoCAD's fillet (or with 'chamfer': true, chamfer) of two straight lines: 'a' and 'b' are "
       "{\"id\": line object, \"segment\": index (default 0), \"near\": [x,y,z] on the part to keep}; each is "
       "cut back or extended to meet the other and joined by an arc of 'size' mm (0: they just meet). "
       "Returns the new arc / cut line, if any.",
       SCHEMA({"type": "object", "properties": {"a": {"type": "object"}, "b": {"type": "object"},
               "size": {"type": "number"}, "chamfer": {"type": "boolean"}}, "required": ["a", "b", "size"]}),
       [](doc::Modeler& m, const json& a) {
         auto pick = [&](const json& p) {
           return doc::Modeler::LinePick{text(p, "id"), p.value("segment", std::size_t(0)), point(p, "near")};
         };
         const auto joint = must(m.filletLines(pick(arg(a, "a")), pick(arg(a, "b")), number(a, "size"),
                                               a.value("chamfer", false)));
         return joint ? json{{"id", *joint}} : json{{"id", nullptr}};
       }},
      {"shell",
       "Rhino's Shell: hollow a solid to walls 'thickness' mm thick, the faces in 'open' (face indices or "
       "{\"normal\": [...]} / {\"near\": [...]} selectors) left open. Returns the parameter key.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "open": {"type": "array"},
               "thickness": {"type": "number"}}, "required": ["id", "open", "thickness"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         std::vector<TopoDS_Face> faces;
         for (const json& f : arg(a, "open")) faces.push_back(selectFace(m, id, f));
         return json{{"id", id}, {"parameter", must(m.shellSolid(id, faces, number(a, "thickness")))}};
       }},

      // ---- structure & attributes
      {"group", "Group objects (same parent).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "name": {"type": "string"}}, "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) { return idResult(must(m.group(ids(a, "ids"), textOr(a, "name", {})))); }},
      {"ungroup", "Dissolve a group, keeping its children in place.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.ungroup(text(a, "id")));
         return json::object();
       }},
      {"make_component", "Turn objects into a shared component definition (returns the instance id).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "name": {"type": "string"}}, "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         return idResult(must(m.makeComponent(ids(a, "ids"), textOr(a, "name", {}))));
       }},
      {"set_definition_parameter", "Edit a parameter inside a component definition (all instances update).",
       SCHEMA({"type": "object", "properties": {"definition": {"type": "string"}, "object": {"type": "string"},
               "key": {"type": "string"}, "value": {"type": "number"}},
               "required": ["definition", "object", "key", "value"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setDefinitionParameter(text(a, "definition"), text(a, "object"), text(a, "key"),
                                       number(a, "value")));
         return json::object();
       }},
      {"placement_parameters",
       "Parameters of a component placement: for each object of its definition, key, value as this placement "
       "shows it, the shared (definition) value, and own=true where only this placement differs. Also the "
       "definition, how many placements share it, and whether this one is mirrored.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         const doc::Object* o = m.document().find(text(a, "id"));
         const auto* c = o ? std::get_if<doc::ComponentInstance>(&o->recipe.base) : nullptr;
         if (!c) throw ApiError("'" + text(a, "id") + "' is not a component placement");
         json list = json::array();
         for (const auto& p : m.placementParameters(o->id)) {
           list.push_back({{"object", p.object}, {"object_name", p.objectName}, {"key", p.parameter.key},
                           {"value", p.parameter.value}, {"shared", p.shared}, {"own", p.own}});
         }
         return json{{"definition", c->definitionId},
                     {"placements", m.document().instanceCount(c->definitionId)},
                     {"mirrored", c->mirrored},
                     {"parameters", list}};
       }},
      {"set_placement_parameter",
       "Change a parameter of a component for ONE placement only (e.g. one door wider); the other placements "
       "keep the shared value, and later shared edits do not touch this one. Setting the shared value again "
       "removes the placement's own value. Use set_definition_parameter to change every placement.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "object": {"type": "string"},
               "key": {"type": "string"}, "value": {"type": "number"}},
               "required": ["id", "object", "key", "value"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setPlacementParameter(text(a, "id"), text(a, "object"), text(a, "key"), number(a, "value")));
         return idResult(text(a, "id"));
       }},
      {"clear_placement_parameters", "Give a component placement the shared values again (drop its own ones).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.clearPlacementParameters(text(a, "id")));
         return idResult(text(a, "id"));
       }},
      {"make_unique",
       "Give one component placement a definition of its own (a copy with its own parameters written in); "
       "the other placements are unaffected. Returns the new definition id.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         return json{{"id", text(a, "id")}, {"definition", must(m.makeUnique(text(a, "id")))}};
       }},
      {"mirror_placement",
       "Mirror component placements in place across their own axis: red (x), green (y) or blue (z). Bounds stay; "
       "the geometry is a mirrored copy of the definition (the placement transform stays rigid).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "axis": {"type": "string", "enum": ["red", "green", "blue", "x", "y", "z"]}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string axis = textOr(a, "axis", "red");
         const int index = axis == "red" || axis == "x" ? 0 : axis == "green" || axis == "y" ? 1
                         : axis == "blue" || axis == "z" ? 2 : -1;
         if (index < 0) throw ApiError("axis must be red, green or blue");
         for (const std::string& id : ids(a, "ids")) must(m.mirrorPlacement(id, index));
         return idsResult(ids(a, "ids"));
       }},
      {"set_component_axes",
       "Move a component's origin to a world point and optionally turn its red axis toward x_axis (about its "
       "blue axis). No placement moves; all placements of the definition are adjusted.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "origin": {"type": "array"},
               "x_axis": {"type": "array"}}, "required": ["id", "origin"]}),
       [](doc::Modeler& m, const json& a) {
         std::optional<gp_Dir> red;
         if (a.contains("x_axis")) red = direction(a, "x_axis", gp::DX());
         must(m.setComponentOrigin(text(a, "id"), point(a, "origin"), red));
         return idResult(text(a, "id"));
       }},
      {"rename", "Rename an object. A room (create_room): its room name, as on plans and in the tables (docs/246).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "name": {"type": "string"}},
               "required": ["id", "name"]}),
       [](doc::Modeler& m, const json& a) {
         // docs/246: a room's name is the text of its marker; renaming the object alone left the room as it was.
         if (const doc::Object* o = m.document().find(text(a, "id"))) {
           const auto kind = o->attributes.find("kind");
           if (kind != o->attributes.end() && kind->second == "room" && std::holds_alternative<doc::TextNote>(o->recipe.base)) {
             if (text(a, "name").empty()) throw ApiError("Name must not be empty");
             must(m.setAnnotationText(text(a, "id"), text(a, "name")));
             return json{{"id", text(a, "id")}, {"room_name", text(a, "name")}};
           }
         }
         must(m.rename(text(a, "id"), text(a, "name")));
         return idResult(text(a, "id"));
       }},
      {"set_tag", "Set an object's tag (layer).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "tag": {"type": "string"}},
               "required": ["id", "tag"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setLayer(text(a, "id"), text(a, "tag")));
         return idResult(text(a, "id"));
       }},
      {"paint", "Paint objects with a plain colour ([r,g,b] 0..1 or \"#rrggbb\"): the material made from that "
                "colour is assigned (created when missing). Groups colour their contents. Prefer named "
                "materials (create_material / assign_material).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "color": {}}, "required": ["ids", "color"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.paint(ids(a, "ids"), color(a, "color")));
         return idsResult(ids(a, "ids"));
       }},
      {"list_materials",
       "The material library: id (stable), name, color, and how many objects (as a whole) and faces use it.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         std::map<std::string, std::pair<int, int>> users;  // objects, faces
         auto count = [&](const doc::Object& o) {
           if (!o.material.empty()) ++users[o.material].first;
           for (const doc::FaceMaterial& f : o.faceMaterials) ++users[f.material].second;
         };
         for (const doc::Object& o : m.document().objects()) count(o);
         for (const doc::ComponentDefinition& d : m.document().definitions()) {
           for (const doc::Object& o : d.objects) count(o);
         }
         json list = json::array();
         for (const doc::Material& mat : m.document().materials()) {
           json j = materialJson(m.document(), mat.id);
           j["objects"] = users[mat.id].first;
           j["faces"] = users[mat.id].second;
           list.push_back(j);
         }
         return json{{"materials", list}};
       }},
      {"create_material",
       "Add a named solid-colour material to the library (color [r,g,b] 0..1 or \"#rrggbb\"). Returns its "
       "stable id; refer to materials by id (or by a unique name).",
       SCHEMA({"type": "object", "properties": {"name": {"type": "string"}, "color": {}},
               "required": ["name", "color"]}),
       [](doc::Modeler& m, const json& a) { return idResult(must(m.createMaterial(text(a, "name"), color(a, "color")))); }},
      {"update_material",
       "Change a material; everything using it changes with it, its id stays. Any of: name, color, opacity "
       "(0..1), texture {width, height (mm per tile), colorize} for its picture or null to remove it, and the "
       "rendering values metallic / roughness / reflectance (0..1; kept for rendering, not shown while "
       "modelling). One undo step. A new picture comes from set_material_texture.",
       SCHEMA({"type": "object", "properties": {"material": {"type": "string"}, "name": {"type": "string"},
               "color": {}, "opacity": {"type": "number"}, "texture": {}, "metallic": {"type": "number"},
               "roughness": {"type": "number"}, "reflectance": {"type": "number"}}, "required": ["material"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string id = materialRef(m.document(), a, "material");
         if (id.empty()) throw ApiError("'material' must name a material");
         doc::Material changed = *m.document().findMaterial(id);
         if (a.contains("name")) changed.name = text(a, "name");
         if (a.contains("color")) changed.color = color(a, "color");
         if (a.contains("opacity")) changed.opacity = number(a, "opacity");
         if (a.contains("metallic")) changed.metallic = number(a, "metallic");
         if (a.contains("roughness")) changed.roughness = number(a, "roughness");
         if (a.contains("reflectance")) changed.reflectance = number(a, "reflectance");
         if (a.contains("texture")) {
           const json& t = a.at("texture");
           if (t.is_null()) {
             changed.texture.reset();
           } else {
             if (!changed.texture) throw ApiError("the material has no texture picture yet (set_material_texture)");
             changed.texture->width = t.value("width", changed.texture->width);
             changed.texture->height = t.value("height", changed.texture->height);
             changed.texture->colorize = t.value("colorize", changed.texture->colorize);
           }
         }
         must(m.editMaterial(changed));
         return materialJson(m.document(), id);
       }},
      {"duplicate_material",
       "Copy a material under a new id (and name, default \"<name> 2\") to change it separately.",
       SCHEMA({"type": "object", "properties": {"material": {"type": "string"}, "name": {"type": "string"}},
               "required": ["material"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string id = materialRef(m.document(), a, "material");
         if (id.empty()) throw ApiError("'material' must name a material");
         return materialJson(m.document(), must(m.duplicateMaterial(id, textOr(a, "name", {}))));
       }},
      {"set_material_texture",
       "Give a material a picture (PNG / JPEG file, embedded in the document) shown at its real size: one tile "
       "covers width x height mm (default 1000 x 1000). colorize tints it with the material colour.",
       SCHEMA({"type": "object", "properties": {"material": {"type": "string"}, "file": {"type": "string"},
               "width": {"type": "number"}, "height": {"type": "number"}, "colorize": {"type": "boolean"}},
               "required": ["material", "file"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string id = materialRef(m.document(), a, "material");
         if (id.empty()) throw ApiError("'material' must name a material");
         const std::string path = text(a, "file");
         std::ifstream in(path, std::ios::binary);
         if (!in) throw ApiError("cannot read " + path);
         const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
         const std::string name = std::filesystem::path(path).filename().string();
         doc::Material changed = *m.document().findMaterial(id);
         changed.texture = doc::MaterialTexture{m.addTextureImage(bytes, name), a.value("width", 1000.0),
                                                a.value("height", 1000.0), a.value("colorize", false)};
         must(m.editMaterial(changed));
         return materialJson(m.document(), id);
       }},
      {"delete_material",
       "Remove a material from the library; objects and faces using it inherit again (same undo step).",
       SCHEMA({"type": "object", "properties": {"material": {"type": "string"}}, "required": ["material"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string id = materialRef(m.document(), a, "material");
         if (id.empty()) throw ApiError("'material' must name a material");
         must(m.deleteMaterial(id));
         return json::object();
       }},
      {"assign_material",
       "Give whole objects a material (id or unique name; \"\" = inherit from the containing group / "
       "component placement again). A group or component placement colours everything inside without a "
       "material of its own; faces painted on their own keep theirs. One undo step. For all walls: "
       "edit_each with where {attributes: {kind: wall}} and ids [\"$id\"].",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "material": {"type": "string"}}, "required": ["ids", "material"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.assignMaterial(ids(a, "ids"), materialRef(m.document(), a, "material")));
         return idsResult(ids(a, "ids"));
       }},
      {"paint_faces",
       "Paint single faces of an object: 'faces' is a list of face selectors (index from get_object, "
       "{\"normal\": [x,y,z]} with optional \"near\"); material \"\" removes their own material (back to the "
       "object's). The paint follows the face through later edits (push/pull, parameters, splits); a face "
       "that cannot be told apart afterwards is left unpainted, never moved to another face.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "faces": {"type": "array"},
               "material": {"type": "string"}}, "required": ["id", "faces", "material"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         const json& list = arg(a, "faces");
         if (!list.is_array() || list.empty()) throw ApiError("'faces' must be a non-empty list");
         std::vector<TopoDS_Face> faces;
         for (const json& f : list) faces.push_back(selectFace(m, id, f));
         must(m.paintFaces(id, faces, materialRef(m.document(), a, "material")));
         return idResult(id);
       }},
      {"clear_face_materials", "Remove the faces' own materials of objects (they show the object's again).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.clearFaceMaterials(ids(a, "ids")));
         return idsResult(ids(a, "ids"));
       }},
      {"material_at",
       "What one face shows and why (source: face, object, container, none): the eyedropper. Use it to "
       "apply \"the same material as this face\" elsewhere.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "face": {}}, "required": ["id", "face"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         const auto at = must(m.materialAt(id, selectFace(m, id, arg(a, "face"))));
         return json{{"material", at.material},
                     {"name", at.material.empty() ? json(nullptr) : materialJson(m.document(), at.material)["name"]},
                     {"source", at.source},
                     {"color", {at.color.r, at.color.g, at.color.b}}};
       }},
      {"delete",
       "Delete, like the Delete key on a selection: 'ids' go as whole objects (with their children); "
       "'elements' = [{\"id\": object, \"faces\": [face selectors], \"edges\": [edge selectors]}] delete "
       "only those faces / edges: a line's edges go alone (the rest of the line stays), a line dividing a flat "
       "face joins the faces, other edges take the faces they bound with them, faces go and leave their edges "
       "as lines. Faces inside a wall's opening (its jambs, head, sill), selected on their own, stand for "
       "the opening: it is removed and the wall is whole again there (as delete_openings). All is located "
       "first, then done as one undo step; on a failure nothing changes. Returns removed, lines, merged, "
       "openings.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "elements": {"type": "array"}}}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<doc::DeleteTarget> targets = selectionTargets(m, a);
         const doc::DeleteOutcome out = must(m.deleteSelection(targets));
         return json{{"removed", out.removed}, {"lines", out.lines}, {"merged", out.merged},
                     {"openings", out.openings}};
       }},

      // ---- view state (kept in the file, not in undo)
      {"set_section",
       "Show the model cut by a section plane through 'origin'; the side 'normal' points to is removed "
       "(e.g. origin [0,0,1200], normal [0,0,1] = floor plan at 1.2 m). Omit both to remove the section.",
       SCHEMA({"type": "object", "properties": {
         "origin": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "normal": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}}}),
       [](doc::Modeler& m, const json& a) {
         if (!a.contains("origin")) {
           m.setSection(std::nullopt);
           return json{{"section", nullptr}};
         }
         doc::SectionPlane plane;
         const gp_Pnt o = point(a, "origin");
         const gp_Dir n = direction(a, "normal", gp::DZ());
         plane.origin = {o.X(), o.Y(), o.Z()};
         plane.normal = {n.X(), n.Y(), n.Z()};
         m.setSection(plane);
         return json{{"section", {{"origin", toJson(o)}, {"normal", toJson(n.XYZ())}}}};
       }},
      {"list_openings",
       "Rectangular openings (doors, windows pushed through) of the given objects (all objects when 'ids' is "
       "omitted): id (stays with the opening when others are removed; use it in 'openings' of "
       "set_opening_size / set_opening_fittings / move_opening / delete_openings), index (its number, "
       "which shifts when an earlier opening is removed), name, width, height, centre, the world height of "
       "the lowest edge (sill), offset (from the wall's left end, seen from the side it was cut from, to "
       "the opening's left edge), sill_height (above the wall's bottom), depth (wall thickness) and its "
       "door / window 'fitting' if set. mm.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}}}),
       [](doc::Modeler& m, const json& a) {
         std::vector<ObjectId> hosts;
         if (a.contains("ids")) {
           hosts = ids(a, "ids");
         } else {
           for (const doc::Object& o : m.document().objects()) hosts.push_back(o.id);
         }
         json list = json::array();
         for (const ObjectId& h : hosts) {
           const auto found = must(m.openings(h));
           if (found.empty()) continue;
           json openings = json::array();
           json fittings = json::object();
           const auto& attrs = object(m, h).attributes;
           if (const auto it = attrs.find("opening_fittings"); it != attrs.end())
             fittings = json::parse(it->second, nullptr, false);
           for (const auto& o : found) {
             json entry = {{"id", o.id}, {"index", o.index}, {"name", o.name}, {"width", o.width},
                           {"height", o.height}, {"center", toJson(o.center)}, {"sill", o.bottom},
                           {"offset", o.offset}, {"sill_height", o.sill}, {"depth", o.depth},
                           {"normal", toJson(o.normal.XYZ())}};
             if (fittings.is_object() && fittings.contains(o.id)) entry["fitting"] = fittings.at(o.id);
             openings.push_back(entry);
           }
           list.push_back({{"host", h}, {"name", object(m, h).name}, {"openings", openings}});
         }
         return json{{"hosts", list}};
       }},
      {"set_opening_size",
       "Resize the rectangular openings of 'host' ('openings': ids or numbers from list_openings, default all) to "
       "'width' and/or 'height' (mm). The horizontal centre and the sill stay put ('keep': \"sill\", "
       "default) or the centre stays ('keep': \"center\"). Stays parametric; one undo step. For many walls "
       "use edit_each with host \"$id\".",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "width": {"type": "number"},
               "height": {"type": "number"}, "keep": {"enum": ["sill", "center"]}}, "required": ["host"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         const std::string keep = textOr(a, "keep", "sill");
         if (keep != "sill" && keep != "center") throw ApiError("'keep' must be sill or center");
         const std::optional<double> w = a.contains("width") ? std::optional<double>(number(a, "width")) : std::nullopt;
         const std::optional<double> h = a.contains("height") ? std::optional<double>(number(a, "height")) : std::nullopt;
         must(m.setOpeningSize(text(a, "host"), which, w, h, keep == "center"));
         return json::object();
       }},
      {"pin_to_grid",
       "Pin members drawn before (create_member, columns and beams) to a grid (docs/122): each end on a crossing "
       "(within 'tolerance', default 10 mm in plan) is pinned to it and to the level at its height; they then "
       "follow set_grid and set_levels as place_members_on_grid members do. Walls (create_wall) too (docs/129): each "
       "end of the wall's line to the nearest crossing within its thickness; when set_grid changes the spacings, "
       "each end goes as far as its crossing (sideways, longer, shorter) and walls joined to it follow. 'ids' "
       "(default: every member and wall not pinned yet), 'grid' (default: the only grid). Members not on crossings "
       "at both ends and walls not between two crossings are left and named. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "grid": {"type": "string"}, "tolerance": {"type": "number"}}}),
       [](doc::Modeler& m, const json& a) {
         const auto pinned = must(m.pinToGrid(a.contains("ids") ? ids(a, "ids") : std::vector<doc::ObjectId>{},
                                               textOr(a, "grid", ""), numberOr(a, "tolerance", 10.0)));
         return json{{"pinned", pinned.pinned}, {"walls", pinned.walls}, {"left", pinned.left}};
       }},
      {"unpin_from_grid",
       "Let members and walls go of their grid (docs/144): they stay where they are and no longer follow set_grid "
       "(nor the levels pinned with it). 'ids' (default: every pinned one). One undo step. Returns how many.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}}}),
       [](doc::Modeler& m, const json& a) {
         return json{{"unpinned", must(m.unpinFromGrid(a.contains("ids") ? ids(a, "ids") : std::vector<doc::ObjectId>{}))}};
       }},
      {"copy_storey",
       "Copy a storey to another level (docs/121): 'from' and 'to' (level ids or names). Every top-level solid "
       "starting in the storey of 'from' (300 under its floor to 300 under the next floor) and its rooms, moved by "
       "the difference of the levels; their levels follow (what was on a level goes onto the one as far above). "
       "Slabs are left when the target storey has a slab. Refused when walls already stand there. One undo step.",
       SCHEMA({"type": "object", "properties": {"from": {"type": "string"}, "to": {"type": "string"}}, "required": ["from", "to"]}),
       [](doc::Modeler& m, const json& a) {
         const auto copied = must(m.copyStorey(text(a, "from"), text(a, "to")));
         return json{{"ids", copied.made}, {"walls", copied.walls}, {"slabs", copied.slabs}, {"rooms", copied.rooms},
                     {"others", copied.others}, {"slabs_left", copied.slabsLeft}};
       }},
      {"copy_opening",
       "Copy one opening of 'host' ('opening': id or number from list_openings): the same width, height and "
       "fitting, and its mark / product number when given by hand (set_opening_mark); a new id. Either 'by': "
       "[dx, dy, dz] (world mm) within the same wall (only the part along the wall and up / down counts), or "
       "'to_host' (another wall) and 'at': [x, y] or [x, y, z], where along that wall it is centred (same sill "
       "height; cut from the side facing the way the original does). 'count' (default 1) copies along 'by' "
       "repeatedly (an array). Refused, with nothing changed, where a copy would not fit or would overlap. One "
       "undo step. Returns the new ids.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "opening": {"type": ["string", "integer"]},
               "by": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
               "to_host": {"type": "string"}, "at": {"type": "array", "items": {"type": "number"}},
               "count": {"type": "integer"}},
               "required": ["host", "opening"]}),
       [](doc::Modeler& m, const json& a) {
         json one = {{"host", arg(a, "host")}, {"openings", json::array({arg(a, "opening")})}};
         const std::size_t index = openingNumbers(m, one).front();
         const bool by = a.contains("by"), to = a.contains("to_host");
         if (by == to) throw ApiError("Give either 'by' (the same wall) or 'to_host' and 'at'");
         json made = json::array();
         if (to) {
           const json& p = arg(a, "at");
           if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) throw ApiError("'at' is [x, y] or [x, y, z]");
           made.push_back(must(m.copyOpening(text(a, "host"), index, gp_Vec(p[0].get<double>(), p[1].get<double>(), 0.0),
                                             text(a, "to_host"))));
           return json{{"ids", made}};
         }
         const int count = a.contains("count") ? static_cast<int>(number(a, "count")) : 1;
         if (count < 1 || count > 100) throw ApiError("'count' is 1 to 100");
         const gp_Vec step(xyz(arg(a, "by"), "by"));
         m.history().beginGroup("Copy Opening");
         for (int k = 1; k <= count; ++k) {
           const auto copy = m.copyOpening(text(a, "host"), index, step * k);
           if (!copy) {
             m.history().abortGroup();
             throw ApiError(copy.error() + (count > 1 ? " (copy " + std::to_string(k) + "; nothing was changed)" : ""));
           }
           made.push_back(*copy);
         }
         m.history().endGroup();
         return json{{"ids", made}};
       }},
      {"move_opening",
       "Move one opening of 'host' within its wall: 'opening' (id or number from list_openings) and either "
       "'by': [dx, dy, dz] (world mm; only the part along the wall and up / down counts) or 'offset' and / "
       "or 'sill_height' as list_openings reports them (from the wall's left end seen from the cut side, "
       "and above the wall's bottom). The opening keeps its id, size and fitting; it must stay inside the "
       "wall and clear of the other openings (otherwise nothing changes). One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "opening": {"type": ["string", "integer"]},
               "by": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
               "offset": {"type": "number"}, "sill_height": {"type": "number"}},
               "required": ["host", "opening"]}),
       [](doc::Modeler& m, const json& a) {
         json one = {{"host", arg(a, "host")}, {"openings", json::array({arg(a, "opening")})}};
         const std::size_t index = openingNumbers(m, one).front();
         const bool by = a.contains("by"), to = a.contains("offset") || a.contains("sill_height");
         if (by == to) throw ApiError("Give either 'by' or 'offset' / 'sill_height'");
         if (by) {
           must(m.moveOpening(text(a, "host"), index, gp_Vec(xyz(arg(a, "by"), "by"))));
         } else {
           must(m.placeOpening(text(a, "host"), index,
                               a.contains("offset") ? std::optional<double>(number(a, "offset")) : std::nullopt,
                               a.contains("sill_height") ? std::optional<double>(number(a, "sill_height"))
                                                         : std::nullopt));
         }
         json moved = json::object();
         for (const auto& o : must(m.openings(text(a, "host"))))
           if (o.index == index)
             moved = {{"id", o.id}, {"index", o.index}, {"offset", o.offset}, {"sill_height", o.sill},
                      {"center", toJson(o.center)}};
         return json{{"opening", moved}};
       }},
      {"delete_openings",
       "Remove openings of 'host' ('openings': ids or numbers from list_openings): the wall is whole again "
       "there. Their fittings go with them; the other openings keep their ids, sizes and fittings (their "
       "numbers shift). One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}, "minItems": 1}},
               "required": ["host", "openings"]}),
       [](doc::Modeler& m, const json& a) {
         const auto which = openingNumbers(m, a);
         if (which.empty()) throw ApiError("'openings' must name at least one opening");
         must(m.removeOpenings(text(a, "host"), which));
         return json{{"remaining", must(m.openings(text(a, "host"))).size()}};
       }},
      {"point_opening_fitting",
       "Choose the way an opening's door opens by a point in plan (docs/188), as clicking beside it does: the side "
       "of the wall the point is on gives in / out, the half of the opening it is over gives the hinge (swing_single, "
       "swing_unequal), the jamb a 折れ戸 gathers at, the way a 片引き opens. 'host', 'opening' (id or number from "
       "list_openings), 'point' [x, y]. The fitting's type stays (give it first with set_opening_fittings). One undo "
       "step. Returns the fitting now.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"}, "opening": {"type": ["string", "integer"]},
               "point": {"type": "array"}}, "required": ["host", "opening", "point"]}),
       [](doc::Modeler& m, const json& a) {
         const auto list = must(m.openings(text(a, "host")));
         const json& which = arg(a, "opening");
         const doc::Modeler::OpeningInfo* op = nullptr;
         for (const auto& o : list)
           if ((which.is_string() && o.id == which.get<std::string>()) ||
               (which.is_number_integer() && static_cast<long long>(o.index) == which.get<long long>()))
             op = &o;
         if (!op) throw ApiError("No such opening on that host");
         const doc::Object* host = m.document().find(text(a, "host"));
         const auto stored = host->attributes.find("opening_fittings");
         const json fittings = stored == host->attributes.end() ? json() : json::parse(stored->second, nullptr, false);
         if (!fittings.is_object() || !fittings.contains(op->id))
           throw ApiError("The opening has no door yet: give it one with set_opening_fittings first");
         const json& p = arg(a, "point");
         if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) throw ApiError("'point' is [x, y]");
         const std::string fitting = must(draw::fittingTowards(fittings.at(op->id).dump(), op->center, op->normal, op->depth,
                                                               gp_Pnt(p[0].get<double>(), p[1].get<double>(), op->center.Z())));
         must(m.setOpeningFittings(text(a, "host"), {op->index}, fitting));
         return json{{"fitting", json::parse(fitting)}};
       }},
      {"set_opening_fittings",
       "Door / window kind of openings of 'host' ('openings': ids or numbers from list_openings, default all), "
       "drawn as plan symbols in the plan cut and on plan drawings (DXF / JWW / PDF) to the real width and "
       "wall thickness. 'fitting': {\"type\": none | window | swing_single (\"hinge\": left|right, "
       "\"swing\": in|out) | swing_double (\"swing\") | swing_unequal (親子開き: \"hinge\" of the parent leaf, "
       "\"swing\", \"child\" leaf width 150-900, default 300) | folding (折れ戸: \"fold\": left|right, the jamb it "
       "gathers at; \"swing\") | sliding_single (\"slide\": left|right; 片引き) | "
       "sliding_double (引違い) | sliding_bi (両引き)}. point_opening_fitting chooses the way it opens by a point. \"out\" is the side the opening was cut from (its "
       "normal); left / right as seen from the side a door opens to, or from the cut side for sliding doors. "
       "\"none\" removes it. One undo step; combine with place_openings in a batch.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "fitting": {"type": "object"}},
               "required": ["host", "fitting"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         if (!arg(a, "fitting").is_object()) throw ApiError("'fitting' must be an object");
         must(m.setOpeningFittings(text(a, "host"), which, a.at("fitting").dump()));
         return json::object();
       }},
      {"assign_levels",
       "Give objects (topmost of 'ids', or all top-level objects) the attribute 'key' (default \"level\") "
       "= the name of the highest level at or below their lowest point. 'levels': [{name, elevation}] in mm "
       "(e.g. 1F 0, 2F 3000). Returns objects below every level (left unchanged). One undo step.",
       SCHEMA({"type": "object", "properties": {
         "levels": {"type": "array", "items": {"type": "object"}}, "ids": {"type": "array", "items": {"type": "string"}},
         "key": {"type": "string"}}, "required": ["levels"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::Level> levels;
         const json& list = arg(a, "levels");
         if (!list.is_array()) throw ApiError("'levels' must be an array");
         for (const json& l : list) levels.push_back({text(l, "name"), number(l, "elevation")});
         const auto below = must(m.assignLevels(a.contains("ids") ? ids(a, "ids") : std::vector<ObjectId>{}, levels,
                                                textOr(a, "key", "level")));
         return json{{"below_all_levels", below}};
       }},
      {"align",
       "Line objects up along a world 'axis' (x, y or z): their bounds' 'edge' (min, max or center) is moved "
       "to 'value' (mm), or to that of the first object. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "axis": {"enum": ["x", "y", "z"]}, "edge": {"enum": ["min", "max", "center"]},
               "value": {"type": "number"}}, "required": ["ids", "axis", "edge"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string axis = text(a, "axis"), edge = text(a, "edge");
         const int ax = axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : -1;
         const int ed = edge == "min" ? -1 : edge == "max" ? 1 : edge == "center" ? 0 : 2;
         if (ax < 0 || ed == 2) throw ApiError("axis must be x/y/z and edge min/max/center");
         must(m.align(ids(a, "ids"), ax, ed, a.contains("value") ? std::optional<double>(number(a, "value"))
                                                                 : std::nullopt));
         return json::object();
       }},
      {"set_attribute",
       "Give objects a meaning attribute (e.g. key \"kind\" value \"wall\", key \"level\" value \"2F\") "
       "to find them later with query_objects / edit_each (attributes filter) or aggregate "
       "(group_by \"attribute:level\"). An empty value removes the key. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "key": {"type": "string"}, "value": {"type": "string"}}, "required": ["ids", "key", "value"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setAttribute(ids(a, "ids"), text(a, "key"), text(a, "value")));
         return json::object();
       }},
      {"set_follow",
       "Objects 'ids' follow 'host' (an id) from where they are now: when the host is moved or turned, "
       "they go with it, keeping their place on it (its own shape changes move nothing). Moved "
       "themselves, they keep following from there; moved together with the host, once. 'host' null: "
       "stop following. Nothing follows unless asked (snapping or placing never makes it). Refused for "
       "itself, an object inside the other, or a circle of followers. The host deleted: they stay put "
       "(get_object follow.state = suspended). One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "host": {"type": ["string", "null"]}}, "required": ["ids", "host"]}),
       [](doc::Modeler& m, const json& a) {
         const json& host = arg(a, "host");
         if (!host.is_null() && !host.is_string()) throw ApiError("'host' must be an id or null");
         must(m.setFollow(ids(a, "ids"), host.is_null() ? std::nullopt : std::optional<ObjectId>(host.get<std::string>())));
         return json::object();
       }},
      {"create_wall",
       "Walls along a plan polyline: 'points' are [x, y] (mm) or grid crossings like \"X1/Y1\"; repeat the "
       "first point at the end for a closed outline. One wall per segment, corners butt-joined (exact at "
       "right angles), 'thickness' and 'height' (mm) from 'base' (default 0). 'justification': center "
       "(default), left or right of the line, looking along it. Length, thickness and height stay editable "
       "(width / height / op0.distance). Walls get attribute kind=wall. One undo step. Timber walls on the "
       "column centre line: 'type' instead of thickness = exterior_siding, exterior_metal, "
       "exterior_lath_mortar, exterior_lath_mortar_direct or interior_board (layers outside -> inside around "
       "the 'core', default 105 = column width); the outside is on the left of the line ('outside': right "
       "swaps), a closed outline turns so it faces out. Layer thicknesses are usual drawing values, not "
       "product specifications. 'mitre': true mitres the corners (as mitre_wall_corners, also with walls "
       "already mitred where they meet), as the wall tool does; default false (butt-joined).",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array"}, "thickness": {"type": "number"}, "height": {"type": "number"},
         "base": {"type": "number"}, "justification": {"enum": ["center", "left", "right"]},
         "type": {"type": "string"}, "core": {"type": "number"}, "outside": {"enum": ["left", "right"]},
         "mitre": {"type": "boolean"}},
         "required": ["points", "height"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         // docs/177: mitred corners only when asked; the setting is put back whatever happens.
         struct Mitre {
           doc::Modeler& m;
           bool was;
           ~Mitre() { m.setMitreNewWalls(was); }
         } keep{m, m.mitreNewWalls()};
         m.setMitreNewWalls(a.value("mitre", false));
         const double base = numberOr(a, "base", 0.0);
         std::vector<gp_Pnt> pts;
         const json& list = arg(a, "points");
         if (!list.is_array()) throw ApiError("'points' must be an array");
         for (const json& p : list) pts.push_back(grid.point(p, base));
         if (a.contains("type")) {
           const std::string side = textOr(a, "outside", "left");
           if (side != "left" && side != "right") throw ApiError("outside: left or right");
           return idsResult(must(m.createTypedWalls(pts, number(a, "height"), text(a, "type"),
                                                    numberOr(a, "core", 105.0), side == "right")));
         }
         if (!a.contains("thickness")) throw ApiError("create_wall needs 'thickness' or 'type'");
         const std::string j = textOr(a, "justification", "center");
         if (j != "center" && j != "left" && j != "right") throw ApiError("justification: center, left or right");
         return idsResult(must(m.createWalls(pts, number(a, "thickness"), number(a, "height"),
                                             j == "left" ? -1 : j == "right" ? 1 : 0)));
       }},
      {"set_wall_type",
       "Give walls already drawn ('ids', kind = wall) a timber build-up ('type' as in create_wall, 'core' "
       "column width, default 105): each keeps the line it was drawn on and gets the layers around it. "
       "'outside': auto (default: a typed wall keeps its outside, otherwise the side away from the middle "
       "of the walls given), left or right of the line, or flip (the other side from auto). Corners and T "
       "joints with neighbouring walls are "
       "fitted again; openings move to the new faces. Returns every wall changed. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "type": {"type": "string"}, "core": {"type": "number"}, "outside": {"enum": ["auto", "left", "right", "flip"]}},
               "required": ["ids", "type"]}),
       [](doc::Modeler& m, const json& a) {
         return idsResult(must(m.setWallType(ids(a, "ids"), text(a, "type"), numberOr(a, "core", 105.0),
                                             textOr(a, "outside", "auto"))));
       }},
      {"create_roof",
       "A roof as one solid over a plan 'outline' ([x, y] or \"X1/Y1\" points: right-angled ones (rectangles, L, T, "
       "U shapes) or with slanted sides (docs/108, docs/138, docs/160: up to four inward corners; "
       "gable: 'gable_sides', 0-based side numbers, side i from corner i to i+1, default the shorter pair of "
       "parallel opposite sides, with inward corners the ends of the wings). 'sides': [{side, pitch, eave}] gives "
       "sides a pitch / an overhang of their own (the overhang of a gable side is its verge; with 'sides' a "
       "right-angled outline is worked out like a slanted one: an L's outer corner gets a hip); 'wall_sides': side "
       "numbers, or \"auto\", where the roof ends against a wall standing higher (a lower roof at the upper "
       "storey's wall, docs/161: upright there, no overhang, for every kind)) or 'grid' {x: [..2 names], y: [..2 names]}. 'kind': gable (切妻, default), "
       "hip (寄棟) or shed (片流れ). 'height' (mm): z of the roof's underside above the outline (the top of the "
       "walls / plates). 'pitch': rise per 10 of run (寸勾配, default 4). 'eave': overhang at eaves in plan "
       "(default 600); 'gable_eave': at gable ends (default 300; shed: at the sloping sides). 'thickness' at "
       "right angles to the slope (default 100). gable over one rectangle: 'ridge' long (default) or short. "
       "shed: 'fall' [x, y], the plan direction it falls towards (default [0, -1]). L / T shapes get valleys. "
       "Attribute kind=roof; sizes stay changeable (change_roof). Made at a level's height it follows that "
       "level. Wall tops are not cut to it. One undo step.",
       SCHEMA({"type": "object", "properties": {
         "outline": {"type": "array"}, "grid": {"type": "object"}, "kind": {"enum": ["gable", "hip", "shed"]},
         "height": {"type": "number"}, "pitch": {"type": "number"}, "eave": {"type": "number"},
         "gable_eave": {"type": "number"}, "thickness": {"type": "number"}, "ridge": {"enum": ["long", "short"]},
         "fall": {"type": "array"}, "gable_sides": {"type": "array", "items": {"type": "integer"}}, "sides": {"type": "array", "items": {"type": "object"}}, "wall_sides": {}}, "required": ["height"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         geom::RoofSpec spec;
         if (a.contains("grid")) {
           const json& g = a.at("grid");
           const auto xs = ObjectFilter::strings(g, "x"), ys = ObjectFilter::strings(g, "y");
           if (xs.size() != 2 || ys.size() != 2) throw ApiError("'grid' needs two x and two y line names");
           const std::string &x0 = *xs.begin(), &x1 = *xs.rbegin(), &y0 = *ys.begin(), &y1 = *ys.rbegin();
           spec.outline = {grid.crossing(x0, y0), grid.crossing(x1, y0), grid.crossing(x1, y1), grid.crossing(x0, y1)};
         } else {
           for (const json& p : arg(a, "outline")) spec.outline.push_back(grid.point(p, 0.0));
         }
         roofArgs(a, spec);
         wallSidesAuto(m, a, spec);
         const std::string id = must(m.createRoof(spec));
         json result = roofJson(must(m.roofInfo(id)));
         result["id"] = id;
         return result;
       }},
      {"roof_info",
       "A roof made by create_roof as it stands: kind, outline, height, pitch, eave, gable_eave, thickness, ridge, "
       "fall, plan_area / slope_area (m2, overhangs included; slope_area is its upper surface), ridge_height, "
       "eave_height (mm), level. Refused for a roof reshaped by hand or tilted.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         json result = roofJson(must(m.roofInfo(text(a, "id"))));
         result["id"] = text(a, "id");
         return result;
       }},
      {"change_roof",
       "Change sizes of a roof made by create_roof (any of kind, outline, height, pitch, eave, gable_eave, "
       "thickness, ridge, fall); the rest stays. The same object. One undo step.",
       SCHEMA({"type": "object", "properties": {
         "id": {"type": "string"}, "outline": {"type": "array"}, "kind": {"enum": ["gable", "hip", "shed"]},
         "height": {"type": "number"}, "pitch": {"type": "number"}, "eave": {"type": "number"},
         "gable_eave": {"type": "number"}, "thickness": {"type": "number"}, "ridge": {"enum": ["long", "short"]},
         "fall": {"type": "array"}, "gable_sides": {"type": "array", "items": {"type": "integer"}}, "sides": {"type": "array", "items": {"type": "object"}}, "wall_sides": {}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         geom::RoofSpec spec = must(m.roofInfo(text(a, "id"))).spec;
         if (a.contains("outline")) {
           spec.outline.clear();
           for (const json& p : a.at("outline")) spec.outline.push_back(grid.point(p, 0.0));
         }
         roofArgs(a, spec);
         wallSidesAuto(m, a, spec);
         must(m.changeRoof(text(a, "id"), spec));
         json result = roofJson(must(m.roofInfo(text(a, "id"))));
         result["id"] = text(a, "id");
         return result;
       }},
      {"create_stair",
       "A stair along 'path': its walking line in plan ([x, y] or \"X1/Y1\" points), the middle of its width, from "
       "the foot of the first riser to the edge of the upper floor. Two points: a straight stair; more: it turns "
       "(right angles only; a U-turn is two turns, the part between them at least 'width' long, exactly 'width' for "
       "a turn about one point). 'base' (z of the lower floor, default 0) and 'rise' (floor to floor) or 'top' (z). "
       "'width' (default 800). 'turns': how each turn's corner square is made, one value for all or a list in "
       "order: 1 or \"landing\" = a flat landing (two in a row with nothing between = one half landing), 2 = two "
       "triangular winders (45 degrees), 3 = three (30 degrees, default), up to 6. 'risers': their number, or 0 "
       "(default) = as few as keep each within 'riser_limit' (default 200), worked out again when the rise changes. "
       "Straight treads are shared out over the flights so that the goings are as even as the line allows. 'style': "
       "block (one stepped solid, default) or boards (tread boards only); 'thickness' (default 30). 'open_floor': "
       "true also opens the slabs over it (open_floor_for_stair). Attribute kind=stair; sizes stay changeable "
       "(change_stair); foot and top at levels' heights follow those levels. 'handrail': none (default), left, right "
       "or both (looking up), 'handrail_height' above the treads (default 850, 600-1200). 'stringer': side boards "
       "(none default, left, right, both) just outside the width, 'stringer_thickness' (default 36, 15-100); their top "
       "50 over the nosings, deep enough to hold the treads; 'stringer_style' side (default) or cut (ささら桁: inside "
       "the width, under the treads, its top cut in steps). 'riser_boards' (boards style; 'riser_thickness' default 15) "
       "and 'newels' (posts with the side boards at their foot and at the inside of turns; 'newel_size' default 105) "
       "(docs/135). 'nosing' 0-50 (段鼻の出, docs/137; goings are measured without it). 'shape' spiral (らせん, docs/139): "
       "'path' is then [centre, foot of the walking line] and 'sweep' the degrees it turns going up (positive "
       "counter-clockwise, 90-720, default 270); a round post inside. Returns riser, goings and 'checks' "
       "against the sizes for a house's stair (see checks_basis; not_evaluated is never a pass).",
       SCHEMA({"type": "object", "properties": {
         "path": {"type": "array"}, "base": {"type": "number"}, "rise": {"type": "number"}, "top": {"type": "number"},
         "width": {"type": "number"}, "turns": {}, "risers": {"type": "integer"}, "riser_limit": {"type": "number"},
         "style": {"enum": ["block", "boards"]}, "thickness": {"type": "number"}, "open_floor": {"type": "boolean"},
         "handrail": {"enum": ["none", "left", "right", "both"]}, "handrail_height": {"type": "number"},
         "stringer": {"enum": ["none", "left", "right", "both"]}, "stringer_thickness": {"type": "number"},
         "stringer_style": {"enum": ["side", "cut"]}, "riser_boards": {"type": "boolean"}, "riser_thickness": {"type": "number"},
         "newels": {"type": "boolean"}, "newel_size": {"type": "number"}, "nosing": {"type": "number"},
         "shape": {"enum": ["line", "spiral"]}, "sweep": {"type": "number"}},
         "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         geom::StairSpec spec;
         for (const json& p : arg(a, "path")) spec.path.push_back(grid.point(p, 0.0));
         stairArgs(a, spec);
         const bool open = a.contains("open_floor") && a.at("open_floor").is_boolean() && a.at("open_floor").get<bool>();
         if (open) m.history().beginGroup("Stair");
         const auto made = m.createStair(spec);
         if (open && made) {
           if (const auto opened = m.openFloorForStair(*made); !opened) {
             m.history().abortGroup();
             throw ApiError(opened.error() + " (the stair was not made: call without open_floor to make it alone)");
           }
         }
         if (open) made ? m.history().endGroup() : m.history().abortGroup();
         const std::string id = must(made);
         json result = stairJson(must(m.stairInfo(id)));
         result["id"] = id;
         return result;
       }},
      {"stair_info",
       "A stair made by create_stair as it stands: path, base, rise, width, risers (as built), risers_automatic, "
       "riser, treads, going_min / going_max (straight treads), winder_going (300 from the narrow end), walk_length, "
       "flights, landings, winders, turns, style, thickness, base_level, top_level, opened_slabs and checks. Refused "
       "for a stair reshaped by hand or tilted.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         json result = stairJson(must(m.stairInfo(text(a, "id"))));
         result["id"] = text(a, "id");
         return result;
       }},
      {"change_stair",
       "Change sizes of a stair made by create_stair (any of path, base, rise or top, width, turns, risers, "
       "riser_limit, style, thickness, handrail, handrail_height, stringer, stringer_thickness, stringer_style, riser_boards, "
       "riser_thickness, newels, newel_size, nosing, shape, sweep); the rest stays. The same object; floors opened for it are opened again. "
       "One undo step.",
       SCHEMA({"type": "object", "properties": {
         "id": {"type": "string"}, "path": {"type": "array"}, "base": {"type": "number"}, "rise": {"type": "number"},
         "top": {"type": "number"}, "width": {"type": "number"}, "turns": {}, "risers": {"type": "integer"},
         "riser_limit": {"type": "number"}, "style": {"enum": ["block", "boards"]}, "thickness": {"type": "number"},
         "handrail": {"enum": ["none", "left", "right", "both"]}, "handrail_height": {"type": "number"},
         "stringer": {"enum": ["none", "left", "right", "both"]}, "stringer_thickness": {"type": "number"},
         "stringer_style": {"enum": ["side", "cut"]}, "riser_boards": {"type": "boolean"}, "riser_thickness": {"type": "number"},
         "newels": {"type": "boolean"}, "newel_size": {"type": "number"}, "nosing": {"type": "number"},
         "shape": {"enum": ["line", "spiral"]}, "sweep": {"type": "number"}},
         "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         geom::StairSpec spec = must(m.stairInfo(text(a, "id"))).spec;
         if (a.contains("path")) {
           spec.path.clear();
           for (const json& p : a.at("path")) spec.path.push_back(grid.point(p, 0.0));
         }
         stairArgs(a, spec);
         must(m.changeStair(text(a, "id"), spec));
         json result = stairJson(must(m.stairInfo(text(a, "id"))));
         result["id"] = text(a, "id");
         return result;
       }},
      {"open_floor_for_stair",
       "Opens the floors over a stair (slabs made by create_slab): the plan of every tread with less than "
       "'headroom' (default 2000) up to the slab's underside. 'slabs': ids, or omit for every slab whose thickness "
       "holds the stair's top. The slab stays a slab; the opening follows the stair and the slab, and closes when "
       "the stair is deleted. Refused when no such slab lies over it. One undo step.",
       SCHEMA({"type": "object", "properties": {"stair": {"type": "string"}, "slabs": {"type": "array", "items": {"type": "string"}},
                                                "headroom": {"type": "number"}}, "required": ["stair"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<doc::ObjectId> slabs = a.contains("slabs") ? ids(a, "slabs") : std::vector<doc::ObjectId>{};
         return json{{"stair", text(a, "stair")},
                     {"slabs", must(m.openFloorForStair(text(a, "stair"), slabs, numberOr(a, "headroom", 2000.0)))}};
       }},
      {"close_floor_for_stair",
       "Closes the floors opened for 'stair' (open_floor_for_stair). One undo step.",
       SCHEMA({"type": "object", "properties": {"stair": {"type": "string"}}, "required": ["stair"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.closeFloorForStair(text(a, "stair")));
         return json{{"stair", text(a, "stair")}};
       }},
      {"mitre_wall_corners",
       "Mitre the corners of walls (docs/159): 'walls' (ids, default all) are marked; where exactly two marked "
       "walls meet at a corner both end on the diagonal of the corner (inner to outer corner point) instead of "
       "one butting against the other: no seam on the faces, no gap / overlap at corners that are not right "
       "angles. The walls stay walls (lengths, joins, openings, rooms as before) and are mitred again when they "
       "or their neighbours change. 'on': false takes the mark off (square ends again). T-junctions and corners "
       "of three walls stay as they are. One undo step. Returns 'corners': those mitred now.",
       SCHEMA({"type": "object", "properties": {"walls": {"type": "array", "items": {"type": "string"}}, "on": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<doc::ObjectId> walls = a.contains("walls") ? ids(a, "walls") : std::vector<doc::ObjectId>{};
         return json{{"corners", must(m.mitreWallCorners(walls, a.value("on", true)))}};
       }},
      {"fit_walls_to_roof",
       "Walls made by create_wall stand up to the underside of 'roof' (create_roof): made tall enough, then cut "
       "to it (gable-end walls get their triangle). 'walls': ids, or omit for every wall under the roof (its line "
       "within the roof's outline, starting below its bearing height). They stay walls (length, type, openings "
       "editable) and are fitted again when the roof or the wall changes or moves. Refused, with nothing "
       "changed, for a wall reshaped by hand or wholly above the roof. One undo step.",
       SCHEMA({"type": "object", "properties": {"roof": {"type": "string"}, "walls": {"type": "array", "items": {"type": "string"}}},
               "required": ["roof"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string roof = text(a, "roof");
         const std::vector<doc::ObjectId> walls = a.contains("walls") ? ids(a, "walls") : m.wallsUnderRoof(roof);
         if (walls.empty()) throw ApiError("No walls made by create_wall stand under this roof");
         must(m.fitWallsToRoof(walls, roof));
         return json{{"roof", roof}, {"walls", walls}};
       }},
      {"release_walls_from_roof",
       "Walls fitted to a roof get flat tops again at the height they had before. 'walls': ids, or 'roof' for "
       "every wall fitted to it. One undo step.",
       SCHEMA({"type": "object", "properties": {"roof": {"type": "string"}, "walls": {"type": "array", "items": {"type": "string"}}}}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<doc::ObjectId> walls = a.contains("walls") ? ids(a, "walls") : m.wallsFittedToRoof(text(a, "roof"));
         if (walls.empty()) throw ApiError("No walls are fitted to this roof");
         must(m.releaseWallsFromRoof(walls));
         return json{{"walls", walls}};
       }},
      {"create_room",
       "A room: a marker (its name as a text note, attribute kind=room) at plan point 'at' ([x, y] or \"X1/Y1\") "
       "on the floor at 'base' (mm, default 0). The room is the region the walls made by create_wall enclose "
       "there (walls a plan cut 1100 above the floor meets). Refused when the place is not enclosed (the error "
       "names it; nothing is closed for you: see room_at for the loose wall ends) or already has a room. "
       "Areas are read from the walls whenever asked (list_rooms): they follow moved walls. One undo step.",
       SCHEMA({"type": "object", "properties": {"at": {}, "base": {"type": "number"}, "name": {"type": "string"}},
               "required": ["at", "name"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         return idResult(must(m.createRoom(grid.point(arg(a, "at"), numberOr(a, "base", 0.0)), text(a, "name"))));
       }},
      {"room_at",
       "The region plan point 'at' is in on the floor at 'base' (default 0), without making a room: closed, "
       "areas (m2: area_centre within wall lines 壁芯, area_inner within wall faces 内法 or null when the faces "
       "give no simple outline - not evaluated, never a guess), walls, and when not closed the nearest "
       "loose_ends of walls.",
       SCHEMA({"type": "object", "properties": {"at": {}, "base": {"type": "number"}}, "required": ["at"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         return roomJson(m.roomAt(grid.point(arg(a, "at"), numberOr(a, "base", 0.0))));
       }},
      {"set_opening_mark",
       "Give openings of 'host' a mark of your own ('mark', e.g. \"AW-1\") and / or a product number ('product'): "
       "used by opening_schedule and the plan tags instead of the worked-out mark, and kept when openings are "
       "added, resized or removed. 'openings': ids or indices (all when omitted). Both empty: back to the "
       "worked-out mark. One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "mark": {"type": "string"},
               "product": {"type": "string"}}, "required": ["host"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         must(m.setOpeningMarks(text(a, "host"), which, textOr(a, "mark", ""), textOr(a, "product", "")));
         return json::object();
       }},
      {"set_opening_daylight",
       "docs/203: what you worked out for openings of 'host' ('openings': ids or indices, all when omitted): 'factor' "
       "the daylight factor (採光補正係数, 令 20 条, 0 to 3) with 'basis' (one line: how it was worked out), and / or "
       "'ventilation_m2' the area that opens for ventilation (法 28 条 2 項). Keys not given stay; a value below 0 "
       "takes it away. Used by "
       "room_opening_inputs and the permit workflow's stage 4. One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "factor": {"type": "number"},
               "basis": {"type": "string"}, "ventilation_m2": {"type": "number"}}, "required": ["host"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         const double keep = std::numeric_limits<double>::quiet_NaN();  // not given: stays
         must(m.setOpeningDaylight(text(a, "host"), which, numberOr(a, "factor", keep), textOr(a, "basis", ""),
                                   numberOr(a, "ventilation_m2", keep)));
         return json::object();
       }},
      {"set_energy_route",
       "docs/210: the route the energy documents follow ('route': spec 仕様基準 / performance 性能基準 / other / \"\"), "
       "the 'region' (1-8, 0: not given) and 'source'. Kept on the site outline (needs the site boundary). One undo step.",
       SCHEMA({"type": "object", "properties": {"route": {"enum": ["spec", "performance", "other", ""]}, "region": {"type": "integer"},
               "source": {"type": "string"}}}),
       [](doc::Modeler& m, const json& a) {
         must(m.setEnergyRoute(textOr(a, "route", ""), a.value("region", 0), textOr(a, "source", "")));
         return json::object();
       }},
      {"set_insulation",
       "docs/210: the insulation of walls, slabs and roofs 'ids': 'material' (\"\" takes it away), 'thickness_mm', "
       "'conductivity' (λ, W/(m·K)) and / or 'resistance' (R, m²·K/W), 'placement' (fill 充填 / outside 外張り / inside), "
       "'source' (catalogue, specification). One undo step. Not compared with any standard.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}, "material": {"type": "string"},
               "thickness_mm": {"type": "number"}, "conductivity": {"type": "number"}, "resistance": {"type": "number"},
               "placement": {"enum": ["fill", "outside", "inside", ""]}, "source": {"type": "string"}}, "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setInsulation(ids(a, "ids"), textOr(a, "material", ""), numberOr(a, "thickness_mm", 0.0), numberOr(a, "conductivity", 0.0),
                              numberOr(a, "resistance", 0.0), textOr(a, "placement", ""), textOr(a, "source", "")));
         return json::object();
       }},
      {"set_opening_performance",
       "docs/210: the performance of openings of 'host' ('openings': ids or indices, all when omitted): 'u_value' heat "
       "transmittance (W/(m²·K); below 0 takes it away), 'glass', 'source', 'solar_gain' 日射熱取得率 η (0-1, docs/227; "
       "omitted: not given). One undo step. Not compared with any standard.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "u_value": {"type": "number"},
               "glass": {"type": "string"}, "source": {"type": "string"}, "solar_gain": {"type": "number"}},
               "required": ["host", "u_value"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         must(m.setOpeningPerformance(text(a, "host"), which, number(a, "u_value"), textOr(a, "glass", ""), textOr(a, "source", ""),
                                      numberOr(a, "solar_gain", -1.0)));
         return json::object();
       }},
      {"set_opening_fire_equipment",
       "docs/335: openings of 'host' ('openings': ids or indices, all when omitted) that are 防火設備: 'spec' as written "
       "(\"アルミサッシ＋複層ガラス A12（網入厚6.8含）\"; empty takes it away), 'certification' (認定番号 or the 告示 it follows) "
       "and 'source' (required with a spec). Drawn on plans as 防 by the opening, listed in the legend, and shown in the "
       "延焼 check of openings. A record: whether it is needed, or the product certified, is not judged. One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "spec": {"type": "string"},
               "certification": {"type": "string"}, "source": {"type": "string"}},
               "required": ["host", "spec"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         must(m.setOpeningFireEquipment(text(a, "host"), which, text(a, "spec"), textOr(a, "certification", ""), textOr(a, "source", "")));
         return json::object();
       }},
      {"set_site_zone",
       "docs/205: the group of use zones the site is in, for the daylight factor (令 20 条 2 項): 'zone' residential "
       "(第一種・第二種低層住居専用, 第一種・第二種中高層住居専用, 第一種・第二種住居, 準住居, 田園住居), industrial "
       "(準工業, 工業, 工業専用) or commercial (近隣商業, 商業, 用途地域の指定のない区域); \"\" takes it away. 'source': "
       "the document it was read from (required with a zone). Needs the site boundary. One undo step.",
       SCHEMA({"type": "object", "properties": {"zone": {"enum": ["residential", "industrial", "commercial", ""]},
               "source": {"type": "string"}}, "required": ["zone"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setSiteZone(text(a, "zone"), textOr(a, "source", "")));
         return json::object();
       }},
      {"set_fire_compartment_boundary",
       "Register selected wall or slab ids as the design boundary of a fire compartment. Use a common 'mark' "
       "for its parts and record the adopted provision and project evidence in 'source'. Use 'clear': true to remove the boundary record. "
       "The required equipment category must be checked in the design documents.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
         "mark": {"type": "string"}, "kind": {"enum": ["area", "shaft", "mixed_use", "other"]},
         "requirement": {"enum": ["unknown", "fire", "specific", "no_openings"]}, "source": {"type": "string"},
         "clear": {"type": "boolean"}}, "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         doc::FireCompartmentBoundary boundary;
         if (!a.value("clear", false)) boundary = {text(a, "mark"), text(a, "kind"), text(a, "requirement"), text(a, "source")};
         must(m.setFireCompartmentBoundary(ids(a, "ids"), boundary));
         return json::object();
       }},
      {"set_fire_compartment_equipment",
       "Record the installed or specified equipment for a wall opening using the stable opening_id from "
       "fire_compartment_review. Add its recognition number or prescribed specification and source in 'reference'. "
       "An empty kind with no reference clears the record.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"}, "opening_id": {"type": "string"},
                   "kind": {"enum": ["", "fire", "specific"]}, "reference": {"type": "string"}},
               "required": ["host", "opening_id", "kind"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setFireCompartmentEquipment(text(a, "host"), text(a, "opening_id"),
                                            {text(a, "kind"), textOr(a, "reference", "")}));
         return json::object();
       }},
      {"fire_compartment_review",
       "Read registered fire-compartment walls and slabs, wall openings and closed holes in slab top faces. "
       "Statuses compare records and indicate what to review. One row per boundary or opening.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         using Part = doc::FireCompartmentRow::Part;
         auto rows = json::array();
         for (const auto& r : doc::fireCompartmentReview(m.document())) {
           const auto& b = r.boundary;
           rows.push_back({{"part", r.part == Part::Boundary ? "boundary" : r.part == Part::WallOpening ? "wall_opening" : "floor_opening"},
               {"host", r.host}, {"host_name", r.hostName}, {"level", r.level}, {"mark", b.mark}, {"kind", b.kind},
               {"requirement", b.requirement}, {"source", b.source}, {"opening_id", r.openingId}, {"opening_mark", r.openingMark},
               {"equipment", r.equipment.kind}, {"reference", r.equipment.reference}, {"status", r.status}, {"attention", r.attention}});
         }
         return json{{"rows", rows}, {"law", doc::kFireCompartmentLaw}, {"yokohama", doc::kFireCompartmentYokohama}};
       }},
      {"set_fire_spread_conditions",
       "Register survey-confirmed ordinary neighbor boundary edges ({start,end}, in world mm) for the exterior fire spread study. "
       "Use parallel_road_confirmed only when the registered width is verified and the straight road center is parallel "
       "to the site/road boundary at half that width. Source required. Empty boundaries with false clears the record.",
       SCHEMA({"type": "object", "properties": {
         "boundaries": {"type": "array", "items": {"type": "object", "properties": {
           "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
         "parallel_road_confirmed": {"type": "boolean"}, "source": {"type": "string"}}, "required": ["boundaries"]}),
       [](doc::Modeler& m, const json& a) {
         const auto& document = m.document();
         const auto& site = document.settings().siteBoundary;
         if (!site) throw ApiError("Register the site boundary first");
         const auto geometry = must(document.siteBoundaryGeometry(site->outline));
         doc::FireSpreadConditions conditions;
         conditions.parallelRoad = a.value("parallel_road_confirmed", false);
         conditions.source = textOr(a, "source", "");
         for (const auto& edge : confirmedNeighborEdges(a)) {
           const auto match = geometry.matchEdge(edge[0], edge[1]);
           if (!match) throw ApiError("Each neighbor edge must match one entire site boundary edge");
           const auto start = std::find_if(geometry.vertices.begin(), geometry.vertices.end(), [&](const gp_Pnt& p) {
             return p.Distance((*match)[0]) <= tolerance::kLinear;
           });
           if (start == geometry.vertices.end()) throw ApiError("Could not locate the neighbor edge");
           conditions.neighbors.push_back(static_cast<std::size_t>(start - geometry.vertices.begin()));
         }
         must(m.setFireSpreadConditions(conditions));
         return json::object();
       }},
      {"fire_spread_review",
       "Read the model's configured exterior fire spread study and opening review. Distances are horizontal millimetres "
       "from an opening edge to a studied line. Equipment review is a candidate under the recorded conditions.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto study = doc::fireSpreadStudy(m.document(), false);
         auto bases = json::array(), openings = json::array();
         for (const auto& b : study.bases)
           bases.push_back({{"kind", b.road ? "road_center" : "neighbor"}, {"start", toJson(b.visibleLine[0])},
                            {"end", toJson(b.visibleLine[1])}});
         for (const auto& r : study.openings)
           openings.push_back({{"host", r.row.host}, {"opening_id", r.row.openingId}, {"mark", r.row.mark},
               {"level", r.row.level}, {"product", r.row.product}, {"status", doc::fireSpreadStatus(r.status)},
               {"distance_mm", r.distance ? json(*r.distance) : json(nullptr)}, {"basis", r.basis}, {"note", r.note}});
         return json{{"configured", study.configured}, {"basis_changed", study.changed}, {"issue", study.issue},
                     {"source", study.conditions.source}, {"bases", bases}, {"openings", openings},
                     {"law", doc::kFireSpreadLaw}, {"yokohama", doc::kFireSpreadYokohama}};
       }},
      {"set_site_height_conditions",
       "Set the confirmed zoning district, height district and supporting source on the selected site boundary. "
       "Use site_height_conditions for the accepted codes. Empty choices clear their fields; both empty clear the record. "
       "This does not assess building height.",
       SCHEMA({"type": "object", "properties": {"zone": {"type": "string"}, "district": {"type": "string"},
               "source": {"type": "string"}}, "required": ["zone", "district"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setSiteHeightConditions(text(a, "zone"), text(a, "district"), textOr(a, "source", "")));
         return json::object();
       }},
      {"site_height_conditions",
       "Read the saved site height conditions and the available zoning and height district codes.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto conditions = doc::siteHeightConditions(m.document());
         return json{{"zone", conditions.zone}, {"district", conditions.district}, {"source", conditions.source},
                     {"zones", doc::heightZoneChoices()}, {"districts", doc::heightDistrictChoices()}};
       }},
      {"yokohama_height_district_at",
       "Calculate a single Yokohama maximum height district's standard limits at site XY points (mm). "
       "Uses the saved district and the supplied common ground Z. For types 1–5, supply confirmed ordinary neighbor "
       "edges as {start,end} and use the site's saved true north. Set single_north_road_confirmed only when the saved "
       "road is a single qualifying parallel strip with confirmed width and no other adjoining open space. "
       "Sites spanning districts, ground difference relief and exceptions require separate review. Read only.",
       SCHEMA({"type": "object", "properties": {
                   "boundaries": {"type": "array", "items": {"type": "object", "properties": {
                       "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
                   "points": {"type": "array", "items": {"type": "array"}},
                   "ground_z_mm": {"type": "number"}, "single_north_road_confirmed": {"type": "boolean"}},
               "required": ["boundaries", "points", "ground_z_mm"]}),
       [](doc::Modeler& m, const json& a) {
         const auto& document = m.document();
         const auto district = doc::siteHeightConditions(document).district;
         const auto positions = points(a, "points");
         const double groundZ = number(a, "ground_z_mm");
         const auto samples = must(doc::yokohamaHeightDistrictAt(
             document, confirmedNeighborEdges(a), positions, groundZ, a.value("single_north_road_confirmed", false)));
         const auto rule = doc::yokohamaHeightDistrictRule(district);
         auto rows = json::array();
         for (std::size_t i = 0; i < samples.size(); ++i) {
           const auto& sample = samples[i];
           rows.push_back({{"point", toJson(positions[i])}, {"status", sample.status},
                           {"slant_limit_z_mm", sample.slantLimitZMm ? json(*sample.slantLimitZMm) : json(nullptr)},
                           {"limit_z_mm", sample.limitZMm ? json(*sample.limitZMm) : json(nullptr)}});
         }
         return json{{"district", district}, {"height_cap_mm", rule->heightCapMm}, {"height_cap_z_mm", groundZ + rule->heightCapMm},
                     {"north_rise_mm", rule->northRiseMm ? json(*rule->northRiseMm) : json(nullptr)},
                     {"north_slope", rule->northRiseMm ? json(doc::kYokohamaHeightDistrictSlope) : json(nullptr)},
                     {"samples", std::move(rows)}, {"source", doc::kYokohamaHeightDistrictSource},
                     {"checked_on", doc::kYokohamaHeightDistrictCheckedOn}, {"legal_status", "未評価"}};
       }},
      {"neighbor_height_at",
       "Evaluate a neighbor-slant study at XY points in the saved site (Z of each point is ignored). "
       "Supply explicitly confirmed straight outline edges as {start, end} world points in mm. "
       "Returns the closest finite edge and its closest point, horizontal distance and limit Z. "
       "For a point outside the site the result has null height. A registered road edge cannot be selected. "
       "Ground Z, base height and slope are supplied study conditions. Does not decide applicability, "
       "setback or other relief. Read only.",
       SCHEMA({"type": "object", "properties": {
                   "boundaries": {"type": "array", "items": {"type": "object", "properties": {
                       "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
                   "points": {"type": "array", "items": {"type": "array"}}, "ground_z_mm": {"type": "number"},
                   "base_height_mm": {"type": "number"}, "slope": {"type": "number"}},
               "required": ["boundaries", "points", "ground_z_mm", "base_height_mm", "slope"]}),
       [](doc::Modeler& m, const json& a) {
         const auto confirmedEdges = confirmedNeighborEdges(a);
         const auto positions = points(a, "points");
         const auto samples = must(doc::neighborHeightAt(m.document(), confirmedEdges, positions,
                                                        number(a, "ground_z_mm"), number(a, "base_height_mm"), number(a, "slope")));
         auto rows = json::array();
         for (std::size_t i = 0; i < samples.size(); ++i) {
           const auto& sample = samples[i];
           json row{{"point", toJson(positions[i])}, {"in_site", sample.has_value()}, {"limit_z_mm", nullptr}};
           if (sample) {
             row["boundary"] = {{"start", toJson(sample->boundary[0])}, {"end", toJson(sample->boundary[1])}};
             row["closest_boundary_point"] = toJson(sample->closestBoundaryPoint);
             row["distance_mm"] = sample->distanceMm;
             row["limit_z_mm"] = sample->limitZMm;
           }
           rows.push_back(std::move(row));
         }
         return json{{"samples", std::move(rows)}, {"legal_status", "未評価"}};
       }},
      {"north_height_at",
       "Study Building Standards Act Art. 56(1)(iii) at XY points using the site's saved true north. "
       "Supply confirmed neighbor edges as {start,end} world points. The registered road width is used for "
       "an exit over the site's road edge. Heights are in mm, rise must be 5000 or 10000, slope is 1.25. "
       "Unconfirmed exits and boundary coincidences report a review status. Read only; no applicability or local district assessment.",
       SCHEMA({"type": "object", "properties": {
                   "boundaries": {"type": "array", "items": {"type": "object", "properties": {
                      "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
                   "points": {"type": "array", "items": {"type": "array"}}, "ground_z_mm": {"type": "number"},
                   "base_height_mm": {"type": "number", "enum": [5000,10000]}},
               "required": ["boundaries", "points", "ground_z_mm", "base_height_mm"]}),
       [](doc::Modeler& m, const json& a) {
         const auto positions = points(a, "points");
         const auto samples = must(doc::northHeightAt(m.document(), confirmedNeighborEdges(a), positions,
                                                      number(a, "ground_z_mm"), number(a, "base_height_mm")));
         auto rows = json::array();
         for (std::size_t i = 0; i < samples.size(); ++i) {
           const auto& sample = samples[i];
           json row{{"point", toJson(positions[i])}, {"status", sample.status},
                    {"limit_z_mm", sample.limitZMm ? json(*sample.limitZMm) : json(nullptr)}};
           if (sample.exitPoint) row["exit_point"] = toJson(*sample.exitPoint);
           if (sample.exitBoundary) {
             row["exit_boundary"] = {{"start", toJson((*sample.exitBoundary)[0])}, {"end", toJson((*sample.exitBoundary)[1])}};
             row["road"] = sample.road;
           }
           if (sample.referencePoint) row["reference_point"] = toJson(*sample.referencePoint);
           if (sample.distanceMm) row["distance_mm"] = *sample.distanceMm;
           rows.push_back(std::move(row));
         }
         return json{{"samples", std::move(rows)}, {"legal_status", "未評価"}};
       }},
      {"compare_yokohama_height_district",
       "Compare selected B-reps with the standard height limits of the site's single saved Yokohama district. "
       "Use the same ground and confirmed boundaries as yokohama_height_district_at. "
       "Returns the maximum vertical excess and which surface controls over confirmed regions. Read only.",
       SCHEMA({"type": "object", "properties": {
                   "boundaries": {"type": "array", "items": {"type": "object", "properties": {
                      "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
                   "ids": {"type": "array", "items": {"type": "string"}}, "ground_z_mm": {"type": "number"},
                   "single_north_road_confirmed": {"type": "boolean"}},
               "required": ["boundaries", "ids", "ground_z_mm"]}),
       [](doc::Modeler& m, const json& a) {
         const auto parts = must(doc::compareYokohamaHeightDistrict(m.document(), confirmedNeighborEdges(a),
             a.at("ids").get<std::vector<doc::ObjectId>>(), number(a, "ground_z_mm"),
             a.value("single_north_road_confirmed", false)));
         auto rows = json::array();
         for (const auto& part : parts)
           rows.push_back({{"id", part.id}, {"name", part.name}, {"state", part.state}, {"needs_review", part.needsReview},
                           {"excess_mm", part.excessMm ? json(*part.excessMm) : json(nullptr)},
                           {"controlling_surface", part.excessMm ? json(part.heightCap ? "height_cap" : "north_slant") : json(nullptr)}});
         return json{{"rows", std::move(rows)}, {"legal_status", "未評価"}};
       }},
      {"compare_north_height",
       "Compare selected world-space B-reps with the north height study. Uses saved true north and road, "
       "with confirmed neighbor edges as {start,end}. Measures the maximum vertical excess over confirmed regions. "
       "Unconfirmed regions touching the part require review. This geometric study does not establish legal compliance.",
       SCHEMA({"type": "object", "properties": {
                   "boundaries": {"type": "array", "items": {"type": "object", "properties": {
                      "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
                   "ids": {"type": "array", "items": {"type": "string"}}, "ground_z_mm": {"type": "number"},
                   "base_height_mm": {"type": "number", "enum": [5000,10000]}},
               "required": ["boundaries", "ids", "ground_z_mm", "base_height_mm"]}),
       [](doc::Modeler& m, const json& a) {
         const auto parts = must(doc::compareNorthHeight(m.document(), confirmedNeighborEdges(a),
             a.at("ids").get<std::vector<doc::ObjectId>>(), number(a, "ground_z_mm"), number(a, "base_height_mm")));
         auto rows = json::array();
         for (const auto& part : parts)
           rows.push_back({{"id", part.id}, {"name", part.name}, {"state", part.state}, {"needs_review", part.needsReview},
                           {"excess_mm", part.excessMm ? json(*part.excessMm) : json(nullptr)}});
         return json{{"rows", std::move(rows)}, {"legal_status", "未評価"}};
       }},
      {"compare_neighbor_height",
       "Compare selected world-space B-reps with a neighbor-height study using confirmed site edges. "
       "The maximum signed excess is between lower_excess_mm (null if no lower bound was found) and upper_excess_mm. "
       "exact means the bounds agree to 0.01 mm. Results are geometric and do not establish legal compliance. Read only.",
       SCHEMA({"type": "object", "properties": {
                   "boundaries": {"type": "array", "items": {"type": "object", "properties": {
                      "start": {"type": "array"}, "end": {"type": "array"}}, "required": ["start", "end"]}},
                   "ids": {"type": "array", "items": {"type": "string"}}, "ground_z_mm": {"type": "number"},
                   "base_height_mm": {"type": "number"}, "slope": {"type": "number"}},
               "required": ["boundaries", "ids", "ground_z_mm", "base_height_mm", "slope"]}),
       [](doc::Modeler& m, const json& a) {
         const auto parts = must(doc::compareNeighborHeight(m.document(), confirmedNeighborEdges(a),
             a.at("ids").get<std::vector<doc::ObjectId>>(), number(a, "ground_z_mm"), number(a, "base_height_mm"),
             number(a, "slope")));
         auto rows = json::array();
         for (const auto& part : parts)
           rows.push_back({{"id", part.id}, {"name", part.name}, {"state", part.state}, {"exact", part.exact},
                           {"lower_excess_mm", part.lowerExcessMm ? json(*part.lowerExcessMm) : json(nullptr)},
                           {"upper_excess_mm", part.upperExcessMm}});
         return json{{"rows", std::move(rows)}, {"legal_status", "未評価"}};
       }},
      {"road_height_datum",
       "Calculate the site-side edge_height_mm for one level road and a confirmed site ground plane, before building "
       "setback. Give road-centre and site-ground heights on the same model Z datum. Applies the ordinary rule in "
       "Building Standards Act Enforcement Order Art. 2(1)(vi)(a) and Art. 135-2(1). Municipal exceptions and "
       "the choice of inputs require separate confirmation. Read only.",
       SCHEMA({"type": "object",
               "properties": {"road_center_z_mm": {"type": "number"}, "site_ground_z_mm": {"type": "number"},
                              "road_width_mm": {"type": "number"}, "slope": {"type": "number"}},
               "required": ["road_center_z_mm", "site_ground_z_mm", "road_width_mm", "slope"]}),
       [](doc::Modeler&, const json& a) {
         const auto datum = must(doc::roadHeightDatum(number(a, "road_center_z_mm"), number(a, "site_ground_z_mm"),
                                                      number(a, "road_width_mm"), number(a, "slope")));
         return json{{"ground_difference_mm", datum.groundDifferenceMm}, {"road_rise_mm", datum.roadRiseMm},
                     {"adopted_road_z_mm", datum.adoptedRoadZMm}, {"edge_height_mm", datum.edgeHeightMm},
                     {"source", doc::kRoadHeightDatumSource}, {"checked_on", doc::kRoadHeightDatumCheckedOn}};
       }},
      {"road_height_table_values",
       "Read the standard numbers in Building Standards Act Appendix 3 items 1–3 for the site's saved zoning. "
       "Enter the confirmed Article 52 floor area ratio limit as a percentage, after applicable width restrictions. "
       "For a site wholly in one zone. Special designations, high-rise residential promotion districts and "
       "multi-zone sites need a separate assessment. If wide_road_rule is true, Article 56(3)–(4) can apply "
       "when the road is at least 12 m wide. Does not determine legal compliance.",
       SCHEMA({"type": "object", "properties": {"floor_area_limit_percent": {"type": "number", "exclusiveMinimum": 0}},
               "required": ["floor_area_limit_percent"]}),
       [](doc::Modeler& m, const json& a) {
         const auto zone = doc::siteHeightConditions(m.document()).zone;
         const auto row = must(doc::roadHeightTableValues(zone, number(a, "floor_area_limit_percent")));
         return json{{"zone", zone}, {"table_item", row.item}, {"slope", row.slope},
                     {"application_distance_mm", row.applicationDistanceMm}, {"wide_road_rule", row.hasWideRoadRule},
                     {"source", doc::kRoadHeightTableSource}, {"checked_on", doc::kRoadHeightTableCheckedOn}};
       }},
      {"measure_road_building_setback",
       "Measure the minimum perpendicular horizontal distance inward from the saved straight site-side road boundary "
       "to the selected world-space B-rep parts. At or beyond the line the measurement is zero. Returns the "
       "closest part id and name with distance_mm. Choose the parts to include; legal exceptions are not assessed.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         const auto measured = must(doc::measureRoadBuildingSetback(m.document(),
                                                                   a.at("ids").get<std::vector<doc::ObjectId>>()));
         return json{{"id", measured.id}, {"name", measured.name}, {"distance_mm", measured.distanceMm}};
       }},
      {"compare_road_height_plane",
       "Compare selected B-rep shapes against a single user-supplied inclined plane. Edge height is model Z "
       "at the saved site-side road boundary before setback, in mm. Slope is rise/run inward. Returns signed vertical excess "
       "by object (positive is above). To limit the comparison, give both road_width_mm (adopted width) "
       "and application_distance_mm. With these two values, optional building_setback_mm shifts the opposite "
       "boundary outward for both height and application distance. Outside rows have excess_mm=null. "
       "This is a geometric comparison, not a legal road-height assessment.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "edge_height_mm": {"type": "number"}, "slope": {"type": "number"},
               "road_width_mm": {"type": "number"}, "application_distance_mm": {"type": "number"},
               "building_setback_mm": {"type": "number", "minimum": 0}},
               "required": ["ids", "edge_height_mm", "slope"]}),
       [](doc::Modeler& m, const json& a) {
         std::optional<doc::RoadHeightPlaneRange> range;
         if (a.contains("road_width_mm") || a.contains("application_distance_mm") || a.contains("building_setback_mm"))
           range = doc::RoadHeightPlaneRange{number(a, "road_width_mm"), number(a, "application_distance_mm"),
                                            a.contains("building_setback_mm") ? number(a, "building_setback_mm") : 0};
         const auto comparison = must(doc::compareRoadHeightPlane(m.document(), a.at("ids").get<std::vector<doc::ObjectId>>(),
                                                                  number(a, "edge_height_mm"), number(a, "slope"), range));
         json result = json::array();
         for (const auto& r : comparison.rows)
           result.push_back({{"id", r.id}, {"name", r.name}, {"excess_mm", r.excessMm ? json(*r.excessMm) : json(nullptr)}});
         return json{{"rows", result}};
       }},
      {"daylight_factor_estimates",
       "docs/205: each wall opening's daylight factor (採光補正係数, 令 20 条 2 項) worked out from the model: "
       "horizontal and vertical distances (mm) of the part of the building over it that sets the smallest ratio, "
       "whether it faces the road, the factor and how it was worked out ('basis'), or 'problem' when it cannot be "
       "(no site boundary, no zone, an inner wall...). Read only; apply_daylight_factors gives them to the openings.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json rows = json::array();
         for (const auto& e : doc::daylightFactorEstimates(m.document())) {
           json row{{"host", e.host}, {"opening", e.openingId}, {"index", e.index}, {"mark", e.mark}};
           if (e.factor >= 0) {
             row["factor"] = e.factor;
             row["ratio"] = e.ratio;
             row["horizontal_mm"] = e.horizontal;
             row["vertical_mm"] = e.vertical;
             row["faces_road"] = e.facesRoad;
             row["over"] = e.over;
             row["basis"] = e.basis;
           } else {
             row["problem"] = e.problem;
           }
           rows.push_back(std::move(row));
         }
         return json{{"openings", std::move(rows)}};
       }},
      {"apply_daylight_factors",
       "docs/205: give the openings the daylight factors daylight_factor_estimates works out, with their basis "
       "('openings': opening ids, all that can be worked out when omitted). Their areas for ventilation stay. One "
       "undo step. Returns how many were given and the ids that could not be.",
       SCHEMA({"type": "object", "properties": {"openings": {"type": "array", "items": {"type": "string"}}}}),
       [](doc::Modeler& m, const json& a) {
         std::vector<std::string> wanted;
         if (a.contains("openings"))
           for (const json& id : a.at("openings")) wanted.push_back(id.get<std::string>());
         json skipped = json::array();
         for (const auto& e : doc::daylightFactorEstimates(m.document()))
           if (e.factor < 0 && (wanted.empty() || std::find(wanted.begin(), wanted.end(), e.openingId) != wanted.end()))
             skipped.push_back({{"opening", e.openingId}, {"problem", e.problem}});
         const int given = must(doc::applyDaylightFactors(m, wanted));
         return json{{"given", given}, {"skipped", std::move(skipped)}};
       }},
      {"set_room_ventilation",
       "docs/211: for a room ('id'): 'joined_with' the room it is one with through fusuma / shoji and the like (法 28 条 "
       "4 項; kept on both; empty string takes it away) and / or 'fan_m3h' the effective airflow of the mechanical "
       "ventilation serving it (令 20 条の 8; 0 takes it away) and / or 'fan_unit' the name of that unit (docs/213: "
       "rooms of one name share one unit, compared as the sum of their required airflows, 令 20 条の 8 第 1 項第 1 号イ"
       "（2）; an airflow given then goes to every room of the name; empty string takes it away). Keys not given stay. "
       "One undo step.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "joined_with": {"type": "string"},
               "fan_m3h": {"type": "number"}, "fan_unit": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         std::string joined;
         if (a.contains("joined_with")) {
           joined = text(a, "joined_with");
         } else if (const doc::Object* marker = m.document().find(text(a, "id"))) {
           const auto it = marker->attributes.find("room_daylight");
           const json stored = it == marker->attributes.end() ? json() : json::parse(it->second, nullptr, false);
           if (stored.is_object()) joined = stored.value("joined_with", std::string());
         }
         must(m.setRoomVentilation(text(a, "id"), joined, numberOr(a, "fan_m3h", std::numeric_limits<double>::quiet_NaN()),
                                   a.contains("fan_unit") ? std::optional<std::string>(text(a, "fan_unit")) : std::nullopt));
         return json::object();
       }},
      {"set_opening_veranda",
       "docs/211: openings of 'host' ('openings': ids or indices, all when omitted) with ('veranda': true) or without a "
       "veranda 90 cm wide or more outside them: the daylight factor worked out from the model is then x 0.7 "
       "(令 20 条 2 項). One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": ["string", "integer"]}}, "veranda": {"type": "boolean"}},
               "required": ["host", "veranda"]}),
       [](doc::Modeler& m, const json& a) {
         const std::vector<std::size_t> which = openingNumbers(m, a);
         must(m.setOpeningVeranda(text(a, "host"), which, a.at("veranda").get<bool>()));
         return json::object();
       }},
      {"set_room_daylight",
       "docs/203: a room's ('id', from create_room) 'kind' for daylight: dwelling (住宅の居住のための居室), other "
       "(another kind of 居室), none (not a 居室) or \"\" (not given); and 'ventilation': window or equipment "
       "(換気設備, 法 28 条 2 項ただし書き) or \"\". One undo step.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"},
               "kind": {"enum": ["dwelling", "other", "none", ""]}, "ventilation": {"enum": ["window", "equipment", ""]}},
               "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setRoomDaylight(text(a, "id"), textOr(a, "kind", ""), textOr(a, "ventilation", "")));
         return json::object();
       }},
      {"set_type_name",
       "Name the type of slabs and roofs (docs/134): 'ids', 'name' (e.g. \"F1\", \"R1\"; empty: no type). Those of one "
       "name are one type: change_type changes them all. Other objects are refused. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}, "name": {"type": "string"}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setTypeName(ids(a, "ids"), textOr(a, "name", "")));
         return json::object();
       }},
      {"change_type",
       "Change a type of slabs / roofs (set_type_name, docs/134): every one named 'name' takes 'thickness' (slabs: "
       "the top kept; roofs: across the slope), and roofs 'pitch' (寸), 'eave', 'gable_eave', whichever are given. "
       "Or 'like': an id: the others of its name take its values. All or nothing, one undo step. Returns how "
       "many changed.",
       SCHEMA({"type": "object", "properties": {"name": {"type": "string"}, "thickness": {"type": "number"},
               "pitch": {"type": "number"}, "eave": {"type": "number"}, "gable_eave": {"type": "number"},
               "like": {"type": "string"}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("like")) return json{{"changed", must(m.matchElementType(text(a, "like")))}};
         doc::Modeler::ElementTypeChange change;
         if (a.contains("thickness")) change.thickness = number(a, "thickness");
         if (a.contains("pitch")) change.pitch = number(a, "pitch");
         if (a.contains("eave")) change.eave = number(a, "eave");
         if (a.contains("gable_eave")) change.gableEave = number(a, "gable_eave");
         return json{{"changed", must(m.changeElementType(text(a, "name"), change))}};
       }},
      {"define_type",
       "Register a floor / roof type (docs/149), kept with the model: 'name', 'part' (floor | roof), 'thickness' "
       "(floor: the only value), roofs 'pitch' (寸), 'eave', 'gable_eave', whichever are given. Floors may have "
       "'layers' (docs/151): [{name, thickness}] top to bottom; the thickness is their sum, and sections draw a "
       "line where two meet ([] takes them off). Or 'like': an id: its type name and values. Every slab / roof "
       "of that name takes the values; set_type_name with a registered name gives them too. All or nothing, one "
       "undo step. Returns how many changed.",
       SCHEMA({"type": "object", "properties": {"name": {"type": "string"}, "part": {"type": "string", "enum": ["floor", "roof"]},
               "thickness": {"type": "number"}, "pitch": {"type": "number"}, "eave": {"type": "number"},
               "gable_eave": {"type": "number"}, "like": {"type": "string"},
               "layers": {"type": "array", "items": {"type": "object", "properties": {"name": {"type": "string"},
                          "thickness": {"type": "number"}}, "required": ["name", "thickness"]}}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("like")) return json{{"changed", must(m.registerElementType(text(a, "like")))}};
         doc::Modeler::ElementTypeChange change;
         if (a.contains("thickness")) change.thickness = number(a, "thickness");
         if (a.contains("pitch")) change.pitch = number(a, "pitch");
         if (a.contains("eave")) change.eave = number(a, "eave");
         if (a.contains("gable_eave")) change.gableEave = number(a, "gable_eave");
         std::optional<std::vector<doc::Modeler::TypeLayer>> layers;
         if (a.contains("layers")) {
           if (!a.at("layers").is_array()) throw std::runtime_error("'layers' must be an array");
           layers.emplace();
           for (const json& l : a.at("layers"))
             layers->push_back({l.value("name", std::string()), l.value("thickness", 0.0)});
         }
         return json{{"changed", must(m.defineElementType(text(a, "name"), text(a, "part"), change, layers))}};
       }},
      {"list_types",
       "The registered floor / roof types (define_type, docs/149): name, part, values given, and how many slabs / "
       "roofs have the name.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json out = json::array();
         for (const auto& t : m.elementTypes()) {
           int count = 0;
           for (const auto& o : m.document().objects()) {
             const auto it = o.attributes.find("type_name");
             count += it != o.attributes.end() && it->second == t.name;
           }
           json row{{"name", t.name}, {"part", t.part}, {"count", count}};
           if (t.values.thickness) row["thickness"] = *t.values.thickness;
           if (t.values.pitch) row["pitch"] = *t.values.pitch;
           if (t.values.eave) row["eave"] = *t.values.eave;
           if (t.values.gableEave) row["gable_eave"] = *t.values.gableEave;
           if (!t.layers.empty()) {
             row["layers"] = json::array();
             for (const auto& l : t.layers) row["layers"].push_back({{"name", l.name}, {"thickness", l.thickness}});
           }
           out.push_back(row);
         }
         return json{{"types", out}};
       }},
      {"remove_type",
       "Take a type off the register (docs/149): 'name'. The slabs / roofs keep their name and sizes. One undo step.",
       SCHEMA({"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.removeElementType(text(a, "name")));
         return json::object();
       }},
      {"change_canopy",
       "Change a canopy's sizes (docs/154): 'id', and any of 'depth', 'thickness', 'extra', 'above' (as make_canopy; "
       "the others kept). Made again over its opening, its name kept. One undo step. Returns its (new) id.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "depth": {"type": "number"},
               "thickness": {"type": "number"}, "extra": {"type": "number"}, "above": {"type": "number"}},
               "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         doc::Modeler::CanopySpec spec = must(m.canopySpec(text(a, "id")));
         if (a.contains("depth")) spec.depth = number(a, "depth");
         if (a.contains("thickness")) spec.thickness = number(a, "thickness");
         if (a.contains("extra")) spec.extra = number(a, "extra");
         if (a.contains("above")) spec.above = number(a, "above");
         return json{{"id", must(m.changeCanopy(text(a, "id"), spec))}};
       }},
      {"make_canopy",
       "A canopy (庇, docs/147) over an opening: a board out from the face the opening was cut from. 'host', "
       "'opening' (id or number), 'depth' (out from the face, default 600), 'thickness' (60), 'extra' (longer at "
       "each side than the opening, 150), 'above' (its underside over the opening's head, 100). Attribute "
       "kind=canopy. Canopies standing out 1 m or more from the wall's line count in the building area "
       "(area_table). One undo step. Returns its id.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"}, "opening": {"type": ["string", "integer"]},
               "depth": {"type": "number"}, "thickness": {"type": "number"}, "extra": {"type": "number"},
               "above": {"type": "number"}}, "required": ["host", "opening"]}),
       [](doc::Modeler& m, const json& a) {
         const auto which = openingNumbers(m, json{{"host", text(a, "host")}, {"openings", json::array({a.at("opening")})}});
         if (which.size() != 1) throw ApiError("'opening': one opening");
         doc::Modeler::CanopySpec spec;
         spec.depth = numberOr(a, "depth", spec.depth);
         spec.thickness = numberOr(a, "thickness", spec.thickness);
         spec.extra = numberOr(a, "extra", spec.extra);
         spec.above = numberOr(a, "above", spec.above);
         return json{{"id", must(m.makeCanopy(text(a, "host"), which.front(), spec))}};
       }},
      {"create_balcony",
       "A balcony (バルコニー, docs/163) on a wall made by create_wall: a floor out from one face with a rail on "
       "its three other sides, one solid (attribute kind=balcony). 'host': the wall. 'at': [x, y] near where its "
       "middle goes (put onto the wall's line; the side of the wall the point is on is the side the balcony "
       "stands out on), or 'along' (mm from the wall's start) and 'side' (1 left of the line, -1 right). 'width' "
       "(2730), 'depth' (out from the face, 910), 'top' (z of its floor, default the wall's foot), 'thickness' "
       "(150), 'rail': wall (a parapet, default), bars or none, 'rail_height' (1100). 'floor_area': count, "
       "exclude or \"\" (default: by the notice 住指発第 115 号 1 (4)(5), docs/175 - open 1.1 m or more and half "
       "its ceiling height or more: only beyond 2 m from the wall's centre line counts; else all). 1 m or more out from the wall's line it counts in the building area (back 1 m "
       "from its end; on posts, up to their centre line). 'posts': 0 (default) or 2 to 6 under its front edge, "
       "'post_size' (105 square), from the next level below its floor (docs/167). 'wrap': \"start\" or \"end\" takes "
       "it round the outer corner where its wall meets another at that end (an L: along its wall from the far side "
       "of 'width' up to the corner, then 'wrap_length' (1820) along the other wall; posts at its far corners; "
       "docs/168), \"\" straight. It goes with its wall(s). In IFC: "
       "a slab BALCONY, a railing, its posts columns. One undo step.",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"}, "at": {"type": "array"}, "along": {"type": "number"},
               "side": {"type": "integer"}, "width": {"type": "number"}, "depth": {"type": "number"}, "top": {"type": "number"},
               "thickness": {"type": "number"}, "rail": {"type": "string"}, "rail_height": {"type": "number"},
               "floor_area": {"type": "string"}, "posts": {"type": "integer"}, "post_size": {"type": "number"}, "wrap": {"type": "string"}, "wrap_length": {"type": "number"}}, "required": ["host"]}),
       [](doc::Modeler& m, const json& a) {
         doc::Modeler::BalconySpec spec = must(m.defaultBalcony(text(a, "host")));
         balconyArgs(m, a, spec);
         const std::string id = must(m.createBalcony(spec));
         return balconyJson(id, must(m.balconySpec(id)));
       }},
      {"change_balcony",
       "A balcony made by create_balcony with other sizes or on another place of its wall: any of the words of "
       "create_balcony but 'host'. It is made again (its name kept): the new id is returned. One undo step.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "at": {"type": "array"}, "along": {"type": "number"},
               "side": {"type": "integer"}, "width": {"type": "number"}, "depth": {"type": "number"}, "top": {"type": "number"},
               "thickness": {"type": "number"}, "rail": {"type": "string"}, "rail_height": {"type": "number"},
               "floor_area": {"type": "string"}, "posts": {"type": "integer"}, "post_size": {"type": "number"}, "wrap": {"type": "string"}, "wrap_length": {"type": "number"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         doc::Modeler::BalconySpec spec = must(m.balconySpec(text(a, "id")));
         balconyArgs(m, a, spec);
         const std::string id = must(m.changeBalcony(text(a, "id"), spec));
         return balconyJson(id, must(m.balconySpec(id)));
       }},
      {"balcony_info",
       "A balcony made by create_balcony as it stands: host, along, side, width, depth, top, thickness, rail, "
       "rail_height, floor_area.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) { return balconyJson(text(a, "id"), must(m.balconySpec(text(a, "id")))); }},
      {"define_fitting_type",
       "A fitting type (docs/171): a door's or window's kind and sizes ('fitting' as set_opening_fittings: type, "
       "frame, leaf, glass; hands are left out) under 'name'. set_opening_fittings with fitting {\"type_name\": name} "
       "(and its own hinge / swing / slide) gives an opening that kind and those sizes. Defined again, every fitting "
       "of that name in every wall changes, the hands kept. Optional opening_size {width, height, sill} in mm "
       "sets the placement defaults without resizing any existing opening. One undo step.",
       SCHEMA({"type": "object", "properties": {"name": {"type": "string"}, "fitting": {"type": "object"},
               "opening_size": {"type": "object", "properties": {"width": {"type": "number"}, "height": {"type": "number"}, "sill": {"type": "number"}},
                                "required": ["width", "height", "sill"]}}, "required": ["name", "fitting"]}),
       [](doc::Modeler& m, const json& a) {
         if (!a.contains("fitting") || !a.at("fitting").is_object()) throw ApiError("'fitting' is an object");
         std::optional<doc::Modeler::FittingOpeningSize> openingSize;
         if (a.contains("opening_size")) {
           const auto& size = a.at("opening_size");
           openingSize = doc::Modeler::FittingOpeningSize{number(size, "width"), number(size, "height"), number(size, "sill")};
         }
         const auto name = text(a, "name");
         return json{{"name", name}, {"changed", must(m.defineFittingType(name, a.at("fitting").dump(), {}, 0, openingSize))}};
       }},
      {"list_fitting_types",
       "The fitting types registered (define_fitting_type): name, fitting and opening_size if saved.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json out = json::array();
         for (const auto& [name, fitting] : m.fittingTypes()) {
           json row = {{"name", name}, {"fitting", json::parse(fitting)}};
           if (const auto size = m.fittingOpeningSize(name))
             row["opening_size"] = {{"width", size->width}, {"height", size->height}, {"sill", size->sill}};
           out.push_back(std::move(row));
         }
         return json{{"types", out}};
       }},
      {"layer_quantities",
       "The quantities of the layers of walls (of a build-up), floors and roofs (of a registered type with layers), "
       "docs/172: per part and layer name and thickness, how many elements, area (m2: each element's volume over "
       "its thickness, openings out, roofs along the slope) and volume (m3). An estimate: corners' differences "
       "between the outer and inner layers are not looked at. include_csv adds UTF-8 CSV (BOM).",
       SCHEMA({"type": "object", "properties": {"include_csv": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         const doc::LayerQuantities q = doc::layerQuantities(m.document());
         json rows = json::array();
         for (const auto& r : q.rows)
           rows.push_back({{"part", r.part}, {"layer", r.name}, {"thickness", r.thickness}, {"elements", r.elements},
                           {"area", r.area / 1e6}, {"volume", r.volume / 1e9}});
         json result{{"rows", rows}, {"without_layers", q.withoutLayers}, {"notes", q.notes}};
         if (a.value("include_csv", false)) result["csv"] = doc::tableCsv(doc::scheduleTable(m.document(), 5));
         return result;
       }},
      {"remove_fitting_type",
       "Removes a fitting type: the fittings of that name stay as they are, without the name. One undo step.",
       SCHEMA({"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}),
       [](doc::Modeler& m, const json& a) { return json{{"name", text(a, "name")}, {"unnamed", must(m.removeFittingType(text(a, "name")))}}; }},
      {"change_opening_type",
       "Change a kind of opening (docs/133): every opening with 'mark' in opening_schedule (given by hand or worked "
       "out), in every wall, takes 'width' / 'height' (centre and sill kept), 'fitting' (an object as "
       "set_opening_fittings; {\"type\":\"none\"} for none) and 'product', whichever are given. Or 'like': "
       "{host, opening}: the others of that opening's mark take its size, fitting and product. All or nothing "
       "(refused where one would not fit), one undo step. Returns how many changed.",
       SCHEMA({"type": "object", "properties": {"mark": {"type": "string"}, "width": {"type": "number"},
               "height": {"type": "number"}, "fitting": {"type": "object"}, "product": {"type": "string"},
               "like": {"type": "object"}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("like")) {
           const json& like = a.at("like");
           if (!like.is_object()) throw ApiError("'like': {host, opening}");
           const std::string host = text(like, "host");
           const auto which = openingNumbers(m, json{{"host", host}, {"openings", json::array({like.value("opening", json())})}});
           if (which.size() != 1) throw ApiError("'like': one opening");
           return json{{"changed", must(m.matchOpeningType(host, which.front()))}};
         }
         doc::Modeler::OpeningTypeChange change;
         if (a.contains("width")) change.width = number(a, "width");
         if (a.contains("height")) change.height = number(a, "height");
         if (a.contains("fitting")) change.fitting = a.at("fitting").dump();
         if (a.contains("product")) change.product = text(a, "product");
         return json{{"changed", must(m.changeOpeningType(text(a, "mark"), change))}};
       }},
      {"opening_schedule",
       "The door and window schedule read from the walls as they are now. 'types': one line per mark (AW1.. "
       "windows and AD1.. doors in outer walls (walls without a build-up count as outer), WW1.. / WD1.. in inner "
       "ones (interior build-ups), O1.. plain openings, docs/176; the same mark for the same kind, side, fitting, "
       "width and height) "
       "with count and the sill heights among them; 'openings': every opening with its mark, host wall, id, "
       "index, level, width, height, sill (mm). A fitting with its sill on the floor is a door, any other "
       "fitting a window. Marks are worked out again each time (they are the ones drawn on plans) unless given "
       "by hand (set_opening_mark: own_mark true; they stay, and worked-out numbers skip them). 'mixed' true: "
       "openings given one mark by hand differ in kind, fitting, size or product (the line shows the first). "
       "include_csv: true adds UTF-8 CSV (BOM) with Japanese headings.",
       SCHEMA({"type": "object", "properties": {"include_csv": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         const doc::OpeningSchedule s = doc::openingSchedule(m.document());
         json types = json::array(), openings = json::array();
         int total = 0;
         for (const auto& t : s.types) {
           types.push_back({{"mark", t.mark}, {"kind", t.kind}, {"fitting", t.fitting}, {"width", t.width},
                            {"height", t.height}, {"count", t.count}, {"sills", t.sills}, {"own_mark", t.ownMark},
                            {"product", t.product}, {"mixed", t.mixed}});
           total += t.count;
         }
         for (const auto& r : s.rows)
           openings.push_back({{"mark", r.mark}, {"kind", r.kind}, {"fitting", r.fitting}, {"width", r.width},
                               {"height", r.height}, {"sill", r.sill}, {"host", r.host}, {"host_name", r.hostName},
                               {"id", r.openingId}, {"index", r.index}, {"level", r.level}, {"own_mark", r.ownMark},
                               {"product", r.product},
                               {"center", {r.center.X(), r.center.Y(), r.center.Z()}}});
         json result{{"types", types}, {"openings", openings}, {"count", total}};
         if (a.value("include_csv", false)) result["csv"] = doc::openingScheduleCsv(s);
         return result;
       }},
      {"set_room_finish",
       "A room's (create_room) use and finishes (docs/115): any of 'use' (用途, e.g. 居間), 'floor', 'wall', "
       "'ceiling' (words as on a finish schedule, e.g. フローリング t15) and 'ceiling_height' (mm from the floor, "
       "1000 to 10000; 0 takes it away). Keys not given stay; an empty word takes it away. Shown in room_schedule "
       "and the finish schedule (export_schedule table finishes). 'ratio_part' (docs/162): the room is a part left "
       "out of the floor area the floor area ratio (容積率) is worked out from: garage (自動車車庫等, up to 1/5 of "
       "the total floor area), stockpile (備蓄倉庫, 1/50), battery (蓄電池, 1/50), generator (自家発電設備, 1/100), "
       "water_tank (貯水槽, 1/100), delivery_box (宅配ボックス, 1/100), elevator (エレベーターの昇降路, all), "
       "common_hall (共同住宅等の共用の廊下・階段, all), basement_dwelling (地階の住宅等, 1/3, only in a basement whose "
       "ceiling is 1 m or less above the level named GL; docs/166); empty: counted. Whether the room really is such a part is "
       "not checked. 'floor_offset' (docs/321): the room's floor from its storey's FL in mm (e.g. -180 for an entrance, 0 "
       "for level; null takes it away); the plans then write (FL±0) / (FL-180) under the room's name. 'use_code' and "
       "'use_name' (docs/324): for a building of more than one use, the room's 用途の区分 (the form's five digits, e.g. "
       "08010) and 具体的な用途の名称, both or neither (empty strings take them away); 第五面 7.用途別床面積 sums the rooms "
       "per use and gives the rest of the storey to the outline's main use. One undo step.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "use": {"type": "string"}, "floor": {"type": "string"},
               "wall": {"type": "string"}, "ceiling": {"type": "string"}, "ceiling_height": {"type": "number"},
               "ratio_part": {"type": "string"}, "floor_offset": {"type": ["number", "null"]},
               "use_code": {"type": "string"}, "use_name": {"type": "string"}},
               "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         const doc::Object* marker = m.document().find(text(a, "id"));
         if (!marker) throw ApiError("Object not found");
         doc::RoomFinish finish = doc::roomFinishOf(*marker);
         if (a.contains("use")) finish.use = text(a, "use");
         if (a.contains("floor")) finish.floor = text(a, "floor");
         if (a.contains("wall")) finish.wall = text(a, "wall");
         if (a.contains("ceiling")) finish.ceiling = text(a, "ceiling");
         if (a.contains("ceiling_height")) finish.ceilingHeight = number(a, "ceiling_height");
         if (a.contains("ratio_part")) finish.ratioPart = text(a, "ratio_part");
         if (a.contains("use_code")) finish.useCode = text(a, "use_code");  // docs/324
         if (a.contains("use_name")) finish.useName = text(a, "use_name");
         if (a.contains("floor_offset")) {  // docs/321
           if (a.at("floor_offset").is_null()) finish.floorOffset.reset();
           else finish.floorOffset = number(a, "floor_offset");
         }
         must(m.setRoomFinish(text(a, "id"), finish));
         return json{{"id", text(a, "id")}, {"use", finish.use}, {"floor", finish.floor}, {"wall", finish.wall},
                     {"ceiling", finish.ceiling}, {"ceiling_height", finish.ceilingHeight}, {"ratio_part", finish.ratioPart},
                     {"floor_offset", finish.floorOffset ? json(*finish.floorOffset) : json()},
                     {"use_code", finish.useCode}, {"use_name", finish.useName}};
       }},
      {"make_ceilings",
       "Ceilings as boards (docs/123): for each room ('rooms': ids; default all) with a ceiling height "
       "(set_room_finish) and an outline within its wall faces, a board 10 thick whose underside is at that "
       "height above the room's floor (attribute kind=ceiling, room=id). A room's ceiling made before is made "
       "again (call again after moving walls or changing the height). Rooms left are named. One undo step.",
       SCHEMA({"type": "object", "properties": {"rooms": {"type": "array", "items": {"type": "string"}}}}),
       [](doc::Modeler& m, const json& a) {
         const auto made = must(m.makeCeilings(a.contains("rooms") ? ids(a, "rooms") : std::vector<doc::ObjectId>{}));
         return json{{"ids", made.made}, {"left", made.left}};
       }},
      {"room_schedule",
       "The room schedule as the walls are now: per room name, level, closed, counted (closed and not sharing "
       "its region), problem, area_centre / area_inner (m2; area_inner null = not evaluated), inner_perimeter "
       "(mm), tag (what is written at its marker on screen and on drawings); totals of the counted rooms "
       "(total_area_inner null when a room's inner area is not evaluated). Not a legal floor area. "
       "include_csv: true adds UTF-8 CSV (BOM).",
       SCHEMA({"type": "object", "properties": {"include_csv": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         const doc::RoomSchedule s = doc::roomSchedule(m.document());
         json rooms = json::array();
         for (const auto& r : s.rows) {
           json row{{"id", r.id}, {"name", r.name}, {"level", r.level}, {"closed", r.closed}, {"counted", r.counted},
                    {"problem", r.problem}, {"tag", doc::roomTagText(r)}, {"use", r.use}, {"floor", r.floor},
                    {"wall", r.wall}, {"ceiling", r.ceiling}, {"ceiling_height", r.ceilingHeight}};
           if (r.closed) {
             row["area_centre"] = r.centreArea / 1e6;
             row["area_inner"] = r.innerKnown ? json(r.innerArea / 1e6) : json(nullptr);
             row["inner_perimeter"] = r.innerKnown ? json(r.innerPerimeter) : json(nullptr);
           }
           rooms.push_back(row);
         }
         json result{{"rooms", rooms}, {"total_area_centre", s.centreTotal / 1e6},
                     {"total_area_inner", s.innerMissing == 0 ? json(s.innerTotal / 1e6) : json(nullptr)},
                     {"inner_not_evaluated", s.innerMissing}, {"not_counted", s.notCounted}};
         if (a.value("include_csv", false)) result["csv"] = doc::roomScheduleCsv(s);
         return result;
       }},
      {"room_opening_inputs",
       "Read room data and geometrically associated wall-opening candidates for daylight and ventilation review. "
       "Candidates are opening rectangles next to a room's wall segment. Exterior exposure, effective areas, "
       "ventilation equipment, and compliance remain unevaluated. Area is m2; dimensions are mm.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto data = doc::roomOpeningInputs(m.document());
         const auto openingJson = [](const doc::OpeningRow& o) {
           return json{{"id", o.openingId}, {"host", o.host}, {"mark", o.mark}, {"kind", o.kind},
                       {"fitting", o.fitting}, {"wall_tagged_inner", o.inner}, {"width_mm", o.width},
                       {"height_mm", o.height}, {"sill_mm", o.sill},
                       {"daylight_factor", o.daylightFactor >= 0 ? json(o.daylightFactor) : json(nullptr)},
                       {"daylight_basis", o.daylightBasis},
                       {"ventilation_m2", o.ventilationM2 >= 0 ? json(o.ventilationM2) : json(nullptr)},
                       {"veranda", o.veranda}};
         };
         json rooms = json::array(), unmatched = json::array();
         for (const auto& entry : data.rooms) {
           const auto& room = entry.room;
           const doc::RoomDaylightCheck check = doc::roomDaylightCheck(entry);
           json candidates = json::array();
           for (std::size_t index : entry.openingIndices) candidates.push_back(openingJson(data.openings[index]));
           rooms.push_back({{"id", room.id}, {"name", room.name}, {"level", room.level}, {"use", room.use},
               {"closed", room.closed}, {"problem", room.problem},
               {"inner_area_m2", room.counted && room.innerKnown ? json(room.innerArea / 1e6) : json(nullptr)},
               {"ceiling_height_mm", room.ceilingHeight > 0 ? json(room.ceilingHeight) : json(nullptr)},
               {"opening_candidates", std::move(candidates)},
               {"daylight_kind", room.daylightKind}, {"ventilation", room.ventilation},
               {"daylight_comparison", {{"state", check.daylightState}, {"text", check.daylightText}}},
               {"ventilation_comparison", {{"state", check.ventilationState}, {"text", check.ventilationText}}},
               {"mechanical_comparison", {{"state", check.mechanicalState}, {"text", check.mechanicalText}}},
               {"joined_with", room.joinedWith}, {"fan_m3h", room.fanM3h > 0 ? json(room.fanM3h) : json(nullptr)},
               {"fan_unit", room.fanUnit}});
         }
         for (std::size_t index : data.unmatched) unmatched.push_back(openingJson(data.openings[index]));
         return json{{"rooms", std::move(rooms)}, {"unmatched_openings", std::move(unmatched)},
                     {"daylight", "未評価"}, {"natural_ventilation", "未評価"}, {"mechanical_ventilation", "未評価"}};
       }},
      {"permit_checks",
       "Rows of a permit workflow stage (docs/194) read from the model: 'stage' 2 (通り芯: levels, grids, walls "
       "on levels and on the grid, columns and beams on the grid) or 3 (平面: enclosed regions and rooms per "
       "storey, rooms that do not close or share, walls reshaped by hand, openings without a fitting) or 5 (階段, docs/197: "
       "each stair against the sizes for a house's own stair, 令 23-25 read on e-Gov 2026-11-20, the basis and what is not "
       "compared) or 6 (屋根・庇・バルコニー, docs/198: roofs' shape, pitch, eaves and walls standing up to them, canopies, "
       "balconies, the building area) or 7 (耐力壁, docs/199: the wall-quantity comparison and quarter-division balance "
       "per storey and direction, as check_wall_quantity) or 8 (基礎, docs/200: foundation plans and whether they are "
       "current, sills and columns found on supports, as check_foundation and check_foundation_supports), 9 (省エネ・設備, "
       "docs/201: wall build-ups and insulation layers, typed floors and roofs, what is not in the model yet) or 10 "
       "(立面・断面, docs/201: GL, the highest point above it, section cuts, the elevations' orientation), 11 (一式の突合せ, "
       "docs/202: rows left per stage, unlike openings under one hand-given mark, rooms sharing a name) or 12 (申請用出力, "
       "docs/202: the permit output on hold and why, and the stages a person marked), 1 (敷地: boundary, north, road, "
       "ground, zone) or 4 (居室: per room the daylight, ventilation and mechanical comparison; docs/212). Each row: "
       "item, state (未入力 / 入力済み / 要確認 / 要修正 / 階待ち / 通り芯待ち), details, targets (object ids), "
       "attention. 'confirmation' (docs/212): a person's mark on the stage, state 未確認 / 確認済み / 要再確認 (the "
       "rows changed since; 'changed' names them), on, by, note. What is found or missing only; never a legal judgement.",
       SCHEMA({"type": "object", "properties": {"stage": {"type": "integer", "minimum": 1, "maximum": 12}}, "required": ["stage"]}),
       [](doc::Modeler& m, const json& a) {
         const int stage = a.at("stage").get<int>();
         if (stage < 1 || stage > 12) throw std::runtime_error("stage must be 1 to 12");
         json rows = json::array();
         for (const auto& c : doc::permitChecks(m.document(), stage))
           rows.push_back({{"item", c.item}, {"state", c.state}, {"details", c.details}, {"targets", c.targets},
                           {"attention", c.attention}});
         const auto mark = doc::permitConfirmation(m.document(), stage);
         json confirmation = {{"state", mark.state}, {"changed", mark.changed}};
         if (!mark.on.empty()) confirmation["on"] = mark.on;
         if (!mark.by.empty()) confirmation["by"] = mark.by;
         if (!mark.note.empty()) confirmation["note"] = mark.note;
         return json{{"stage", stage}, {"rows", std::move(rows)}, {"confirmation", std::move(confirmation)}};
       }},
      {"ground_level_table",
       "docs/288: the ground level (地盤面, 建築基準法施行令 第2条第2項 read on e-Gov 2026-11-26) from the measured "
       "contact perimeter (set_site_ground): per edge the horizontal length, the heights at its ends (from the datum), "
       "their average and length x average; 'level_m' = sum(L x h) / sum(L). 'state' 未入力 / 要確認 (perimeter, points or "
       "placement do not hold: 'problem') / 算出 / 区分が必要 (the heights differ by more than 3 m: the division into "
       "parts within 3 m is the designer's; 'level_m' is then the whole average, for reference). Also as 'csv'; the "
       "drawing set carries it as the sheet 地盤面算出表 after the site plan. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto study = doc::groundLevelStudy(m.document());
         const auto table = doc::groundLevelTable(study);
         json out = {{"state", study.state}, {"problem", study.problem}, {"datum", study.datum}, {"source", study.source},
                     {"rows", table.rows}, {"notes", table.notes}, {"csv", doc::tableCsv(table)},
                     {"basis", doc::kGroundLevelLaw}};
         if (study.report) {
           out["level_m"] = study.levelMm / 1000.0;
           out["low_m"] = study.lowMm / 1000.0;
           out["high_m"] = study.highMm / 1000.0;
           out["range_m"] = study.rangeMm() / 1000.0;
           out["perimeter_m"] = study.report->lengthMm / 1000.0;
         }
         if (study.levelZ) out["level_z_mm"] = *study.levelZ;
         if (study.glZ) out["gl_z_mm"] = *study.glZ;
         if (study.levelZ && study.glZ) out["gl_minus_level_mm"] = *study.glZ - *study.levelZ;
         return out;
       }},
      {"permit_intake_review",
       "docs/286: the Yokohama intake forms held up against the model. 'rows': each 図書の種類 of 様式1-2 "
       "(意匠・設備受付チェックシート, section 4) and 様式2 (構造受付チェックシート, table 2/3) with form, group "
       "(表1（い） ...), kind, need (必要 / 該当時 / 特定木造建築物を除き必要 / 構造の規定・ルートによる / 添付の協力依頼), "
       "state (図面あり / 要確認 / 図面一式にない / 該当時に確認 / アプリ外 / 図面一式を確認できない), sheets "
       "(\"A-04 1FL 平面図\" ... from the drawing set made now), note. 'fee' (様式3): with 'whole_floor_for_fee' true "
       "(the person says a single new building's whole floor area is the application part, 第三面 11 欄イ) the "
       "area (m2, 2 decimals), the bracket and the fee in yen (①, the route-2 addition ② not included), state 未選択 / "
       "工事種別を確認 / 床面積を確認 / 候補 / 区分の境界を確認. Names matched, not contents: the sheets' boxes stay the "
       "designer's; never a legal judgement. 'source' names the form versions. Rows of 様式1-2 sections 1, 2, 5 and 6 "
       "(概要書, 消防同意, 協定等届出, 委任状, 工事監理者, 構造適判, 省エネ適判) are read from the project outline as "
       "candidates. 'drawing_list': each sheet with the 図書の種類 it stands for (様式1-2 別紙), also as CSV.",
       SCHEMA({"type": "object", "properties": {"whole_floor_for_fee": {"type": "boolean"}, "scale": {"type": "number"}}}),
       [](doc::Modeler& m, const json& a) {
         doc::DrawingSetSpec setSpec;
         setSpec.scale = numberOr(a, "scale", 100.0);
         const auto set = doc::drawingSet(m.document(), setSpec);
         const auto review = doc::yokohamaIntakeReview(m.document(), set ? &*set : nullptr, set ? std::string() : set.error(),
                                                       a.value("whole_floor_for_fee", false));
         json rows = json::array();
         for (const auto& r : review.rows)
           rows.push_back({{"form", r.form}, {"group", r.group}, {"kind", r.kind}, {"need", r.need}, {"state", r.state},
                           {"sheets", r.sheets}, {"note", r.note}});
         const auto& f = review.fee;
         json fee = {{"state", f.state}, {"note", f.note}};
         if (f.known) {
           fee["area_m2"] = f.area;
           fee["bracket"] = f.bracket;
           fee["fee_yen"] = f.fee;
         }
         json list = json::array();
         for (const auto& d : review.drawingList) list.push_back({{"number", d.number}, {"name", d.name}, {"kinds", d.kinds}});
         json out = {{"rows", std::move(rows)}, {"fee", std::move(fee)}, {"source", review.source},
                     {"drawing_list", std::move(list)}, {"drawing_list_csv", doc::tableCsv(doc::intakeDrawingList(review))},
                     {"forms_url", doc::kYokohamaIntakeForms}};
         if (!review.drawingError.empty()) out["drawing_error"] = review.drawingError;
         return out;
       }},
      {"set_equipment",
       "docs/224: the building's equipment for the energy documents, as the person gives it (the whole list "
       "replaced; [] takes all away): 'items' [{kind: heating_cooling | ventilation | hot_water | lighting | solar | "
       "other, name (required), model, value (性能値 as the catalogue writes it, e.g. \"エネルギー消費効率区分（い）\", "
       "\"JIS効率 3.8\"), count (default 1), source}]. Only what the person said: never guess a maker, model or value. "
       "One undo step. list_equipment reads them; drawn as the 機器表 on the insulation sheets.",
       SCHEMA({"type": "object", "properties": {"items": {"type": "array", "items": {"type": "object", "properties": {
                   "kind": {"enum": ["heating_cooling", "ventilation", "hot_water", "lighting", "solar", "other"]},
                   "name": {"type": "string"}, "model": {"type": "string"}, "value": {"type": "string"},
                   "count": {"type": "integer"}, "source": {"type": "string"}}, "required": ["kind", "name"]}}},
               "required": ["items"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::EquipmentItem> items;
         for (const auto& j : a.at("items")) {
           doc::Modeler::EquipmentItem e;
           e.kind = j.at("kind").get<std::string>();
           e.name = j.at("name").get<std::string>();
           e.model = j.value("model", std::string());
           e.value = j.value("value", std::string());
           e.source = j.value("source", std::string());
           e.count = j.value("count", 1);
           items.push_back(std::move(e));
         }
         must(m.setEquipment(items));
         return json{{"count", items.size()}};
       }},
      {"set_vicinity_map",
       "docs/233: where the vicinity map (付近見取図) of the drawing set is centred: 'lat', 'lon' (WGS84 degrees, the site; "
       "Japan only), 'zoom' (tile zoom 14-18, default 16), 'layer' (std 標準地図 / pale 淡色地図 / seamlessphoto 写真, default "
       "std); 'marks' (docs/235): landmarks [{name, lat, lon}] up to 20, written on the map; 'clear' true takes it away. "
       "Only positions the person gave: never look one up from an address or a name. The app "
       "fetches 国土地理院's tiles when it writes the drawing set (the position is sent to 国土地理院); 標準地図・淡色地図 are "
       "基本測量成果: whether an application is needed is the person's to check. One undo step.",
       SCHEMA({"type": "object", "properties": {"lat": {"type": "number"}, "lon": {"type": "number"},
               "zoom": {"type": "integer"}, "layer": {"enum": ["std", "pale", "seamlessphoto"]}, "clear": {"type": "boolean"},
               "marks": {"type": "array", "items": {"type": "object", "properties": {"name": {"type": "string"},
                         "lat": {"type": "number"}, "lon": {"type": "number"}}, "required": ["name", "lat", "lon"]}}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.value("clear", false)) {
           must(m.clearVicinityMap());
           return json{{"cleared", true}};
         }
         std::vector<doc::VicinityMark> marks;
         for (const auto& j : a.value("marks", json::array()))
           marks.push_back({j.at("name").get<std::string>(), j.at("lat").get<double>(), j.at("lon").get<double>()});
         must(m.setVicinityMap(number(a, "lat"), number(a, "lon"), a.value("zoom", 16), textOr(a, "layer", "std"), marks));
         const auto map = doc::vicinityMapOf(m.document());
         const auto tiles = doc::vicinityTiles(*map);
         json urls = json::array();
         for (const auto& t : tiles.tiles) urls.push_back(t.url);
         return json{{"tiles", std::move(urls)}, {"metres_per_pixel", tiles.metresPerPixel},
                     {"attribution", doc::vicinityAttribution(map->layer)}};
       }},
      {"list_equipment",
       "docs/224: the equipment given with set_equipment, in order, with each kind's label.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json out = json::array();
         for (const auto& e : doc::Modeler::equipmentOf(m.document())) {
           std::string label = e.kind;
           for (const auto& [key, text] : doc::Modeler::equipmentKinds())
             if (key == e.kind) label = text;
           out.push_back({{"kind", e.kind}, {"label", label}, {"name", e.name}, {"model", e.model}, {"value", e.value},
                          {"count", e.count}, {"source", e.source}});
         }
         return json{{"items", std::move(out)}};
       }},
      {"set_structural_materials",
       "docs/291: the structural timber's material and grade per member role, as the person gives them (the whole "
       "list replaced; [] takes all away): 'items' [{role (a timber role of get_construction_guide, e.g. sill, column, "
       "stud, floor_beam, brace, rafter, or 'default' for every role not given), material (樹種等, required), grade "
       "(等級・規格, e.g. \"E70 機械等級区分 JAS\"), source}]. Only what the person said: never guess a species or grade. "
       "One undo step. structural_material_table reads the 使用構造材料一覧表 (also in the drawing set).",
       SCHEMA({"type": "object", "properties": {"items": {"type": "array", "items": {"type": "object", "properties": {
                   "role": {"type": "string"}, "material": {"type": "string"}, "grade": {"type": "string"},
                   "source": {"type": "string"}}, "required": ["role", "material"]}}}, "required": ["items"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::StructuralMaterial> items;
         for (const auto& j : a.at("items"))
           items.push_back({j.at("role").get<std::string>(), j.at("material").get<std::string>(),
                            j.value("grade", std::string()), j.value("source", std::string())});
         if (const auto done = m.setStructuralMaterials(items); !done) throw ApiError(done.message());
         return json{{"items", items.size()}};
       }},
      {"set_foundation_ground",
       "docs/308: the ground survey's results for the 基礎・地盤説明書, as the person gives them (all replaced; "
       "{} takes them away): survey_method (e.g. スクリューウエイト貫入試験), survey_date, soil, allowable_kn_m2 (地盤の長期許容応力度, "
       "after improvement when improved), basis (how it was found), improvement (なし or the method), frost_depth_mm, "
       "source (the survey report, required when anything is given). Never guess a bearing value. One undo step. "
       "foundation_ground_table reads the statement (also in the drawing set).",
       SCHEMA({"type": "object", "properties": {"survey_method": {"type": "string"}, "survey_date": {"type": "string"},
               "soil": {"type": "string"}, "allowable_kn_m2": {"type": "number"}, "basis": {"type": "string"},
               "improvement": {"type": "string"}, "frost_depth_mm": {"type": "number"}, "source": {"type": "string"}}}),
       [](doc::Modeler& m, const json& a) {
         doc::FoundationGround g;
         g.surveyMethod = a.value("survey_method", std::string());
         g.surveyDate = a.value("survey_date", std::string());
         g.soil = a.value("soil", std::string());
         if (a.contains("allowable_kn_m2")) g.allowableKNm2 = number(a, "allowable_kn_m2");
         g.basis = a.value("basis", std::string());
         g.improvement = a.value("improvement", std::string());
         if (a.contains("frost_depth_mm")) g.frostDepthMm = number(a, "frost_depth_mm");
         g.source = a.value("source", std::string());
         if (const auto done = m.setFoundationGround(g); !done) throw ApiError(done.message());
         return json{{"ok", true}};
       }},
      {"foundation_ground_table",
       "docs/308: 基礎・地盤説明書: the ground survey's results given with set_foundation_ground and the model's "
       "foundation plans (kind, stem, slab or footing, depth under GL) beside the figures of 平成12年建設省告示第1347号, "
       "each row 規定内 / 規定外 / 未入力 / 未評価. A comparison with the notice's figures only, not a structural check; "
       "rebar is not in the model (未評価). Rows, notes and CSV. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto study = doc::foundationGroundStudy(m.document());
         const auto t = doc::foundationGroundTable(study);
         return json{{"headers", t.headers}, {"rows", t.rows}, {"notes", t.notes},
                     {"has_foundation", study.hasFoundation}, {"csv", doc::tableCsv(t)}};
       }},
      {"set_structural_specs",
       "docs/311: what the 構造詳細図 of a timber building must show (規則第1条の3 表2) that the model does not hold, as "
       "the person gives it (all replaced; {} takes them away): roofing (屋根ふき材の種別, 令39条), frame_method (軸組等の"
       "構造方法, 令46条), joints (継手・仕口の構造方法, 令47条), wall_base (腐りやすい外壁の下地, 令49条1項), decay (地面から"
       "1m以内の防腐・防蟻, 令49条2項), source (required when anything is given). One line each. Never guess. One undo step.",
       SCHEMA({"type": "object", "properties": {"roofing": {"type": "string"}, "frame_method": {"type": "string"},
               "joints": {"type": "string"}, "wall_base": {"type": "string"}, "decay": {"type": "string"},
               "source": {"type": "string"}}}),
       [](doc::Modeler& m, const json& a) {
         doc::StructuralSpecs s;
         s.roofing = a.value("roofing", std::string());
         s.frameMethod = a.value("frame_method", std::string());
         s.joints = a.value("joints", std::string());
         s.wallBase = a.value("wall_base", std::string());
         s.decay = a.value("decay", std::string());
         s.source = a.value("source", std::string());
         if (const auto done = m.setStructuralSpecs(s); !done) throw ApiError(done.message());
         return json{{"ok", true}};
       }},
      {"structural_spec_table",
       "docs/311: 構造詳細図（仕様）: the items of 規則第1条の3 表2 for timber with what was given (set_structural_specs) "
       "and the columns' largest slenderness against 令43条6項. Rows, notes and CSV. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto t = doc::structuralSpecTable(m.document());
         return json{{"headers", t.headers}, {"rows", t.rows}, {"notes", t.notes}, {"csv", doc::tableCsv(t)}};
       }},
      {"interior_area_table",
       "docs/314: 令第20条の7第1項第2号 per living room that uses 第二種 / 第三種 materials (set_interior_materials): "
       "their areas (floor and ceiling the inner floor area, walls the inner perimeter times the ceiling height) times "
       "the table's figures for 住宅等の居室 or not and the air changes from the room's fan airflow, against the floor "
       "area. Rows, notes and CSV. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto t = doc::interiorAreaTable(m.document());
         return json{{"headers", t.headers}, {"rows", t.rows}, {"notes", t.notes}, {"csv", doc::tableCsv(t)}};
       }},
      {"set_height_study",
       "docs/318: the height study (高さ制限の合成) as the person gives its figures, kept with the model for the current "
       "site (used while the site's outline, zone and district stay the same): ground_z (mm, model Z), road {edge_z (the "
       "slant's height at the site-side road boundary before setback, model Z), slope, width_m, distance_m (application "
       "distance), setback_m}, neighbor {rise_m, slope}, north_rise_m (5 or 10), district (bool), single_north_road (bool), "
       "boundaries [[[x,y,z],[x,y,z]], ...] (the confirmed neighbour edges). clear: true takes it away. Never guess a "
       "figure: these are the person's. The drawing set draws the road slant from it. One undo step.",
       SCHEMA({"type": "object", "properties": {"ground_z": {"type": "number"}, "road": {"type": "object"},
               "neighbor": {"type": "object"}, "north_rise_m": {"type": "number"}, "district": {"type": "boolean"},
               "single_north_road": {"type": "boolean"}, "boundaries": {"type": "array"}, "clear": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.value("clear", false)) {
           if (const auto done = m.setHeightStudyText("{}"); !done) throw ApiError(done.message());
           return json{{"ok", true}};
         }
         if (!m.document().settings().siteBoundary) throw ApiError("先に敷地境界を指定してください");
         doc::HeightLimitStudy s;
         s.groundZMm = a.value("ground_z", 0.0);
         if (a.contains("road")) {
           const auto& r = a.at("road");
           s.road = doc::HeightLimitStudy::Road{number(r, "edge_z"), number(r, "slope"),
               doc::RoadHeightPlaneRange{number(r, "width_m") * 1000, number(r, "distance_m") * 1000,
                                         numberOr(r, "setback_m", 0.0) * 1000}};
           if (!(s.road->slope > 0) || !(s.road->range.roadWidthMm > 0) || !(s.road->range.applicationDistanceMm > 0))
             throw ApiError("道路斜線の勾配・幅員・適用距離は 0 より大きい値です");
         }
         if (a.contains("neighbor"))
           s.neighbor = doc::HeightLimitStudy::Neighbor{number(a.at("neighbor"), "rise_m") * 1000, number(a.at("neighbor"), "slope")};
         if (a.contains("north_rise_m")) {
           const double rise = number(a, "north_rise_m");
           if (rise != 5 && rise != 10) throw ApiError("north_rise_m は 5 か 10 です");
           s.northRiseMm = rise * 1000;
         }
         s.district = a.value("district", false);
         s.singleNorthRoadConfirmed = a.value("single_north_road", false);
         if (a.contains("boundaries"))
           for (const auto& e : a.at("boundaries"))
             s.boundaries.push_back({gp_Pnt(e.at(0).at(0).get<double>(), e.at(0).at(1).get<double>(), e.at(0).at(2).get<double>()),
                                     gp_Pnt(e.at(1).at(0).get<double>(), e.at(1).at(1).get<double>(), e.at(1).at(2).get<double>())});
         if (const auto done = m.setHeightStudyText(doc::heightStudyText(m.document(), s)); !done) throw ApiError(done.message());
         return json{{"ok", true}};
       }},
      {"height_study",
       "docs/318: the height study kept with the model: its figures and whether it is for the current site ('current': "
       "false when the site's outline, zone or district changed since; it is then not used). Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto stored = json::parse(m.document().settings().heightStudy, nullptr, false);
         return json{{"stored", stored}, {"current", doc::savedHeightStudy(m.document()).has_value()}};
       }},
      {"column_table",
       "docs/310: 構造詳細図（柱の小径・有効細長比）: the timber columns (take_off column / through_column) grouped by "
       "section and the vertical distance between the horizontal members they meet, with smaller side / distance and the "
       "effective slenderness ratio against 令第43条第6項 (150 以下). The ratio of 令第43条第1項 is set by a notice and "
       "is not compared (未評価). Rows, notes and CSV. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto t = doc::columnTable(doc::columnRows(m.document()));
         return json{{"headers", t.headers}, {"rows", t.rows}, {"notes", t.notes}, {"csv", doc::tableCsv(t)}};
       }},
      {"structural_material_table",
       "docs/291: 使用構造材料一覧表: the timber members by role and section with their count (from take_off) and "
       "the material and grade given per role (未入力 where not given). Rows, notes and CSV. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto t = doc::structuralMaterialTable(m.document());
         json given = json::array();
         for (const auto& s : doc::Modeler::structuralMaterialsOf(m.document()))
           given.push_back({{"role", s.role}, {"material", s.material}, {"grade", s.grade}, {"source", s.source}});
         return json{{"headers", t.headers}, {"rows", t.rows}, {"notes", t.notes}, {"given", given},
                     {"csv", doc::tableCsv(t)}};
       }},
      {"set_interior_materials",
       "docs/292: the formaldehyde class (令 20 条の 7, read on e-Gov 2026-11-26) of the interior finish materials, per "
       "material name written exactly as on the rooms' finishes (set_room_finish), as the person gives them (the whole "
       "list replaced; [] takes all away): 'items' [{name (required), class: first (第一種, may not be used) / second "
       "(第二種) / third (第三種) / exempt (規制対象外, e.g. F☆☆☆☆ or not such a material), mark (表示, e.g. F☆☆☆☆), "
       "source}]. Never guess a class from a product name. One undo step. interior_material_table reads the 使用建築材料表.",
       SCHEMA({"type": "object", "properties": {"items": {"type": "array", "items": {"type": "object", "properties": {
                   "name": {"type": "string"}, "class": {"enum": ["first", "second", "third", "exempt"]},
                   "mark": {"type": "string"}, "source": {"type": "string"}}, "required": ["name", "class"]}}},
               "required": ["items"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::Modeler::InteriorMaterial> items;
         for (const auto& j : a.at("items"))
           items.push_back({j.at("name").get<std::string>(), j.at("class").get<std::string>(), j.value("mark", std::string()),
                            j.value("source", std::string())});
         if (const auto done = m.setInteriorMaterials(items); !done) throw ApiError(done.message());
         return json{{"items", items.size()}};
       }},
      {"interior_material_table",
       "docs/292: 使用建築材料表: per 居室 the floor, wall and ceiling finishes with the class given per material and "
       "what 令 20 条の 7 says (第一種: may not be used; 第二種・第三種: area limited, not compared; 規制対象外). Rows, "
       "notes, CSV. Read only; never a legal judgement.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto t = doc::interiorMaterialTable(m.document());
         return json{{"headers", t.headers}, {"rows", t.rows}, {"notes", t.notes}, {"csv", doc::tableCsv(t)}};
       }},
      {"set_project_outline",
       "docs/214: items of the project outline that are not in the model, as the person gives them: 'items' "
       "{key: text}; keys (docs/231: as the application form 第二号様式 第二面・第三面) project_name (物件名, also the "
       "title block's 工事名 when the drawing set has none), client (建築主), agent (代理者), designer (設計者), "
       "supervisor (工事監理者), contractor (工事施工者), structural_review (構造計算適合性判定の申請: submitted / not_yet / "
       "not_required), energy_plan (省エネ確保計画の提出: submitted / not_yet / not_required), address (地名地番), "
       "residence (住居表示), city_planning "
       "(urbanization / control / non_divided / quasi / outside), fire_zone (fire / quasi_fire / none; article22 = "
       "指定なし（法 22 条区域）), other_zones (その他の区域、地域、地区又は街区), main_use (主要用途), use_code (主要用途の区分), "
       "work_kind (new / extension / rebuild / relocation / use_change / major_repair / major_remodel), structure (構造), "
       "permissions (許可・認定等), start_date, finish_date, specific_process (特定工程), other_matters (その他必要な事項), "
       "source (確認資料). Which of submitted / not_required applies is the person's call: never work it out. \"\" takes one away; "
       "keys not given stay. Only what the person said: never guess a client or an address. One undo step. "
       "project_outline reads them.",
       SCHEMA({"type": "object", "properties": {"items": {"type": "object", "additionalProperties": {"type": "string"}}},
               "required": ["items"]}),
       [](doc::Modeler& m, const json& a) {
         std::map<std::string, std::string> items;
         for (const auto& [key, value] : a.at("items").items()) {
           if (!value.is_string()) throw std::runtime_error("items." + key + " must be a string");
           items[key] = value.get<std::string>();
         }
         must(m.setProjectOutline(items));
         json out = json::object();
         for (const auto& [key, value] : doc::projectOutline(m.document())) out[key] = value;
         return json{{"items", std::move(out)}};
       }},
      {"project_outline",
       "docs/214: the project outline items given (set_project_outline) with their labels, and the fields with their "
       "choices. Read only.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto given = doc::projectOutline(m.document());
         json fields = json::array();
         for (const auto& f : doc::projectOutlineFields()) {
           json field = {{"key", f.key}, {"label", f.label}};
           if (const auto it = given.find(f.key); it != given.end())
             field["value"] = it->second, field["text"] = doc::projectOutlineText(f.key, it->second);
           if (!f.choices.empty()) {
             json choices = json::object();
             for (const auto& [code, label] : f.choices) choices[code] = label;
             field["choices"] = std::move(choices);
           }
           fields.push_back(std::move(field));
         }
         return json{{"fields", std::move(fields)}};
       }},
      {"confirm_permit_stage",
       "docs/212: a person's mark that a permit workflow stage (1-12) was looked at. It keeps a fingerprint of the "
       "stage's rows now; when the model changes them, permit_checks shows the stage as 要再確認 with the rows that "
       "changed. 'confirmed' false takes the mark away. 'by' (who), 'note', 'on' (YYYY-MM-DD, default today). Only "
       "on the person's word: never mark a stage yourself. A record that a person looked, never a pass. One undo step.",
       SCHEMA({"type": "object", "properties": {"stage": {"type": "integer", "minimum": 1, "maximum": 12},
               "confirmed": {"type": "boolean"}, "by": {"type": "string"}, "note": {"type": "string"},
               "on": {"type": "string"}}, "required": ["stage"]}),
       [](doc::Modeler& m, const json& a) {
         const int stage = a.at("stage").get<int>();
         must(m.setPermitConfirmation(stage, a.value("confirmed", true), textOr(a, "by", ""), textOr(a, "note", ""),
                                      textOr(a, "on", "")));
         return json{{"stage", stage}, {"state", doc::permitConfirmation(m.document(), stage).state}};
       }},
      {"area_table",
       "Areas by storey measured from the model (m2): per level 'enclosed' (everything the walls standing on it "
       "enclose, within the wall lines, with or without rooms), regions, rooms, regions_without_room, room_centre, "
       "room_inner (null = not evaluated), slab (plan area of the slabs whose top lies on the level, openings "
       "taken out), slabs, unreadable_walls (walls reshaped by hand: not read), legal_floor (床面積 by the Building "
       "Standards Act Enforcement Order art. 2 (1) iii: within the wall lines where there is a floor; "
       "\"not_evaluated\" when it cannot be read). total_legal_floor (延べ面積, iv), ratio_floor_area (the total "
       "the floor area ratio is worked out from: less the rooms marked with set_room_finish ratio_part, each kind "
       "up to its share, listed in ratio_parts; docs/162) and building_area (建築面積, ii: every storey within the wall lines plus eaves "
       "beyond 1 m, back 1 m) with 'legal_notes' (the basis, what was assumed and what is not evaluated; docs/141). "
       "include_csv: true adds UTF-8 CSV (BOM).",
       SCHEMA({"type": "object", "properties": {"include_csv": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         const doc::AreaTable t = doc::areaTable(m.document());
         json rows = json::array();
         for (const auto& r : t.rows)
           rows.push_back({{"level", r.level}, {"elevation", r.elevation},
                           {"enclosed", r.enclosedKnown ? json(r.enclosedArea / 1e6) : json(nullptr)}, {"regions", r.regions},
                           {"rooms", r.rooms}, {"regions_without_room", r.regionsWithoutRoom},
                           {"room_centre", r.roomCentre / 1e6},
                           {"room_inner", r.innerMissing == 0 ? json(r.roomInner / 1e6) : json(nullptr)},
                           {"slab", r.slabArea / 1e6}, {"slabs", r.slabs}, {"unreadable_walls", r.unreadableWalls},
                           {"legal_floor", !r.enclosedKnown ? json(nullptr) : r.legalKnown ? json(r.legalFloor / 1e6) : json("not_evaluated")},
                           {"basement", r.basement}, {"ceiling", r.ceiling > 0 ? json(r.ceiling) : json(nullptr)}});
         bool allKnown = true;
         for (const auto& r : t.rows) allKnown = allKnown && (!r.enclosedKnown || r.legalKnown);
         json result{{"levels", rows}, {"total_enclosed", t.enclosedTotal / 1e6}, {"total_room_centre", t.roomCentreTotal / 1e6},
                     {"total_slab", t.slabTotal / 1e6},
                     {"total_legal_floor", allKnown ? json(t.legalTotal / 1e6) : json("not_evaluated")},
                     {"building_area", t.buildingKnown ? json(t.buildingArea / 1e6) : json("not_evaluated")},
                     {"ratio_floor_area", t.ratioKnown ? json(t.ratioTotal / 1e6) : json("not_evaluated")},
                     {"ground", t.groundKnown ? json(t.ground) : json("not_evaluated")},
                     {"legal_notes", t.legalNotes}};
         json parts = json::array();
         for (const auto& part : t.ratioParts)
           parts.push_back({{"part", part.code}, {"label", part.label}, {"basis", part.basis}, {"rooms", part.rooms}, {"area", part.area / 1e6},
                            {"limit", part.limit >= 0.0 ? json(part.limit / 1e6) : json(nullptr)}, {"excluded", part.excluded / 1e6}});
         result["ratio_parts"] = parts;
         if (a.value("include_csv", false)) result["csv"] = doc::areaTableCsv(t);
         return result;
       }},
      {"list_rooms",
       "Every room (create_room) with its region as the walls are now: id, name, level, closed, problem, "
       "area_centre / area_inner (m2; area_inner null = not evaluated), inner_perimeter (mm), outlines, walls. "
       "A room whose walls were opened reports closed=false and no area.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json rooms = json::array();
         double centre = 0.0;
         int open = 0;
         for (const doc::RoomInfo& r : m.rooms()) {
           rooms.push_back(roomJson(r));
           if (r.closed && r.problem.empty()) centre += r.centreArea;
           else ++open;
         }
         return json{{"rooms", rooms}, {"total_area_centre", centre / 1e6}, {"not_counted", open},
                     {"basis", "area_centre: within the lines the walls were drawn on; area_inner: within the wall faces. "
                               "Not a legal floor area (no rules on what counts are applied)."}};
       }},
      {"create_slab",
       "A floor slab: 'outline' of plan points ([x, y] or \"X1/Y1\") or 'grid' {x: [\"X1\", \"X4\"], y: "
       "[\"Y1\", \"Y3\"]} for the rectangle between those lines, widened by 'overhang' (mm). Its top is "
       "at 'top' (default 0), 'thickness' below. Attribute kind=slab. One undo step.",
       SCHEMA({"type": "object", "properties": {
         "outline": {"type": "array"}, "grid": {"type": "object"}, "overhang": {"type": "number"},
         "top": {"type": "number"}, "thickness": {"type": "number"}}, "required": ["thickness"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         std::vector<gp_Pnt> pts;
         if (a.contains("grid")) {
           const json& g = a.at("grid");
           const auto xs = ObjectFilter::strings(g, "x"), ys = ObjectFilter::strings(g, "y");
           if (xs.size() != 2 || ys.size() != 2) throw ApiError("'grid' needs two x and two y line names");
           const double over = numberOr(a, "overhang", 0.0);
           double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
           for (const std::string& xn : xs) {
             for (const std::string& yn : ys) {
               const gp_Pnt p = grid.crossing(xn, yn);
               x0 = std::min(x0, p.X()), x1 = std::max(x1, p.X()), y0 = std::min(y0, p.Y()), y1 = std::max(y1, p.Y());
             }
           }
           pts = {gp_Pnt(x0 - over, y0 - over, 0), gp_Pnt(x1 + over, y0 - over, 0), gp_Pnt(x1 + over, y1 + over, 0),
                  gp_Pnt(x0 - over, y1 + over, 0)};
         } else {
           const json& list = arg(a, "outline");
           if (!list.is_array()) throw ApiError("'outline' must be an array");
           for (const json& p : list) pts.push_back(grid.point(p, 0.0));
         }
         return idResult(must(m.createSlab(pts, numberOr(a, "top", 0.0), number(a, "thickness"))));
       }},
      {"list_sections",
       "Sections for create_member. system: steel (default, legacy behavior) or timber; role filters timber roles. "
       "Read get_construction_guide first. Timber returns provenance, default_section and section objects. "
       "Sheets and unselected products cannot be swept into members. Built-in steel catalog (1,204 steel "
       "sections: H, I, BOX, PIPE, ANGLE, CHANNEL, C, CT, FB, SB, RB, HB). 'query': part of the id or name "
       "(e.g. \"400x200\", \"BCP-350\"); 'family' narrows to one family. 'exact' lists the id / name equal "
       "to the query; 'matches' the others, 'limit' (default 50) from 'offset'. A missing size is never "
       "replaced by another one. Rectangles and circles (timber, RC) need no entry: {kind: rectangle, "
       "width, depth} or {kind: circle, diameter}.",
       SCHEMA({"type": "object", "properties": {"system": {"type": "string", "enum": ["steel", "timber", "rc"]}, "role": {"type": "string"}, "query": {"type": "string"}, "family": {"type": "string"},
                                                "limit": {"type": "integer"}, "offset": {"type": "integer"}}}),
       [](doc::Modeler& m, const json& a) {
         const std::string system = textOr(a, "system", "steel"), role = textOr(a, "role", "");
         const std::string query = textOr(a, "query", ""), family = textOr(a, "family", "");
         const int limit = static_cast<int>(numberOr(a, "limit", 50)), offset = static_cast<int>(numberOr(a, "offset", 0));
         if (limit < 1 || limit > 500 || offset < 0) throw ApiError("limit: 1 to 500, offset: 0 or more");
         if (system == "timber") {
           const json data = json::parse(construction::guideData(system, constructionPatch(m, system)));
           const auto& roles = data.at("roles").at("entries");
           if (!role.empty() && !roles.contains(role)) throw ApiError("unknown timber role '" + role + "'");
           json exact = json::array(), matches = json::array();
           int total = 0;
           for (const auto& [id, s] : data.at("sections").at("entries").items()) {
             if (!role.empty()) {
               const auto& choices = roles.at(role).at("sections");
               if (std::find(choices.begin(), choices.end(), json(id)) == choices.end()) continue;
             }
             const std::string kind = s.at("kind"), name = s.at("name");
             const std::string entryFamily = kind == "rectangle" ? "TIMBER" : kind == "sheet" ? "PLYWOOD" : "PRODUCT";
             if (!family.empty() && family != entryFamily) continue;
             json e = s;
             e["id"] = id; e["family"] = entryFamily;
             e["source"] = data.at("sections").at("source");
             e["normative"] = data.at("sections").at("normative");
             e["creatable_member"] = kind == "rectangle";
             if (kind == "rectangle") e["section"] = {{"kind", "rectangle"}, {"width", s.at("width")}, {"depth", s.at("depth")}};
             if (!query.empty() && (id == query || name == query)) { exact.push_back(e); continue; }
             if (!query.empty() && id.find(query) == std::string::npos && name.find(query) == std::string::npos) continue;
             if (total >= offset && total < offset + limit) matches.push_back(e);
             ++total;
           }
           return json{{"system", system}, {"role", role},
                       {"default_section", role.empty() ? json(nullptr) : roles.at(role).at("default_section")},
                       {"exact", exact}, {"matches", matches}, {"total_matches", total}, {"offset", offset}, {"limit", limit},
                       {"units", "mm"}, {"source", data.at("sections").at("source")},
                       {"normative", data.at("sections").at("normative")},
                       {"overrides", data.value("overrides", json::object())}, {"notice", data.at("notice")}};
         }
         if (system != "steel") throw ApiError("section catalog not implemented for system '" + system + "'");
         if (!role.empty()) throw ApiError("steel role filters are not implemented; omit role");
         auto entry = [](const geom::CatalogSection& c) {
           json dims = json::object();
           for (const auto& [k, v] : c.dimensions) dims[k] = v;
           return json{{"id", c.id}, {"name", c.name}, {"family", c.family}, {"standard", c.standard},
                       {"subtype", c.subtype}, {"material_class", c.material}, {"dimensions", dims},
                       {"width", c.section.width}, {"depth", c.section.depth}};
         };
         json exact = json::array(), matches = json::array();
         int total = 0;
         for (const geom::CatalogSection& c : geom::sectionCatalog()) {
           if (!family.empty() && c.family != family) continue;
           if (!query.empty() && (c.id == query || c.name == query)) {
             exact.push_back(entry(c));
             continue;
           }
           if (!query.empty() && c.id.find(query) == std::string::npos && c.name.find(query) == std::string::npos) continue;
           if (total >= offset && total < offset + limit) matches.push_back(entry(c));
           ++total;
         }
         return json{{"exact", exact}, {"matches", matches}, {"total_matches", total}, {"offset", offset},
                     {"limit", limit}, {"units", "mm"}, {"source", geom::sectionCatalogSource()},
                     {"own_sections", "{kind: rectangle, width, depth}, {kind: circle, diameter}, "
                                      "{kind: h, width, depth, web, flange, radius}"}};
       }},
      {"generate_foundation",
       "Plan concrete strip foundations from existing timber sills. inputs: frame (group) OR sources:[sillIds], "
       "stem_width:150,stem_height:600,footing_width:450,footing_thickness:150,packing_mm:20 (planning defaults in mm, not design values). "
       "top_z optional world elevation; omitted = sill bottom minus packing. Sources must be horizontal, coplanar rectangular sills. "
       "Unions intersections per layer for net concrete volume. Preview by default; apply requires expected_revision or atomic proposal/batch. "
       "replace regenerates a plan; edited outputs need on_modified:replace. Identical re-runs do not duplicate a plan. "
       "For isolated pads use inputs:{type:pads,sources:[real floor_post/column IDs],width:300,depth:300,height:150,top_z?}. "
       "Pad top defaults to each source bottom; world-XY rectangle, planning dimensions only. No frame scope for pads. "
       "Coincident sources share a pad; intersecting pads or existing solids cause an atomic error. "
       "For a mat foundation (beta kiso) use inputs:{type:mat,frame OR sources,stem_width:150,stem_height:400 (rise above the slab top),"
       "slab_thickness:150,edge_width:300,edge_depth:150 (edge footing under the outer stems below the slab bottom; 0 = none),"
       "inner_width:300,inner_depth:0 (ground beams under the interior stems below the slab, clipped to the outline; 0 = none),"
       "packing_mm:20,top_z?,passages?:[{at:[x,y],width:600,height:330}]}: stems under the sills on a slab that fills "
       "the outer outline of the stems (the sills must form one closed loop). Passages cut one interior stem each; outer stems, "
       "junctions and overlaps are refused. Type cannot change on replace. "
       "Geometry only: soil, capacity, reinforcement, frost, passages and ventilation remain unassessed.",
       SCHEMA({"type":"object","properties":{"inputs":{"type":"object"},"replace":{"type":"string"},
         "preview":{"type":"boolean"},"expected_revision":{"type":"integer"},
         "on_modified":{"enum":["ask","replace"]},"name":{"type":"string"}}}),
       [](doc::Modeler& m,const json& a) {
         if(a.contains("expected_revision") && a["expected_revision"]!=m.history().revision())
           throw ApiError("The model changed; preview the foundation again");
         if(a.value("preview",true)) {
           doc::Modeler scratch;scratch.copyFrom(m);json trial=a;trial.erase("expected_revision");
           auto result=json::parse(must(scratch.generateFoundation(trial.dump())));
           result["preview"]=true;result["revision"]=m.history().revision();return result;
         }
         if(!a.contains("expected_revision") && !m.history().inGroup())
           throw ApiError("Preview first and supply expected_revision, or apply in a revision-checked batch/proposal");
         auto result=json::parse(must(m.generateFoundation(a.dump())));result["preview"]=false;return result;
       }},
      {"check_foundation",
       "Read-only generated strip-foundation plan status, live source changes, packing gaps and exact concrete volumes. "
       "Optional foundation scopes one plan. current is recipe/geometry freshness, not a structural safety verdict. "
       "Missing sources and edited outputs are reported; no model changes.",
       SCHEMA({"type":"object","properties":{"foundation":{"type":"string"}}}),
       [](doc::Modeler& m,const json& a) { auto result=json::parse(must(construction::checkFoundation(m.document(),a.dump())));
         result["revision"]=m.history().revision();return result; }},
      {"check_foundation_supports",
       "Read-only base-face support geometry for timber sills, vertical columns and floor posts. "
       "within/ids scope subjects; supports are searched across the document, including hidden parts. "
       "Measures actual horizontal face overlap and vertical gaps, traces column -> sill -> foundation. "
       "Partial, ambiguous, missing, gap and unsupported cases are explicit. Display-only floor posts and upper-storey routes are unassessed. "
       "Generated plan freshness is separate from geometric contact. Never establishes load transfer, anchorage, capacity or soil safety.",
       SCHEMA({"type":"object","properties":{"within":{"type":"string"},"ids":{"type":"array","items":{"type":"string"}},
         "tolerance_mm":{"type":"number"},"search_depth_mm":{"type":"number"},"packing_mm":{"type":"number"},
         "pair_budget":{"type":"integer"}}}),
       [](doc::Modeler& m,const json& a) {auto result=json::parse(must(construction::foundationSupports(m.document(),a.dump())));
         result["revision"]=m.history().revision();return result;}},
      {"check_structure",
       "Read-only structural overview for an on-screen overlay: wall quantity rows (existing/required per storey and "
       "direction, with the coefficient-table caveat), designated walls with world coordinates (drawn at the nFL level), "
       "and markers from check_frame (fail / not_evaluated) and check_foundation_supports (no detected support). "
       "frame_rules optionally limits the frame rules. Quarter-division balance, N-value joints, post size and member "
       "sections are listed as unassessed, never as passing. Changes nothing.",
       SCHEMA({"type":"object","properties":{"frame_rules":{"type":"array","items":{"type":"string"}}}}),
       [](doc::Modeler& m,const json& a) {auto result=json::parse(must(construction::structuralDiagnostics(m.document(),a.dump())));
         result["revision"]=m.history().revision();return result;}},
      {"structure_assist",
       "Read-only figures taken from the model's shape for the designer to check before the wall calculation "
       "(docs/87): wind projected areas per storey and direction (FL + wind_cut_mm, default 1350, left out), "
       "floor-slab areas and plan extents (kind=slab within floor_band_mm below each floor level), column "
       "alignment (share of upper posts with a post below) with the posts standing on beams, and beam spans "
       "between the supports found under or at the ends of each beam with span / depth. Storeys from the "
       "levels named 1FL, 2FL... or 'storeys' [{id, fl}]. A beam is judged only against span_depth_limit "
       "with its limit_basis. suggested_wall_floors repeats the floor figures in set_wall_calculation form. "
       "No loads, joints, sections or compliance. Changes nothing.",
       SCHEMA({"type":"object","properties":{"storeys":{"type":"array","items":{"type":"object"}},
         "cell_mm":{"type":"number"},"wind_cut_mm":{"type":"number"},"floor_band_mm":{"type":"number"},
         "span_depth_limit":{"type":"number"},"limit_basis":{"type":"string"}}}),
       [](doc::Modeler& m,const json& a) {auto result=json::parse(must(construction::structureAssist(m.document(),a.dump())));
         result["revision"]=m.history().revision();return result;}},
      {"generate_frame",
       "Generate a timber post_beam_2storey frame (1 or 2 storeys, gable/shed Japanese roof). "
       "Read get_construction_guide first. inputs: {x_bays:10,y_bays:8,module:910,storeys:2,roof:gable|shed, "
       "ridge_axis:x|y,roof_pitch:4 (sun),roof_angle (degrees instead),origin:[x,y,z],grid (existing id), "
       "second_floor:{x:[0,10],y:[0,8]} (zero-based grid indices),columns:[[i,j],...], "
       "first_floor_system/second_floor_system:joist|joistless,floor_zones:[{storey, x:[i,j],y:[i,j],system}], "
       "floor_openings:[{storey:2,x:[2,4],y:[1,3]}] omits flooring/joists/beams inside grid-aligned apertures "
       "and adds missing boundary beams. Columns stay; support/joints/finished clear size unassessed. "
       "opening_hosts:[id],braces:[{storey,from:[i,j],to:[i,j]}],levels:{first_fl:height_or_id,...}, "
       "interior_sills:[{from:[i,j],to:[i,j]}] (interior grid runs; one sill per bay; a first-floor girder on the same "
       "line gives way there), sills_under_braces:true adds sills under first-storey braces on interior lines, "
       "spacing:{joist/rafter:{mode:equal_division|fixed_pitch,divisions:2,pitch:455}},sections:{role:catalog_id}}. "
       "Shared component parts, six stage groups, levels and hardware (illustrative, products unselected). "
       "preview defaults true: scratch copy, no changes; returns revision. Apply with preview:false and "
       "expected_revision from preview. One undo. replace: a prior frame group; edited/added/deleted parts "
       "require a user decision; on_modified:preserve keeps them and skips their generated counterparts. "
       "No section safety or wall-quantity evaluation.",
       SCHEMA({"type":"object","properties":{"system":{"type":"string","enum":["timber"]},
         "recipe":{"type":"string","enum":["post_beam_2storey"]},"inputs":{"type":"object"},
         "preview":{"type":"boolean"},"expected_revision":{"type":"integer"},"replace":{"type":"string"},
         "on_modified":{"type":"string","enum":["ask","preserve"]},"name":{"type":"string"}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.value("preview", true)) {
           doc::Modeler scratch;scratch.copyFrom(m);
           if (a.contains("expected_revision") && a.at("expected_revision") != m.history().revision())
             throw ApiError("The model changed; preview the frame again");
           json trial=a;trial.erase("expected_revision");
           json result=json::parse(must(scratch.generateFrame(trial.dump())));
           result["preview"]=true;result["revision"]=m.history().revision();
           result["apply"]={{"preview",false},{"expected_revision",m.history().revision()}};
           return result;
         }
         if (!a.contains("expected_revision") && !m.history().inGroup())
           throw ApiError("Preview first and supply expected_revision, or apply inside a revision-checked batch/proposal");
         json result=json::parse(must(m.generateFrame(a.dump())));
         result["preview"]=false;return result;
       }},
      {"take_off",
       "Read-only timber quantities grouped by role, section and design length. Counts placements (not shared definitions or solids). "
       "within scopes a group, tags filters leaf tags, visible_only defaults false. stock_length overrides the reference length. "
       "Unknown or edited member sizes remain not_evaluated/null. Design volume and actual shape volume are separate. "
       "Stock equivalents/lower bounds are not cutting or procurement plans. include_csv returns UTF-8 CSV. Marks match a drawing with identical filters.",
       SCHEMA({"type":"object","properties":{"system":{"type":"string","enum":["timber"]},"within":{"type":"string"},
         "tags":{"type":"array","items":{"type":"string"}},"visible_only":{"type":"boolean"},
         "stock_length":{"type":"number","exclusiveMinimum":0},"include_csv":{"type":"boolean"}}}),
       [](doc::Modeler& m,const json& a){
         json report=json::parse(must(construction::takeOff(m.document(),a.dump())));
         report["revision"]=m.history().revision();
         if(a.value("include_csv",false)) report["csv"]=construction::takeOffCsv(report.dump());
         return report;
       }},
        {"list_wall_bays",
         "Read-only geometric wall-bay candidates, split at existing timber post centres and rectangular openings. "
         "Optional ids selects wall hosts; elevation (world Z mm) limits walls spanning that height. "
         "Returns endpoints, t0/t1, source signatures and start/end post, bottom/top member references, "
         "real-shape joint distances and frame_geometry (touching/gaps/incomplete/ambiguous/unassessed). "
         "split_at_posts:false retains opening-only intervals. Hidden walls are omitted; hidden members are still inspected. "
         "unsupported walls/cuts are reported. Candidate IDs belong only to this snapshot. "
         "Geometric contact does not establish joint capacity or specifications. Never added to wall quantity.",
         SCHEMA({"type":"object","properties":{"ids":{"type":"array","items":{"type":"string"}},
           "elevation":{"type":"number"},"split_at_posts":{"type":"boolean"}}}),
         [](doc::Modeler& m, const json& a) {
           auto result = json::parse(must(construction::wallBayCandidates(m, a.dump())));
           result["revision"] = m.history().revision();
           return result;
         }},
        {"check_wall_quantity",
       "Read-only 1-2-storey timber wall-quantity comparison. Uses saved inputs or optional inputs draft. "
       "Returns X/Y floor rows, individual wall reasons, provenance, pinned table and date notice. "
       "include_csv:true adds UTF-8 BOM CSV. Missing loads, specifications or tables remain not_evaluated. "
       "Not a building safety or code-compliance assessment. Read get_construction_guide first.",
       SCHEMA({"type":"object","properties":{"inputs":{"type":"object"},"include_csv":{"type":"boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         auto report = json::parse(must(construction::checkWallQuantity(m.document(), a.dump())));
         report["revision"] = m.history().revision();
         if (a.value("include_csv", false)) report["csv"] = construction::wallQuantityCsv(report.dump());
         return report;
       }},
      {"set_wall_calculation",
       "Replace timber wall calculation inputs and pin coefficient-table snapshot in project; one undo. "
       "inputs: {height_m,co_designated:bool,wind_coefficient_cm_m2,input_basis,frame?,"
       "floors:[{id,name,area_m2,weight_kn,wind_x_m2,wind_y_m2,walls_complete}],"
       "walls:[{id,floor,direction:X|Y,spec,verified,height_mm,bay_mm?,start:[x,y],end:[x,y]}],table_snapshot?}. "
       "Floors are bottom to top; weight_kn is cumulative burden including upper floors. mm/m2/kN units. "
       "Alternatively wall grid+start_axes/end_axes:[xAxisId,yAxisId] follows grid edits. "
       "verified confirms full construction/connection conditions from guide; not inferred from geometry. "
       "Omitting table_snapshot selects current built-in table; null removes it; custom tables require metadata. "
       "Read check_wall_quantity to see resulting report. Null inputs remain unknown, never zero.",
       SCHEMA({"type":"object","properties":{"inputs":{"type":"object"}},"required":["inputs"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setWallCalculation(arg(a, "inputs").dump()));
         return json::parse(must(construction::checkWallQuantity(m.document())));
       }},
      {"check_frame",
       "Read-only timber diagnosis: COM-01..03, MOD-01..02, TMB-01..04, LAT-* placeholders and SEC-* outside scope. "
       "Returns four outcomes, reason codes, measurements/units, provenance and checker version. "
       "Optional frame scopes the diagnosis to one generated group; rules filters IDs or prefixes with *. "
       "Geometry contact does not establish connection capacity or structural safety. No model changes, no undo.",
       SCHEMA({"type":"object","properties":{"system":{"type":"string","enum":["timber"]},"frame":{"type":"string"},
         "rules":{"type":"array","items":{"type":"string"}},"pair_budget":{"type":"integer","minimum":1,"maximum":20000}}}),
       [](doc::Modeler& m, const json& a) {
         json result=json::parse(must(construction::checkFrame(m.document(),a.dump())));
         result["revision"]=m.history().revision(); return result;
       }},
      {"get_construction_guide",
       "Read the construction guide before choosing sections. Returns data and Markdown generated from the "
       "same embedded tables plus this project's overrides. Timber only; steel/rc guides are not implemented.",
       SCHEMA({"type": "object", "properties": {"system": {"type": "string", "enum": ["timber", "steel", "rc"]}}, "required": ["system"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string system = text(a, "system"), patch = constructionPatch(m, system);
         return json{{"data", json::parse(construction::guideData(system, patch))},
                     {"markdown", construction::guideMarkdown(system, patch)}};
       }},
      {"set_construction_overrides",
       "Replace this project's construction defaults patch (saved in .sbr; one undo). system: timber. "
       "overrides: {} resets defaults, otherwise {source:{name,version,date,url},normative:user, "
       "sections/roles/presets:{entries:{id:{fields}}}}. Merge patch; invalid values change nothing. "
       "Existing members are not resized. Read get_construction_guide to see effective values and share the patch.",
       SCHEMA({"type": "object", "properties": {"system": {"type": "string"}, "overrides": {"type": "object"}}, "required": ["system", "overrides"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string system = text(a, "system");
         must(m.setConstructionOverrides(system, arg(a, "overrides").dump()));
         return json{{"data", json::parse(construction::guideData(system, constructionPatch(m, system)))}};
       }},
      {"create_member",
       "A linear member (beam, column, brace; steel, timber or RC): 'section' (a catalog id from "
       "list_sections, or {kind: rectangle, width, depth}, {kind: circle, diameter} or {kind: h, width, depth, "
       "web, flange, radius}) "
       "swept from 'start' to 'end' ([x, y, z] or a grid crossing \"X1/Y1\" at height 'base', mm), or "
       "'length' along 'direction' (default up: a column). 'anchor' {across: left|center|right, up: "
       "top|center|bottom} is the point of the section on the start-end line (left / right looking from "
       "start to end; e.g. a beam on its top edge: up=top). 'up' [x, y, z]: the section's up hint (default "
       "world up; members along it face +Y); 'roll' (deg) turns it about the member. 'role' (column, beam, "
       "brace, ...) and 'material_class' (steel, timber, rc) are optional attributes kind / material_class. "
       "A plain solid afterwards: every edit works on it. Returns the id and member_info.",
       SCHEMA({"type": "object", "properties": {
         "section": {}, "start": {}, "end": {}, "length": {"type": "number"}, "direction": {"type": "array"},
         "base": {"type": "number"}, "anchor": {"type": "object"}, "up": {"type": "array"},
         "roll": {"type": "number"}, "role": {"type": "string"}, "material_class": {"type": "string"},
         "name": {"type": "string"}},
         "required": ["section", "start"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         const double base = numberOr(a, "base", 0.0);
         doc::Modeler::MemberSpec spec;
         spec.section = sectionFrom(arg(a, "section"), m);
         // [x, y, z] as given; [x, y] and grid crossings at 'base'.
         auto at = [&](const json& v) {
           return v.is_array() && v.size() == 3 ? gp_Pnt(xyz(v, "point")) : grid.point(v, base);
         };
         spec.start = at(arg(a, "start"));
         if (a.contains("end")) {
           spec.end = at(a.at("end"));
         } else if (a.contains("length")) {
           spec.end = spec.start.Translated(gp_Vec(direction(a, "direction", gp::DZ())) * number(a, "length"));
         } else {
           throw ApiError("give 'end', or 'length' (and 'direction')");
         }
         if (a.contains("anchor")) spec.anchor = anchorFrom(a.at("anchor"));
         if (a.contains("up")) spec.up = direction(a, "up", gp::DZ());
         spec.roll = numberOr(a, "roll", 0.0) * M_PI / 180.0;
         spec.role = textOr(a, "role", "");
         // A catalog section brings its material class unless one is given.
         const geom::CatalogSection* entry = spec.section.catalog.empty() ? nullptr : geom::findSection(spec.section.catalog);
         spec.materialClass = textOr(a, "material_class", entry ? entry->material : (spec.section.catalog.rfind("TIMBER-", 0) == 0 ? "timber" : ""));
         spec.name = textOr(a, "name", "");
         const ObjectId id = must(m.createMember(spec));
         return memberInfoJson(must(m.memberInfo(id)));
       }},
      {"set_grid",
       "Edit a grid (the group returned by create_grid as 'grid'; get_object shows its lines): 'x' and "
       "'y' list the lines wanted, in order, each {id (to keep a line; leave out for a new one), name, "
       "position (mm from the grid's origin) or spacing (from the line before)}; lines not listed are "
       "taken away. Lines, labels and dimensions are made again; members pinned to its crossings "
       "(place_members_on_grid) follow, keeping their heights. One undo step. Returns the grid.",
       SCHEMA({"type": "object", "properties": {"grid": {"type": "string"}, "x": {"type": "array"},
               "y": {"type": "array"}, "label_offset": {"type": "number"}}, "required": ["grid"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "grid");
         const doc::GridSpec* current = m.gridSpec(id);
         if (!current) throw ApiError("'" + id + "' is not a grid");
         doc::GridSpec spec = *current;
         auto axes = [&](const char* key, std::vector<doc::GridAxis>& out) {
           if (!a.contains(key)) return;
           const json& list = a.at(key);
           if (!list.is_array()) throw ApiError(std::string("'") + key + "' must be a list of lines");
           out.clear();
           double previous = 0.0;
           for (std::size_t i = 0; i < list.size(); ++i) {
             const json& l = list[i];
             doc::GridAxis axis;
             axis.id = textOr(l, "id", "");
             if (axis.id.empty()) {
               axis.id = "a" + std::to_string(spec.nextId++);
             } else if (!current->find(axis.id)) {
               throw ApiError("no grid line with id '" + axis.id + "'");
             }
             const doc::GridAxis* was = current->find(axis.id);
             axis.name = textOr(l, "name", was ? was->name : "");
             if (l.contains("position")) {
               axis.offset = number(l, "position");
             } else if (l.contains("spacing")) {
               if (i == 0) throw ApiError("the first line needs a 'position'");
               axis.offset = previous + number(l, "spacing");
             } else if (was) {
               axis.offset = was->offset;
             } else {
               throw ApiError("a new line needs a 'position' or 'spacing'");
             }
             previous = axis.offset;
             out.push_back(axis);
           }
         };
         axes("x", spec.x);
         axes("y", spec.y);
         spec.labelOffset = numberOr(a, "label_offset", spec.labelOffset);
         must(m.setGrid(id, spec));
         return json{{"grid", id}, {"lines", gridJson(*m.gridSpec(id))}};
       }},
      {"set_site_north",
       "docs/295: the site's true north (真北) for the plans' north arrow, the site plan, the vicinity map and the "
       "sun: 'angle' (degrees clockwise from the model's +Y, as seen from above) or 'from' and 'to' ([x, y] plan "
       "points, the direction from 'from' towards 'to' is north); 'source' (the document it was read from, "
       "required) and 'checked_on' (YYYY-MM-DD; default today, as the window does). Only what the person gave: never guess north from a "
       "map. One undo step. Returns the angle.",
       SCHEMA({"type": "object", "properties": {"angle": {"type": "number"}, "from": {"type": "array"},
               "to": {"type": "array"}, "source": {"type": "string"}, "checked_on": {"type": "string"}},
               "required": ["source"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string source = text(a, "source");
         std::string checkedOn = textOr(a, "checked_on", "");
         if (checkedOn.empty()) {  // as the window does: the day it is set
           const std::time_t now = std::time(nullptr);
           std::tm local{};
#ifdef _WIN32
           localtime_s(&local, &now);
#else
           localtime_r(&now, &local);
#endif
           char day[16];
           std::strftime(day, sizeof day, "%Y-%m-%d", &local);
           checkedOn = day;
         }
         if (a.contains("from") || a.contains("to")) {
           const auto point = [&](const char* key) {
             const json& p = a.at(key);
             if (!p.is_array() || p.size() < 2) throw ApiError(std::string("'") + key + "' is [x, y]");
             return gp_Pnt(p[0].get<double>(), p[1].get<double>(), 0.0);
           };
           must(m.setSiteNorthFromPoints(point("from"), point("to"), source, checkedOn));
         } else {
           if (!a.contains("angle")) throw ApiError("give 'angle' or 'from' and 'to'");
           must(m.setSiteNorth({number(a, "angle"), source, checkedOn}));
         }
         const auto& north = m.document().settings().siteNorth;
         return json{{"angle", north ? json(north->angle) : json(nullptr)}, {"source", source}};
       }},
      {"set_site_boundary",
       "Assign a closed horizontal outline object to the draft site plan. Provide its source. "
       "A null outline clears the assignment. Saves one undo step; returns the outline and its area in m².",
       SCHEMA({"type": "object", "properties": {"outline": {"type": ["string", "null"]},
               "source": {"type": "string"}}, "required": ["outline"]}),
       [](doc::Modeler& m, const json& a) {
         if (a.at("outline").is_null()) {
           must(m.setSiteBoundary(std::nullopt));
           return json{{"outline", nullptr}};
         }
         const doc::SiteBoundary boundary{text(a, "outline"), text(a, "source")};
         must(m.setSiteBoundary(boundary));
         const auto geometry = must(m.document().siteBoundaryGeometry(boundary.outline));
         return json{{"outline", boundary.outline}, {"area_m2", geometry.areaMm2 / 1e6}};
       }},
      {"set_site_road",
       "Attach road facts to one outer edge of the assigned site boundary. Edge endpoints are model points "
       "in mm; width_m and frontage_m are confirmed measurements in metres and may be omitted. "
       "Pass clear:true to remove the assignment. One undo step.",
       SCHEMA({"type": "object", "properties": {"start": {"type": "array"}, "end": {"type": "array"},
               "kind": {"type": "string"}, "source": {"type": "string"}, "width_m": {"type": "number"},
               "frontage_m": {"type": "number"}, "clear": {"type": "boolean"}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.value("clear", false)) {
           m.clearSiteRoad();
         } else {
           doc::SiteRoad road;
           road.kind = textOr(a, "kind", {});
           road.source = text(a, "source");
           road.widthMm = numberOr(a, "width_m", 0.0) * 1000.0;
           road.frontageMm = numberOr(a, "frontage_m", 0.0) * 1000.0;
           must(m.setSiteRoad({point(a, "start"), point(a, "end")}, std::move(road)));
         }
         return json::object();
       }},
      {"set_site_ground",
       "Save measured heights for a selected building-to-ground perimeter, in one undo step. "
       "Samples are [world X, world Y, height from datum] in mm, in closed perimeter order without repeating "
       "the first point. Pass building (object id) to record its placement. Review the perimeter and measurements before "
       "passing confirm_building_placement:true when the saved association or placement changes. An omitted building keeps the "
       "existing association. Pass clear:true to remove the assignment. 'datum_z' (docs/293): the datum's height in "
       "the model (Z, mm), so that ground_level_table gives the ground level's model height and its difference from "
       "the level named GL; 'legal_status' is ground_level_table's state.",
       SCHEMA({"type": "object", "properties": {"outline": {"type": "string"}, "datum": {"type": "string"},
               "source": {"type": "string"}, "samples": {"type": "array", "items": {"type": "array",
               "minItems": 3, "maxItems": 3, "items": {"type": "number"}}}, "building": {"type": "string"},
               "confirm_building_placement": {"type": "boolean"}, "clear": {"type": "boolean"},
               "datum_z": {"type": ["number", "null"]}}}),
       [](doc::Modeler& m, const json& a) {
         if (a.value("clear", false)) {
           m.clearSiteGround();
           return json::object();
         }
         doc::SiteGround ground{text(a, "outline"), text(a, "datum"), text(a, "source"), {}};
         if (a.contains("datum_z") && !a.at("datum_z").is_null()) ground.datumZ = number(a, "datum_z");  // docs/293
         const auto& samples = a.at("samples");
         if (!samples.is_array()) throw std::runtime_error("samples must be an array");
         for (const auto& row : samples) {
           if (!row.is_array() || row.size() != 3) throw std::runtime_error("Each sample must be [X, Y, height]");
           ground.samples.push_back(row.get<std::array<double, 3>>());
         }
         const auto report = must(m.document().siteGroundReport(ground));
         must(m.setSiteGround(std::move(ground), textOr(a, "building", {}),
                              a.value("confirm_building_placement", false)));
         const auto study = doc::groundLevelStudy(m.document());
         return json{{"perimeter_mm", report.lengthMm}, {"reference_average_mm", report.averageMm()},
                     {"legal_status", study.state}};
       }},
      {"get_site_ground_status",
       "Read the current contact perimeter status: unassigned, unlinked, needs_review, or ready. "
       "A previous reference average is withheld when the outline or associated building needs review.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         const auto& document = m.document();
         const auto& site = document.settings().siteBoundary;
         json result{{"state", "unassigned"}, {"legal_status", "未評価"}};
         if (!site || !site->ground) return result;
         const auto& ground = *site->ground;
         result["outline"] = ground.outline;
         if (ground.placement) result["building"] = ground.placement->building;
         const auto placement = document.checkSiteGroundPlacement(ground);
         const auto report = document.siteGroundReport(ground);
         if (!placement || !report) {
           result["state"] = "needs_review";
           result["reason"] = !placement ? placement.message() : report.error();
           return result;
         }
         result["state"] = ground.placement ? "ready" : "unlinked";
         result["reference_average_mm"] = report->averageMm();
         return result;
       }},
      {"get_sun",
       "The sun of the realistic display (docs/82): month, day, hour (local clock, 14.5 = 14:30), latitude, "
       "longitude (degrees, north / east positive), time_zone (hours from UTC), north_angle (degrees from the "
       "model's +Y to true north, clockwise from above), and where it is: altitude and azimuth (degrees, "
       "azimuth from north clockwise), toward_sun (unit vector, Z up), up (above the horizon).",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) { return sunJson(m.document().settings().sun); }},
      {"set_sun",
       "Set the sun's date, time, place or north (any of month, day, hour, latitude, longitude, time_zone, "
       "north_angle; the rest stay). Saved with the model, one undo step. Returns as get_sun.",
       SCHEMA({"type": "object", "properties": {"month": {"type": "integer"}, "day": {"type": "integer"},
               "hour": {"type": "number"}, "latitude": {"type": "number"}, "longitude": {"type": "number"},
               "time_zone": {"type": "number"}, "north_angle": {"type": "number"}}}),
       [](doc::Modeler& m, const json& a) {
         geom::SunSettings sun = m.document().settings().sun;
         sun.month = a.value("month", sun.month);
         sun.day = a.value("day", sun.day);
         sun.hour = a.value("hour", sun.hour);
         sun.latitude = a.value("latitude", sun.latitude);
         sun.longitude = a.value("longitude", sun.longitude);
         sun.timeZone = a.value("time_zone", sun.timeZone);
         sun.northAngle = a.value("north_angle", sun.northAngle);
         must(m.setSun(sun));
         return sunJson(m.document().settings().sun);
       }},
      {"get_levels", "The levels (GL, 1FL..., docs/45): [{id, name, elevation}] by elevation (world z, mm).",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) { return json{{"levels", levelsJson(m)}}; }},
      {"attach_to_levels",
       "Put walls and slabs ('ids', made by create_wall / create_slab) on levels and keep them there: "
       "'base' (walls: the foot) and / or 'top' (walls: the top; slabs: the top face) name a level by id "
       "or name; null lets that end go. A wall's foot moves to 'base' (the wall keeps its height, or its "
       "top when that is on a level); its top moves to 'top' (the height changes). Afterwards set_levels "
       "moves them with their levels; openings keep their sill heights. Walls and slabs made at a "
       "level's height are attached from the start (attributes base_level / top_level hold the level "
       "ids). Refused as a whole when a wall's top would not be above its foot. One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "base": {"type": ["string", "null"]}, "top": {"type": ["string", "null"]}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         auto end = [&](const char* key) -> std::optional<std::string> {
           if (!a.contains(key)) return std::nullopt;
           if (a.at(key).is_null()) return std::string();
           if (!a.at(key).is_string()) throw ApiError(std::string("'") + key + "' must be a level id, a name or null");
           return a.at(key).get<std::string>();
         };
         must(m.attachToLevels(ids(a, "ids"), end("base"), end("top")));
         json out = json::array();
         for (const ObjectId& id : ids(a, "ids")) {
           const auto& attrs = object(m, id).attributes;
           auto level = [&](const char* key) -> json {
             const auto it = attrs.find(key);
             return it == attrs.end() ? json(nullptr) : json(it->second);
           };
           out.push_back({{"id", id}, {"base_level", level("base_level")}, {"top_level", level("top_level")}});
         }
         return json{{"objects", out}};
       }},
      {"set_levels",
       "Replace the levels: 'levels' [{id (keep a level; leave out for a new one), name, elevation (world z, "
       "mm)}]; levels not listed are taken away. Members placed on a grid with levels (place_members_on_grid "
       "base_level / top_level) and walls / slabs on levels (attach_to_levels) move with their levels. One "
       "undo step. Returns the levels.",
       SCHEMA({"type": "object", "properties": {"levels": {"type": "array"}}, "required": ["levels"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<doc::FloorLevel> levels;
         for (const json& l : arg(a, "levels")) {
           const std::string id = textOr(l, "id", "");
           if (!id.empty() && std::none_of(m.levels().begin(), m.levels().end(),
                                           [&](const doc::FloorLevel& x) { return x.id == id; })) {
             throw ApiError("no level with id '" + id + "'");
           }
           levels.push_back({id, text(l, "name"), number(l, "elevation")});
         }
         must(m.setLevels(levels));
         return json{{"levels", levelsJson(m)}};
       }},
      {"place_members_on_grid",
       "Columns and beams on a grid's crossings, pinned to them (when the grid is edited or moved they "
       "follow; heights kept). 'columns', 'x_beams' (on the x lines, running along the grid's Y) and "
       "'y_beams' each take a member like create_member's {section, anchor, roll, role, material_class} "
       "(role defaults: column, beam; beams default to anchor up=top). 'x' / 'y' limit to these lines "
       "(names or ids; default all). Columns stand from 'base' (mm above the grid, default 0) up "
       "'height' (default 3000); beams lie at base + height. Or 'base_level' and 'top_level' (level names "
       "or ids, see get_levels): columns between them, beams on the top level, and the ends then follow "
       "those levels. What is already pinned the same way is skipped. One undo step; returns the new ids.",
       SCHEMA({"type": "object", "properties": {"grid": {"type": "string"}, "columns": {"type": "object"},
               "x_beams": {"type": "object"}, "y_beams": {"type": "object"}, "x": {"type": "array"},
               "y": {"type": "array"}, "base": {"type": "number"}, "height": {"type": "number"},
               "base_level": {"type": "string"}, "top_level": {"type": "string"}},
               "required": ["grid"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "grid");
         const doc::GridSpec* spec = m.gridSpec(id);
         if (!spec) throw ApiError("'" + id + "' is not a grid");
         doc::Modeler::GridMemberOptions options;
         auto lines = [&](const char* key, const std::vector<doc::GridAxis>& all, std::vector<std::string>& out) {
           if (!a.contains(key)) return;
           for (const json& v : a.at(key)) {
             const std::string name = v.get<std::string>();
             const auto it = std::find_if(all.begin(), all.end(),
                                          [&](const doc::GridAxis& g) { return g.id == name || g.name == name; });
             if (it == all.end()) throw ApiError("no grid line '" + name + "' in '" + key + "'");
             out.push_back(it->id);
           }
         };
         lines("x", spec->x, options.x);
         lines("y", spec->y, options.y);
         options.base = numberOr(a, "base", 0.0);
         options.height = numberOr(a, "height", 3000.0);
         auto level = [&](const char* key) -> std::string {
           if (!a.contains(key)) return {};
           const std::string name = text(a, key);
           for (const doc::FloorLevel& l : m.levels()) {
             if (l.id == name || l.name == name) return l.id;
           }
           throw ApiError("no level '" + name + "' (get_levels)");
         };
         options.baseLevel = level("base_level");
         options.topLevel = level("top_level");
         auto member = [&](const char* key, const std::string& role, int anchorUp) -> std::optional<doc::Modeler::MemberSpec> {
           if (!a.contains(key)) return std::nullopt;
           const json& v = a.at(key);
           doc::Modeler::MemberSpec s;
           s.section = sectionFrom(arg(v, "section"), m);
           s.anchor.up = anchorUp;
           if (v.contains("anchor")) s.anchor = anchorFrom(v.at("anchor"));
           s.roll = numberOr(v, "roll", 0.0) * M_PI / 180.0;
           s.role = textOr(v, "role", role);
           const geom::CatalogSection* entry = s.section.catalog.empty() ? nullptr : geom::findSection(s.section.catalog);
           s.materialClass = textOr(v, "material_class", entry ? entry->material : (s.section.catalog.rfind("TIMBER-", 0) == 0 ? "timber" : ""));
           return s;
         };
         options.columns = member("columns", "column", 0);
         options.xBeams = member("x_beams", "beam", 1);
         options.yBeams = member("y_beams", "beam", 1);
         const auto made = must(m.placeMembersOnGrid(id, options));
         return json{{"columns", made.columns}, {"beams", made.beams}, {"skipped", made.skipped}};
       }},
      {"member_info",
       "What can be edited of a member now: section (current name, catalog id or own dimensions, and the "
       "catalog it was made from), anchor, role, material_class, and fields [{key, meaning, unit, value, "
       "basis, editable, note}] for length (measured along the axis after other edits: basis shape), "
       "width, depth (H: web, flange, radius) and roll. Values are what it is now, never the original "
       "sizes after edits. Walls (create_wall) give length, thickness, height; slabs (create_slab) "
       "thickness and top. Other objects: an error (their shape stays editable).",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}),
       [](doc::Modeler& m, const json& a) { return memberInfoJson(must(m.memberInfo(text(a, "id")))); }},
      {"change_member",
       "Basic member edits in one step, checked together: 'section' (a whole new section as in create_member: "
       "a catalog id or {kind: rectangle|circle|h, ...}; axis, anchor and roll kept), 'catalog' (another catalog section; anchor, "
       "rotation and length kept), own 'width' / 'depth' (H: 'web', 'flange', 'radius'; the section "
       "becomes its own; catalog sections change size by 'catalog' only), 'diameter' (round), 'length' "
       "(absolute, mm; the start stays, a column's foot), 'roll' (deg), "
       "'anchor' {across, up} (the member stays put; only its axis line moves). Edits made to it since are "
       "redone; if they cannot be, nothing changes and the reason is returned. Same id; returns "
       "member_info. Role: set_attribute kind. Walls: 'thickness' (keep: left | right | center line, "
       "default the line it was drawn on), 'height' (foot kept), 'length'. Slabs: 'thickness' (keep: top "
       "(default) | bottom | center).",
       SCHEMA({"type": "object", "properties": {
         "id": {"type": "string"}, "section": {}, "catalog": {"type": "string"}, "width": {"type": "number"},
         "depth": {"type": "number"}, "web": {"type": "number"}, "flange": {"type": "number"},
         "radius": {"type": "number"}, "length": {"type": "number"}, "roll": {"type": "number"},
         "diameter": {"type": "number"}, "thickness": {"type": "number"}, "height": {"type": "number"},
         "keep": {"type": "string"}, "anchor": {"type": "object"}},
         "required": ["id"]}),
       [](doc::Modeler& m, const json& a) {
         doc::Modeler::MemberChange change;
         if (a.contains("section")) change.section = sectionFrom(a.at("section"), m);
         if (a.contains("catalog")) {
           const std::string id = text(a, "catalog");
           if (id.rfind("TIMBER-", 0) == 0) change.section = sectionFrom(a.at("catalog"), m);
           else change.catalog = id;
         }
         auto opt = [&](const char* key) -> std::optional<double> {
           return a.contains(key) ? std::optional<double>(number(a, key)) : std::nullopt;
         };
         change.width = opt("width");
         change.depth = opt("depth");
         change.web = opt("web");
         change.flange = opt("flange");
         change.radius = opt("radius");
         change.length = opt("length");
         change.diameter = opt("diameter");
         change.thickness = opt("thickness");
         change.height = opt("height");
         if (a.contains("keep")) change.keep = text(a, "keep");
         if (a.contains("roll")) change.roll = number(a, "roll") * M_PI / 180.0;
         if (a.contains("anchor")) change.anchor = anchorFrom(a.at("anchor"));
         return memberInfoJson(must(m.changeMember(text(a, "id"), change)));
       }},
      {"place_openings",
       "Cut 'count' equal openings ('width' x 'height', lower edge 'sill' above the face's bottom, mm) "
       "through a wall 'host', on its face facing 'face_normal' (horizontal, e.g. [0, -1, 0]), evenly "
       "spread along it or 'spacing' apart centre to centre around its middle; or a single one centred on "
       "the point 'at' ([x, y, z], its position along the face; count is then ignored). Same as drawing "
       "them on the face and pushing them through; one undo step. Returns the wall's id (the wall stays "
       "the same object; each new opening gets an id, see list_openings). 'fitting' (optional, as set_opening_fittings) gives the new openings their door / "
       "window symbol in the same step.",
       SCHEMA({"type": "object", "properties": {
         "host": {"type": "string"}, "face_normal": {"type": "array", "items": {"type": "number"}},
         "count": {"type": "integer"}, "width": {"type": "number"}, "height": {"type": "number"},
         "sill": {"type": "number"}, "spacing": {"type": "number"},
         "at": {"type": "array", "items": {"type": "number"}}, "fitting": {"type": "object"}},
         "required": ["host", "face_normal", "width", "height"]}),
       [](doc::Modeler& m, const json& a) {
         // A door / window fitting for the new openings (as set_opening_fittings), same step.
         const std::string fitting = a.contains("fitting") ? a.at("fitting").dump() : R"({"type":"none"})";
         if (a.contains("fitting") && !a.at("fitting").is_object()) throw ApiError("'fitting' must be an object");
         // Walls whose end meets the host where an opening is (docs/118): said, not stopped.
         auto said = [&](json result) {
           json meeting = json::array();
           for (const auto& [index, wall] : m.wallsMeetingOpenings(text(a, "host")))
             meeting.push_back({{"opening", index}, {"wall", wall}});
           if (!meeting.empty()) result["walls_meeting_openings"] = meeting;
           return result;
         };
         if (a.contains("at")) {
           return said(idResult(must(m.placeOpeningAt(text(a, "host"), direction(a, "face_normal", gp::DY()),
                                                       point(a, "at"), number(a, "width"), number(a, "height"),
                                                       numberOr(a, "sill", 0.0), fitting))));
         }
         const json& count = arg(a, "count");
         if (!count.is_number_integer()) throw ApiError("'count' must be an integer");
         return said(idResult(must(m.placeOpenings(text(a, "host"), direction(a, "face_normal", gp::DY()),
                                                    count.get<int>(), number(a, "width"), number(a, "height"),
                                                    numberOr(a, "sill", 0.0),
                                                    a.contains("spacing") ? std::optional<double>(number(a, "spacing"))
                                                                          : std::nullopt,
                                                    fitting))));
       }},
      {"place_on_grid",
       "Copy 'source' (e.g. a column) to crossings of grid lines (from create_grid): 'x' and 'y' are line "
       "names like [\"X1\", \"X3\"] or \"all\" (default); 'skip' lists crossings like \"X2/Y1\". The "
       "bottom centre of the source lands on each crossing (its height is kept). One undo step; returns "
       "the new ids by crossing (the source itself where it already stands on one; null where the same thing stands there already). Lines with the same name twice are refused as ambiguous.",
       SCHEMA({"type": "object", "properties": {"source": {"type": "string"}, "x": {}, "y": {}, "skip": {}},
               "required": ["source"]}),
       [](doc::Modeler& m, const json& a) { return runPlaceOnGrid(m, a); }},
      {"query_objects",
       "Find objects without reading the whole model: filters (all optional, combined with AND) kind, tag, "
       "ids (string or list), name (substring), parent (direct parent id, \"\" = top level), within "
       "(any ancestor group), definition (component instances of it), visible, region {min, max} (bounds "
       "overlap), attributes {key: value, or \"*\" for any value}, material (what it shows as a whole: id or name). 'fields' picks what to return (default id, name, kind, tag; also visible, parent, color, material, "
       "bounds, volume, area, definition, history, parameters, attributes, text, or all). Paged: limit (default 200) "
       "and next_cursor -> cursor. 'sequence' is for get_changes.",
       SCHEMA({"type": "object", "properties": {
         "kind": {}, "tag": {}, "ids": {}, "name": {"type": "string"}, "parent": {"type": "string"},
         "within": {"type": "string"}, "definition": {"type": "string"}, "visible": {"type": "boolean"}, "material": {"type": "string"},
         "region": {"type": "object"}, "attributes": {"type": "object"}, "fields": {}, "limit": {"type": "integer"},
         "cursor": {"type": "string"}}}),
       [](doc::Modeler& m, const json& a) { return queryObjects(m, a); }},
      {"aggregate",
       "Count objects and sum volumes of solids and component placements (mm3) and face areas (mm2), with the same filters as "
       "query_objects, optionally per group_by: kind, tag, definition, name, parent, material or attribute:<key>.",
       SCHEMA({"type": "object", "properties": {
         "kind": {}, "tag": {}, "ids": {}, "name": {"type": "string"}, "parent": {"type": "string"},
         "within": {"type": "string"}, "definition": {"type": "string"}, "visible": {"type": "boolean"}, "material": {"type": "string"},
         "region": {"type": "object"}, "attributes": {"type": "object"}, "group_by": {"type": "string"}}}),
       [](doc::Modeler& m, const json& a) { return aggregateObjects(m, a); }},
      {"get_changes",
       "Objects changed or removed since a 'sequence' (from query_objects or an earlier get_changes), so "
       "an agent can keep its picture of a large model current. full: true means re-query.",
       SCHEMA({"type": "object", "properties": {"since": {"type": "integer"}}, "required": ["since"]}),
       [](doc::Modeler& m, const json& a) { return changesSince(m, a); }},
      {"list_tags", "List tags (layers) in use and whether each is visible.",
       SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) {
         json list = json::array();
         for (const std::string& t : m.tags()) {
           list.push_back({{"tag", t}, {"visible", !m.document().settings().hiddenTags.count(t)}});
         }
         return json{{"tags", list}};
       }},
      {"set_tag_visible", "Show or hide every object with a tag (e.g. hide 'Annotations').",
       SCHEMA({"type": "object", "properties": {"tag": {"type": "string"}, "visible": {"type": "boolean"}},
               "required": ["tag", "visible"]}),
       [](doc::Modeler& m, const json& a) {
         const json& v = arg(a, "visible");
         if (!v.is_boolean()) throw ApiError("'visible' must be true or false");
         m.setTagVisible(text(a, "tag"), v.get<bool>());
         return json::object();
       }},

      // ---- history & files
      {"undo", "Undo the last step.", SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) { return json{{"undone", m.undo()}}; }},
      {"redo", "Redo the last undone step.", SCHEMA({"type": "object", "properties": {}}),
       [](doc::Modeler& m, const json&) { return json{{"redone", m.redo()}}; }},
      {"save", "Save the project (.sbr).",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.save(text(a, "path")));
         return json::object();
       }},
      {"save_copy",
       "Save a copy of the model as a new .sbr file at 'path' (full path, the folder must exist). Never "
       "replaces a file that is there (pick another name); the user's own file and its saved state are not "
       "touched. In the app this is allowed by the menu AI 連携 > AI に別名保存を許可.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.saveCopy(text(a, "path")));
         return json{{"path", text(a, "path")}};
       }},
      {"open", "Open a project (.sbr), replacing the current model.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.load(text(a, "path")));
         return json{{"objects", m.document().objects().size()}};
       }},
      {"export_3dm", "Export visible objects (or 'ids') to a Rhino .3dm file: exact NURBS, names, colours, tags as layers.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "ids": {"type": "array", "items": {"type": "string"}}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.export3dm(text(a, "path"), a.contains("ids") ? ids(a, "ids") : std::vector<ObjectId>{}));
         return json::object();
       }},
      {"import_jww", "Import a Jw_cad drawing (.jww): lines, arcs, circles and ellipses at real size (each layer "
       "group's scale), block instances expanded, dimension lines kept; text, points and fills are not read "
       "(allow_partial to import anyway). Layers become tags 'Jw <group>/<layer>'.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}, "allow_partial": {"type": "boolean"},
               "group_by_layer": {"type": "boolean"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::JwwImportOptions options;
         options.allowPartial = a.value("allow_partial", false);
         options.groupByLayer = a.value("group_by_layer", false);
         geom::JwwImportReport report;
         json out = idsResult(must(m.importJww(text(a, "path"), options, &report)));
         out["report"] = {{"imported", report.imported}, {"omitted", report.omitted}, {"version", report.version}};
         return out;
       }},
      {"import_dxf", "Import ASCII DXF model-space curves as exact editable edges/wires; layers become tags. "
       "Unsupported entities require allow_partial. unitless_scale_to_mm applies only when INSUNITS is absent/zero.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "allow_partial": {"type": "boolean"}, "group_by_layer": {"type": "boolean"}, "unitless_scale_to_mm": {"type": "number", "exclusiveMinimum": 0}},
               "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::DxfImportOptions options;
         options.allowPartial = a.value("allow_partial", false);
         options.groupByLayer = a.value("group_by_layer", false);
         options.unitlessScaleToMm = a.value("unitless_scale_to_mm", 1.0);
         geom::DxfImportReport report;
         json out = idsResult(must(m.importDxf(text(a, "path"), options, &report)));
         out["report"] = {{"imported", report.imported}, {"omitted", report.omitted},
                          {"paper_space", report.paperSpace}, {"units_known", report.unitsKnown},
                          {"scale_to_mm", report.scaleToMm}, {"complete", !report.incomplete()}};
         return out;
       }},
      {"import_3dm",
       "Import the surfaces and solids of a Rhino .3dm file (units converted to mm). A file that does not "
       "come through whole (meshes, points, curves, failed or partial conversions) is refused with what was "
       "found, unless 'allow_partial'; the result's 'report' counts everything either way.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}, "allow_partial": {"type": "boolean"}},
               "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::Import3dmReport report;
         json out = idsResult(must(m.import3dm(text(a, "path"), a.value("allow_partial", false), &report)));
         out["report"] = {{"objects", report.objects}, {"imported", report.imported}, {"failed", report.failed},
                          {"partial", report.partial}, {"skipped_faces", report.skippedFaces},
                          {"skipped_trims", report.skippedTrims}, {"unsupported", report.unsupported},
                          {"units", report.units}, {"scale_to_mm", report.scaleToMm},
                          {"complete", !report.incomplete()}, {"summary", report.summary()}};
         return out;
       }},
      {"import_brep", "Import OCCT text BRep geometry, assuming mm. Project history and materials are not imported.",
       SCHEMA({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}),
       [](doc::Modeler& m,const json& a){return idsResult(must(m.importBrep(text(a,"path"))));}},
      {"import_iges", "Import supported visible IGES geometry, normalizing units to mm. Reject incomplete root transfers.",
       SCHEMA({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}),
       [](doc::Modeler& m,const json& a){return idsResult(must(m.importIges(text(a,"path"))));}},
      {"export_brep", "Export selected or visible geometry as OCCT text BRep in model mm coordinates, without history or materials.",
       SCHEMA({"type":"object","properties":{"path":{"type":"string"},"ids":{"type":"array","items":{"type":"string"}}},"required":["path"]}),
       [](doc::Modeler& m,const json& a){must(m.exportBrep(text(a,"path"),a.contains("ids")?ids(a,"ids"):std::vector<ObjectId>{}));return json::object();}},
      {"export_iges", "Export selected or visible exact geometry as IGES B-rep in mm, without history or materials.",
       SCHEMA({"type":"object","properties":{"path":{"type":"string"},"ids":{"type":"array","items":{"type":"string"}}},"required":["path"]}),
       [](doc::Modeler& m,const json& a){must(m.exportIges(text(a,"path"),a.contains("ids")?ids(a,"ids"):std::vector<ObjectId>{}));return json::object();}},
      {"export_stl", "Export visible face/solid geometry (or ids) as binary STL in mm. Curves and materials are omitted; surfaces remain open. The canonical B-rep is unchanged.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "ids": {"type": "array", "items": {"type": "string"}},
               "linear_deflection_mm": {"type": "number", "minimum": 0.001}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::StlExportOptions options; options.linearDeflection=a.value("linear_deflection_mm",0.1);
         must(m.exportStl(text(a,"path"),a.contains("ids") ? ids(a,"ids") : std::vector<ObjectId>{},options));
         return json::object();
       }},
      {"export_dae",
       "Export visible objects (or 'ids') to COLLADA (.dae) for SketchUp Pro (File > Import there): meshes in mm, "
       "Z up, each object's colours and faces painted on their own; loose lines as lines. 'deflection_mm' (default "
       "1) and 'angle_deg' (default 15): how finely curved faces are cut.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "ids": {"type": "array", "items": {"type": "string"}}, "deflection_mm": {"type": "number"},
               "angle_deg": {"type": "number"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::ColladaExportOptions options;
         options.linearDeflection = numberOr(a, "deflection_mm", 1.0);
         options.angularDeflection = numberOr(a, "angle_deg", 15.0) * M_PI / 180.0;
         must(m.exportCollada(text(a, "path"), a.contains("ids") ? ids(a, "ids") : std::vector<ObjectId>{}, options));
         return json::object();
       }},
      {"export_skp",
       "Export visible objects (or 'ids') to a SketchUp file (.skp, docs/181) through the SketchUp SDK on this PC "
       "(fails, saying so, without it): each object a group named as it is, flat faces whole with their holes, "
       "curved faces as soft triangles, colours as materials, loose lines as edges; SketchUp 2021 format and later. "
       "'deflection_mm' (default 1) and 'angle_deg' (default 15): how finely curved faces are cut.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "ids": {"type": "array", "items": {"type": "string"}}, "deflection_mm": {"type": "number"},
               "angle_deg": {"type": "number"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::SkpExportOptions options;
         options.linearDeflection = numberOr(a, "deflection_mm", 1.0);
         options.angularDeflection = numberOr(a, "angle_deg", 15.0) * M_PI / 180.0;
         must(m.exportSkp(text(a, "path"), a.contains("ids") ? ids(a, "ids") : std::vector<ObjectId>{}, options));
         return json{{"sdk_dll", geom::skpLibrary()}};
       }},
      {"export_gltf",
       "Export visible objects (or 'ids') to glTF 2.0 for renderers (Blender, Twinmotion, Unreal Engine): "
       "'.glb' (one binary file) or '.gltf' (text and .bin). Metres, Y up as glTF wants; each object's material "
       "colour, opacity, metallic and roughness (physically based); lines and guides left out. 'deflection_mm' "
       "(default 1) and 'angle_deg' (default 15): how finely curved faces are cut. docs/85.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "ids": {"type": "array", "items": {"type": "string"}}, "deflection_mm": {"type": "number"},
               "angle_deg": {"type": "number"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::GltfExportOptions options;
         options.linearDeflection = numberOr(a, "deflection_mm", 1.0);
         options.angularDeflection = numberOr(a, "angle_deg", 15.0) * M_PI / 180.0;
         must(m.exportGltf(text(a, "path"), a.contains("ids") ? ids(a, "ids") : std::vector<ObjectId>{}, options));
         return json::object();
       }},
      {"export_drawing",
       "2D drawing (docs/50) as DXF or JWW (Jw_cad), by the path's extension. 'kind': 'plan' (cut at "
       "'cut_z' mm, default 1200, seen from above), 'section' ('point' [x,y,z] on the cut and 'look' "
       "[x,y,z] towards the viewer, horizontal), 'elevation' ('side': 0 south, 1 east, 2 north, 3 west) or "
       "'site' (draft site plan using the outline from set_site_boundary). "
       "'scale' (denominator, default 100), 'paper' (auto, A4-A1), 'frame' (default true), 'title', "
       "'project', 'number', 'author', 'date', 'hidden' (hidden lines), 'fill', 'grid', 'levels', "
       "'annotations', 'hatch' (default true: the cut layers of walls of a build-up and floors of a type with layers "
       "hatched for the scale - structural timber crossed, plywood single lines; docs/173). kind:framing with "
       "preset:foundation|first_floor|second_floor|roof makes a tag-filtered framing plan. "
       "within scopes a group; tags filters leaf tags, include_hidden includes hidden objects, member_labels adds section marks. "
       "'tables' (docs/130): any of openings, rooms, areas, finishes, types, layers, daylight (docs/207), wall_quantity, bracing_walls, quarters (docs/208), put beside the drawing on the same sheet (to its "
       "right, or under it when that takes a smaller paper; empty tables are left out). "
       "'bracing_floor' (docs/208): a plan of that storey's bracing walls. 'insulation' true (docs/216): a plan with "
       "the walls' insulation marked (a zigzag along each wall given insulation, broken at openings) and a legend "
       "(best with 'fill' and 'hatch' false); with kind section (docs/217): the walls', floors' and roofs' insulation "
       "marked in the cut (up a wall's middle, along a floor or roof at half its depth). "
       "Returns the paper chosen and notes (what did not fit or was left out).",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}, "kind": {"type": "string"},
               "cut_z": {"type": "number"}, "point": {"type": "array"}, "look": {"type": "array"}, "hatch": {"type": "boolean"},
               "side": {"type": "integer"}, "scale": {"type": "number"}, "paper": {"type": "string"},
               "frame": {"type": "boolean"}, "title": {"type": "string"}, "project": {"type": "string"},
               "number": {"type": "string"}, "author": {"type": "string"}, "date": {"type": "string"},
               "hidden": {"type": "boolean"}, "fill": {"type": "boolean"}, "grid": {"type": "boolean"},
               "levels": {"type": "boolean"}, "annotations": {"type": "boolean"}, "preset":{"type":"string"},"within":{"type":"string"},
               "tags":{"type":"array","items":{"type":"string"}},"include_hidden":{"type":"boolean"},"member_labels":{"type":"boolean"},
               "tables": {"type": "array", "items": {"enum": ["openings", "rooms", "areas", "finishes", "types", "layers", "daylight",
                                                               "wall_quantity", "bracing_walls", "quarters", "summary", "energy", "bracing_specs", "existing_walls"]}},
               "bracing_floor": {"type": "string"}, "insulation": {"type": "boolean"}}, "required": ["path", "kind"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string kind = text(a, "kind");
         draw::ViewSpec spec;
         if (kind == "plan") {
           spec = draw::planView(numberOr(a, "cut_z", 1200.0));
         } else if (kind == "site") {
           spec = draw::sitePlanView();
         } else if (kind == "section") {
           spec = draw::sectionView(point(a, "point"), direction(a, "look", gp_Dir(0, -1, 0)));
         } else if (kind == "elevation") {
           spec = draw::elevationView(m.document(), a.value("side", 0));
         } else if(kind=="framing") {
           spec=must(draw::framingView(m.document(),text(a,"preset"),textOr(a,"within",{}),a.value("include_hidden",false),a.value("tags",std::vector<std::string>{})));
         } else {
           throw std::runtime_error("kind must be plan, section, elevation, framing or site");
         }
         spec.within=textOr(a,"within",{});
         if(a.contains("tags") && kind!="framing") spec.tags=a.at("tags").get<std::vector<std::string>>();
         spec.includeHidden=a.value("include_hidden",false);
         spec.memberLabels=a.value("member_labels",kind=="framing");
         spec.title = textOr(a, "title", spec.title);
         spec.hidden = a.value("hidden", spec.hidden);
         spec.fill = a.value("fill", spec.fill);
         spec.grid = a.value("grid", spec.grid);
         spec.levels = a.value("levels", spec.levels);
         spec.annotations = a.value("annotations", spec.annotations);
         spec.bracingFloor = textOr(a, "bracing_floor", "");  // docs/208: a plan of that storey's bracing walls
         spec.insulation = a.value("insulation", false);        // docs/216: the insulation marks and legend
         if (a.value("hatch", true)) spec.hatchScale = numberOr(a, "scale", 100.0);  // docs/173
         spec.paperScale = numberOr(a, "scale", 100.0);  // docs/219: the insulation marks for this scale
         const draw::View view = must(draw::makeView(m.document(), spec));
         draw::SheetSpec sheetSpec;
         sheetSpec.scale = numberOr(a, "scale", 100.0);
         sheetSpec.paper = textOr(a, "paper", "auto");
         sheetSpec.frame = a.value("frame", true);
         sheetSpec.title = {textOr(a, "project", {}), spec.title, textOr(a, "number", {}), textOr(a, "author", {}),
                            textOr(a, "date", {})};
         std::vector<draw::Table> tables;
         json leftOut = json::array();
         if (a.contains("tables")) {
           if (!a.at("tables").is_array()) throw ApiError("'tables': a list of openings, rooms, areas, finishes");
           for (const json& t : a.at("tables")) {
             const std::string which = t.is_string() ? t.get<std::string>() : std::string();
             const int index = which == "openings" ? 0 : which == "rooms" ? 1 : which == "areas" ? 2 : which == "finishes" ? 3 : which == "types" ? 4 : which == "layers" ? 5 : which == "daylight" ? 6 : which == "wall_quantity" ? 7 : which == "bracing_walls" ? 8 : which == "quarters" ? 9 : which == "summary" ? 10 : which == "energy" ? 11 : which == "bracing_specs" ? 12 : which == "existing_walls" ? 13 : -1;
             if (index < 0) throw ApiError("'tables': openings, rooms, areas, finishes, types, layers, daylight, wall_quantity, bracing_walls, quarters, summary, energy, bracing_specs or existing_walls");
             draw::Table table = doc::scheduleTable(m.document(), index);
             if (!doc::tableHasContent(table, index)) leftOut.push_back(which);
             else tables.push_back(std::move(table));
           }
         }
         const draw::Sheet sheet = must(draw::layoutSheetWithTables(view, tables, sheetSpec));
         const std::string path = text(a, "path");
         const std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : std::string();
         if (ext == ".jww" || ext == ".JWW") must(draw::writeJww(sheet, path));
         else if (ext == ".dxf" || ext == ".DXF") must(draw::writeDxf(sheet, path));
         else if (path.size() > 7 && (path.compare(path.size() - 7, 7, ".layout") == 0 || path.compare(path.size() - 7, 7, ".LAYOUT") == 0))
           must(draw::writeLayout({sheet}, {sheet.title}, path));  // docs/185
         else throw std::runtime_error("The path must end in .dxf, .jww or .layout (PDF: from the app's File menu)");
         json notes = json::array();
         for (const json& w : leftOut) notes.push_back("The " + w.get<std::string>() + " table is empty: left out");
         for (const std::string& n : view.notes) notes.push_back(n);
         for (const std::string& n : sheet.notes) notes.push_back(n);
         return json{{"paper", sheet.paper}, {"scale", sheet.scale}, {"objects", view.objects}, {"tables", tables.size()}, {"notes", notes}};
       }},
      {"export_schedule",
       "A table on a sheet of its own as DXF or JWW (by the path's extension): 'table' openings (建具表, one "
       "line per mark), rooms (室の表), areas (面積の表), finishes (仕上げ表, set_room_finish) or types (種類の表 of floors and roofs, "
       "set_type_name), layers (層の数量), daylight (採光・換気計算表, docs/207), wall_quantity (壁量計算表), bracing_walls (耐力壁の一覧) quarters (四分割, docs/208) , summary (計画概要, docs/209) or energy (省エネ仕様の表, docs/210), read from the model as it is (as opening_schedule / "
       "room_schedule / area_table). 'paper' (auto, A4-A1), 'frame' (default true), 'project', 'number', "
       "'author', 'date' for the title block.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}, "table": {"enum": ["openings", "rooms", "areas", "finishes", "types", "layers", "daylight", "wall_quantity", "bracing_walls", "quarters", "summary", "energy", "bracing_specs", "existing_walls"]},
               "paper": {"type": "string"}, "frame": {"type": "boolean"}, "project": {"type": "string"},
               "number": {"type": "string"}, "author": {"type": "string"}, "date": {"type": "string"}},
               "required": ["path", "table"]}),
       [](doc::Modeler& m, const json& a) {
         const std::string which = text(a, "table");
         const int index = which == "openings" ? 0 : which == "rooms" ? 1 : which == "areas" ? 2 : which == "finishes" ? 3 : which == "types" ? 4 : which == "layers" ? 5 : which == "daylight" ? 6 : which == "wall_quantity" ? 7 : which == "bracing_walls" ? 8 : which == "quarters" ? 9 : which == "summary" ? 10 : which == "energy" ? 11 : which == "bracing_specs" ? 12 : which == "existing_walls" ? 13 : -1;
         if (index < 0) throw ApiError("'table': openings, rooms, areas, finishes, types, layers, daylight, wall_quantity, bracing_walls, quarters, summary, energy, bracing_specs or existing_walls");
         const draw::Table table = doc::scheduleTable(m.document(), index);
         draw::SheetSpec sheetSpec;
         sheetSpec.paper = textOr(a, "paper", "auto");
         sheetSpec.frame = a.value("frame", true);
         sheetSpec.title = {textOr(a, "project", {}), table.title, textOr(a, "number", {}), textOr(a, "author", {}),
                            textOr(a, "date", {})};
         const draw::Sheet sheet = must(draw::tableSheet(table, sheetSpec));
         const std::string path = text(a, "path");
         const std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : std::string();
         if (ext == ".jww" || ext == ".JWW") must(draw::writeJww(sheet, path));
         else if (ext == ".dxf" || ext == ".DXF") must(draw::writeDxf(sheet, path));
         else throw ApiError("The path must end in .dxf or .jww (PDF: from the tables window in the app)");
         return json{{"paper", sheet.paper}, {"rows", table.rows.size()}, {"notes", sheet.notes}};
       }},
      {"check_drawing_set",
       "docs/247: the drawing set looked over for what makes it hard to read, per sheet (number, name): words on "
       "words (text_on_text), a line through words (line_through_text; 'other' is the kind of line), words beyond "
       "the paper (off_paper), words under 1.8 mm (tiny_text); each with the words and where on the paper (mm from "
       "the lower left). Estimates to look at, not a judgement. 'sheet': only that one (number such as A-15, "
       "1-based index, or name); 'limit' per sheet (default 50). The vicinity map's picture is not fetched here.",
       SCHEMA({"type": "object", "properties": {"sheet": {"type": ["string", "integer"]}, "limit": {"type": "integer"},
               "scale": {"type": "number"}}}),
       [](doc::Modeler& m, const json& a) {
         doc::DrawingSetSpec setSpec;
         setSpec.scale = numberOr(a, "scale", 100.0);  // docs/250
         const auto set = doc::drawingSet(m.document(), setSpec);
         if (!set) throw ApiError(set.error());
         std::string wanted;
         if (a.contains("sheet")) wanted = a["sheet"].is_number() ? std::to_string(a["sheet"].get<int>()) : a["sheet"].get<std::string>();
         const std::size_t limit = static_cast<std::size_t>(std::max(1, a.value("limit", 50)));
         json sheets = json::array();
         std::size_t total = 0;
         for (std::size_t i = 0; i < set->sheets.size(); ++i) {
           if (!wanted.empty() && wanted != set->numbers[i] && wanted != std::to_string(i + 1) && wanted != set->names[i]) continue;
           json findings = json::array(), counts = json::object();
           for (const draw::SheetFinding& f : draw::checkSheet(set->sheets[i], limit)) {
             findings.push_back({{"kind", f.kind}, {"text", f.text}, {"other", f.other},
                                 {"at", {std::round(f.at.X()), std::round(f.at.Y())}}, {"height", f.height}});
             counts[f.kind] = counts.value(f.kind, 0) + 1;
             ++total;
           }
           sheets.push_back({{"index", i + 1}, {"number", set->numbers[i]}, {"name", set->names[i]}, {"counts", counts},
                             {"findings", findings}});
         }
         if (!wanted.empty() && sheets.empty()) throw ApiError("no sheet '" + wanted + "' in the set");
         return json{{"sheets", sheets}, {"total", total}};
       }},
      {"export_drawing_set",
       "A set of drawings with sheet numbers (docs/125) as DXF or JWW files in 'folder' (which must exist): a "
       "sheet list (01), a plan per storey, the four elevations and the tables, numbered 'prefix' (default A-) "
       "01, 02, ...; each file named by its number and drawing name. 'format' dxf (default) or jww; 'scale' "
       "(default 100), 'paper' (auto), 'project', 'author', 'date'. 'tables_with_plan' true (docs/131): the tables "
       "go beside the lowest storey's plan on its sheet instead of sheets of their own. 'combine_views' true (docs/306): the "
       "storeys' plans on one sheet and the elevations and sections on another, side by side. 'sections' (default true, docs/132): "
       "sections after the plans, one per active vertical section plane (set_section) or else A-A (cut east-west, seen "
       "from the south) and B-B (cut north-south, seen from the east) through the middle; the plans mark where they are "
       "cut, with the sections' sheet numbers. Files that are there are not replaced: "
       "refused, nothing written. (One PDF of all the sheets: from the app's File menu.) 'format' layout (docs/185): "
       "one LayOut document '<prefix>drawings.layout' in 'folder', a page per sheet named by its number and name "
       "(needs the LayOut part of the SketchUp SDK).",
       SCHEMA({"type": "object", "properties": {"folder": {"type": "string"}, "format": {"enum": ["dxf", "jww", "layout"]},
               "scale": {"type": "number"}, "paper": {"type": "string"}, "prefix": {"type": "string"},
               "project": {"type": "string"}, "author": {"type": "string"}, "date": {"type": "string"},
               "tables_with_plan": {"type": "boolean"}, "combine_views": {"type": "boolean"}, "sections": {"type": "boolean"}},
               "required": ["folder"]}),
       [](doc::Modeler& m, const json& a) {
         doc::DrawingSetSpec spec;
         spec.scale = numberOr(a, "scale", 100.0);
         spec.paper = textOr(a, "paper", "auto");
         spec.prefix = textOr(a, "prefix", "A-");
         spec.title.project = textOr(a, "project", {});
         spec.title.author = textOr(a, "author", {});
         spec.title.date = textOr(a, "date", {});
         spec.tablesWithPlan = a.value("tables_with_plan", false);
         spec.combineViews = a.value("combine_views", false);
         spec.sections = a.value("sections", true);
         const std::string format = textOr(a, "format", "dxf");
         if (format != "dxf" && format != "jww" && format != "layout") throw ApiError("'format': dxf, jww or layout");
         const auto set = must(doc::drawingSet(m.document(), spec));
         const std::filesystem::path folder = std::filesystem::u8path(text(a, "folder"));
         std::error_code error;
         if (!std::filesystem::is_directory(folder, error)) throw ApiError("The folder does not exist");
         if (format == "layout") {
           const std::filesystem::path file = folder / std::filesystem::u8path(spec.prefix + "drawings.layout");
           if (std::filesystem::exists(file, error)) throw ApiError("'" + file.filename().u8string() + "' is there already (nothing was written)");
           std::vector<std::string> names;
           json pages = json::array();
           for (std::size_t i = 0; i < set.sheets.size(); ++i) {
             names.push_back(set.numbers[i] + " " + set.names[i]);
             pages.push_back({{"number", set.numbers[i]}, {"name", set.names[i]}, {"paper", set.sheets[i].paper}});
           }
           must(draw::writeLayout(set.sheets, names, file.u8string()));
           return json{{"path", file.u8string()}, {"pages", pages}, {"notes", set.notes}};
         }
         std::vector<std::filesystem::path> paths;
         for (std::size_t i = 0; i < set.sheets.size(); ++i) {
           std::string name = set.numbers[i] + "_" + set.names[i] + "." + format;
           for (char& c : name)
             if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
           paths.push_back(folder / std::filesystem::u8path(name));
           if (std::filesystem::exists(paths.back(), error)) throw ApiError("'" + paths.back().filename().u8string() + "' is there already (nothing was written)");
         }
         json files = json::array();
         for (std::size_t i = 0; i < set.sheets.size(); ++i) {
           const std::string path = paths[i].u8string();
           must(format == "jww" ? draw::writeJww(set.sheets[i], path) : draw::writeDxf(set.sheets[i], path));
           files.push_back({{"number", set.numbers[i]}, {"name", set.names[i]}, {"path", path}, {"paper", set.sheets[i].paper}});
         }
         return json{{"sheets", files}, {"notes", set.notes}};
       }},
      {"import_dae",
       "Import a COLLADA (.dae) file (e.g. exported from SketchUp Pro): each mesh an object, triangles joined "
       "into faces, closed meshes solids, colours from the materials, units and up axis converted (mm, Z up), "
       "components placed. One undo step; 'report' says what was found.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::ColladaReport report;
         json out = idsResult(must(m.importCollada(text(a, "path"), &report)));
         out["report"] = {{"units", report.units}, {"scale_to_mm", report.scaleToMm}, {"up_axis", report.upAxis},
                          {"parts", report.parts}, {"solids", report.solids}, {"faces", report.triangles},
                          {"lines", report.lines}, {"left_out", report.degenerate}, {"textured", report.textured},
                          {"unsupported", report.unsupported}, {"summary", report.summary()}};
         return out;
       }},
      {"import_skp",
       "Import a SketchUp file (.skp, docs/181) through the SketchUp SDK on this PC (fails, saying so, without it): "
       "the model's loose faces and each group / component (placed, nested) an object, faces joined, closed ones "
       "solids, colours from the materials, in mm. Hidden items are left out. One undo step; 'report' says what "
       "was found.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         geom::SkpReport report;
         json out = idsResult(must(m.importSkp(text(a, "path"), &report)));
         out["report"] = {{"groups", report.groups}, {"instances", report.instances}, {"faces", report.faces},
                          {"lines", report.lines}, {"parts", report.parts}, {"solids", report.solids},
                          {"left_out", report.leftOut}, {"hidden", report.hidden}, {"textured", report.textured},
                          {"summary", report.summary()}, {"sdk_dll", geom::skpLibrary()}};
         return out;
       }},
      {"sync_skp",
       "Bring back a SketchUp file written by export_skp and changed in SketchUp (docs/186): groups / components "
       "carrying an object's id are compared with it. Moved or turned in SketchUp with its shape as it was: the "
       "object is moved the same way (walls stay walls, their joins follow). Shape changed: listed in 'edited', or "
       "with 'replace_edited' true the object takes the shape from SketchUp ('replaced'; it is then a plain shape). "
       "Drawn anew in SketchUp: added ('added'). Written but gone from the file: listed in 'missing', or removed "
       "with 'remove_missing' true ('removed'). One undo step.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}, "replace_edited": {"type": "boolean"},
               "remove_missing": {"type": "boolean"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         const auto s = must(m.syncSkp(text(a, "path"), a.value("replace_edited", false), a.value("remove_missing", false)));
         return json{{"moved", s.moved}, {"unchanged", s.unchanged}, {"edited", s.edited}, {"replaced", s.replaced},
                     {"added", s.added}, {"missing", s.missing}, {"removed", s.removed}, {"locked", s.locked},
                     {"summary", s.report.summary()}};
       }},
      {"export_ifc",
       "Export the building as IFC4 (.ifc) for other BIM software: storeys from the levels; walls (create_wall) "
       "with their openings as voids, and a window / door for each opening with a fitting (a fitting with its "
       "sill on the floor is a door); slabs; roofs; stairs; ceiling boards (make_ceilings) as ceilings of their rooms; canopies (make_canopy) as slabs of type CANOPY connected to their walls; columns, beams and members; rooms as spaces with their areas; "
       "grids; other solids as proxies. Lengths in mm. Element ids stay the same when exported again. Returns "
       "counts and 'notes' (what was assumed or left out). Lines, faces and notes are not exported.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         const doc::IfcSummary s = must(doc::exportIfc(m, text(a, "path")));
         return json{{"storeys", s.storeys}, {"walls", s.walls}, {"slabs", s.slabs}, {"roofs", s.roofs}, {"stairs", s.stairs}, {"columns", s.columns},
                     {"beams", s.beams}, {"members", s.members}, {"openings", s.openings}, {"windows", s.windows},
                     {"doors", s.doors}, {"spaces", s.spaces}, {"grids", s.grids}, {"proxies", s.proxies},
                     {"ceilings", s.coverings}, {"canopies", s.canopies}, {"balconies", s.balconies}, {"railings", s.railings}, {"layer_sets", s.layerSets}, {"skipped", s.skipped}, {"notes", s.notes}};
       }},
      {"export_step", "Export visible objects (or 'ids') to STEP with names and colours.",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"},
               "ids": {"type": "array", "items": {"type": "string"}}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.exportStep(text(a, "path"), a.contains("ids") ? ids(a, "ids") : std::vector<ObjectId>{}));
         return json::object();
       }},
  };
  return table;
}

#undef SCHEMA

bool isFileCommand(const std::string& name) {
  return name == "save" || name == "save_copy" || name == "open" || name == "export_ifc" || name == "export_step" || name == "export_stl" || name == "export_3dm" ||
         name == "import_brep" || name == "export_brep" || name == "import_iges" || name == "export_iges" ||
         name == "import_3dm" || name == "import_dxf" || name == "import_jww" || name == "set_material_texture" || name == "export_dae" ||
         name == "import_dae" || name == "export_skp" || name == "import_skp" || name == "sync_skp" || name == "export_drawing" || name == "export_schedule" || name == "export_drawing_set" || name == "export_gltf";
}

CommandApi::Effect builtinEffect(const std::string& name) {
  using E = CommandApi::Effect;
  if (name == "list_objects" || name == "get_object" || name == "list_tags" || name == "describe" ||
      name == "preview" || name == "query_objects" || name == "aggregate" || name == "get_changes" ||
      name == "list_openings" || name == "list_materials" || name == "material_at" ||
      name == "placement_parameters" || name == "list_sections" || name == "get_construction_guide" ||
      name == "get_sun" || name == "get_site_ground_status" || name == "room_opening_inputs" || name == "permit_checks" || name == "permit_intake_review" || name == "ground_level_table" || name == "structural_material_table" || name == "foundation_ground_table" || name == "column_table" || name == "height_study" || name == "interior_area_table" || name == "structural_spec_table" || name == "interior_material_table" || name == "check_drawing_set" || name == "project_outline" || name == "daylight_factor_estimates" ||
      name == "fire_compartment_review" || name == "fire_spread_review" || name == "site_height_conditions" || name == "yokohama_height_district_at" || name == "compare_yokohama_height_district" ||
      name == "neighbor_height_at" || name == "north_height_at" || name == "compare_north_height" || name == "compare_neighbor_height" || name == "road_height_datum" || name == "road_height_table_values" || name == "measure_road_building_setback" || name == "compare_road_height_plane" || name == "check_frame" || name == "take_off" || name == "check_wall_quantity" || name == "list_wall_bays" || name == "check_foundation" || name == "check_foundation_supports" || name == "check_structure" || name == "structure_assist") {
    return E::Read;
  }
  if (isFileCommand(name)) return E::ExternalIo;
  return E::ModelWrite;
}

struct Context {
  doc::Modeler& modeler;
  const CommandApi::Options& options;
  const std::vector<CommandApi::Extension>& extensions;
  const CommandApi& api;
};

json run(const Context& c, const json& request, int depth);

// Inside a batch, a string "$2.id" (or "$2.ids.0", "$2" ...) is replaced by that field of
// step 2's result, so later steps can use objects created earlier in the same batch.
json resolveRefs(const json& v, const json& results) {
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s.size() < 2 || s[0] != '$' || !std::isdigit(static_cast<unsigned char>(s[1]))) return v;
    std::size_t pos = 1;
    const std::size_t step = std::stoul(s.substr(1), &pos);
    pos += 1;
    if (step < 1 || step > results.size()) throw ApiError("reference '" + s + "' points to a later step");
    const json* cur = &results[step - 1].at("result");
    while (pos < s.size()) {
      if (s[pos] != '.') throw ApiError("bad reference '" + s + "'");
      const std::size_t next = s.find('.', pos + 1);
      const std::string key = s.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
      if (cur->is_array() && !key.empty() && std::isdigit(static_cast<unsigned char>(key[0]))) {
        const std::size_t i = std::stoul(key);
        if (i >= cur->size()) throw ApiError("reference '" + s + "' is out of range");
        cur = &(*cur)[i];
      } else if (cur->is_object() && cur->contains(key)) {
        cur = &cur->at(key);
      } else {
        throw ApiError("reference '" + s + "' does not exist");
      }
      pos = next == std::string::npos ? s.size() : next;
    }
    return *cur;
  }
  if (v.is_array() || v.is_object()) {
    json out = v;
    for (auto it = out.begin(); it != out.end(); ++it) *it = resolveRefs(*it, results);
    return out;
  }
  return v;
}

json changesJson(const doc::ChangeSummary& s) {
  json modified = json::array();
  for (const auto& m : s.modified) modified.push_back({{"id", m.id}, {"aspects", doc::aspectNames(m.aspects)}});
  return json{{"added", s.added},
              {"removed", s.removed},
              {"modified", modified},
              {"definitions", s.definitions},
              {"materials", s.materials},
              {"settings", s.settings}};
}

json runBatch(const Context& c, const json& args, int depth) {
  doc::Modeler& m = c.modeler;
  // Applying a previewed proposal: refuse if the model changed meanwhile.
  if (args.contains("expected_revision") &&
      args.at("expected_revision").get<long long>() != m.history().revision()) {
    throw ApiError("the model changed since the proposal was previewed; preview it again");
  }
  const json& list = arg(args, "commands");
  if (!list.is_array()) throw ApiError("'commands' must be an array");
  const bool atomic = args.value("atomic", true);
  // All-or-nothing only holds for the model: a file written by an earlier
  // step stays written when a later one fails. Files are read / written
  // outside atomic batches, on a committed model (review P0-02).
  if (atomic) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (c.api.effectOf(list[i].dump()) == CommandApi::Effect::ExternalIo) {
        throw ApiError("step " + std::to_string(i + 1) + " ('" + list[i].value("command", std::string()) +
                       "') reads or writes files, which an atomic batch could not undo; run it on its own "
                       "after the batch (or use atomic: false)");
      }
    }
  }
  m.history().beginGroup(args.value("name", std::string("AI batch")));
  json results = json::array();
  for (std::size_t i = 0; i < list.size(); ++i) {
    json r;
    try {
      r = run(c, resolveRefs(list[i], results), depth + 1);
    } catch (const ApiError& e) {
      r = json{{"ok", false}, {"error", e.what()}};
    }
    if (!r.value("ok", false) && atomic) {
      m.history().abortGroup();
      throw ApiError("step " + std::to_string(i + 1) + " failed: " + r.value("error", std::string()) +
                     " (all steps reverted)");
    }
    results.push_back(std::move(r));
  }
  const long long before = m.history().revision();
  m.history().endGroup();
  // What this batch committed, from its own undo step (empty if nothing).
  doc::ChangeSummary changes;
  if (m.history().revision() != before && m.history().lastCommand()) {
    changes = doc::summarize(*m.history().lastCommand());
  }
  return json{{"results", results}, {"changes", changesJson(changes)}};
}

// "$id" / "$index" in a step become the current target's id / position.
json substituteTarget(const json& v, const std::string& id, std::size_t index) {
  if (v.is_string()) {
    if (v == "$id") return id;
    if (v == "$index") return index;
    return v;
  }
  if (v.is_array() || v.is_object()) {
    json out = v;
    for (auto it = out.begin(); it != out.end(); ++it) *it = substituteTarget(*it, id, index);
    return out;
  }
  return v;
}

doc::ChangeSummary committedChanges(doc::Modeler& m, long long before) {
  if (m.history().revision() == before || !m.history().lastCommand()) return {};
  return doc::summarize(*m.history().lastCommand());
}

// The set-based edit (performance step 5 / review §8): find the targets
// with the query_objects filter, check how many were meant, run the steps
// for each, all as one undo step that is reverted entirely on any failure.
json runEditEach(const Context& c, const json& args, int depth) {
  doc::Modeler& m = c.modeler;
  const ObjectFilter filter(arg(args, "where"));
  const json& steps = arg(args, "commands");
  if (!steps.is_array() || steps.empty()) throw ApiError("'commands' must be a non-empty array");
  std::vector<ObjectId> targets;
  for (const doc::Object& o : m.document().objects()) {
    if (filter.matches(m.document(), o)) targets.push_back(o.id);
  }
  // Nested objects are reached through their group: only the topmost.
  const std::set<ObjectId> matched(targets.begin(), targets.end());
  std::vector<ObjectId> topmost;
  for (const ObjectId& id : targets) {
    bool nested = false;
    const doc::Object* o = m.document().find(id);
    for (int guard = 0; o && !o->parent.empty() && guard < 1000; ++guard) {
      if (matched.count(o->parent)) {
        nested = true;
        break;
      }
      o = m.document().find(o->parent);
    }
    if (!nested) topmost.push_back(id);
  }
  if (topmost.empty()) throw ApiError("no object matches 'where' (check it with query_objects)");
  if (args.contains("expect_count")) {
    const json& expected = args.at("expect_count");
    if (!expected.is_number_integer()) throw ApiError("'expect_count' must be an integer");
    if (expected.get<std::size_t>() != topmost.size()) {
      throw ApiError("'where' matches " + std::to_string(topmost.size()) + " objects, expected " +
                     std::to_string(expected.get<long long>()) + "; nothing was changed");
    }
  }
  if (topmost.size() > 10000) throw ApiError("more than 10000 targets: narrow 'where'");
  const long long before = m.history().revision();
  m.history().beginGroup(args.value("name", std::string("Edit each")));
  for (std::size_t i = 0; i < topmost.size(); ++i) {
    for (std::size_t k = 0; k < steps.size(); ++k) {
      const json r = run(c, substituteTarget(steps[k], topmost[i], i), depth + 1);
      if (!r.value("ok", false)) {
        m.history().abortGroup();
        const doc::Object* o = m.document().find(topmost[i]);
        throw ApiError("step " + std::to_string(k + 1) + " failed on '" + (o ? o->name : topmost[i]) +
                       "': " + r.value("error", std::string()) + " (nothing was changed)");
      }
    }
  }
  m.history().endGroup();
  return json{{"targets", topmost}, {"changes", changesJson(committedChanges(m, before))}};
}

// Column grids: copies of 'source' at the crossings of named grid lines,
// its reference point (bottom centre of its bounds) on each crossing.
json runPlaceOnGrid(doc::Modeler& m, const json& args) {
  // Copy what is needed: copying below adds objects, which can move the
  // document's storage under a reference.
  const ObjectId sourceId = object(m, text(args, "source")).id;
  const doc::Document& d = m.document();
  const GridLookup grid(d);
  auto pick = [&](const char* key, char prefix) {
    if (!args.contains(key) || args.at(key) == "all") return grid.named(prefix);
    std::vector<std::string> names;
    for (const std::string& n : ObjectFilter::strings(args, key)) names.push_back(n);
    return names;
  };
  const std::set<std::string> skip = ObjectFilter::strings(args, "skip");
  const json b = worldBounds(d, object(m, sourceId));
  if (b.is_null()) throw ApiError("the source has no geometry");
  const gp_Pnt lo(xyz(b["min"], "min")), hi(xyz(b["max"], "max"));
  const gp_Pnt reference(0.5 * (lo.X() + hi.X()), 0.5 * (lo.Y() + hi.Y()), lo.Z());

  std::vector<std::pair<std::string, gp_Pnt>> spots;
  for (const std::string& xn : pick("x", 'X')) {
    for (const std::string& yn : pick("y", 'Y')) {
      if (skip.count(xn + "/" + yn)) continue;
      const gp_Pnt p = grid.crossing(xn, yn);
      spots.emplace_back(xn + "/" + yn, gp_Pnt(p.X(), p.Y(), reference.Z()));
    }
  }
  if (spots.empty()) throw ApiError("no crossings left to place at");
  json placed = json::object();
  m.history().beginGroup("Place on grid");
  for (const auto& [where, p] : spots) {
    gp_Trsf delta;
    delta.SetTranslation(reference, p);
    // The source standing on a crossing already is the one there (a copy
    // onto it would be the same object twice, which copy does not make).
    if (reference.Distance(p) < tolerance::kLinear) {
      placed[where] = sourceId;
      continue;
    }
    auto copy = m.copy({sourceId}, delta);
    if (!copy) {
      m.history().abortGroup();
      throw ApiError("could not place at " + where + ": " + copy.error());
    }
    // Nothing new when the same thing stands there already: that one.
    placed[where] = copy->empty() ? json(nullptr) : json(copy->front());
  }
  m.history().endGroup();
  return json{{"placed", placed}, {"count", spots.size()}};
}

// Runs a batch on a scratch copy of the model and reports what it would
// change; the real model is untouched (spec 07 §9.2).
json runPreview(const Context& c, const json& args) {
  // A preview only tries: no file is read or written, whatever the options.
  const json& steps = arg(args, "commands");
  if (steps.is_array()) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (c.api.effectOf(steps[i].dump()) == CommandApi::Effect::ExternalIo) {
        throw ApiError("step " + std::to_string(i + 1) + " ('" + steps[i].value("command", std::string()) +
                       "') reads or writes files; previews never touch files");
      }
    }
  }
  doc::Modeler scratch;
  scratch.copyFrom(c.modeler);
  CommandApi::Options trial = c.options;
  trial.allowFileCommands = false;
  trial.allowSaveCopy = false;
  CommandApi api(scratch, trial);
  json batch = {{"command", "batch"}, {"args", {{"commands", arg(args, "commands")}}}};
  const json outcome = json::parse(api.execute(batch.dump()));
  // The change set recorded by the trial run is the one the commit records.
  const json changes = outcome.value("ok", false) ? outcome["result"]["changes"]
                                                  : changesJson(doc::ChangeSummary{});
  json added = json::array(), removed = json::array(), changed = json::array();
  for (const json& id : changes["added"]) added.push_back(describeObject(scratch, *scratch.document().find(id), false));
  for (const json& m : changes["modified"]) {
    changed.push_back(describeObject(scratch, *scratch.document().find(m["id"].get<std::string>()), false));
  }
  for (const json& id : changes["removed"]) removed.push_back(id);
  json result = {{"would_succeed", outcome.value("ok", false)},
                 {"added", added},
                 {"changed", changed},
                 {"removed", removed},
                 {"changes", changes},
                 {"revision", c.modeler.history().revision()}};
  if (!outcome.value("ok", false)) result["error"] = outcome.value("error", std::string());
  return result;
}

json run(const Context& c, const json& request, int depth) {
  try {
    if (!request.is_object()) throw ApiError("request must be an object");
    const std::string name = request.value("command", std::string());
    const json args = request.value("args", json::object());
    if (!args.is_object()) throw ApiError("'args' must be an object");
    if (name == "batch") {
      if (depth > 0) throw ApiError("batches cannot be nested");
      return json{{"ok", true}, {"result", runBatch(c, args, depth)}};
    }
    if (name == "describe") return json{{"ok", true}, {"result", json::parse(c.api.schemaJson())}};
    if (name == "edit_each") return json{{"ok", true}, {"result", runEditEach(c, args, depth)}};
    if (name == "preview") {
      if (depth > 0) throw ApiError("preview cannot be nested");
      return json{{"ok", true}, {"result", runPreview(c, args)}};
    }
    if (isFileCommand(name) && !c.options.allowFileCommands && !(name == "save_copy" && c.options.allowSaveCopy)) {
      throw ApiError("'" + name + "' is disabled here (the user opens and saves files)");
    }
    for (const CommandDef& def : commands()) {
      if (name == def.name) return json{{"ok", true}, {"result", def.run(c.modeler, args)}};
    }
    for (const CommandApi::Extension& e : c.extensions) {
      if (name == e.name) return json{{"ok", true}, {"result", json::parse(e.run(args.dump()))}};
    }
    throw ApiError("unknown command '" + name + "' (use 'describe' for the list)");
  } catch (const ApiError& e) {
    return json{{"ok", false}, {"error", e.what()}};
  } catch (const json::exception& e) {
    return json{{"ok", false}, {"error", std::string("invalid argument: ") + e.what()}};
  } catch (const std::exception& e) {
    return json{{"ok", false}, {"error", e.what()}};
  } catch (const Standard_Failure& e) {
    return json{{"ok", false}, {"error", std::string("geometry kernel error: ") + sbr::kernelMessage(e)}};
  } catch (...) {
    return json{{"ok", false}, {"error", "internal error"}};
  }
}

}  // namespace

std::string CommandApi::execute(const std::string& requestJson) {
  json request;
  try {
    request = json::parse(requestJson);
  } catch (const json::exception& e) {
    return json{{"ok", false}, {"error", std::string("invalid JSON: ") + e.what()}}.dump();
  }
  const bool inGroup = modeler_.history().inGroup();
  json result = run(Context{modeler_, options_, extensions_, *this}, request, 0);
  // A step group a failing command left open (an exception past its own
  // clean-up) is taken back whole: later edits must not pile into it.
  if (!inGroup && modeler_.history().inGroup()) {
    modeler_.history().abortGroup();
    if (result.value("ok", false)) result = json{{"ok", false}, {"error", "internal error: an edit was left open (reverted)"}};
  }
  return result.dump();
}

const char* CommandApi::toString(Effect effect) {
  switch (effect) {
    case Effect::Read: return "read";
    case Effect::ViewWrite: return "view_write";
    case Effect::ModelWrite: return "model_write";
    case Effect::ExternalIo: return "external_io";
  }
  return "model_write";
}

CommandApi::Effect CommandApi::effectOf(const std::string& requestJson) const {
  const json request = json::parse(requestJson, nullptr, false);
  if (!request.is_object() || !request.contains("command") || !request["command"].is_string()) {
    return Effect::ModelWrite;
  }
  const std::string name = request["command"].get<std::string>();
  if (name == "generate_frame" || name == "generate_foundation") {
    const json a = request.value("args", json::object());
    return a.is_object() && (!a.contains("preview") || a["preview"] == true) ? Effect::Read
                                                                             : Effect::ModelWrite;
  }
  if (name == "batch") {
    const json args = request.value("args", json::object());
    if (!args.is_object() || !args.contains("commands") || !args["commands"].is_array()) return Effect::ModelWrite;
    Effect most = Effect::Read;
    for (const json& step : args["commands"]) {
      if (step.is_object() && step.value("command", std::string()) == "batch") return Effect::ModelWrite;
      most = std::max(most, effectOf(step.dump()));
    }
    return most;
  }
  for (const Extension& e : extensions_) {
    if (e.name == name) return e.effect;
  }
  for (const CommandDef& def : commands()) {
    if (name == def.name) return builtinEffect(name);
  }
  if (name == "describe" || name == "preview") return Effect::Read;
  return Effect::ModelWrite;
}

std::string CommandApi::schemaJson() const {
  json list = json::array();
  for (const CommandDef& c : commands()) {
    if (isFileCommand(c.name) && !options_.allowFileCommands && !(c.name == "save_copy" && options_.allowSaveCopy)) continue;
    list.push_back({{"name", c.name},
                    {"description", c.description},
                    {"input_schema", json::parse(c.schema)},
                    {"effect", toString(builtinEffect(c.name))}});
  }
  for (const Extension& e : extensions_) {
    list.push_back({{"name", e.name},
                    {"description", e.description},
                    {"input_schema", json::parse(e.inputSchema)},
                    {"effect", toString(e.effect)}});
  }
  list.push_back({{"name", "preview"},
                  {"effect", "read"},
                  {"description",
                   "Try 'commands' (as in batch) on a scratch copy and report what they would add, change "
                   "and remove, without touching the model. To apply, send the same commands to batch with "
                   "expected_revision = the returned revision (refused if the model changed meanwhile)."},
                  {"input_schema",
                   {{"type", "object"},
                    {"properties", {{"commands", {{"type", "array"}, {"items", {{"type", "object"}}}}}}},
                    {"required", {"commands"}}}}});
  list.push_back({{"name", "edit_each"},
                  {"effect", "model_write"},
                  {"description",
                   "Apply 'commands' to every object matching 'where' (the filters of query_objects; nested "
                   "matches are reached through their topmost matching group). In the commands \"$id\" is "
                   "the current target and \"$index\" its position (0, 1, ...). Give 'expect_count' (from a "
                   "query_objects total) so a wider or narrower match than intended is refused. One undo "
                   "step; any failure reverts everything. Works inside preview / propose."},
                  {"input_schema",
                   {{"type", "object"},
                    {"properties",
                     {{"where", {{"type", "object"}}},
                      {"commands", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                      {"expect_count", {{"type", "integer"}}},
                      {"name", {{"type", "string"}}}}},
                    {"required", {"where", "commands"}}}}});
  list.push_back({{"name", "batch"},
                  {"effect", "model_write"},
                  {"description",
                   "Run several commands as ONE undo step. atomic (default true): if a step fails, every "
                   "step is reverted. A string \"$N.field\" in a later step is replaced by that field of "
                   "step N's result (1-based), e.g. \"$1.id\" or \"$2.ids.0\"."},
                  {"input_schema",
                   {{"type", "object"},
                    {"properties",
                     {{"commands", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                      {"name", {{"type", "string"}}},
                      {"atomic", {{"type", "boolean"}}},
                      {"expected_revision", {{"type", "integer"}}}}},
                    {"required", {"commands"}}}}});
  return list.dump();
}

std::vector<CommandApi::Extension> CommandApi::appCommandSpecs() {
  return {
      {"get_selection",
       "What the user has selected in the app: object ids, picked faces/edges (index as in get_object) and "
       "the group being edited. Use it for requests like 'make this taller'.",
       R"({"type": "object", "properties": {}})", nullptr, Effect::Read},
      {"select", "Select objects in the app (shows the user what you mean).",
       R"({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
           "required": ["ids"]})",
       nullptr, Effect::ViewWrite},
      {"set_view", "Change the camera: iso, top, front, right, back, left, or fit (optionally to 'ids').",
       R"({"type": "object", "properties": {
           "view": {"enum": ["iso", "top", "front", "right", "back", "left", "fit"]},
           "ids": {"type": "array", "items": {"type": "string"}}}, "required": ["view"]})",
       nullptr, Effect::ViewWrite},
      {"set_display",
       "The display (docs/81_realistic_display): 'realistic' (materials' metallic / roughness, the sun of "
       "get_sun / set_sun, shadows) or the CAD display; 'edges' (faces' outlines in the realistic display); "
       "'shadows_on_ground'. Omitted keys stay. Returns all three. Display only, not saved in the model.",
       R"({"type": "object", "properties": {"realistic": {"type": "boolean"}, "edges": {"type": "boolean"},
           "shadows_on_ground": {"type": "boolean"}}})",
       nullptr, Effect::ViewWrite},
      {"capture_view", "Capture the current 3D view as a PNG image so you can look at the model.",
       R"({"type": "object", "properties": {}})", nullptr, Effect::Read},
      {"render_drawing_set",
       "docs/241: the drawing set as the app's File menu writes it: one PDF and a PNG per sheet, in a folder of the "
       "app's own under the temporary folder (emptied each time). Returns the paths, each sheet's number, name, "
       "paper and notes (e.g. does not fit), and the set's notes. 'sheet' (a number such as A-15, the 1-based "
       "index, or the drawing's name such as 断熱範囲図（1FL）) also returns that sheet's picture; with 'texts': true, "
       "also every text on it (docs/246), to check what it says. 'scale' (default 100, docs/250): e.g. 20. With 'sheet', only that sheet is pictured and no PDF is written unless 'pdf': true. 'tables_with_plan' and 'combine_views' as in export_drawing_set. With a vicinity "
       "map set, its tiles come from 国土地理院 (cache, else fetched), as the File menu does.",
       R"({"type": "object", "properties": {"sheet": {"type": ["string", "integer"]},
           "texts": {"type": "boolean"}, "tables_with_plan": {"type": "boolean"}, "combine_views": {"type": "boolean"},
           "scale": {"type": "number"},
           "pdf": {"type": "boolean"}}})",
       nullptr, Effect::Read},
      {"capture_window",
       "docs/241: a picture of the app as the user sees it. 'window': main (default: the whole main window with "
       "its panels) or one of project_outline, energy_route, vicinity_map, equipment, insulation (needs walls / "
       "floors / roofs / ceilings selected), schedules, rooms, levels, site_road, site_ground, opening_performance "
       "('host' wall id, 'index' 1-based). The window is opened, pictured and closed with Cancel: nothing "
       "changes. If it does not open (e.g. nothing selected), the error gives the status bar's message.",
       R"({"type": "object", "properties": {"window": {"type": "string"}, "host": {"type": "string"},
           "index": {"type": "integer"}}})",
       nullptr, Effect::Read},
      {"ui_windows",
       "docs/242, the AI on this PC only: the app's visible windows (title, object name, which is "
       "waiting for an answer) and the status bar's message. Use after ui_click / ui_trigger / ui_key.",
       R"({"type": "object", "properties": {}})", nullptr, Effect::Read},
      {"ui_list",
       "docs/242: a window's widgets as a person reads them (buttons, fields, check "
       "boxes, lists, tables with their cells, labels, panels), each with 'ref' (object name, else #n). 'window': "
       "top (default: the window waiting for an answer, else the main window), main, modal, or a window's title. "
       "'filter' keeps the widgets whose ref, kind or text holds it; 'menus': true adds the main window's menu "
       "items (path, object name, enabled, checked); 'limit' (300).",
       R"({"type": "object", "properties": {"window": {"type": "string"}, "filter": {"type": "string"},
           "menus": {"type": "boolean"}, "limit": {"type": "integer"}}})",
       nullptr, Effect::Read},
      {"ui_capture",
       "docs/242: a picture of a window as it is now (not closed). 'window' as in ui_list.",
       R"({"type": "object", "properties": {"window": {"type": "string"}}})", nullptr, Effect::Read},
      {"ui_set",
       "docs/242: set a widget as a person would: 'widget' (ref from ui_list), 'value' "
       "(text, number, true / false, a list's item text or data, a tab's name). A table cell: 'row' and 'column' "
       "(0-based) with the cell's text or the value of the list / number / check box in it. 'window' as in ui_list.",
       R"({"type": "object", "properties": {"window": {"type": "string"}, "widget": {"type": "string"},
           "value": {}, "row": {"type": "integer"}, "column": {"type": "integer"}}, "required": ["widget", "value"]})",
       nullptr, Effect::ViewWrite},
      {"ui_click",
       "docs/242: press a button ('widget': ref from ui_list or the button's text, e.g. OK, "
       "キャンセル). The press happens just after the answer; then ui_windows / ui_list show what it did.",
       R"({"type": "object", "properties": {"window": {"type": "string"}, "widget": {"type": "string"}},
           "required": ["widget"]})",
       nullptr, Effect::ViewWrite},
      {"ui_trigger",
       "docs/242: choose a menu item ('action': object name, its text, or its menus' path "
       "such as ファイル/図面一式を書き出す; ui_list with menus: true lists them). It runs just after the answer; "
       "a window it opens is then 'top' for the ui_* commands.",
       R"({"type": "object", "properties": {"action": {"type": "string"}}, "required": ["action"]})",
       nullptr, Effect::ViewWrite},
      {"ui_key",
       "docs/242: press a key ('key': Escape, Return, Delete, Ctrl+Z...) in a widget "
       "('widget', else the window's focus). 'window' as in ui_list.",
       R"({"type": "object", "properties": {"window": {"type": "string"}, "widget": {"type": "string"},
           "key": {"type": "string"}}, "required": ["key"]})",
       nullptr, Effect::ViewWrite},
      // docs/252: the ui_* commands are ViewWrite (though a press may change the model, as a person's does):
      // a ModelWrite runs the bridge's hooks, which cancel the drawing tool before and start it again
      // after the request, and that undid the clicks the request had just set going.
      {"ui_tool",
       "docs/252, the AI on this PC only: choose a drawing tool as a person would ('tool': select, line, rectangle, "
       "push_pull, move, rotate, offset, measure, circle, paint, eraser, arc, dimension, text, section, polygon, scale, "
       "wall, slab, opening, member, room, roof, stair, door_swing, site_north).",
       R"({"type": "object", "properties": {"tool": {"type": "string"}}, "required": ["tool"]})", nullptr, Effect::ViewWrite},
      {"ui_view_click",
       "docs/252, the AI on this PC only: click in the 3D view at model points as the view shows them now ('at' "
       "[x, y, z] mm, or 'points' for several clicks in turn; 'button' left / right / middle; 'modifiers' [ctrl, shift, "
       "alt]; 'double'). Set the view first (set_view top / fit) so the points are on the screen. The clicks are done "
       "before the answer; 'async': true sends them just after it instead (a click that opens a window waiting for an "
       "answer; then ui_windows / ui_list).",
       R"({"type": "object", "properties": {"at": {"type": "array"}, "points": {"type": "array"}, "button": {"type": "string"},
           "modifiers": {"type": "array", "items": {"type": "string"}}, "double": {"type": "boolean"}, "async": {"type": "boolean"}}})",
       nullptr, Effect::ViewWrite},
      {"ui_view_drag",
       "docs/252, the AI on this PC only: press at model point 'from', move to 'to' and let go, in the 3D view "
       "('button', 'modifiers', 'async' as ui_view_click).",
       R"({"type": "object", "properties": {"from": {"type": "array"}, "to": {"type": "array"}, "button": {"type": "string"},
           "modifiers": {"type": "array", "items": {"type": "string"}}, "async": {"type": "boolean"}}, "required": ["from", "to"]})",
       nullptr, Effect::ViewWrite},
      {"ui_view_hover",
       "docs/252, the AI on this PC only: move the mouse over model point 'at' in the 3D view (a tool's preview, snaps).",
       R"({"type": "object", "properties": {"at": {"type": "array"}}, "required": ["at"]})", nullptr, Effect::ViewWrite},
      {"ui_plan",
       "docs/262, the AI on this PC only: the plan mode of a level as the toolbar's 平面で描く menu shows it "
       "(seen from above, cut 1100 above the floor; openings can be grabbed at their symbols): 'level' (name or "
       "id from get_levels; omitted: the ground, Z=0), or 'off': true back to the 3D view.",
       R"({"type": "object", "properties": {"level": {"type": "string"}, "off": {"type": "boolean"}}})",
       nullptr, Effect::ViewWrite},
      {"ui_type",
       "docs/252, the AI on this PC only: type 'text' in the 3D view as a person types a length or a count while a tool "
       "is working (it goes to the measurement box), then Return unless 'enter' is false ('async' as ui_view_click). "
       "docs/262: 'keys' (sent to the view first, each pressed and let go): key names such as Ctrl (the move tool's "
       "copy), Shift, Alt, Up, Down, Left, Right, Delete, Escape, Return, Tab.",
       R"({"type": "object", "properties": {"text": {"type": "string"}, "enter": {"type": "boolean"}, "async": {"type": "boolean"},
           "keys": {"type": "array", "items": {"type": "string"}}}})",
       nullptr, Effect::ViewWrite},
      {"list_scenes", "List the saved views (scenes) of the model.", R"({"type": "object", "properties": {}})",
       nullptr, Effect::Read},
      {"propose",
       "Show the user a proposal instead of changing the model: 'commands' (as in batch) are tried on a "
       "copy, the result is drawn as a ghost with Apply / Reject buttons, and nothing changes until the user "
       "applies it. Returns at once; poll proposal_status. 'description' is shown to the user.",
       R"({"type": "object", "properties": {"commands": {"type": "array", "items": {"type": "object"}},
           "description": {"type": "string"}}, "required": ["commands"]})",
       nullptr, Effect::ViewWrite},
      {"proposal_status", "State of the last proposal: pending, applied, rejected or failed (with error).",
       R"({"type": "object", "properties": {}})", nullptr, Effect::Read},
      {"go_to_scene", "Show a saved view by name or 0-based index; with 'save': true, save the current view "
                      "as a new scene named 'name' instead.",
       R"({"type": "object", "properties": {"name": {"type": "string"}, "index": {"type": "integer"},
           "save": {"type": "boolean"}}})",
       nullptr, Effect::ModelWrite},
  };
}

}  // namespace sbr::api
