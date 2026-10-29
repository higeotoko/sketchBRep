#include "sbr/doc/NativeFormat.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <sstream>

#include <nlohmann/json.hpp>
#include <TopExp_Explorer.hxx>

#include "sbr/Codec.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/ShapeIO.h"

namespace sbr::doc {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// --- primitive helpers ------------------------------------------------------
json toJson(const gp_Pnt& p) { return json::array({p.X(), p.Y(), p.Z()}); }
json toJson(const gp_Dir& d) { return json::array({d.X(), d.Y(), d.Z()}); }

gp_Pnt pointFrom(const json& j) {
  return gp_Pnt(j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>());
}
gp_Dir dirFrom(const json& j) {
  return gp_Dir(j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>());
}

json faceRefToJson(const geom::FaceRef& f) {
  json j = {{"normal", toJson(f.normal)}, {"centroid", toJson(f.centroid)}, {"area", f.area}};
  if (f.inside) j["inside"] = toJson(*f.inside);
  if (f.extent) j["extent"] = json::array({toJson(f.extent->first), toJson(f.extent->second)});
  return j;
}

geom::FaceRef faceRefFrom(const json& f) {
  geom::FaceRef ref;
  ref.normal = dirFrom(f.at("normal"));
  ref.centroid = pointFrom(f.at("centroid"));
  ref.area = f.value("area", 0.0);
  if (f.contains("inside")) ref.inside = pointFrom(f.at("inside"));
  if (f.contains("extent")) ref.extent = std::make_pair(pointFrom(f.at("extent").at(0)), pointFrom(f.at("extent").at(1)));
  return ref;
}

json surfaceRefToJson(const geom::SurfaceRef& f) {
  return {{"point", toJson(f.point)}, {"normal", toJson(f.normal)}, {"surfaceType", f.surfaceType}};
}

geom::SurfaceRef surfaceRefFrom(const json& f) {
  geom::SurfaceRef ref;
  ref.point = pointFrom(f.at("point"));
  ref.normal = dirFrom(f.at("normal"));
  ref.surfaceType = f.value("surfaceType", 0);
  return ref;
}


json toJson(const gp_Trsf& t) {
  json m = json::array();
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 4; ++c) m.push_back(t.Value(r, c));
  }
  return m;
}

gp_Trsf trsfFrom(const json& j) {
  gp_Trsf t;
  if (j.size() != 12) throw std::runtime_error("transform must have 12 values");
  auto v = [&](int i) { return j.at(static_cast<std::size_t>(i)).get<double>(); };
  t.SetValues(v(0), v(1), v(2), v(3), v(4), v(5), v(6), v(7), v(8), v(9), v(10), v(11));
  return t;
}

json pointsToJson(const std::vector<gp_Pnt>& points) {
  json arr = json::array();
  for (const gp_Pnt& p : points) arr.push_back(toJson(p));
  return arr;
}

std::vector<gp_Pnt> pointsFrom(const json& j) {
  std::vector<gp_Pnt> out;
  for (const json& p : j) out.push_back(pointFrom(p));
  return out;
}

BooleanKind booleanKindFrom(const std::string& s) {
  if (s == "Union") return BooleanKind::Union;
  if (s == "Subtract") return BooleanKind::Subtract;
  if (s == "Intersect") return BooleanKind::Intersect;
  throw std::runtime_error("unknown boolean kind: " + s);
}

// --- recipe ----------------------------------------------------------------
json recipeToJson(const Recipe& recipe);
Recipe recipeFrom(const json& j);

// Recipes are shared by the features using them (a part copied and joined
// with itself, again and again): one used in more than one place is written
// once in the file's "recipes" table and referred to by index, so the file
// does not grow with every path through the shared inputs (it doubled on
// each copy-and-join). Read back once and shared again on load.
struct RecipeWriter {
  bool counting = true;  // first pass: how often each recipe is used
  std::unordered_map<const Recipe*, int> uses;
  std::unordered_map<const Recipe*, std::size_t> index;
  json entries = json::array();
};
thread_local RecipeWriter* recipeWriter = nullptr;

struct RecipeReader {
  const json* entries = nullptr;
  std::vector<std::shared_ptr<const Recipe>> parsed;
};
thread_local RecipeReader* recipeReader = nullptr;

// The ["recipe", inline] or ["ref", index] member of an operand.
json sharedRecipe(const std::shared_ptr<const Recipe>& recipe) {
  RecipeWriter* w = recipeWriter;
  if (!w) return json::array({"recipe", recipeToJson(*recipe)});
  const Recipe* key = recipe.get();
  if (w->counting) {
    if (++w->uses[key] == 1) (void)recipeToJson(*recipe);  // its inputs, once
    return json::array({"recipe", nullptr});
  }
  if (w->uses[key] < 2) return json::array({"recipe", recipeToJson(*recipe)});
  auto it = w->index.find(key);
  if (it == w->index.end()) {
    it = w->index.emplace(key, w->entries.size()).first;
    w->entries.push_back(nullptr);  // the place, before its own inputs
    json written = recipeToJson(*recipe);
    w->entries[it->second] = std::move(written);
  }
  return json::array({"ref", it->second});
}

std::shared_ptr<const Recipe> sharedRecipeFrom(const json& operand) {
  if (!operand.contains("ref")) return std::make_shared<const Recipe>(recipeFrom(operand.at("recipe")));
  const std::size_t i = operand.at("ref").get<std::size_t>();
  RecipeReader* r = recipeReader;
  if (!r || !r->entries || i >= r->entries->size()) throw std::runtime_error("missing shared recipe");
  if (r->parsed.size() < r->entries->size()) r->parsed.resize(r->entries->size());
  if (!r->parsed[i]) r->parsed[i] = std::make_shared<const Recipe>(recipeFrom((*r->entries)[i]));
  return r->parsed[i];
}

json overridesToJson(const std::vector<ParameterOverride>& overrides) {
  json list = json::array();
  for (const ParameterOverride& p : overrides) list.push_back({{"object", p.object}, {"key", p.key}, {"value", p.value}});
  return list;
}

std::vector<ParameterOverride> overridesFrom(const json& j) {
  std::vector<ParameterOverride> out;
  for (const json& p : j) {
    out.push_back({p.at("object").get<std::string>(), p.at("key").get<std::string>(), p.at("value").get<double>()});
  }
  return out;
}

json baseToJson(const BaseFeature& base) {
  return std::visit(
      Overloaded{
          [](const RectangleProfile& r) {
            return json{{"type", "rectangle"},
                        {"plane",
                         {{"origin", toJson(r.plane.origin)},
                          {"normal", toJson(r.plane.normal)},
                          {"xDir", toJson(r.plane.xDir)}}},
                        {"width", r.width},
                        {"height", r.height}};
          },
          [](const CircleProfile& c) {
            return json{{"type", "circle"},
                        {"plane",
                         {{"origin", toJson(c.plane.origin)},
                          {"normal", toJson(c.plane.normal)},
                          {"xDir", toJson(c.plane.xDir)}}},
                        {"radius", c.radius}};
          },
          [](const RegularPolygonProfile& p) {
            return json{{"type", "regular_polygon"},
                        {"plane",
                         {{"origin", toJson(p.plane.origin)},
                          {"normal", toJson(p.plane.normal)},
                          {"xDir", toJson(p.plane.xDir)}}},
                        {"radius", p.radius},
                        {"sides", p.sides}};
          },
          [](const PolygonProfile& p) {
            return json{{"type", "polygon"}, {"points", pointsToJson(p.points)}};
          },
          [](const PolylineCurve& p) {
            return json{{"type", "polyline"}, {"points", pointsToJson(p.points)}};
          },
          [](const InterpolatedCurve& c) {
            return json{{"type", "curve"}, {"points", pointsToJson(c.points)}, {"closed", c.closed}};
          },
          [](const ArcCurve& a) {
            return json{{"type", "arc"},       {"start", toJson(a.start)}, {"end", toJson(a.end)},
                        {"normal", toJson(a.normal)}, {"bulge", a.bulge},  {"closed", a.closed}};
          },
          [](const LinearDimension& d) {
            return json{{"type", "dimension"}, {"start", toJson(d.start)}, {"end", toJson(d.end)},
                        {"offset", json::array({d.offset.X(), d.offset.Y(), d.offset.Z()})}, {"text", d.text}};
          },
          [](const GuideLine& g) {
            return json{{"type", "guide"}, {"point", toJson(g.point)}, {"direction", toJson(g.direction)}};
          },
          [](const ImageUnderlay& u) {
            return json{{"type", "image"},
                        {"image", u.image},
                        {"plane",
                         {{"origin", toJson(u.plane.origin)},
                          {"normal", toJson(u.plane.normal)},
                          {"xDir", toJson(u.plane.xDir)}}},
                        {"width", u.width},
                        {"height", u.height},
                        {"opacity", u.opacity}};
          },
          [](const TextNote& t) {
            json j = {{"type", "text"}, {"position", toJson(t.position)}, {"text", t.text}};
            if (t.target) j["target"] = toJson(*t.target);
            return j;
          },
          [](const ImportedShape& s) {
            if (recipeWriter && recipeWriter->counting) return json();  // nothing to count inside
            return json{{"type", "imported"},
                        {"source", s.source},
                        {"brep", geom::shapeToBRepString(s.shape)}};
          },
          [](const BooleanFeature& b) {
            json operands = json::array();
            for (const BooleanOperand& o : b.operands) {
              operands.push_back({{"name", o.name},
                                  {"placement", toJson(o.placement)},
                                  sharedRecipe(o.recipe)});
            }
            return json{{"type", "boolean"}, {"kind", toString(b.kind)}, {"operands", operands}};
          },
          [](const SewnFeature& s) {
            json faces = json::array();
            for (const BooleanOperand& o : s.faces) {
              faces.push_back({{"name", o.name}, {"placement", toJson(o.placement)}, sharedRecipe(o.recipe)});
            }
            return json{{"type", "sewn"}, {"faces", faces}};
          },
          [](const LoftFeature& l) {
            json sections = json::array();
            for (const BooleanOperand& o : l.sections) {
              sections.push_back({{"name", o.name},
                                  {"placement", toJson(o.placement)},
                                  sharedRecipe(o.recipe)});
            }
            return json{{"type", "loft"}, {"solid", l.solid}, {"sections", sections}};
          },
          [](const LoopProfile& l) {
            json curves = json::array();
            for (const BooleanOperand& o : l.curves) {
              curves.push_back({{"name", o.name}, {"placement", toJson(o.placement)}, sharedRecipe(o.recipe)});
            }
            json j = {{"type", "loop"}, {"curves", curves}};
            if (l.reversed) j["reversed"] = true;
            return j;
          },
          [](const FacePart& f) {
            json source = json::array();
            for (const BooleanOperand& o : f.source) {
              source.push_back({{"name", o.name}, {"placement", toJson(o.placement)}, sharedRecipe(o.recipe)});
            }
            return json{{"type", "facepart"}, {"source", source}, {"line", pointsToJson(f.line)}, {"inside", toJson(f.inside)}};
          },
          [](const SweepFeature& f) {
            json inputs = json::array();
            for (const BooleanOperand& o : f.inputs) {
              inputs.push_back({{"name", o.name},
                                {"placement", toJson(o.placement)},
                                sharedRecipe(o.recipe)});
            }
            return json{{"type", "sweep"}, {"inputs", inputs}};
          },
          [](const GroupNode& g) {
            json j = {{"type", "group"}};
            if (!g.editingDefinition.empty()) {
              j["editing"] = {{"definition", g.editingDefinition},
                              {"instance", g.editingInstance},
                              {"instanceName", g.editingInstanceName}};
              if (!g.editingOverrides.empty()) j["editing"]["overrides"] = overridesToJson(g.editingOverrides);
              if (g.editingMirrored) j["editing"]["mirrored"] = true;
            }
            return j;
          },
          [](const ComponentInstance& c) {
            json j = {{"type", "instance"}, {"definition", c.definitionId}};
            if (!c.overrides.empty()) j["overrides"] = overridesToJson(c.overrides);
            if (c.mirrored) j["mirrored"] = true;
            if (!c.stretches.empty()) {
              json list = json::array();
              for (const StretchOp& op : c.stretches) {
                list.push_back({{"center", toJson(op.center)}, {"x", toJson(op.xDir)}, {"y", toJson(op.yDir)},
                                {"factors", {op.fx, op.fy, op.fz}}});
              }
              j["stretches"] = list;
            }
            return j;
          },
      },
      base);
}

BaseFeature baseFrom(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  if (type == "rectangle") {
    RectangleProfile r;
    const json& plane = j.at("plane");
    r.plane.origin = pointFrom(plane.at("origin"));
    r.plane.normal = dirFrom(plane.at("normal"));
    r.plane.xDir = dirFrom(plane.at("xDir"));
    r.width = j.at("width").get<double>();
    r.height = j.at("height").get<double>();
    return r;
  }
  if (type == "circle") {
    CircleProfile c;
    const json& plane = j.at("plane");
    c.plane.origin = pointFrom(plane.at("origin"));
    c.plane.normal = dirFrom(plane.at("normal"));
    c.plane.xDir = dirFrom(plane.at("xDir"));
    c.radius = j.at("radius").get<double>();
    return c;
  }
  if (type == "regular_polygon") {
    RegularPolygonProfile p;
    const json& plane = j.at("plane");
    p.plane.origin = pointFrom(plane.at("origin"));
    p.plane.normal = dirFrom(plane.at("normal"));
    p.plane.xDir = dirFrom(plane.at("xDir"));
    p.radius = j.at("radius").get<double>();
    p.sides = j.at("sides").get<int>();
    return p;
  }
  if (type == "polygon") return PolygonProfile{pointsFrom(j.at("points"))};
  if (type == "polyline") return PolylineCurve{pointsFrom(j.at("points"))};
  if (type == "curve") return InterpolatedCurve{pointsFrom(j.at("points")), j.value("closed", false)};
  if (type == "arc") {
    return ArcCurve{pointFrom(j.at("start")), pointFrom(j.at("end")), dirFrom(j.at("normal")),
                    j.at("bulge").get<double>(), j.value("closed", false)};
  }
  if (type == "dimension") {
    const gp_Pnt offset = pointFrom(j.at("offset"));
    return LinearDimension{pointFrom(j.at("start")), pointFrom(j.at("end")), gp_Vec(offset.XYZ()),
                           j.value("text", std::string())};
  }
  if (type == "guide") return GuideLine{pointFrom(j.at("point")), dirFrom(j.at("direction"))};
  if (type == "image") {
    ImageUnderlay u;
    u.image = j.at("image").get<std::string>();
    const json& plane = j.at("plane");
    u.plane.origin = pointFrom(plane.at("origin"));
    u.plane.normal = dirFrom(plane.at("normal"));
    u.plane.xDir = dirFrom(plane.at("xDir"));
    u.width = j.at("width").get<double>();
    u.height = j.at("height").get<double>();
    u.opacity = j.value("opacity", 1.0);
    return u;
  }
  if (type == "text") {
    TextNote t{pointFrom(j.at("position")), std::nullopt, j.value("text", std::string())};
    if (j.contains("target")) t.target = pointFrom(j.at("target"));
    return t;
  }
  if (type == "imported") {
    auto shape = geom::shapeFromBRepString(j.at("brep").get<std::string>());
    if (!shape) throw std::runtime_error(shape.error());
    return ImportedShape{shape.value(), j.value("source", std::string())};
  }
  if (type == "boolean") {
    BooleanFeature b;
    b.kind = booleanKindFrom(j.at("kind").get<std::string>());
    for (const json& o : j.at("operands")) {
      b.operands.push_back(BooleanOperand{sharedRecipeFrom(o),
                                          trsfFrom(o.at("placement")),
                                          o.value("name", std::string())});
    }
    return b;
  }
  if (type == "loft") {
    LoftFeature l;
    l.solid = j.value("solid", true);
    for (const json& o : j.at("sections")) {
      l.sections.push_back(BooleanOperand{sharedRecipeFrom(o),
                                          trsfFrom(o.at("placement")),
                                          o.value("name", std::string())});
    }
    return l;
  }
  if (type == "sewn") {
    SewnFeature s;
    for (const json& o : j.at("faces")) {
      s.faces.push_back(BooleanOperand{sharedRecipeFrom(o),
                                       trsfFrom(o.at("placement")), o.value("name", std::string())});
    }
    return s;
  }
  if (type == "loop") {
    LoopProfile l;
    l.reversed = j.value("reversed", false);
    for (const json& o : j.at("curves")) {
      l.curves.push_back(BooleanOperand{sharedRecipeFrom(o),
                                        trsfFrom(o.at("placement")), o.value("name", std::string())});
    }
    return l;
  }
  if (type == "facepart") {
    FacePart f;
    for (const json& o : j.at("source")) {
      f.source.push_back(BooleanOperand{sharedRecipeFrom(o),
                                        trsfFrom(o.at("placement")), o.value("name", std::string())});
    }
    f.line = pointsFrom(j.at("line"));
    f.inside = pointFrom(j.at("inside"));
    return f;
  }
  if (type == "sweep") {
    SweepFeature f;
    for (const json& o : j.at("inputs")) {
      f.inputs.push_back(BooleanOperand{sharedRecipeFrom(o),
                                        trsfFrom(o.at("placement")),
                                        o.value("name", std::string())});
    }
    return f;
  }
  if (type == "group") {
    GroupNode g;
    if (j.contains("editing")) {
      const json& e = j.at("editing");
      g.editingDefinition = e.value("definition", std::string());
      g.editingInstance = e.value("instance", std::string());
      g.editingInstanceName = e.value("instanceName", std::string());
      if (e.contains("overrides")) g.editingOverrides = overridesFrom(e.at("overrides"));
      g.editingMirrored = e.value("mirrored", false);
    }
    return g;
  }
  if (type == "instance") {
    ComponentInstance c;
    c.definitionId = j.at("definition").get<std::string>();
    if (j.contains("overrides")) c.overrides = overridesFrom(j.at("overrides"));
    c.mirrored = j.value("mirrored", false);
    if (j.contains("stretches")) {
      for (const json& item : j.at("stretches")) {
        StretchOp op;
        op.center = pointFrom(item.at("center"));
        op.xDir = dirFrom(item.at("x"));
        op.yDir = dirFrom(item.at("y"));
        const json& f = item.at("factors");
        op.fx = f.at(0).get<double>();
        op.fy = f.at(1).get<double>();
        op.fz = f.at(2).get<double>();
        c.stretches.push_back(op);
      }
    }
    return c;
  }
  throw std::runtime_error("unknown base feature type: " + type);
}

json modifierToJson(const Modifier& m) {
  return std::visit(
      Overloaded{
          [](const ExtrudeOp& op) { return json{{"type", "extrude"}, {"distance", op.distance}}; },
          [](const OffsetOp& op) { return json{{"type", "offset"}, {"distance", op.distance}}; },
          [](const RevolveOp& op) {
            return json{{"type", "revolve"},
                        {"origin", toJson(op.axis.Location())},
                        {"direction", toJson(op.axis.Direction())},
                        {"angle", op.angle}};
          },
          [](const HoleOp& op) {
            return json{{"type", "hole"},
                        {"name", op.outline.name},
                        {"placement", toJson(op.outline.placement)},
                        sharedRecipe(op.outline.recipe)};
          },
          [](const OffsetFaceOp& op) {
            json j{{"type", "offsetface"},
                   {"distance", op.distance},
                   {"face",
                    {{"point", toJson(op.face.point)},
                     {"normal", toJson(op.face.normal)},
                     {"surfaceType", op.face.surfaceType}}}};
            if (op.part) j["part"] = true;
            return j;
          },
          [](const InsetOp& op) {
            return json{{"type", "inset"}, {"distance", op.distance}, {"face", faceRefToJson(op.face)}};
          },
          [](const SplitOp& op) {
            json j{{"type", "split"}, {"points", pointsToJson(op.points)}, {"face", faceRefToJson(op.face)}};
            if(op.curve)j["curve"]={{"name",op.curve->name},{"placement",toJson(op.curve->placement)},sharedRecipe(op.curve->recipe)};
            return j;
          },
          [](const StretchOp& op) {
            return json{{"type", "stretch"}, {"center", toJson(op.center)}, {"x", toJson(op.xDir)},
                        {"y", toJson(op.yDir)}, {"factors", {op.fx, op.fy, op.fz}}};
          },
          [](const ScaleOp& op) {
            return json{{"type", "scale"}, {"center", toJson(op.center)}, {"factor", op.factor}};
          },
          [](const RemoveFaceOp& op) {
            json j = {{"type", "removeface"}};
            if (op.plane) j["face"] = faceRefToJson(*op.plane);
            if (op.surface) j["surface"] = surfaceRefToJson(*op.surface);
            return j;
          },
          [](const CapHolesOp&) { return json{{"type", "capholes"}}; },
          [](const MergeAcrossOp& op) {
            return json{{"type", "mergeacross"}, {"point", toJson(op.point)}, {"direction", toJson(op.direction)}};
          },
          [](const ImprintOp& op) {
            return json{{"type", "imprint"},
                        {"edges", {{"name", op.edges.name}, {"placement", toJson(op.edges.placement)}, sharedRecipe(op.edges.recipe)}}};
          },
          [](const CurveIntervalOp& op) {
            return json{{"type", "curveinterval"}, {"edge", op.edge}, {"edge_count", op.edgeCount},
                        {"first", op.first}, {"last", op.last}, {"start", toJson(op.start)}, {"end", toJson(op.end)}};
          },
          [](const ReverseOp&) { return json{{"type", "reverse"}}; },
          [](const MoveVerticesOp& op) {
            json j{{"type", "movevertices"},
                   {"points", pointsToJson(op.points)},
                   {"direction", toJson(op.direction)},
                   {"distance", op.distance}};
            if (op.autofold) j["autofold"] = true;
            return j;
          },
          [](const PushPullOp& op) {
            return json{{"type", "pushpull"},
                        {"distance", op.distance},
                        {"face", faceRefToJson(op.face)}};
          },
      },
      m);
}

Modifier modifierFrom(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  if (type == "extrude") return ExtrudeOp{j.at("distance").get<double>()};
  if (type == "offset") return OffsetOp{j.at("distance").get<double>()};
  if (type == "revolve") {
    return RevolveOp{gp_Ax1(pointFrom(j.at("origin")), dirFrom(j.at("direction"))),
                     j.at("angle").get<double>()};
  }
  if (type == "pushpull") {
    PushPullOp op;
    op.distance = j.at("distance").get<double>();
    const json& f = j.at("face");
    op.face = faceRefFrom(f);
    return op;
  }
  if (type == "hole") {
    return HoleOp{BooleanOperand{sharedRecipeFrom(j),
                                 trsfFrom(j.at("placement")), j.value("name", std::string())}};
  }
  if (type == "offsetface") {
    OffsetFaceOp op;
    op.distance = j.at("distance").get<double>();
    const json& f = j.at("face");
    op.face.point = pointFrom(f.at("point"));
    op.face.normal = dirFrom(f.at("normal"));
    op.face.surfaceType = f.value("surfaceType", 0);
    op.part = j.value("part", false);
    return op;
  }
  if (type == "scale") return ScaleOp{pointFrom(j.at("center")), j.at("factor").get<double>()};
  if (type == "stretch") {
    StretchOp op;
    op.center = pointFrom(j.at("center"));
    op.xDir = dirFrom(j.at("x"));
    op.yDir = dirFrom(j.at("y"));
    const json& f = j.at("factors");
    op.fx = f.at(0).get<double>();
    op.fy = f.at(1).get<double>();
    op.fz = f.at(2).get<double>();
    return op;
  }
  if (type == "removeface") {
    RemoveFaceOp op;
    if (j.contains("face")) op.plane = faceRefFrom(j.at("face"));
    if (j.contains("surface")) op.surface = surfaceRefFrom(j.at("surface"));
    return op;
  }
  if (type == "capholes") return CapHolesOp{};
  if (type == "mergeacross") return MergeAcrossOp{pointFrom(j.at("point")), dirFrom(j.at("direction"))};
  if (type == "curveinterval") return CurveIntervalOp{j.at("edge").get<int>(), j.at("edge_count").get<int>(),
                                                       j.at("first").get<double>(), j.at("last").get<double>(),
                                                       pointFrom(j.at("start")), pointFrom(j.at("end"))};
  if (type == "reverse") return ReverseOp{};
  if (type == "movevertices") {
    MoveVerticesOp op;
    for (const json& p : j.at("points")) op.points.push_back(pointFrom(p));
    op.direction = dirFrom(j.at("direction"));
    op.distance = j.at("distance").get<double>();
    op.autofold = j.value("autofold", false);
    return op;
  }
  if (type == "inset") {
    InsetOp op;
    op.distance = j.at("distance").get<double>();
    op.face = faceRefFrom(j.at("face"));
    return op;
  }
  if (type == "split") {
    SplitOp op;
    op.points = pointsFrom(j.at("points"));
    const json& f = j.at("face");
    op.face = faceRefFrom(f);
    if(j.contains("curve")) {
      const auto& c=j.at("curve");
      op.curve=BooleanOperand{sharedRecipeFrom(c),trsfFrom(c.at("placement")),c.value("name",std::string())};
    }
    return op;
  }
  if (type == "imprint") {
    const json& e = j.at("edges");
    return ImprintOp{BooleanOperand{sharedRecipeFrom(e), trsfFrom(e.at("placement")), e.value("name", std::string())}};
  }
  throw std::runtime_error("unknown modifier type: " + type);
}

json recipeToJson(const Recipe& recipe) {
  json mods = json::array();
  for (const Modifier& m : recipe.modifiers) mods.push_back(modifierToJson(m));
  return json{{"base", baseToJson(recipe.base)}, {"modifiers", mods}};
}

Recipe recipeFrom(const json& j) {
  Recipe recipe;
  recipe.base = baseFrom(j.at("base"));
  for (const json& m : j.at("modifiers")) recipe.modifiers.push_back(modifierFrom(m));
  return recipe;
}

bool sameGeometry(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  if (a.IsNull() || b.IsNull()) return a.IsNull() == b.IsNull();
  const double va = geom::volume(a);
  const double vb = geom::volume(b);
  if (std::abs(va - vb) > 1e-6 * std::max(1.0, std::abs(va))) return false;
  const Bnd_Box ba = geom::exactBounds(a);
  const Bnd_Box bb = geom::exactBounds(b);
  if (ba.IsVoid() != bb.IsVoid()) return false;
  if (ba.IsVoid()) return true;
  return ba.CornerMin().Distance(bb.CornerMin()) < 1e-3 &&
         ba.CornerMax().Distance(bb.CornerMax()) < 1e-3;
}

json cameraToJson(const CameraState& c) {
  return json{{"eye", c.eye}, {"target", c.target}, {"up", c.up},
              {"perspective", c.perspective}, {"scale", c.scale}};
}

CameraState cameraFrom(const json& j) {
  CameraState c;
  c.eye = j.at("eye").get<std::array<double, 3>>();
  c.target = j.at("target").get<std::array<double, 3>>();
  c.up = j.at("up").get<std::array<double, 3>>();
  c.perspective = j.value("perspective", true);
  c.scale = j.value("scale", 0.0);
  return c;
}

// Geometry written once per distinct shape (placements of a component share
// theirs), referenced by index; read back once and shared again on load.
struct ShapeTable {
  json entries = json::array();
  std::map<std::pair<const void*, int>, std::size_t> index;

  // Volume for a reader's information; only solids have one (computing it
  // for each of thousands of faces took a good part of saving).
  static double volumeOf(const TopoDS_Shape& shape) {
    return TopExp_Explorer(shape, TopAbs_SOLID).More() ? geom::volume(shape) : 0.0;
  }

  json reference(const TopoDS_Shape& shape) {
    if (!shape.Location().IsIdentity()) {
      return {{"brep", geom::shapeToBRepString(shape)}, {"volume", volumeOf(shape)}};
    }
    const auto key = std::make_pair(static_cast<const void*>(shape.TShape().get()),
                                    static_cast<int>(shape.Orientation()));
    auto it = index.find(key);
    if (it == index.end()) {
      it = index.emplace(key, entries.size()).first;
      entries.push_back({{"brep", geom::shapeToBRepString(shape)}, {"volume", volumeOf(shape)}});
    }
    return {{"shape", it->second}, {"volume", entries[it->second]["volume"]}};
  }
};

struct ShapeReader {
  const json* entries = nullptr;
  std::vector<std::optional<Result<TopoDS_Shape>>> parsed;
  // Placements compare the same pair of shapes: once is enough.
  std::map<std::pair<const void*, const void*>, bool> same;

  bool sameAs(const TopoDS_Shape& regenerated, const TopoDS_Shape& stored) {
    if (regenerated.IsNull() || stored.IsNull()) return sameGeometry(regenerated, stored);
    const auto key = std::make_pair(static_cast<const void*>(regenerated.TShape().get()),
                                    static_cast<const void*>(stored.TShape().get()));
    const auto it = same.find(key);
    if (it != same.end()) return it->second;
    return same[key] = sameGeometry(regenerated, stored);
  }

  Result<TopoDS_Shape> read(const json& geometry) {
    if (geometry.contains("brep")) return geom::shapeFromBRepString(geometry.at("brep").get<std::string>());
    const std::size_t i = geometry.at("shape").get<std::size_t>();
    if (!entries || i >= entries->size()) return Result<TopoDS_Shape>::failure("missing shared shape");
    if (parsed.size() < entries->size()) parsed.resize(entries->size());
    if (!parsed[i]) parsed[i] = geom::shapeFromBRepString((*entries)[i].at("brep").get<std::string>());
    return *parsed[i];
  }
};

json colorToJson(const Color& c) { return json::array({c.r, c.g, c.b}); }

Color colorFrom(const json& c) { return Color{c.at(0).get<double>(), c.at(1).get<double>(), c.at(2).get<double>()}; }

json objectToJson(const Object& o, const Color& shown, ShapeTable& shapes) {
  json j = {
      {"id", o.id},
      {"name", o.name},
      {"layer", o.layer},
      {"visible", o.visible},
      {"color", colorToJson(shown)},  // what older versions show
      {"transform", toJson(o.transform)},
      {"recipe", recipeToJson(o.recipe)},
  };
  if (!o.parent.empty()) j["parent"] = o.parent;
  if (!o.material.empty()) j["material"] = o.material;
  if (o.locked) j["locked"] = true;
  if (!o.faceMaterials.empty()) {
    json faces = json::array();
    for (const FaceMaterial& fm : o.faceMaterials) {
      json f = {{"material", fm.material}};
      if (fm.plane) f["face"] = faceRefToJson(*fm.plane);
      if (fm.surface) f["surface"] = surfaceRefToJson(*fm.surface);
      faces.push_back(f);
    }
    j["faceMaterials"] = faces;
  }
  if (!o.hiddenEdges.empty()) {
    json edges = json::array();
    for (const EdgeMark& e : o.hiddenEdges) edges.push_back({{"point", toJson(e.point)}, {"direction", toJson(e.direction)}});
    j["hiddenEdges"] = edges;
  }
  if (!o.attributes.empty()) j["attributes"] = o.attributes;
  if (!o.shape.IsNull()) j["geometry"] = shapes.reference(o.shape);
  return j;
}

// `shownColor` gives the colour each object shows (written for older
// versions, which only know colours).
json objectsToJson(const std::vector<Object>& objects, ShapeTable& shapes,
                   const std::function<Color(const Object&)>& shownColor) {
  json arr = json::array();
  for (const Object& o : objects) arr.push_back(objectToJson(o, shownColor(o), shapes));
  return arr;
}

// Parses an object and resolves its geometry: the recipe is regenerated and
// compared with the stored BRep; stored geometry wins on any doubt.
Object objectFrom(const json& j, const RegenContext& context, std::vector<std::string>* warnings,
                  ShapeReader& shapes, bool regenerate) {
  Object o;
  o.id = j.at("id").get<std::string>();
  o.parent = j.value("parent", std::string());
  o.name = j.value("name", std::string());
  o.layer = j.value("layer", std::string("Default"));
  o.visible = j.value("visible", true);
  if (j.contains("color")) o.color = colorFrom(j.at("color"));
  o.material = j.value("material", std::string());
  o.locked = j.value("locked", false);
  if (j.contains("faceMaterials")) {
    for (const json& f : j.at("faceMaterials")) {
      FaceMaterial fm;
      fm.material = f.at("material").get<std::string>();
      if (f.contains("face")) fm.plane = faceRefFrom(f.at("face"));
      if (f.contains("surface")) fm.surface = surfaceRefFrom(f.at("surface"));
      o.faceMaterials.push_back(std::move(fm));
    }
  }
  if (j.contains("hiddenEdges")) {
    for (const json& e : j.at("hiddenEdges")) o.hiddenEdges.push_back({pointFrom(e.at("point")), dirFrom(e.at("direction"))});
  }
  if (j.contains("transform")) o.transform = trsfFrom(j.at("transform"));
  if (j.contains("attributes")) o.attributes = j.at("attributes").get<std::map<std::string, std::string>>();
  o.recipe = recipeFrom(j.at("recipe"));

  Result<TopoDS_Shape> stored = Result<TopoDS_Shape>::failure("no stored geometry");
  if (j.contains("geometry")) stored = shapes.read(j.at("geometry"));
  if (!regenerate && stored) {
    o.shape = stored.value();
    return o;
  }
  const Result<TopoDS_Shape> regenerated = doc::regenerate(o.recipe, context);

  auto warn = [&](const std::string& message) {
    if (warnings) warnings->push_back("'" + o.name + "': " + message);
  };
  if (regenerated && stored) {
    o.shape = stored.value();
    if (!shapes.sameAs(regenerated.value(), stored.value())) {
      warn("regenerated geometry differs from saved geometry; saved geometry kept");
    }
  } else if (stored) {
    o.shape = stored.value();
    warn("recipe could not be regenerated (" + regenerated.error() + "); saved geometry kept");
  } else if (regenerated) {
    o.shape = regenerated.value();
    if (!o.shape.IsNull()) {
      warn("saved geometry unreadable (" + stored.error() + "); regenerated from recipe");
    }
  } else {
    throw std::runtime_error("Object '" + o.name + "' has no usable geometry: " +
                             regenerated.error());
  }
  return o;
}

}  // namespace

std::string serializeDocument(const Document& document) {
  RecipeWriter recipes;
  struct Scope {
    explicit Scope(RecipeWriter* w) { recipeWriter = w; }
    ~Scope() { recipeWriter = nullptr; }
  } scope(&recipes);
  for (const ComponentDefinition& d : document.definitions()) {
    for (const Object& o : d.objects) (void)recipeToJson(o.recipe);
  }
  for (const Object& o : document.objects()) (void)recipeToJson(o.recipe);
  recipes.counting = false;
  ShapeTable shapes;
  json definitions = json::array();
  for (const ComponentDefinition& d : document.definitions()) {
    definitions.push_back({{"id", d.id},
                           {"name", d.name},
                           {"objects", objectsToJson(d.objects, shapes, [&](const Object& o) {
                              return document.materialColor(o.material, o.color);
                            })}});
  }
  json objects =
      objectsToJson(document.objects(), shapes, [&](const Object& o) { return document.displayColor(o); });
  // Embedded images that something still shows (by content hash), so
  // the file carries them wherever it is copied.
  std::set<std::string> used;
  auto collect = [&](const Object& o) {
    if (const auto* u = std::get_if<ImageUnderlay>(&o.recipe.base)) used.insert(u->image);
  };
  for (const Object& o : document.objects()) collect(o);
  for (const ComponentDefinition& d : document.definitions()) {
    for (const Object& o : d.objects) collect(o);
  }
  for (const Material& m : document.materials()) {
    if (m.texture) used.insert(m.texture->image);
  }
  json images = json::object();
  for (const std::string& hash : used) {
    if (const Document::Image* image = document.findImage(hash)) {
      images[hash] = {{"name", image->name}, {"data", codec::base64Encode(image->bytes)}};
    }
  }
  json materials = json::array();
  for (const Material& m : document.materials()) {
    json j = {{"id", m.id}, {"name", m.name}, {"color", colorToJson(m.color)}};
    if (m.opacity != 1.0) j["opacity"] = m.opacity;
    if (m.texture) {
      j["texture"] = {{"image", m.texture->image},
                      {"width", m.texture->width},
                      {"height", m.texture->height},
                      {"colorize", m.texture->colorize}};
    }
    j["pbr"] = {{"metallic", m.metallic}, {"roughness", m.roughness}, {"reflectance", m.reflectance}};
    materials.push_back(j);
  }
  const DocumentSettings& s = document.settings();
  json scenes = json::array();
  for (const Scene& scene : s.scenes) scenes.push_back({{"name", scene.name}, {"camera", cameraToJson(scene.camera)}});
  json sections = json::array();
  for (const SectionPlane& p : s.sections) {
    sections.push_back({{"name", p.name}, {"origin", p.origin}, {"normal", p.normal}, {"active", p.active}, {"visible", p.visible}});
  }
  json root = {
      {"format", kNativeFormatMagic},
      // Files without shared recipes stay readable by the version before.
      {"version", recipes.entries.empty() ? 3 : kNativeFormatVersion},
      {"units", s.units},
      {"linearTolerance", s.linearTolerance},
      {"camera", cameraToJson(s.camera)},
      {"scenes", scenes},
      {"sections", sections},
      {"sectionDisplay", {{"planes", s.sectionDisplay.planes}, {"cuts", s.sectionDisplay.cuts}, {"fill", s.sectionDisplay.fill}}},
      {"hiddenTags", s.hiddenTags},
      {"shapes", shapes.entries},
      {"recipes", recipes.entries},
      {"materials", materials},
      {"images", images},
      {"definitions", definitions},
      {"objects", objects},
  };
  return root.dump(1);
}

Status deserializeDocument(const std::string& text, Document& document,
                           std::vector<std::string>* warnings, bool regenerate) {
  try {
    const json root = json::parse(text);
    if (root.value("format", std::string()) != kNativeFormatMagic) {
      return Status::failure("Not a SketchBRep file");
    }
    const int version = root.value("version", 0);
    if (version < 1 || version > kNativeFormatVersion) {
      return Status::failure("Unsupported file version " + std::to_string(version));
    }

    Document loaded;
    DocumentSettings& s = loaded.settings();
    s.units = root.value("units", std::string("mm"));
    if (s.units != "mm") return Status::failure("Unsupported units: " + s.units);
    s.linearTolerance = root.value("linearTolerance", tolerance::kLinear);
    if (root.contains("camera")) s.camera = cameraFrom(root.at("camera"));
    if (root.contains("sectionDisplay")) {
      const auto& display = root.at("sectionDisplay");
      s.sectionDisplay.planes = display.value("planes", true);
      s.sectionDisplay.cuts = display.value("cuts", true);
      s.sectionDisplay.fill = display.value("fill", true);
    }
    if (root.contains("sections")) {
      for (const json& p : root.at("sections")) {
        SectionPlane plane;
        plane.name = p.value("name", std::string());
        plane.origin = p.at("origin").get<std::array<double, 3>>();
        plane.normal = p.at("normal").get<std::array<double, 3>>();
        plane.active = p.value("active", true);
        plane.visible = p.value("visible", true);
        s.sections.push_back(plane);
      }
    }
    if (root.contains("hiddenTags")) s.hiddenTags = root.at("hiddenTags").get<std::set<std::string>>();
    if (root.contains("scenes")) {
      for (const json& scene : root.at("scenes")) {
        s.scenes.push_back({scene.value("name", std::string()), cameraFrom(scene.at("camera"))});
      }
    }

    ShapeReader shapes;
    if (root.contains("shapes")) shapes.entries = &root.at("shapes");
    RecipeReader recipes;
    if (root.contains("recipes")) recipes.entries = &root.at("recipes");
    struct Scope {
      explicit Scope(RecipeReader* r) { recipeReader = r; }
      ~Scope() { recipeReader = nullptr; }
    } scope(&recipes);
    // Definitions first: instances regenerate from their shapes.
    if (root.contains("definitions")) {
      for (const json& d : root.at("definitions")) {
        ComponentDefinition definition;
        definition.id = d.at("id").get<std::string>();
        definition.name = d.value("name", std::string());
        for (const json& o : d.at("objects")) {
          definition.objects.push_back(objectFrom(o, loaded.regenContext(), warnings, shapes, regenerate));
        }
        auto shape = buildDefinitionShape(definition);
        if (!shape) return Status::failure("Component '" + definition.name + "': " + shape.error());
        definition.shape = shape.value();
        loaded.putDefinition(definition);
      }
    }
    for (const json& j : root.at("objects")) {
      loaded.insert(objectFrom(j, loaded.regenContext(), warnings, shapes, regenerate));
    }
    for (const Object& o : loaded.objects()) {
      if (!o.parent.empty() && !loaded.find(o.parent)) {
        return Status::failure("Object '" + o.name + "' refers to a missing parent");
      }
    }

    // Materials. Files from before materials only have colours: each colour
    // other than the default becomes the material made from it (same
    // colour, same material), assigned to the object.
    std::vector<Material> library;
    if (root.contains("materials")) {
      for (const json& m : root.at("materials")) {
        Material mat;
        mat.id = m.at("id").get<std::string>();
        mat.name = m.value("name", std::string());
        mat.color = colorFrom(m.at("color"));
        mat.opacity = m.value("opacity", 1.0);
        if (m.contains("texture")) {
          const json& t = m.at("texture");
          mat.texture = MaterialTexture{t.at("image").get<std::string>(), t.value("width", 1000.0),
                                        t.value("height", 1000.0), t.value("colorize", false)};
        }
        if (m.contains("pbr")) {
          const json& p = m.at("pbr");
          mat.metallic = p.value("metallic", 0.0);
          mat.roughness = p.value("roughness", 0.5);
          mat.reflectance = p.value("reflectance", 0.5);
        }
        library.push_back(mat);
      }
    } else {
      std::set<std::string> known;
      auto adopt = [&](Object& o) {
        if (o.color == Color{} || o.isContainer()) return;
        const Material m = colorMaterial(o.color);
        o.material = m.id;
        if (known.insert(m.id).second) library.push_back(m);
      };
      std::vector<ComponentDefinition> definitions = loaded.definitions();
      for (ComponentDefinition& d : definitions) {
        for (Object& o : d.objects) adopt(o);
        loaded.putDefinition(d);
      }
      std::vector<Object> objects = loaded.objects();
      for (Object& o : objects) {
        adopt(o);
        loaded.replace(o);
      }
    }

    std::vector<std::pair<std::string, Document::Image>> images;
    if (root.contains("images")) {
      for (auto it = root.at("images").begin(); it != root.at("images").end(); ++it) {
        Document::Image image{it.value().value("name", std::string()),
                              codec::base64Decode(it.value().at("data").get<std::string>())};
        if (codec::sha256Hex(image.bytes) != it.key()) {
          if (warnings) warnings->push_back("embedded image '" + image.name + "' is damaged");
          continue;
        }
        images.emplace_back(it.key(), std::move(image));
      }
    }

    // A texture whose picture did not come through: the material keeps its colour.
    for (Material& m : library) {
      if (!m.texture) continue;
      const bool present = std::any_of(images.begin(), images.end(), [&](const auto& i) { return i.first == m.texture->image; });
      if (!present) {
        if (warnings) warnings->push_back("material '" + m.name + "': texture picture missing; colour only");
        m.texture.reset();
      }
    }
    document.clear();
    document.settings() = loaded.settings();
    for (const Material& m : library) document.putMaterial(m);
    for (const auto& [hash, image] : images) document.addImage(image.bytes, image.name);
    for (const ComponentDefinition& d : loaded.definitions()) document.putDefinition(d);
    for (const Object& o : loaded.objects()) document.insert(o);
    return Status::success();
  } catch (const std::exception& e) {
    return Status::failure(std::string("Corrupt project file: ") + e.what());
  }
}

Status saveDocument(const Document& document, const std::string& path) {
  const fs::path target = fs::u8path(path);
  fs::path temp = target;
  temp += ".tmp";
  std::string text = serializeDocument(document);
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return Status::failure("Cannot write " + path);
    out << text;
    out.flush();
    if (!out) return Status::failure("Write failed for " + path);
  }
  // Verify what actually reached the disk before replacing the old file.
  // One copy of the file read back at a time (a stream buffer and copies
  // of it held the text up to four times over while checking).
  {
    std::string written;
    {
      std::ifstream in(temp, std::ios::binary | std::ios::ate);
      const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : -1;
      if (size >= 0) {
        written.resize(static_cast<std::size_t>(size));
        in.seekg(0);
        in.read(written.data(), size);
        if (!in) written.clear();
      }
    }
    const bool same = written == text;
    std::string().swap(text);  // not needed any more
    if (!same) {
      std::error_code ec;
      fs::remove(temp, ec);
      return Status::failure("Verification of saved data failed");
    }
    // Structure and stored geometry read back; recipes are not re-run here
    // (that is what loading does), so saving a large model stays quick.
    Document check;
    const Status parsed = deserializeDocument(written, check, nullptr, false);
    if (!parsed) {
      std::error_code ec;
      fs::remove(temp, ec);
      return Status::failure("Verification of saved data failed: " + parsed.message());
    }
  }
  std::error_code ec;
  fs::rename(temp, target, ec);
  if (ec) return Status::failure("Cannot replace " + path + ": " + ec.message());
  return Status::success();
}

std::string backupPath(const std::string& path, int generation) {
  return path + ".bak" + std::to_string(generation);
}

Status rotateBackups(const std::string& path, int generations) {
  const fs::path current = fs::u8path(path);
  std::error_code ec;
  if (generations <= 0 || !fs::exists(current, ec)) return Status::success();
  fs::remove(fs::u8path(backupPath(path, generations)), ec);
  for (int k = generations - 1; k >= 1; --k) {
    const fs::path from = fs::u8path(backupPath(path, k));
    if (fs::exists(from, ec)) fs::rename(from, fs::u8path(backupPath(path, k + 1)), ec);
  }
  fs::copy_file(current, fs::u8path(backupPath(path, 1)), fs::copy_options::overwrite_existing, ec);
  if (ec) return Status::failure("Cannot keep a backup of " + path + ": " + ec.message());
  return Status::success();
}

std::optional<std::string> checkRecipe(const Object& object, const RegenContext& context) {
  const Result<TopoDS_Shape> regenerated = regenerate(object.recipe, context);
  if (!regenerated) {
    return "'" + object.name + "': recipe could not be regenerated (" + regenerated.error() +
           "); saved geometry kept";
  }
  if (!sameGeometry(regenerated.value(), object.shape)) {
    return "'" + object.name + "': regenerated geometry differs from saved geometry; saved geometry kept";
  }
  return std::nullopt;
}

Status loadDocument(Document& document, const std::string& path,
                    std::vector<std::string>* warnings, bool regenerate) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) return Status::failure("Cannot open " + path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return deserializeDocument(buffer.str(), document, warnings, regenerate);
}

}  // namespace sbr::doc
