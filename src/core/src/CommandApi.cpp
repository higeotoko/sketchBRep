#include "sbr/api/CommandApi.h"

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

gp_Dir direction(const json& args, const char* key, const gp_Dir& fallback) {
  if (!args.contains(key)) return fallback;
  const gp_XYZ v = xyz(args.at(key), key);
  if (v.Modulus() < 1e-12) throw ApiError(std::string("'") + key + "' must not be zero");
  return gp_Dir(v);
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

doc::BooleanKind booleanKind(const std::string& s) {
  if (s == "union") return doc::BooleanKind::Union;
  if (s == "subtract") return doc::BooleanKind::Subtract;
  if (s == "intersect") return doc::BooleanKind::Intersect;
  throw ApiError("operation must be union, subtract or intersect");
}

json idResult(const ObjectId& id) { return json{{"id", id}}; }
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
       "'Grid' tag. Drawing snaps to the lines and their crossings. Returns line ids by name.",
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
         return json{{"lines", lines}, {"labels", grid.labels}, {"dimensions", grid.dimensions}};
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
       "id, so always continue with the returned id. A flat face of an open shape (faces deleted) gives a new "
       "solid of its own from that face (returned id), the face is taken out of the open shape, which stays "
       "open.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"},
         "face": {"oneOf": [{"type": "integer"}, {"type": "object", "properties": {"normal": {"type": "array"},
                                                                                   "near": {"type": "array"}}}]},
         "distance": {"type": "number"}}, "required": ["id", "face", "distance"]}),
       [](doc::Modeler& m, const json& a) {
         const ObjectId id = text(a, "id");
         const auto r = must(m.pushPull(id, selectFace(m, id, arg(a, "face")), number(a, "distance")));
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
       "lines/arcs it becomes a face (auto_face, default true; one undo step) and the result says so. A "
       "line from edge to edge across a face splits it (use split_face).",
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
         m.history().endGroup();
         if (face) return json{{"id", *face}, {"face", true}};
         json r = {{"id", *line}, {"face", false}};
         if (loopFound) r["warning"] = "closed loop could not become a face: " + face.error();
         return r;
       }},
      {"split_face",
       "Draw a line (2+ points) from edge to edge across a planar face of a solid to split that face, so "
       "each part can be pushed/pulled separately (e.g. a band at 1 m on a wall). Returns the solid's id.",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}},
         "required": ["points"]}),
       [](doc::Modeler& m, const json& a) { return idResult(must(m.splitWithLine(points(a, "points")))); }},
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
      {"move", "Move objects by a vector.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "vector": {"type": "array", "items": {"type": "number"}}}, "required": ["ids", "vector"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.move(ids(a, "ids"), gp_Vec(xyz(arg(a, "vector"), "vector"))));
         return idsResult(ids(a, "ids"));
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
       "Scale objects uniformly by 'factor' about the world point 'center' (parametric: the factor stays "
       "editable as op<N>.scale). Groups scale their contents; components cannot be scaled.",
       SCHEMA({"type": "object", "properties": {
         "ids": {"type": "array", "items": {"type": "string"}},
         "center": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         "factor": {"type": "number"}}, "required": ["ids", "center", "factor"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.scale(ids(a, "ids"), point(a, "center"), number(a, "factor")));
         return json::object();
       }},
      {"copy_array",
       "Copy objects ('ids') or only some edges ('edges': [{\"id\": object, \"edge\": index or "
       "{\"near\": [x,y,z]}}], copied as lines; a straight copy lying across a face from edge to edge divides "
       "it, a copy on an existing edge is not made twice) 'count' times. Linear: give 'vector'; it is the step "
       "between copies, or with 'divide': true the whole distance divided into 'count' steps (SketchUp '/3'). "
       "Polar: give axis_origin, axis_direction and 'angle' (step, degrees; 'divide' divides it too). "
       "One undo step.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "edges": {"type": "array"}, "count": {"type": "integer"}, "vector": {"type": "array"},
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
      {"rename", "Rename an object.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "name": {"type": "string"}},
               "required": ["id", "name"]}),
       [](doc::Modeler& m, const json& a) {
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
       "as lines. All is located first, then done as one undo step; on a failure nothing changes. Returns "
       "removed, lines, merged.",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "elements": {"type": "array"}}}),
       [](doc::Modeler& m, const json& a) {
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
         const doc::DeleteOutcome out = must(m.deleteSelection(targets));
         return json{{"removed", out.removed}, {"lines", out.lines}, {"merged", out.merged}};
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
       "omitted): index (for set_opening_size), name, width, height, centre and the world height of the "
       "lowest edge (sill). mm.",
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
           for (const auto& o : found) {
             openings.push_back({{"index", o.index}, {"name", o.name}, {"width", o.width}, {"height", o.height},
                                 {"center", toJson(o.center)}, {"sill", o.bottom},
                                 {"normal", toJson(o.normal.XYZ())}});
           }
           list.push_back({{"host", h}, {"name", object(m, h).name}, {"openings", openings}});
         }
         return json{{"hosts", list}};
       }},
      {"set_opening_size",
       "Resize the rectangular openings of 'host' ('openings': indices from list_openings, default all) to "
       "'width' and/or 'height' (mm). The horizontal centre and the sill stay put ('keep': \"sill\", "
       "default) or the centre stays ('keep': \"center\"). Stays parametric; one undo step. For many walls "
       "use edit_each with host \"$id\".",
       SCHEMA({"type": "object", "properties": {"host": {"type": "string"},
               "openings": {"type": "array", "items": {"type": "integer"}}, "width": {"type": "number"},
               "height": {"type": "number"}, "keep": {"enum": ["sill", "center"]}}, "required": ["host"]}),
       [](doc::Modeler& m, const json& a) {
         std::vector<std::size_t> which;
         if (a.contains("openings")) {
           for (const json& i : a.at("openings")) {
             if (!i.is_number_integer() || i.get<long long>() < 1) throw ApiError("'openings' must be indices >= 1");
             which.push_back(i.get<std::size_t>());
           }
         }
         const std::string keep = textOr(a, "keep", "sill");
         if (keep != "sill" && keep != "center") throw ApiError("'keep' must be sill or center");
         const std::optional<double> w = a.contains("width") ? std::optional<double>(number(a, "width")) : std::nullopt;
         const std::optional<double> h = a.contains("height") ? std::optional<double>(number(a, "height")) : std::nullopt;
         must(m.setOpeningSize(text(a, "host"), which, w, h, keep == "center"));
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
      {"create_wall",
       "Walls along a plan polyline: 'points' are [x, y] (mm) or grid crossings like \"X1/Y1\"; repeat the "
       "first point at the end for a closed outline. One wall per segment, corners butt-joined (exact at "
       "right angles), 'thickness' and 'height' (mm) from 'base' (default 0). 'justification': center "
       "(default), left or right of the line, looking along it. Length, thickness and height stay editable "
       "(width / height / op0.distance). Walls get attribute kind=wall. One undo step.",
       SCHEMA({"type": "object", "properties": {
         "points": {"type": "array"}, "thickness": {"type": "number"}, "height": {"type": "number"},
         "base": {"type": "number"}, "justification": {"enum": ["center", "left", "right"]}},
         "required": ["points", "thickness", "height"]}),
       [](doc::Modeler& m, const json& a) {
         const GridLookup grid(m.document());
         const double base = numberOr(a, "base", 0.0);
         std::vector<gp_Pnt> pts;
         const json& list = arg(a, "points");
         if (!list.is_array()) throw ApiError("'points' must be an array");
         for (const json& p : list) pts.push_back(grid.point(p, base));
         const std::string j = textOr(a, "justification", "center");
         if (j != "center" && j != "left" && j != "right") throw ApiError("justification: center, left or right");
         return idsResult(must(m.createWalls(pts, number(a, "thickness"), number(a, "height"),
                                             j == "left" ? -1 : j == "right" ? 1 : 0)));
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
      {"place_openings",
       "Cut 'count' equal openings ('width' x 'height', lower edge 'sill' above the face's bottom, mm) "
       "through a wall 'host', on its face facing 'face_normal' (horizontal, e.g. [0, -1, 0]), evenly "
       "spread along it or 'spacing' apart centre to centre around its middle; or a single one centred on "
       "the point 'at' ([x, y, z], its position along the face; count is then ignored). Same as drawing "
       "them on the face and pushing them through; one undo step. Returns the wall's new id (use it "
       "afterwards).",
       SCHEMA({"type": "object", "properties": {
         "host": {"type": "string"}, "face_normal": {"type": "array", "items": {"type": "number"}},
         "count": {"type": "integer"}, "width": {"type": "number"}, "height": {"type": "number"},
         "sill": {"type": "number"}, "spacing": {"type": "number"},
         "at": {"type": "array", "items": {"type": "number"}}},
         "required": ["host", "face_normal", "width", "height"]}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("at")) {
           return idResult(must(m.placeOpeningAt(text(a, "host"), direction(a, "face_normal", gp::DY()),
                                                 point(a, "at"), number(a, "width"), number(a, "height"),
                                                 numberOr(a, "sill", 0.0))));
         }
         const json& count = arg(a, "count");
         if (!count.is_number_integer()) throw ApiError("'count' must be an integer");
         return idResult(must(m.placeOpenings(text(a, "host"), direction(a, "face_normal", gp::DY()),
                                              count.get<int>(), number(a, "width"), number(a, "height"),
                                              numberOr(a, "sill", 0.0),
                                              a.contains("spacing") ? std::optional<double>(number(a, "spacing"))
                                                                    : std::nullopt)));
       }},
      {"place_on_grid",
       "Copy 'source' (e.g. a column) to crossings of grid lines (from create_grid): 'x' and 'y' are line "
       "names like [\"X1\", \"X3\"] or \"all\" (default); 'skip' lists crossings like \"X2/Y1\". The "
       "bottom centre of the source lands on each crossing (its height is kept). One undo step; returns "
       "the new ids by crossing. Lines with the same name twice are refused as ambiguous.",
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
  return name == "save" || name == "open" || name == "export_step" || name == "export_3dm" ||
         name == "import_3dm" || name == "set_material_texture";
}

CommandApi::Effect builtinEffect(const std::string& name) {
  using E = CommandApi::Effect;
  if (name == "list_objects" || name == "get_object" || name == "list_tags" || name == "describe" ||
      name == "preview" || name == "query_objects" || name == "aggregate" || name == "get_changes" ||
      name == "list_openings" || name == "list_materials" || name == "material_at" ||
      name == "placement_parameters") {
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
    auto copy = m.copy({sourceId}, delta);
    if (!copy) {
      m.history().abortGroup();
      throw ApiError("could not place at " + where + ": " + copy.error());
    }
    placed[where] = copy->front();
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
    if (isFileCommand(name) && !c.options.allowFileCommands) {
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
  return run(Context{modeler_, options_, extensions_, *this}, request, 0).dump();
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
    if (isFileCommand(c.name) && !options_.allowFileCommands) continue;
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
      {"capture_view", "Capture the current 3D view as a PNG image so you can look at the model.",
       R"({"type": "object", "properties": {}})", nullptr, Effect::Read},
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
