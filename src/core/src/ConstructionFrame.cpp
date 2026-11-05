#include "sbr/ConstructionFrame.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "sbr/Codec.h"
#include "sbr/ConstructionGuide.h"
#include "sbr/KernelError.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/NativeFormat.h"

namespace sbr::construction {
using namespace doc;
std::optional<Object> frameMemberObject(const Document& document, const Object& instance) {
  const auto* ref = std::get_if<ComponentInstance>(&instance.recipe.base);
  if (!ref || ref->mirrored || !ref->stretches.empty()) return std::nullopt;
  const auto* stored = document.findDefinition(ref->definitionId);
  if (!stored || stored->objects.size() != 1 ||
      !std::holds_alternative<LinearMember>(stored->objects[0].recipe.base))
    return std::nullopt;
  const auto own = applyOverrides(*stored, ref->overrides, document.regenContext());
  if (!own) return std::nullopt;
  Object o = instance;
  o.transform = instance.transform * own->objects[0].transform;
  o.recipe = own->objects[0].recipe;
  const auto anchor = instance.attributes.find("member_anchor_up");
  if (anchor != instance.attributes.end()) {
    auto& member = std::get<LinearMember>(o.recipe.base);
    member.anchor.up = anchor->second == "1" ? 1 : anchor->second == "-1" ? -1 : 0;
    const gp_Vec offset = gp_Vec(member.up) * (member.anchor.up * member.section.depth / 2);
    member.start.Translate(offset);
    member.end.Translate(offset);
  }
  o.recipe.modifiers.insert(o.recipe.modifiers.end(), instance.recipe.modifiers.begin(),
                            instance.recipe.modifiers.end());
  return o;
}

// A picked opening is on a wall FACE. Only a wall made by the wall tool
// provides a known reference line; do not infer one from an arbitrary B-rep.
std::optional<gp_Ax1> wallReferenceLine(const Object& host, const Document& document) {
  const auto kind = host.attributes.find("kind");
  if (kind == host.attributes.end() || kind->second != "wall") return std::nullopt;
  const Recipe* recipe = &host.recipe;
  gp_Trsf placement = document.worldTransform(host);
  for (int depth = 0; depth < 64; ++depth) {
    const auto* cut = std::get_if<BooleanFeature>(&recipe->base);
    if (!cut) break;
    if (!recipe->modifiers.empty() || cut->kind != BooleanKind::Subtract || cut->operands.empty() ||
        !cut->operands.front().recipe)
      return std::nullopt;
    placement = placement * cut->operands.front().placement;
    recipe = cut->operands.front().recipe.get();
  }
  const auto* rect = std::get_if<RectangleProfile>(&recipe->base);
  if (!rect || recipe->modifiers.size() != 1 ||
      !std::holds_alternative<ExtrudeOp>(recipe->modifiers.front()))
    return std::nullopt;
  const auto line = host.attributes.find("wall_line");
  const std::string ref = line == host.attributes.end() ? "center" : line->second;
  if (ref != "center" && ref != "left" && ref != "right") return std::nullopt;
  const double offset = ref == "right" ? 0 : ref == "left" ? rect->height : rect->height / 2;
  const auto origin = rect->plane.toWorld(0, offset).Transformed(placement);
  const auto direction = gp_Dir(rect->plane.xDir).Transformed(placement);
  if (std::abs(direction.Z()) > 1e-6) return std::nullopt;
  return gp_Ax1(origin, direction);
}

namespace {
using json = nlohmann::json;
void rounded(json& j) {
  if (j.is_number_float()) {
    const double x = j.get<double>();
    if (std::isfinite(x) && std::abs(x) < 1e12) j = std::llround(x * 1e6);
  } else if (j.is_array() || j.is_object())
    for (auto& value : j) rounded(value);
}
}  // namespace
std::string frameFingerprint(const doc::Object& o) {
  json j = json::parse(doc::objectStateJson(o));
  rounded(j);
  return codec::sha256Hex(j.dump());
}
std::string frameDefinitionFingerprint(const doc::ComponentDefinition& d) {
  std::string s = d.id + "/" + d.name;
  for (const auto& o : d.objects) s += frameFingerprint(o);
  return codec::sha256Hex(s);
}

Status moveFrameInstance(doc::Object& o, const doc::Document& document, const gp_Trsf& world,
                         const gp_Vec& ds, const gp_Vec& de) {
  auto* instance = std::get_if<doc::ComponentInstance>(&o.recipe.base);
  if (!instance) return Status::failure("Not a shared frame part");
  const auto* stored = document.findDefinition(instance->definitionId);
  if (!stored) return Status::failure("Frame part definition is missing");
  const gp_Trsf parent = world * o.transform.Inverted();
  if ((ds - de).Magnitude() < 1e-9) {
    gp_Trsf shift;
    shift.SetTranslation(ds);
    o.transform = parent.Inverted() * shift * world;
    return Status::success();
  }
  if (instance->mirrored || !instance->stretches.empty() || stored->objects.size() != 1 ||
      !o.recipe.modifiers.empty())
    return Status::failure("Edited frame part cannot follow unequal endpoint moves");
  auto own = doc::applyOverrides(*stored, instance->overrides, document.regenContext());
  if (!own) return Status::failure(own.error());
  const auto* member = std::get_if<doc::LinearMember>(&own->objects[0].recipe.base);
  if (!member) return Status::failure("Only a member can follow two different levels");
  const gp_Trsf part = world * own->objects[0].transform;
  const gp_Pnt from = member->start.Transformed(part).Translated(ds);
  const gp_Pnt to = member->end.Transformed(part).Translated(de);
  auto frame = geom::memberFrame(from, to, gp::DY().Transformed(part), 0.0);
  if (!frame) return Status::failure(frame.error());
  // Generated parts have their axis on local +Z with their foot at zero.
  if (member->start.Distance(gp::Origin()) > 1e-6 || std::abs(member->end.X()) > 1e-6 ||
      std::abs(member->end.Y()) > 1e-6)
    return Status::failure("The frame part's design axis was edited");
  gp_Trsf placement;
  placement.SetDisplacement(gp_Ax3(), *frame);
  o.transform = parent.Inverted() * placement;
  auto& overrides = instance->overrides;
  overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
                                 [&](const doc::ParameterOverride& p) {
                                   return p.object == own->objects[0].id && p.key == "length";
                                 }),
                  overrides.end());
  const auto* original = std::get_if<doc::LinearMember>(&stored->objects[0].recipe.base);
  if (!original) return Status::failure("Frame member definition was changed");
  if (std::abs(from.Distance(to) - original->start.Distance(original->end)) > 1e-6)
    overrides.push_back({stored->objects[0].id, "length", from.Distance(to)});
  return doc::rebuild(o, document.regenContext());
}
}  // namespace sbr::construction

namespace sbr::doc {
namespace {
using json = nlohmann::json;
void need(bool test, const std::string& why) {
  if (!test) throw std::runtime_error(why);
}
double num(const json& j, const char* key, double fallback) {
  if (!j.contains(key)) return fallback;
  need(j.at(key).is_number(), std::string(key) + " must be a number");
  const double value = j.at(key).get<double>();
  need(std::isfinite(value), std::string(key) + " must be finite");
  return value;
}
int integer(const json& j, const char* key, int fallback, int minimum, int maximum) {
  const double v = num(j, key, fallback);
  need(v == std::floor(v) && v >= minimum && v <= maximum, std::string(key) + " is outside the recipe range");
  return static_cast<int>(v);
}
gp_Pnt point3(const json& j) {
  need(j.is_array() && j.size() == 3, "origin needs [x,y,z]");
  for (const auto& n : j) need(n.is_number() && std::isfinite(n.get<double>()), "origin must be finite");
  return gp_Pnt(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}
std::string keyNumber(double n) {
  std::ostringstream s;
  s.precision(14);
  s << std::round(n * 1e6) / 1e6;
  return s.str();
}
struct Rect {
  int x0, x1, y0, y1;
  bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  bool boundary(int x, int y) const { return contains(x, y) && (x == x0 || x == x1 || y == y0 || y == y1); }
};
Rect rectangle(const json& j, int nx, int ny) {
  need(j.is_object() && j.contains("x") && j.contains("y"),
       "range needs x:[first,last], y:[first,last] grid indices");
  auto range = [](const json& a, int last) {
    need(a.is_array() && a.size() == 2 && a[0].is_number_integer() && a[1].is_number_integer(),
         "range needs two integer indices");
    const int first = a[0].get<int>(), end = a[1].get<int>();
    need(first >= 0 && end <= last && first < end, "grid range is outside the frame");
    return std::pair<int, int>(first, end);
  };
  const auto x = range(j.at("x"), nx), y = range(j.at("y"), ny);
  return {x.first, x.second, y.first, y.second};
}
struct Piece {
  std::string key, role, layer, stage, catalog, hardware, startLevel, endLevel;
  gp_Pnt from, to;
  geom::Section section;
  int anchorUp = 0;
  std::map<std::string, std::string> attributes;
};
struct Opening {
  gp_Pnt center;
  gp_Dir along;
  double width, bottom, top;
  int storey;
  std::string host;
  std::size_t index;
};

// Plans geometry from the guide's effective tables. All dimensions are in
// the grid's coordinates; level heights are world Z. The same plan is used
// by preview, GUI application and the headless command.
struct Plan {
  const Modeler& modeler;
  json inputs, data, defaults;
  std::string patch, name, gridId;
  gp_Trsf gridWorld;
  GridSpec grid;
  std::vector<Piece> pieces;
  std::vector<FloorLevel> levels;
  std::map<std::string, std::string> levelIds;
  std::map<std::string, double> heights;
  json warnings = json::array(), spacing = json::array();
  std::vector<Opening> openings;
  std::set<std::string> keys;
  int nx = 0, ny = 0, storeys = 2;
  double module = 910, finish = 0, stock = 4000, pitch = .4, over = 600, gableOver = 450;
  bool axisX = true;
  bool regenerating = false;
  std::string roof;
  Rect full{0, 0, 0, 0}, upper{0, 0, 0, 0};
  // Interior sill bays (i,j)-(k,l), one grid bay each, normalised with (i,j) < (k,l).
  std::set<std::array<int, 4>> innerSills;

  Plan(const Modeler& m, const json& args, std::string frameName, bool replacing = false)
      : modeler(m),
        inputs(args.value("inputs", json::object())),
        name(std::move(frameName)),
        regenerating(replacing) {
    need(inputs.is_object(), "inputs must be an object");
    const auto& patches = m.document().settings().constructionOverrides;
    const auto it = patches.find("timber");
    patch = it == patches.end() ? "{}" : it->second;
    data = json::parse(construction::guideData("timber", patch));
    defaults = data.at("recipes").at("entries").at("post_beam_2storey").at("defaults");
    levels = m.levels();
  }
  double preset(const char* id) const {
    return data.at("presets").at("entries").at(id).at("value").get<double>();
  }
  geom::Section section(const std::string& role) const {
    std::string id = data.at("roles").at("entries").at(role).at("default_section");
    if (inputs.contains("sections") && inputs.at("sections").contains(role))
      id = inputs.at("sections").at(role).get<std::string>();
    geom::Section selected = construction::section(id, patch);
    const bool beam =
        role == "rim_beam" || role == "floor_beam" || role == "eave_beam" || role == "roof_beam";
    const auto overrides = data.value("overrides", json::object());
    const bool explicitInput = inputs.value("sections", json::object()).contains(role);
    const bool explicitRole =
        overrides.value("roles", json::object()).value("entries", json::object()).contains(role);
    const bool explicitSize =
        overrides.value("sections", json::object()).value("entries", json::object()).contains(id);
    if (beam && !explicitInput && !explicitRole && !explicitSize) {
      const double width = section(storeys == 2 ? "through_column" : "column").width;
      const std::string matching = "TIMBER-" + keyNumber(width) + "x" + keyNumber(selected.depth);
      if (data.at("sections").at("entries").contains(matching))
        selected = construction::section(matching, patch);
      else {
        selected.width = width;
        selected.catalog.clear();
        selected.label.clear();
      }
    }
    return selected;
  }
  double supportTop(const std::string& ref) const {
    if (ref != "second_fl") return height(ref);
    const auto mode = inputs.value(
        "second_floor_system",
        data.at("presets").at("entries").at("second_floor_system").at("value").get<std::string>());
    const std::string id = data.at("roles").at("entries").at("floor_sheathing").at("default_section");
    const double subfloor = mode == "joistless"
                                ? data.at("sections").at("entries").at(id).at("thickness").get<double>()
                                : section("joist").depth;
    return height(ref) - finish - subfloor;
  }
  double height(const std::string& key) const { return heights.at(key); }
  std::string level(const std::string& key) const { return levelIds.at(key); }
  gp_Pnt world(const gp_Pnt& local) const {
    gp_Pnt at(local.X(), local.Y(), 0);
    at.Transform(gridWorld);
    at.SetZ(local.Z());
    return at;
  }
  double x(int i) const { return grid.x.at(i).offset; }
  double y(int i) const { return grid.y.at(i).offset; }
  gp_Pnt at(int i, int j, double z) const { return gp_Pnt(x(i), y(j), z); }
  void add(Piece p) {
    need(pieces.size() < 20000,
         "Frame would exceed 20000 parts; reduce the input or generate smaller regions");
    need(keys.insert(p.key).second, "Duplicate recipe key: " + p.key);
    if (p.hardware.empty())
      need(p.from.Distance(p.to) > tolerance::kMinDimension, "Zero-length frame member: " + p.key);
    pieces.push_back(std::move(p));
  }
  void hardware(const std::string& type, const std::string& key, const gp_Pnt& at, const std::string& ref,
                const std::string& stage) {
    Piece p;
    p.key = key;
    p.hardware = type;
    p.role = type;
    p.stage = stage;
    p.layer = type == "joint" ? "継手" : "金物";
    p.from = p.to = at;
    p.startLevel = p.endLevel = level(ref);
    p.attributes["selection_state"] = data.at("hardware").at("entries").at(type).at("selection");
    p.attributes["display_only"] = "true";
    add(std::move(p));
  }
  void member(const std::string& role, const std::string& key, const gp_Pnt& a, const gp_Pnt& b,
              const std::string& base, const std::string& top, const std::string& stage, int anchor = 1,
              std::optional<geom::Section> custom = std::nullopt) {
    Piece p;
    p.role = role;
    p.key = key;
    p.from = a;
    p.to = b;
    p.startLevel = level(base);
    p.endLevel = level(top);
    p.stage = p.layer = stage;
    p.section = custom ? *custom : section(role);
    p.anchorUp = anchor;
    p.catalog = p.section.catalog;
    add(p);
    if (role == "rim_beam" || role == "floor_beam" || role == "eave_beam" || role == "roof_beam" ||
        role == "horizontal_brace") {
      hardware("beam_end_plate", key + "/fitting/start", a, base, stage);
      hardware("beam_end_plate", key + "/fitting/end", b, top, stage);
    }
    if (a.Distance(b) > stock + 1e-6) {
      const gp_Vec axis(a, b);
      for (double length = stock; length < a.Distance(b) - 1e-6; length += stock) {
        hardware("joint", key + "/joint/" + keyNumber(length), a.Translated(axis * (length / a.Distance(b))),
                 base, stage);
      }
    }
  }
  void addLevel(const std::string& key, const std::string& label, double value) {
    const json spec = inputs.value("levels", json::object());
    if (spec.contains(key) && spec.at(key).is_string()) {
      const std::string wanted = spec.at(key);
      const auto found = std::find_if(levels.begin(), levels.end(), [&](const FloorLevel& l) {
        return l.id == wanted || l.name == wanted;
      });
      need(found != levels.end(), "Unknown level: " + wanted);
      levelIds[key] = found->id;
      heights[key] = found->elevation;
      return;
    }
    if (spec.contains(key)) value = num(spec, key.c_str(), value);
    need(std::isfinite(value), "Level height must be finite");
    std::string title = name + " " + label;
    const auto existing =
        std::find_if(levels.begin(), levels.end(), [&](const FloorLevel& l) { return l.name == title; });
    if (existing != levels.end() && regenerating) {
      need(std::abs(existing->elevation - value) < 1e-6,
           "Use set_levels / レベル編集 to change existing heights before regenerating the frame");
      levelIds[key] = existing->id;
      heights[key] = existing->elevation;
      return;
    }
    need(std::none_of(levels.begin(), levels.end(), [&](const FloorLevel& l) { return l.name == title; }),
         "Level name already used: " + title);
    const std::string id = generateObjectId();
    levels.push_back({id, title, value});
    levelIds[key] = id;
    heights[key] = value;
  }
  std::vector<double> positions(double from, double to, int divisions, const std::string& type) {
    need(to > from, "Spacing range must be positive");
    std::vector<double> result{from};
    const json spec = inputs.value("spacing", json::object()).value(type, json::object());
    const std::string mode = spec.value("mode", std::string("equal_division"));
    need(mode == "equal_division" || mode == "fixed_pitch",
         "Spacing mode must be equal_division or fixed_pitch");
    const double wanted = mode == "fixed_pitch" ? num(spec, "pitch", module / divisions)
                                                : module / integer(spec, "divisions", divisions, 1, 12);
    need(wanted >= 10 && wanted <= 10000, "Spacing pitch must be 10 to 10000 mm");
    const int count = static_cast<int>(std::ceil((to - from) / wanted));
    need(count <= 2000, "Too many spacing intervals");
    const double step = mode == "equal_division" ? (to - from) / count : wanted;
    for (int i = 1; i < count; ++i) result.push_back(from + i * step);
    result.push_back(to);
    spacing.push_back({{"kind", type},
                       {"mode", mode},
                       {"requested_pitch", wanted},
                       {"pitch", step},
                       {"last_interval", result.back() - result[result.size() - 2]},
                       {"from", from},
                       {"to", to}});
    return result;
  }
  void prepareGrid() {
    module = num(inputs, "module", preset("module"));
    need(module >= 100 && module <= 10000, "module must be 100 to 10000 mm");
    if (inputs.contains("grid")) {
      gridId = inputs.at("grid").get<std::string>();
      const auto* g = modeler.gridSpec(gridId);
      need(g != nullptr, "inputs.grid is not a structural grid");
      grid = *g;
      const auto* object = modeler.document().find(gridId);
      gridWorld = modeler.document().worldTransform(*object);
      need(std::abs(gridWorld.Value(3, 1)) < 1e-8 && std::abs(gridWorld.Value(3, 2)) < 1e-8 &&
               std::abs(gridWorld.Value(3, 3) - 1) < 1e-8 && std::abs(gridWorld.ScaleFactor() - 1) < 1e-8,
           "The recipe needs a horizontal, unscaled grid");
    } else {
      nx = integer(inputs, "x_bays", defaults.at("x_bays"), 1, 40);
      ny = integer(inputs, "y_bays", defaults.at("y_bays"), 1, 40);
      const gp_Pnt origin = point3(inputs.value("origin", json::array({0, 0, 0})));
      gridWorld.SetTranslation(gp_Vec(origin.XYZ()));
      for (int i = 0; i <= nx; ++i)
        grid.x.push_back({"x" + std::to_string(i), "X" + std::to_string(i + 1), i * module});
      for (int j = 0; j <= ny; ++j)
        grid.y.push_back({"y" + std::to_string(j), "Y" + std::to_string(j + 1), j * module});
    }
    auto order = [](auto& list) {
      std::sort(list.begin(), list.end(),
                [](const GridAxis& a, const GridAxis& b) { return a.offset < b.offset; });
    };
    order(grid.x);
    order(grid.y);
    nx = static_cast<int>(grid.x.size()) - 1;
    ny = static_cast<int>(grid.y.size()) - 1;
    need(nx >= 1 && ny >= 1 && nx <= 40 && ny <= 40, "Grid needs 1 to 40 bays in each direction");
    full = {0, nx, 0, ny};
    upper = inputs.contains("second_floor") ? rectangle(inputs.at("second_floor"), nx, ny) : full;
    storeys = integer(inputs, "storeys", defaults.at("storeys"), 1, 2);
    finish = num(inputs, "finish_thickness", defaults.at("finish_thickness"));
    need(finish >= 0 && finish < 1000, "finish_thickness must be nonnegative and below 1000");
    stock = num(inputs, "stock_length", preset("stock_length"));
    need(stock >= 100, "stock_length must be at least 100 mm");
    roof = inputs.value("roof", defaults.at("roof").get<std::string>());
    need(roof == "gable" || roof == "shed", "roof must be gable or shed");
    const auto axis = inputs.value("ridge_axis", defaults.at("ridge_axis").get<std::string>());
    need(axis == "x" || axis == "y", "ridge_axis must be x or y");
    axisX = axis == "x";
    pitch = num(inputs, "roof_pitch", preset("roof_pitch")) / 10;
    if (inputs.contains("roof_angle")) {
      const double angle = num(inputs, "roof_angle", 0);
      need(angle > 0 && angle < 75, "roof_angle must be greater than 0 and below 75 degrees");
      pitch = std::tan(angle * std::acos(-1.0) / 180);
    }
    need(pitch > 0 && pitch < 4, "roof_pitch must be greater than 0 and below 40 sun");
    over = num(inputs, "eave_projection", preset("eave_projection"));
    gableOver = num(inputs, "gable_projection", preset("gable_projection"));
    need(over >= 0 && over <= 10000 && gableOver >= 0 && gableOver <= 10000,
         "Roof projections must be 0 to 10000 mm, measured from column axes");
    addLevel("gl", "GL", gridWorld.TranslationPart().Z());
    addLevel("foundation_top", "基礎天端",
             height("gl") + num(inputs, "foundation_top", preset("foundation_top")));
    addLevel("sill_top", "土台天端",
             height("foundation_top") + preset("sill_packing") + section("sill").depth);
    const auto floor1 = inputs.value(
        "first_floor_system",
        data.at("presets").at("entries").at("first_floor_system").at("value").get<std::string>());
    need(floor1 == "joist" || floor1 == "joistless", "first_floor_system must be joist or joistless");
    const double ply =
        data.at("sections")
            .at("entries")
            .at(data.at("roles").at("entries").at("floor_sheathing").at("default_section").get<std::string>())
            .at("thickness");
    addLevel("first_fl", "1FL",
             height("sill_top") + (floor1 == "joist" ? section("joist").depth : ply) + finish);
    if (storeys == 2)
      addLevel("second_fl", "2FL",
               height("first_fl") + num(inputs, "storey_height", preset("storey_height")));
    addLevel(
        "eave_top", "桁天端",
        height(storeys == 2 ? "second_fl" : "first_fl") + num(inputs, "eave_height", preset("eave_height")));
    need(height("gl") <= height("foundation_top") && height("foundation_top") < height("sill_top") &&
             height("sill_top") < height("eave_top"),
         "Foundation, sill and eave levels are not ordered");
    need(height("first_fl") >= height("sill_top") && height("first_fl") < height("eave_top"),
         "First floor level is outside the frame");
    if (storeys == 2)
      need(height("second_fl") > height("first_fl") && height("second_fl") < height("eave_top"),
           "Second floor level is outside the frame");
    if (finish == 0 && !inputs.value("levels", json::object()).contains("first_fl"))
      warnings.push_back("仕上げ厚は未決（0）。1FLは床組から算出した計画値です。");
    warnings.push_back(data.at("notice"));
  }
  void readOpenings() {
    for (const auto& host : inputs.value("opening_hosts", json::array())) {
      auto list = modeler.openings(host.get<std::string>());
      need(list.ok(), list.error());
      const auto* wall = modeler.document().find(host.get<std::string>());
      const auto reference = wall ? construction::wallReferenceLine(*wall, modeler.document()) : std::nullopt;
      if (!reference && !list->empty())
        warnings.push_back("開口参照壁の基準線は未評価です（手編集・任意形状）: " + host.get<std::string>());
      for (const auto& o : *list) {
        need(std::abs(o.normal.Z()) < 1e-6, "Only vertical wall openings can be framed");
        gp_Dir along = gp::DZ().Crossed(o.normal);
        gp_Pnt worldCenter = o.center;
        if (reference && std::abs(reference->Direction().Dot(o.normal)) < 1e-6) {
          const double offset = gp_Vec(reference->Location(), o.center).Dot(gp_Vec(o.normal));
          worldCenter.Translate(gp_Vec(o.normal) * -offset);
        }
        gp_Pnt center = worldCenter.Transformed(gridWorld.Inverted());
        gp_Dir direction = along.Transformed(gridWorld.Inverted());
        const int floor = storeys == 2 && o.bottom >= height("second_fl") - 1e-6 ? 2 : 1;
        openings.push_back({center, direction, o.along, o.bottom, o.bottom + o.height, floor,
                            host.get<std::string>(), o.index});
      }
    }
  }
  void stud(const gp_Pnt& location, int floor, const std::string& base, const std::string& top, double bottom,
            double ceiling, const std::string& key) {
    std::vector<std::pair<double, double>> spans{{bottom, ceiling}};
    for (const auto& o : openings)
      if (o.storey == floor) {
        const gp_Vec d(o.center, location);
        const gp_Vec along(o.along);
        const double dot = d.Dot(along), cross = d.X() * along.Y() - d.Y() * along.X();
        if (std::abs(cross) > 1 || std::abs(dot) > o.width / 2 + 1e-6) continue;
        std::vector<std::pair<double, double>> next;
        for (const auto& s : spans) {
          if (s.first < o.bottom)
            next.emplace_back(s.first, std::min(s.second, o.bottom - section("window_sill").depth));
          if (s.second > o.top + section("lintel").depth)
            next.emplace_back(std::max(s.first, o.top + section("lintel").depth), s.second);
        }
        spans = std::move(next);
      }
    int n = 0;
    for (const auto& s : spans)
      if (s.second - s.first > 1) {
        gp_Pnt a = location, b = location;
        a.SetZ(s.first);
        b.SetZ(s.second);
        member("stud", key + "/" + std::to_string(n++), a, b, base, top, floor == 1 ? "1階軸組" : "2階軸組",
               0);
      }
  }
  void verticals() {
    std::set<std::pair<int, int>> points;
    for (int i = 0; i <= nx; ++i)
      for (int j = 0; j <= ny; ++j)
        if (full.boundary(i, j) || (storeys == 2 && upper.boundary(i, j))) points.emplace(i, j);
    for (const auto& node : inputs.value("columns", json::array())) {
      need(node.is_array() && node.size() == 2 && node[0].is_number_integer() && node[1].is_number_integer(),
           "columns needs grid index pairs");
      const int i = node[0], j = node[1];
      need(full.contains(i, j), "Column is outside the frame");
      points.emplace(i, j);
    }
    for (const auto& [i, j] : points) {
      const bool upperColumn = storeys == 2 && upper.contains(i, j);
      const bool through = upperColumn && upper.boundary(i, j);
      const std::string key = "column/" + std::to_string(i) + "/" + std::to_string(j);
      const std::string end = storeys == 1 || through ? "eave_top" : "second_fl";
      const double ceiling =
          (upperColumn && !through ? supportTop(end) : height(end)) -
          (storeys == 1 || through ? section("eave_beam").depth : section("floor_beam").depth);
      member(through ? "through_column" : "column", key + "/1", at(i, j, height("sill_top")),
             at(i, j, ceiling), "sill_top", end, "1階軸組", 0);
      hardware("column_connection", key + "/foot", at(i, j, height("sill_top")), "sill_top", "1階軸組");
      hardware("column_connection", key + "/head", at(i, j, ceiling), end, "1階軸組");
      if (upperColumn && !through) {
        member("column", key + "/2", at(i, j, supportTop("second_fl")),
               at(i, j, height("eave_top") - section("eave_beam").depth), "second_fl", "eave_top", "2階軸組",
               0);
        hardware("column_connection", key + "/2/foot", at(i, j, supportTop("second_fl")), "second_fl", "2階軸組");
        hardware("column_connection", key + "/2/head",
                 at(i, j, height("eave_top") - section("eave_beam").depth), "eave_top", "2階軸組");
      }
    }
    for (int floor = 1; floor <= storeys; ++floor) {
      const Rect area = floor == 1 ? full : upper;
      const std::string base = floor == 1 ? "sill_top" : "second_fl",
                        top = floor == 1 && storeys == 2 ? "second_fl" : "eave_top";
      const double z = height(base),
                   h = supportTop(top) - section(top == "second_fl" ? "floor_beam" : "eave_beam").depth;
      for (int i = area.x0; i < area.x1; ++i)
        for (int j : {area.y0, area.y1})
          stud(gp_Pnt((x(i) + x(i + 1)) / 2, y(j), z), floor, base, top, z, h,
               "stud/" + std::to_string(floor) + "/x/" + std::to_string(i) + "/" + std::to_string(j));
      for (int j = area.y0; j < area.y1; ++j)
        for (int i : {area.x0, area.x1})
          stud(gp_Pnt(x(i), (y(j) + y(j + 1)) / 2, z), floor, base, top, z, h,
               "stud/" + std::to_string(floor) + "/y/" + std::to_string(i) + "/" + std::to_string(j));
    }
    int n = 0;
    for (const auto& o : openings) {
      const auto sec = section("lintel");
      const gp_Vec half = gp_Vec(o.along) * (o.width / 2);
      gp_Pnt a = o.center.Translated(-half), b = o.center.Translated(half);
      a.SetZ(o.top + sec.depth);
      b.SetZ(a.Z());
      const std::string ref = o.storey == 2 ? "second_fl" : "first_fl",
                        stage = o.storey == 2 ? "2階軸組" : "1階軸組";
      member("lintel", "opening/" + std::to_string(n) + "/head", a, b, ref, ref, stage, 1, sec);
      a.SetZ(o.bottom);
      b.SetZ(o.bottom);
      member("window_sill", "opening/" + std::to_string(n++) + "/sill", a, b, ref, ref, stage);
    }
  }
  void ring(const Rect& a, const std::string& role, const std::string& ref, double z,
            const std::string& stage, const std::string& prefix, bool bolts) {
    auto span = [&](int i, int j, int k, int l) {
      const std::string id = prefix + "/" + std::to_string(i) + "," + std::to_string(j) + "-" +
                             std::to_string(k) + "," + std::to_string(l);
      member(role, id, at(i, j, z), at(k, l, z), ref, ref, stage);
      if (bolts) hardware("anchor_bolt", id + "/hardware", at(i, j, z), ref, stage);
    };
    for (int i = a.x0; i < a.x1; ++i)
      for (int j : {a.y0, a.y1}) span(i, j, i + 1, j);
    for (int j = a.y0; j < a.y1; ++j)
      for (int i : {a.x0, a.x1}) span(i, j, i, j + 1);
  }
  // Interior sills: explicit grid runs, and (when asked) the runs under first-storey braces
  // on interior grid lines, so a bearing wall inside the house sits on a sill (and a
  // foundation can follow it). Each bay is one member, like the perimeter ring.
  void interiorSills() {
    auto run = [&](const json& from, const json& to, bool explicitRun) {
      need(from.is_array() && to.is_array() && from.size() == 2 && to.size() == 2 && from[0].is_number_integer() &&
               from[1].is_number_integer() && to[0].is_number_integer() && to[1].is_number_integer(),
           "interior_sills need grid index pairs from:[i,j], to:[i,j]");
      int i = from[0], j = from[1], u = to[0], v = to[1];
      need(full.contains(i, j) && full.contains(u, v) && (i == u || j == v) && (i != u || j != v),
           "An interior sill must lie on one grid line inside the frame");
      const bool alongX = j == v;
      const bool onBoundary = alongX ? (j == full.y0 || j == full.y1) : (i == full.x0 || i == full.x1);
      if (onBoundary) {
        need(!explicitRun, "The perimeter already has sills; give interior grid runs only");
        return;
      }
      if (alongX) { if (i > u) std::swap(i, u); for (int k = i; k < u; ++k) innerSills.insert({k, j, k + 1, j}); }
      else { if (j > v) std::swap(j, v); for (int k = j; k < v; ++k) innerSills.insert({i, k, i, k + 1}); }
    };
    for (const auto& s : inputs.value("interior_sills", json::array())) {
      need(s.is_object() && s.contains("from") && s.contains("to"), "interior_sills entries need from and to");
      run(s.at("from"), s.at("to"), true);
    }
    const bool underBraces = inputs.value("sills_under_braces", false);
    std::size_t missing = 0;
    for (const auto& brace : inputs.value("braces", json::array())) {
      if (integer(brace, "storey", 1, 1, storeys) != 1) continue;
      if (underBraces) { run(brace.at("from"), brace.at("to"), false); continue; }
      const json a = brace.at("from"), b = brace.at("to");
      const int i = a[0], j = a[1], u = b[0], v = b[1];
      const bool boundary = j == v ? (j == full.y0 || j == full.y1) : (i == full.x0 || i == full.x1);
      if (boundary) continue;
      bool covered = true;
      if (j == v) { for (int k = std::min(i, u); k < std::max(i, u); ++k) covered = covered && innerSills.count({k, j, k + 1, j}); }
      else { for (int k = std::min(j, v); k < std::max(j, v); ++k) covered = covered && innerSills.count({i, k, i, k + 1}); }
      missing += !covered;
    }
    if (missing)
      warnings.push_back("1階の内部の筋かい壁 " + std::to_string(missing) +
                         " か所の下に土台がありません。sills_under_braces か interior_sills で土台を入れると基礎計画の対象になります。");
    for (const auto& bay : innerSills) {
      const std::string id = "sill/inner/" + std::to_string(bay[0]) + "," + std::to_string(bay[1]) + "-" +
                             std::to_string(bay[2]) + "," + std::to_string(bay[3]);
      member("sill", id, at(bay[0], bay[1], height("sill_top")), at(bay[2], bay[3], height("sill_top")), "sill_top",
             "sill_top", "基礎・土台");
      hardware("anchor_bolt", id + "/hardware", at(bay[0], bay[1], height("sill_top")), "sill_top", "基礎・土台");
    }
  }
  void floors() {
    ring(full, "sill", "sill_top", height("sill_top"), "基礎・土台", "sill", true);
    interiorSills();
    const double ply =
        data.at("sections")
            .at("entries")
            .at(data.at("roles").at("entries").at("floor_sheathing").at("default_section").get<std::string>())
            .at("thickness");
    for (int floor = 1; floor <= storeys; ++floor) {
      const Rect area = floor == 1 ? full : upper;
      const std::string ref = floor == 1 ? "first_fl" : "second_fl",
                        stage = floor == 1 ? "1階床組" : "2階床組";
      const std::string system =
          inputs.value(floor == 1 ? "first_floor_system" : "second_floor_system",
                       data.at("presets")
                           .at("entries")
                           .at(floor == 1 ? "first_floor_system" : "second_floor_system")
                           .at("value")
                           .get<std::string>());
      need(system == "joist" || system == "joistless", "Floor system must be joist or joistless");
      const double beamZ = height(ref) - finish - (system == "joistless" ? ply : section("joist").depth);
      if (floor == 2) ring(area, "rim_beam", ref, beamZ, stage, "floor2/rim", false);
      for (int i = (floor == 1 ? area.y0 + 1 : area.x0 + 1); i < (floor == 1 ? area.y1 : area.x1);
           i += (floor == 1 ? 2 : 1)) {
        const auto role = floor == 1 ? "floor_girder" : "floor_beam";
        // A first-floor girder on the line of an interior sill gives way to it: the girder
        // is split into the runs between sill bays, and the sill carries the floor there.
        if (floor != 1) {
          member(role, "floor/" + std::to_string(floor) + "/beam/" + std::to_string(i), at(i, area.y0, beamZ),
                 at(i, area.y1, beamZ), ref, ref, stage);
          continue;
        }
        auto sillBay = [&](int k) { return innerSills.count({k, i, k + 1, i}) > 0; };
        std::vector<std::pair<int, int>> runs;  // girder runs between sill bays, in grid indices
        for (int k = area.x0; k < area.x1;) {
          if (sillBay(k)) { ++k; continue; }
          const int start = k;
          while (k < area.x1 && !sillBay(k)) ++k;
          runs.push_back({start, k});
        }
        const bool split = !(runs.size() == 1 && runs[0].first == area.x0 && runs[0].second == area.x1);
        for (const auto& [a, b] : runs) {
          // Unchanged key when nothing is split, so existing frames regenerate the same parts.
          const std::string key = "floor/1/beam/" + std::to_string(i) +
                                  (split ? "/" + std::to_string(a) + "-" + std::to_string(b) : std::string());
          member(role, key, at(a, i, beamZ), at(b, i, beamZ), ref, ref, stage);
        }
        auto postAt = [&](int j) {
          if (!split) return true;
          if (sillBay(j - 1) || sillBay(j)) return false;  // the girder end rests on the sill
          for (const auto& [a, b] : runs) if (j >= a && j <= b) return true;
          return false;
        };
        for (int j = area.x0; j <= area.x1; j += 2) {
            if (!postAt(j)) continue;
            const auto& entry = data.at("roles").at("entries").at("floor_post");
            const std::string id = inputs.value("sections", json::object())
                                       .value("floor_post", entry.at("default_section").get<std::string>());
            if (id == "STEEL-POST-UNSELECTED")
              hardware("floor_post", "floor/post/" + std::to_string(i) + "/" + std::to_string(j),
                       at(j, i, (height("foundation_top") + beamZ - section(role).depth) / 2), ref, stage);
            else
              member("floor_post", "floor/post/" + std::to_string(i) + "/" + std::to_string(j),
                     at(j, i, height("foundation_top")), at(j, i, beamZ - section(role).depth),
                     "foundation_top", ref, stage, 0, construction::section(id, patch));
          }
      }
      struct Zone {
        Rect area;
        std::string mode;
      };
      std::vector<Zone> zones;
      for (const auto& z : inputs.value("floor_zones", json::array())) {
        if (integer(z, "storey", 1, 1, 2) != floor) continue;
        const Rect rect = rectangle(z, nx, ny);
        need(area.contains(rect.x0, rect.y0) && area.contains(rect.x1, rect.y1),
             "Floor zone is outside this storey");
        const std::string mode = z.at("system");
        need(mode == "joist" || mode == "joistless", "Unknown floor zone system");
        zones.push_back({rect, mode});
      }
      // Plan every grid cell once. Boundaries get bearing members; mixed
      // systems keep their own depth, with the resulting step recorded.
      for (int i = area.x0; i < area.x1; ++i)
        for (int j = area.y0; j < area.y1; ++j) {
          std::string mode = system;
          int covered = 0;
          for (const auto& z : zones)
            if (i >= z.area.x0 && i < z.area.x1 && j >= z.area.y0 && j < z.area.y1) {
              mode = z.mode;
              ++covered;
            }
          need(covered <= 1, "Floor zones must not overlap");
          const std::string key =
              "floor/" + std::to_string(floor) + "/cell/" + std::to_string(i) + "/" + std::to_string(j);
          if (mode == "joistless") {
            geom::Section s;
            s.width = x(i + 1) - x(i);
            s.depth = ply;
            const double mid = (x(i) + x(i + 1)) / 2;
            member("floor_sheathing", key + "/ply", gp_Pnt(mid, y(j), height(ref) - finish),
                   gp_Pnt(mid, y(j + 1), height(ref) - finish), ref, ref, stage, 1, s);
          }
        }
      auto cellMode = [&](int i, int j) {
        for (const auto& zone : zones)
          if (i >= zone.area.x0 && i < zone.area.x1 && j >= zone.area.y0 && j < zone.area.y1)
            return zone.mode;
        return system;
      };
      for (int i = area.x0; i < area.x1; ++i) {
        const auto ps =
            positions(x(i), x(i + 1),
                      integer(inputs, "joist_divisions", defaults.at("joist_divisions"), 1, 12), "joist");
        int first = area.y0;
        while (first < area.y1) {
          if (cellMode(i, first) != "joist") {
            ++first;
            continue;
          }
          int last = first + 1;
          while (last < area.y1 && cellMode(i, last) == "joist") ++last;
          const std::string key =
              "floor/" + std::to_string(floor) + "/joists/" + std::to_string(i) + "/" + std::to_string(first);
          for (std::size_t k = 0; k + 1 < ps.size(); ++k)
            member("joist", key + "/" + std::to_string(k), gp_Pnt(ps[k], y(first), height(ref) - finish),
                   gp_Pnt(ps[k], y(last), height(ref) - finish), ref, ref, stage);
          if (i + 1 == area.x1)
            member("joist", key + "/end", gp_Pnt(x(i + 1), y(first), height(ref) - finish),
                   gp_Pnt(x(i + 1), y(last), height(ref) - finish), ref, ref, stage);
          first = last;
        }
      }
      // Second-floor cross beams frame into the floor beams one bay at a time (they end at
      // the beams instead of crossing them at the same height).
      if (floor == 2)
        for (int j = area.y0 + 1; j < area.y1; ++j)
          for (int i = area.x0; i < area.x1; ++i)
            member("floor_beam", "floor2/cross/" + std::to_string(j) + "/" + std::to_string(i), at(i, j, beamZ),
                   at(i + 1, j, beamZ), ref, ref, stage);
      for (std::size_t n = 0; n < zones.size(); ++n) {
        ring(zones[n].area, "floor_girder", ref,
             height(ref) - finish - (zones[n].mode == "joistless" ? ply : section("joist").depth), stage,
             "floor/" + std::to_string(floor) + "/zone/" + std::to_string(n), false);
        warnings.push_back("床区域 " + std::to_string(floor) + "階：" + zones[n].mode +
                           "。境界受け材を生成。床組厚差=" + keyNumber(ply - section("joist").depth) +
                           " mm（同じ計画FL）。");
      }
      const double d = std::min({module, x(area.x1) - x(area.x0), y(area.y1) - y(area.y0)}) * .7;
      int k = 0;
      for (int i : {area.x0, area.x1})
        for (int j : {area.y0, area.y1}) {
          const double sx = i == area.x0 ? 1 : -1, sy = j == area.y0 ? 1 : -1;
          member("horizontal_brace",
                 "floor/" + std::to_string(floor) + "/horizontal_brace/" + std::to_string(k++),
                 gp_Pnt(x(i) + sx * d, y(j), beamZ), gp_Pnt(x(i), y(j) + sy * d, beamZ), ref, ref, stage);
        }
    }
  }
  void floorOpenings() {
    const auto requested = inputs.value("floor_openings", json::array());
    need(requested.is_array() && requested.size() <= 100, "floor_openings needs at most 100 rectangles");
    struct Void {
      int storey;
      Rect area;
    };
    std::vector<Void> holes;
    for (const auto& entry : requested) {
      const int floor = integer(entry, "storey", 2, 1, storeys);
      const Rect rect = rectangle(entry, nx, ny), allowed = floor == 1 ? full : upper;
      need(allowed.contains(rect.x0, rect.y0) && allowed.contains(rect.x1, rect.y1),
           "Floor opening is outside this storey");
      for (const auto& other : holes)
        need(other.storey != floor || std::max(rect.x0, other.area.x0) >= std::min(rect.x1, other.area.x1) ||
                 std::max(rect.y0, other.area.y0) >= std::min(rect.y1, other.area.y1),
             "Floor openings must not overlap");
      holes.push_back({floor, rect});
    }
    if (holes.empty()) return;
    auto floorOf = [](const Piece& p) { return p.stage == "1階床組" ? 1 : p.stage == "2階床組" ? 2 : 0; };
    // Clip design axes, not B-reps: retain shared, editable member recipes.
    // A beam on an aperture boundary remains an edge support. The opening is
    // specified between grid axes; its finished clear width is not certified.
    auto intervals = [&](const Piece& p) {
      std::vector<std::pair<double, double>> spans{{0, 1}};
      for (const auto& hole : holes) {
        if (hole.storey != floorOf(p)) continue;
        double first = 0, last = 1;
        bool crosses = true;
        const double a[] = {p.from.X(), p.from.Y()}, b[] = {p.to.X(), p.to.Y()};
        const double lo[] = {x(hole.area.x0), y(hole.area.y0)}, hi[] = {x(hole.area.x1), y(hole.area.y1)};
        for (int axis = 0; axis < 2; ++axis) {
          const double delta = b[axis] - a[axis];
          if (std::abs(delta) < 1e-9) {
            if (a[axis] <= lo[axis] + 1e-6 || a[axis] >= hi[axis] - 1e-6) crosses = false;
          } else {
            double t0 = (lo[axis] - a[axis]) / delta, t1 = (hi[axis] - a[axis]) / delta;
            if (t0 > t1) std::swap(t0, t1);
            first = std::max(first, t0);
            last = std::min(last, t1);
          }
        }
        if (!crosses || last <= first + 1e-9) continue;
        std::vector<std::pair<double, double>> remaining;
        for (const auto& span : spans) {
          if (last <= span.first || first >= span.second)
            remaining.push_back(span);
          else {
            if (first > span.first + 1e-9) remaining.emplace_back(span.first, first);
            if (last < span.second - 1e-9) remaining.emplace_back(last, span.second);
          }
        }
        spans = std::move(remaining);
      }
      return spans;
    };
    const auto original = std::move(pieces);
    pieces.clear();
    keys.clear();
    std::set<std::string> clipped;
    for (const auto& p : original) {
      if (!floorOf(p) || (!p.hardware.empty() && p.role != "floor_post")) continue;
      const auto spans = intervals(p);
      if (spans.size() != 1 || spans[0].first != 0 || spans[0].second != 1) clipped.insert(p.key);
    }
    for (const auto& p : original) {
      if (clipped.count(p.key)) {
        if (p.role == "horizontal_brace" || !p.hardware.empty()) continue;
        int n = 0;
        for (const auto& span : intervals(p)) {
          Piece segment = p;
          segment.from = p.from.Translated(gp_Vec(p.from, p.to) * span.first);
          segment.to = p.from.Translated(gp_Vec(p.from, p.to) * span.second);
          if (segment.from.Distance(segment.to) <= tolerance::kMinDimension) continue;
          segment.key += "/opening/" + std::to_string(n++);
          add(segment);
          const auto ref = floorOf(p) == 1 ? "first_fl" : "second_fl";
          if (p.role == "floor_beam" || p.role == "rim_beam") {
            hardware("beam_end_plate", segment.key + "/fitting/start", segment.from, ref, p.stage);
            hardware("beam_end_plate", segment.key + "/fitting/end", segment.to, ref, p.stage);
          }
          const double length = segment.from.Distance(segment.to);
          for (double at = stock; at < length - 1e-6; at += stock)
            hardware("joint", segment.key + "/joint/" + keyNumber(at),
                     segment.from.Translated(gp_Vec(segment.from, segment.to) * (at / length)), ref, p.stage);
        }
        continue;
      }
      if (!p.hardware.empty()) {
        const auto fitting = p.key.find("/fitting/"), joint = p.key.find("/joint/");
        const auto end = fitting != std::string::npos ? fitting : joint;
        if (end != std::string::npos && clipped.count(p.key.substr(0, end))) continue;
      }
      add(p);
    }
    for (std::size_t n = 0; n < holes.size(); ++n) {
      const auto& hole = holes[n];
      const auto& a = hole.area;
      const std::string ref = hole.storey == 1 ? "first_fl" : "second_fl";
      const std::string stage = hole.storey == 1 ? "1階床組" : "2階床組";
      const std::string mode =
          inputs.value(hole.storey == 1 ? "first_floor_system" : "second_floor_system",
                       data.at("presets")
                           .at("entries")
                           .at(hole.storey == 1 ? "first_floor_system" : "second_floor_system")
                           .at("value")
                           .get<std::string>());
      const double ply = data.at("sections")
                             .at("entries")
                             .at(data.at("roles")
                                     .at("entries")
                                     .at("floor_sheathing")
                                     .at("default_section")
                                     .get<std::string>())
                             .at("thickness");
      const double z = height(ref) - finish - (mode == "joistless" ? ply : section("joist").depth);
      const gp_Pnt corners[] = {at(a.x0, a.y0, z), at(a.x1, a.y0, z), at(a.x1, a.y1, z), at(a.x0, a.y1, z)};
      for (int edge = 0; edge < 4; ++edge) {
        const auto start = corners[edge], end = corners[(edge + 1) % 4];
        const bool alongX = std::abs(end.Y() - start.Y()) < 1e-6;
        const double low = alongX ? std::min(start.X(), end.X()) : std::min(start.Y(), end.Y());
        const double high = alongX ? std::max(start.X(), end.X()) : std::max(start.Y(), end.Y());
        std::vector<std::pair<double, double>> gaps{{low, high}};
        for (const auto& existing : pieces) {
          if (existing.stage != stage || !existing.hardware.empty() ||
              (existing.role != "floor_beam" && existing.role != "rim_beam" &&
               existing.role != "floor_girder") ||
              std::abs(existing.from.Z() - z) > 1e-6 || std::abs(existing.to.Z() - z) > 1e-6)
            continue;
          if (alongX ? (std::abs(existing.from.Y() - start.Y()) > 1e-6 ||
                        std::abs(existing.to.Y() - start.Y()) > 1e-6)
                     : (std::abs(existing.from.X() - start.X()) > 1e-6 ||
                        std::abs(existing.to.X() - start.X()) > 1e-6))
            continue;
          const double e0 = alongX ? existing.from.X() : existing.from.Y(),
                       e1 = alongX ? existing.to.X() : existing.to.Y();
          std::vector<std::pair<double, double>> remaining;
          for (const auto& gap : gaps) {
            if (std::max(e0, e1) <= gap.first || std::min(e0, e1) >= gap.second)
              remaining.push_back(gap);
            else {
              if (std::min(e0, e1) > gap.first + 1e-6) remaining.emplace_back(gap.first, std::min(e0, e1));
              if (std::max(e0, e1) < gap.second - 1e-6) remaining.emplace_back(std::max(e0, e1), gap.second);
            }
          }
          gaps = std::move(remaining);
        }
        int part = 0;
        for (const auto& gap : gaps)
          member(hole.storey == 1 ? "floor_girder" : "floor_beam",
                 "floor/opening/" + std::to_string(hole.storey) + "/" + std::to_string(n) + "/edge/" +
                     std::to_string(edge) + "/" + std::to_string(part++),
                 alongX ? gp_Pnt(gap.first, start.Y(), z) : gp_Pnt(start.X(), gap.first, z),
                 alongX ? gp_Pnt(gap.second, start.Y(), z) : gp_Pnt(start.X(), gap.second, z), ref, ref,
                 stage);
      }
      warnings.push_back(std::to_string(hole.storey) +
                         "階の床開口：通り芯間の概略範囲。受け梁の断面・接合・柱への荷重伝達と仕上げ後の有効"
                         "寸法は未照査。保持した編集部材・柱が開口内に残る場合は別途確認してください。");
    }
  }
  void roofArea(const Rect& area, const std::string& ref, const std::string& kind, const std::string& prefix,
                std::optional<bool> orientation = std::nullopt, bool reverse = false) {
    const bool alongX = orientation.value_or(axisX);
    const std::string stage = "小屋組";
    const double z = height(ref);
    ring(area, "eave_beam", ref, z, stage, prefix + "/eaves", false);
    // Ties on the interior lines only: the end lines already have the eave beams of the ring.
    for (int i = (alongX ? area.x0 : area.y0) + 1; i < (alongX ? area.x1 : area.y1); ++i)
      member("roof_beam", prefix + "/tie/" + std::to_string(i),
             alongX ? at(i, area.y0, z) : at(area.x0, i, z), alongX ? at(i, area.y1, z) : at(area.x1, i, z),
             ref, ref, stage);
    const double a0 = alongX ? x(area.x0) : y(area.y0), a1 = alongX ? x(area.x1) : y(area.y1);
    const double c0 = alongX ? y(area.y0) : x(area.x0), c1 = alongX ? y(area.y1) : x(area.x1),
                 middle = (c0 + c1) / 2;
    auto roofZ = [&](double cross) {
      return z + pitch * (kind == "gable" ? (middle - c0) - std::abs(cross - middle)
                                          : (reverse ? c1 - cross : cross - c0));
    };
    auto p = [&](double along, double cross, double h) {
      return alongX ? gp_Pnt(along, cross, h) : gp_Pnt(cross, along, h);
    };
    std::vector<double> crosses;
    if (alongX)
      for (int j = area.y0; j <= area.y1; ++j) crosses.push_back(y(j));
    else
      for (int i = area.x0; i <= area.x1; ++i) crosses.push_back(x(i));
    if (kind == "gable") crosses.push_back(middle);
    std::sort(crosses.begin(), crosses.end());
    crosses.erase(std::unique(crosses.begin(), crosses.end()), crosses.end());
    int k = 0;
    for (double cross : crosses) {
      const std::string role = kind == "gable" && std::abs(cross - middle) < 1e-6 ? "ridge" : "purlin";
      const std::string key = prefix + "/purlin/" + std::to_string(k++);
      // On an eave line the eave beam already carries the rafters at this height: a purlin
      // there would be a duplicate in the same place. (The key number is still used up, so
      // the other purlins keep their keys.)
      if (role == "purlin" && roofZ(cross) <= z + 1e-6) continue;
      member(role, key, p(a0 - gableOver, cross, roofZ(cross)), p(a1 + gableOver, cross, roofZ(cross)), ref,
             ref, stage);
      const auto& alongLines = alongX ? grid.x : grid.y;
      for (const auto& axis : alongLines)
        if (axis.offset >= a0 - 1e-6 && axis.offset <= a1 + 1e-6) {
          const double top = roofZ(cross) - section(role).depth;
          if (top > z + 1)
            member("roof_post", key + "/post/" + axis.id, p(axis.offset, cross, z),
                   p(axis.offset, cross, top), ref, ref, stage, 0);
        }
    }
    const auto ps =
        positions(a0 - gableOver, a1 + gableOver,
                  integer(inputs, "rafter_divisions", defaults.at("rafter_divisions"), 1, 12), "rafter");
    // Rafters sit on the purlins, ridge and eave beams (whose tops follow roofZ): the rafter
    // axis is its top edge, lifted by its depth measured plumb, so its underside touches them.
    // Plus the rise of the slope across half the widest support, so the underside clears
    // the support's top on its downhill edge too (no overlap; the seat detail is not modelled).
    const double support = std::max({section("purlin").width, section("ridge").width, section("eave_beam").width});
    const double lift = section("rafter").depth * std::sqrt(1 + pitch * pitch) + pitch * support / 2;
    auto rafterZ = [&](double cross) { return roofZ(cross) + lift; };
    int n = 0;
    for (double along : ps) {
      const std::string key = prefix + "/rafter/" + std::to_string(n++);
      if (kind == "gable") {
        member("rafter", key + "/left", p(along, c0 - over, rafterZ(c0 - over)),
               p(along, middle, rafterZ(middle)), ref, ref, stage);
        member("rafter", key + "/right", p(along, middle, rafterZ(middle)),
               p(along, c1 + over, rafterZ(c1 + over)), ref, ref, stage);
      } else
        member("rafter", key, p(along, c0 - over, rafterZ(c0 - over)), p(along, c1 + over, rafterZ(c1 + over)),
               ref, ref, stage);
    }
  }
  void roofParts() {
    roofArea(storeys == 2 ? upper : full, "eave_top", roof, "roof/main");
    if (storeys == 2 && (upper.x0 != 0 || upper.y0 != 0 || upper.x1 != nx || upper.y1 != ny)) {
      std::vector<Rect> lower;
      if (upper.x0 > 0) lower.push_back({0, upper.x0, 0, ny});
      if (upper.x1 < nx) lower.push_back({upper.x1, nx, 0, ny});
      if (upper.y0 > 0) lower.push_back({upper.x0, upper.x1, 0, upper.y0});
      if (upper.y1 < ny) lower.push_back({upper.x0, upper.x1, upper.y1, ny});
      int i = 0;
      for (const auto& a : lower) {
        const bool side = a.x1 <= upper.x0 || a.x0 >= upper.x1;
        const bool reverse = side ? a.x0 >= upper.x1 : a.y0 >= upper.y1;
        roofArea(a, "second_fl", "shed", "roof/lower/" + std::to_string(i++), !side, reverse);
      }
      warnings.push_back(
          "下屋は区域ごとの片流れの概略形です。上階の壁・屋根との取り合いを調整してください。");
    }
  }
  void braces() {
    int k = 0;
    for (const auto& brace : inputs.value("braces", json::array())) {
      const int floor = integer(brace, "storey", 1, 1, storeys);
      const json a = brace.at("from"), b = brace.at("to");
      need(a.is_array() && b.is_array() && a.size() == 2 && b.size() == 2,
           "brace endpoints need grid index pairs");
      const int i = a[0], j = a[1], u = b[0], v = b[1];
      need(full.contains(i, j) && full.contains(u, v) && (i == u || j == v) && (i != u || j != v),
           "brace must be on one grid line");
      if (floor == 2)
        need(upper.contains(i, j) && upper.contains(u, v), "Upper brace is outside second floor");
      const std::string base = floor == 1 ? "sill_top" : "second_fl",
                        top = floor == 1 && storeys == 2 ? "second_fl" : "eave_top",
                        stage = floor == 1 ? "1階軸組" : "2階軸組";
      const auto from = at(i, j, height(base)), to = at(u, v, height(top) - section("floor_beam").depth);
      const std::string key = "brace/" + std::to_string(k++);
      member("brace", key, from, to, base, top, stage, 0);
      hardware("brace_plate", key + "/foot", from, base, stage);
      hardware("brace_plate", key + "/head", to, top, stage);
    }
  }
  void build() {
    prepareGrid();
    readOpenings();
    verticals();
    floors();
    floorOpenings();
    roofParts();
    braces();
  }
  void openingConflicts(const std::set<std::string>& suppressed) {
    for (auto& piece : pieces) {
      if (piece.role != "column" && piece.role != "through_column" && piece.role != "brace") continue;
      if (suppressed.count(piece.key)) continue;
      for (const auto& opening : openings) {
        const gp_Vec along(opening.along), delta(piece.from, piece.to);
        const gp_Vec start(opening.center, piece.from), endOffset(opening.center, piece.to);
        const auto across = [&](const gp_Vec& v) { return v.X() * along.Y() - v.Y() * along.X(); };
        if (std::abs(across(start)) > 1e-6 || std::abs(across(endOffset)) > 1e-6) continue;
        double first = 0, last = 1;
        const auto clip = [&](double a, double d, double low, double high) {
          low += 1e-6; high -= 1e-6;
          if (std::abs(d) < 1e-12) return a > low && a < high;
          double t0 = (low - a) / d, t1 = (high - a) / d;
          if (t0 > t1) std::swap(t0, t1);
          first = std::max(first, t0); last = std::min(last, t1);
          return first < last;
        };
        if (!clip(start.Dot(along), delta.Dot(along), -opening.width / 2, opening.width / 2) ||
            !clip(piece.from.Z(), delta.Z(), opening.bottom, opening.top)) continue;
        piece.attributes["opening_conflict"] = "design_axis";
        piece.attributes["opening_host"] = opening.host;
        piece.attributes["opening_index"] = std::to_string(opening.index);
        warnings.push_back("開口と柱・筋かいの設計芯が競合: " + piece.key + " / " + opening.host +
                           " 開口" + std::to_string(opening.index) +
                           "。支持条件を確認してください。部材は自動削除していません。");
        break;
      }
    }
  }
};
}  // namespace

Result<std::string> Modeler::generateFrame(const std::string& text) {
  using R = Result<std::string>;
  const auto began = std::chrono::steady_clock::now();
  try {
    json args = json::parse(text);
    need(args.is_object(), "Frame arguments must be an object");
    need(args.value("system", std::string("timber")) == "timber", "Only the timber recipe is implemented");
    need(args.value("recipe", std::string("post_beam_2storey")) == "post_beam_2storey",
         "Unknown timber recipe");
    if (args.contains("expected_revision"))
      need(args.at("expected_revision") == history_.revision(), "The model changed; preview the frame again");
    const std::string replacing = args.value("replace", std::string());
    const Object* oldRoot = replacing.empty() ? nullptr : document_.find(replacing);
    json previous = json::object();
    std::set<ObjectId> dirty, keep, oldIds;
    std::set<std::string> skipKeys;
    const bool preserve = args.value("on_modified", std::string("ask")) == "preserve";
    need(!args.contains("on_modified") || args.at("on_modified") == "ask" ||
             args.at("on_modified") == "preserve",
         "on_modified must be ask or preserve");
    if (!replacing.empty()) {
      need(oldRoot != nullptr, "Frame to replace was not found");
      const auto* group = std::get_if<GroupNode>(&oldRoot->recipe.base);
      need(group && !group->constructionFrame.empty(), "replace must name a generated frame group");
      previous = json::parse(group->constructionFrame);
      if (previous.contains("root_world")) {
        Object worldRoot = *oldRoot;
        worldRoot.transform = document_.worldTransform(*oldRoot);
        if (construction::frameFingerprint(worldRoot) != previous.at("root_world")) dirty.insert(replacing);
      } else if (!oldRoot->parent.empty()) {
        // Older nested-frame records did not retain the parent's world pose.
        // Without that evidence, preserve the existing frame rather than reset it.
        dirty.insert(replacing);
      }
      for (const auto& key : previous.value("suppressed_keys", json::array()))
        skipKeys.insert(key.get<std::string>());
      if (!skipKeys.empty() && !preserve)
        return R::failure(
            "requires_decision: 以前に保持・削除した生成物があります。保持を続けるなら on_modified: preserve "
            "を指定してください。");
      for (const auto& id : document_.subtree(replacing)) oldIds.insert(id);
      const auto& snapshots = previous.at("objects");
      for (const auto& [id, stamp] : snapshots.items()) {
        const auto* current = document_.find(id);
        if (!current || construction::frameFingerprint(*current) != stamp) dirty.insert(id);
      }
      for (const auto& id : oldIds)
        if (!snapshots.contains(id)) dirty.insert(id);
      for (const auto& [id, stamp] : previous.at("definitions").items()) {
        const auto* d = document_.findDefinition(id);
        if (!d || construction::frameDefinitionFingerprint(*d) != stamp) {
          for (const auto& oid : oldIds)
            if (const auto* o = document_.find(oid)) {
              const auto* instance = std::get_if<ComponentInstance>(&o->recipe.base);
              if (instance && instance->definitionId == id) dirty.insert(oid);
            }
        }
      }
      need(!dirty.count(replacing),
           "requires_decision: 架構全体のコンテナーが編集されています。元の架構を保持して、replace "
           "を省略し別に生成してください。");
      if (!dirty.empty() && !preserve) {
        json list = json::array();
        for (const auto& id : dirty) list.push_back(id);
        return R::failure(
            "requires_decision: 手で編集・追加・削除された生成物があります。保持するなら on_modified: "
            "preserve、別に作るなら replace を省略してください。対象=" +
            list.dump());
      }
      for (const auto& id : dirty) {
        const Object* o = document_.find(id);
        if (o)
          for (const auto& child : document_.subtree(id)) keep.insert(child);
        if (previous.value("keys", json::object()).contains(id))
          skipKeys.insert(previous.at("keys").at(id).get<std::string>());
      }
      for (const auto& id : keep) {
        const auto* o = document_.find(id);
        const auto it = o->attributes.find("frame_key");
        if (it != o->attributes.end()) skipKeys.insert(it->second);
      }
      json inputs = previous.at("inputs");
      if (args.contains("inputs")) {
        auto elevation = [&](const char* key) {
          const std::string id = previous.at("levels").at(key);
          for (const auto& l : document_.settings().levels)
            if (l.id == id) return l.elevation;
          throw std::runtime_error("A frame level is missing: " + std::string(key));
        };
        const auto& requested = args.at("inputs");
        for (const auto& [key, base, top] :
             {std::tuple<const char*, const char*, const char*>{"foundation_top", "gl", "foundation_top"},
              {"storey_height", "first_fl", "second_fl"},
              {"eave_height", previous.at("levels").contains("second_fl") ? "second_fl" : "first_fl",
               "eave_top"}}) {
          if (requested.contains(key) && previous.at("levels").contains(top))
            need(std::abs(num(requested, key, 0) - (elevation(top) - elevation(base))) < 1e-6,
                 "Existing frame heights follow levels. Change them with set_levels / レベル編集 before "
                 "regeneration");
        }
      }
      if (args.contains("inputs")) inputs.merge_patch(args.at("inputs"));
      // Existing level edits remain the planning heights on regeneration.
      // Existing heights are edited through setLevels, which also moves any
      // other objects attached to them; generation does not bypass that path.
      json bindings = previous.at("levels");
      if (inputs.contains("levels")) bindings.merge_patch(inputs.at("levels"));
      inputs["levels"] = bindings;
      args["inputs"] = inputs;
    }
    std::string name = oldRoot ? oldRoot->name : args.value("name", std::string());
    if (name.empty()) {
      int n = 1;
      do {
        name = "木造架構 " + std::to_string(n++);
      } while (std::any_of(document_.objects().begin(), document_.objects().end(),
                           [&](const Object& o) { return o.name == name; }));
    }
    Plan plan(*this, args, name, oldRoot != nullptr);
    plan.build();
    plan.openingConflicts(skipKeys);
    if (!keep.empty() && !plan.openings.empty())
      plan.warnings.push_back("保持した手編集部材と開口の関係は未評価です。生成計画の競合警告とは別に確認してください。");
    Object root;
    if (oldRoot)
      root = *oldRoot;
    else {
      root.id = generateObjectId();
      root.parent = activeGroup_;
      root.name = name;
      root.recipe.base = GroupNode{};
      if (!activeGroup_.empty())
        root.transform = document_.worldTransform(*document_.find(activeGroup_)).Inverted();
    }
    const gp_Trsf rootWorld = oldRoot ? document_.worldTransform(*oldRoot) : gp_Trsf();
    const gp_Trsf intoRoot = rootWorld.Inverted();
    std::vector<Object> created;
    std::set<ObjectId> reusedGrid;
    std::vector<ComponentDefinition> addedDefinitions;
    std::map<std::string, ComponentDefinition> definitions;
    std::map<std::string, ObjectId> stages;
    auto group = [&](const std::string& title) {
      Object g;
      g.id = generateObjectId();
      g.parent = root.id;
      g.name = title;
      g.layer = title;
      g.recipe.base = GroupNode{};
      created.push_back(g);
      return g.id;
    };
    for (const char* title : {"基礎・土台", "1階床組", "1階軸組", "2階床組", "2階軸組", "小屋組"}) {
      if (plan.storeys == 1 && std::string(title).rfind("2階", 0) == 0) continue;
      stages[title] = group(title);
    }
    if (!plan.gridId.empty() && oldIds.count(plan.gridId) && !keep.count(plan.gridId)) {
      for (const auto& id : document_.subtree(plan.gridId)) {
        reusedGrid.insert(id);
        created.push_back(*document_.find(id));
      }
    }
    if (!keep.empty()) {
      const ObjectId retained = group("手編集（保持）");
      for (const auto& id : keep) {
        Object o = *document_.find(id);
        if (!keep.count(o.parent)) {
          o.transform = intoRoot * document_.worldTransform(o);
          o.parent = retained;
        }
        created.push_back(std::move(o));
      }
    }
    if (plan.gridId.empty()) {
      Modeler scratch;
      std::vector<double> xs, ys;
      for (int i = 1; i <= plan.nx; ++i) xs.push_back(plan.x(i) - plan.x(i - 1));
      for (int j = 1; j <= plan.ny; ++j) ys.push_back(plan.y(j) - plan.y(j - 1));
      const auto grid = scratch.createGridLines(plan.gridWorld.TranslationPart(), xs, ys);
      need(grid.ok(), grid.error());
      plan.gridId = grid->grid;
      plan.grid = *scratch.gridSpec(grid->grid);
      for (const auto& o : scratch.document().objects()) {
        Object copy = o;
        if (copy.id == plan.gridId) {
          copy.parent = root.id;
          copy.transform = intoRoot * copy.transform;
          copy.name = name + " 通り芯";
        }
        copy.attributes["frame_key"] = "grid/" + copy.id;
        created.push_back(std::move(copy));
      }
    }
    // Definitions use stable content ids and are reused across roles/frames.
    // If a user edited a shared definition, never overwrite that definition.
    auto definition = [&](const Piece& p) -> const ComponentDefinition& {
      json identity;
      if (p.hardware.empty())
        identity = {{"section", {p.section.width, p.section.depth}},
                    {"kind", geom::toString(p.section.kind)},
                    {"catalog", p.catalog},
                    {"length", keyNumber(p.from.Distance(p.to))}};
      else
        identity = plan.data.at("hardware").at("entries").at(p.hardware);
      const std::string digest = codec::sha256Hex(identity.dump());
      const auto known = definitions.find(digest);
      if (known != definitions.end()) return known->second;
      const auto stable = [](const std::string& hash) {
        return hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-" + hash.substr(12, 4) + "-" +
               hash.substr(16, 4) + "-" + hash.substr(20, 12);
      };
      ComponentDefinition def;
      def.id = stable(digest);
      def.name = p.hardware.empty() ? "木材 " + keyNumber(p.section.width) + "×" +
                                          keyNumber(p.section.depth) + " L" + keyNumber(p.from.Distance(p.to))
                                    : identity.at("name").get<std::string>();
      auto addPart = [&](BaseFeature feature) {
        Object o;
        o.id = stable(codec::sha256Hex(digest + std::to_string(def.objects.size())));
        o.name = def.name;
        o.recipe.base = std::move(feature);
        def.objects.push_back(std::move(o));
      };
      if (p.hardware.empty()) {
        geom::Section s = p.section;
        s.label.clear();
        addPart(
            LinearMember{gp::Origin(), gp_Pnt(0, 0, p.from.Distance(p.to)), gp::DY(), 0, s, {}, s.catalog});
      } else {
        const std::string shape = identity.at("shape");
        if (shape == "marker") {
          const double h = identity.at("size").get<double>() / 2;
          addPart(PolylineCurve{{gp_Pnt(-h, 0, 0), gp_Pnt(h, 0, 0)}});
          addPart(PolylineCurve{{gp_Pnt(0, -h, 0), gp_Pnt(0, h, 0)}});
          addPart(PolylineCurve{{gp_Pnt(0, 0, -h), gp_Pnt(0, 0, h)}});
        } else {
          geom::Section s;
          double length;
          if (shape == "bolt") {
            s.kind = geom::Section::Kind::Circle;
            s.width = s.depth = identity.at("diameter");
            length = identity.at("length");
          } else {
            s.width = identity.at("width");
            s.depth = identity.at("thickness");
            length = identity.at("height");
          }
          addPart(LinearMember{gp::Origin(), gp_Pnt(0, 0, length), gp::DY(), 0, s, {}, {}});
        }
      }
      const auto* stored = document_.findDefinition(def.id);
      if (stored && construction::frameDefinitionFingerprint(*stored) ==
                        construction::frameDefinitionFingerprint(def)) {
        return definitions.emplace(digest, *stored).first->second;
      }
      if (stored) def.id = generateObjectId();
      for (auto& o : def.objects) {
        const Status built = rebuild(o);
        need(built.ok(), built.message());
      }
      const auto shape = buildDefinitionShape(def);
      need(shape.ok(), shape.error());
      def.shape = *shape;
      addedDefinitions.push_back(def);
      return definitions.emplace(digest, std::move(def)).first->second;
    };
    json allocation = json::object();
    if (previous.is_object() && previous.contains("allocation"))
      for (const auto& id : keep)
        if (previous.at("allocation").contains(id)) allocation[id] = previous.at("allocation").at(id);
    json counts = json::object();
    std::size_t hardwareCount = 0;
    for (const auto& p : plan.pieces) {
      if (skipKeys.count(p.key)) continue;
      const auto split = p.key.find("/opening/");
      if (split != std::string::npos && skipKeys.count(p.key.substr(0, split))) continue;
      const auto& def = definition(p);
      Object o;
      o.id = generateObjectId();
      o.parent = stages.at(p.stage);
      o.name = p.hardware.empty() ? (p.role + " " + p.key) : def.name;
      o.layer = p.layer;
      o.attributes = p.attributes;
      o.attributes["kind"] = p.role;
      o.attributes["material_class"] = p.hardware.empty()      ? "timber"
                                       : p.hardware == "joint" ? "timber"
                                                               : "steel";
      o.attributes["frame_key"] = p.key;
      if (p.hardware.empty()) o.attributes["member_anchor_up"] = std::to_string(p.anchorUp);
      o.recipe.base = ComponentInstance{def.id, {}, false};
      o.shape = def.shape;
      const gp_Pnt start = plan.world(p.from), end = plan.world(p.to);
      gp_Trsf placement;
      if (p.hardware.empty()) {
        const auto up = geom::memberUp(gp_Dir(gp_Vec(start, end)));
        need(up.ok(), up.error());
        const auto frame = geom::memberFrame(start, end, *up, 0);
        need(frame.ok(), frame.error());
        gp_Ax3 centered = *frame;
        centered.SetLocation(
            start.Translated(gp_Vec(frame->YDirection()) * (-p.anchorUp * p.section.depth / 2)));
        placement.SetDisplacement(gp_Ax3(), centered);
      } else {
        gp_Pnt location = start;
        const auto& h = plan.data.at("hardware").at("entries").at(p.hardware);
        if (p.hardware == "anchor_bolt") location.SetZ(start.Z() - h.at("length").get<double>() + 60);
        if (p.hardware == "beam_end_plate") location.SetZ(start.Z() - h.at("height").get<double>());
        placement.SetTranslation(gp_Vec(location.XYZ()));
        ++hardwareCount;
      }
      o.transform = intoRoot * placement;
      GridPin pin;
      pin.grid = plan.gridId;
      pin.startLevel = p.startLevel;
      pin.endLevel = p.endLevel;
      auto closest = [](const std::vector<GridAxis>& axes, double value) {
        return std::min_element(axes.begin(), axes.end(),
                                [&](const GridAxis& a, const GridAxis& b) {
                                  return std::abs(a.offset - value) < std::abs(b.offset - value);
                                })
            ->id;
      };
      pin.startX = closest(plan.grid.x, p.from.X());
      pin.endX = closest(plan.grid.x, p.to.X());
      pin.startY = closest(plan.grid.y, p.from.Y());
      pin.endY = closest(plan.grid.y, p.to.Y());
      o.gridPin = pin;
      if (p.role == "joist" || p.role == "rafter")
        allocation[o.id] = {
            {"start", {p.from.X(), p.from.Y()}}, {"end", {p.to.X(), p.to.Y()}}, {"kind", p.role}};
      counts[p.role] = counts.value(p.role, 0) + 1;
      created.push_back(std::move(o));
    }
    Object worldRoot = root;
    worldRoot.transform = rootWorld;
    json manifest = {{"version", 3},
                     {"allocation", allocation},
                     {"spacing", plan.spacing},
                     {"root_world", construction::frameFingerprint(worldRoot)},
                     {"recipe", "post_beam_2storey"},
                     {"inputs", plan.inputs},
                     {"levels", plan.levelIds},
                     {"grid", plan.gridId},
                     {"objects", json::object()},
                     {"definitions", json::object()},
                     {"keys", json::object()},
                     {"suppressed_keys", skipKeys}};
    manifest["objects"][root.id] = construction::frameFingerprint(root);
    for (const auto& o : created) {
      // Preserved edits stay foreign to this generation, so another rebuild
      // asks again instead of silently accepting them as fresh generated parts.
      if (!keep.count(o.id)) manifest["objects"][o.id] = construction::frameFingerprint(o);
      const auto key = o.attributes.find("frame_key");
      if (key != o.attributes.end()) manifest["keys"][o.id] = key->second;
      if (const auto* ref = std::get_if<ComponentInstance>(&o.recipe.base)) {
        const ComponentDefinition* d = nullptr;
        for (const auto& [key, def] : definitions)
          if (def.id == ref->definitionId) {
            d = &def;
            break;
          }
        if (!d) d = document_.findDefinition(ref->definitionId);
        if (d) manifest["definitions"][d->id] = construction::frameDefinitionFingerprint(*d);
      }
    }
    std::get<GroupNode>(root.recipe.base).constructionFrame = manifest.dump();
    Command command;
    command.name = "Generate Timber Frame";
    // Removals descending: undo re-inserts the old hierarchy in ascending order.
    std::vector<ObjectId> removed;
    for (const auto& id : oldIds)
      if (id != root.id && !keep.count(id) && !reusedGrid.count(id)) removed.push_back(id);
    std::sort(removed.begin(), removed.end(),
              [&](const auto& a, const auto& b) { return document_.indexOf(a) > document_.indexOf(b); });
    for (const auto& id : removed)
      command.deltas.push_back({id, *document_.find(id), std::nullopt, document_.indexOf(id)});
    command.deltas.push_back({root.id, oldRoot ? std::optional<Object>(*oldRoot) : std::nullopt, root,
                              oldRoot ? document_.indexOf(root.id) : std::nullopt});
    for (const auto& o : created) {
      const auto* beforeObject = document_.find(o.id);
      command.deltas.push_back({o.id, beforeObject ? std::optional<Object>(*beforeObject) : std::nullopt, o,
                                beforeObject ? document_.indexOf(o.id) : std::nullopt});
    }
    for (const auto& d : addedDefinitions) command.definitionDeltas.push_back({d.id, std::nullopt, d});
    // Remove only unused generated definitions; copies elsewhere keep theirs.
    if (!previous.empty())
      for (const auto& [id, stamp] : previous.at("definitions").items()) {
        (void)stamp;
        bool used = false;
        auto references = [&](const Object& o) {
          const auto* ref = std::get_if<ComponentInstance>(&o.recipe.base);
          return ref && ref->definitionId == id;
        };
        for (const auto& o : created) used |= references(o);
        for (const auto& o : document_.objects())
          if (!oldIds.count(o.id)) used |= references(o);
        if (!used)
          if (const auto* d = document_.findDefinition(id))
            command.definitionDeltas.push_back({id, *d, std::nullopt});
      }
    const auto before = PersistentSettings::of(document_.settings());
    auto after = before;
    after.levels = plan.levels;
    std::stable_sort(after.levels.begin(), after.levels.end(),
                     [](const FloorLevel& a, const FloorLevel& b) { return a.elevation < b.elevation; });
    if (after != before) command.settings = SettingsDelta{before, after};
    const std::string result =
        json{{"frame", root.id},
             {"grid", plan.gridId},
             {"levels", plan.levelIds},
             {"counts", counts},
             {"parts", plan.pieces.size()},
             {"shared_definitions", definitions.size()},
             {"new_definitions", addedDefinitions.size()},
             {"hardware", hardwareCount},
             {"preserved", keep.size()},
             {"warnings", plan.warnings},
             {"spacing", plan.spacing},
             {"generation_ms",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count()}}
            .dump();
    history_.execute(std::move(command));
    return result;
  } catch (const std::exception& e) {
    return R::failure(e.what());
  } catch (const Standard_Failure& e) {
    return R::failure(kernelMessage(e));
  }
}
}  // namespace sbr::doc
