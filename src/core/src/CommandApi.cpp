#include "sbr/api/CommandApi.h"

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include <BRepTools.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

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

json describeObject(const doc::Modeler& m, const doc::Object& o, bool withFaces) {
  const doc::Document& d = m.document();
  const TopoDS_Shape world = d.worldShape(o);
  json j = {{"id", o.id},
            {"name", o.name},
            {"kind", doc::toString(o.kind())},
            {"tag", o.layer},
            {"visible", o.visible},
            {"color", {o.color.r, o.color.g, o.color.b}},
            {"history", doc::describe(o.recipe)},
            {"bounds", bounds(world)}};
  if (!o.parent.empty()) j["parent"] = o.parent;
  if (o.kind() == doc::ObjectKind::Solid) j["volume"] = geom::volume(world);
  if (o.kind() == doc::ObjectKind::Face) j["area"] = geom::area(world);
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

// Face selection: {"face": 3} (index from get_object) or
// {"face": {"normal": [0,0,1]}} (the outermost planar face with that normal).
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
    TopoDS_Face best;
    double bestOffset = -1e300;
    for (int i = 1; i <= faces.Extent(); ++i) {
      const TopoDS_Face face = TopoDS::Face(faces(i));
      const auto ref = geom::FaceRef::fromFace(face);
      if (!ref || ref->normal.Angle(wanted) > 1e-6) continue;
      const double offset = gp_Vec(ref->centroid.XYZ()).Dot(gp_Vec(wanted));
      if (offset > bestOffset) {
        bestOffset = offset;
        best = face;
      }
    }
    if (best.IsNull()) throw ApiError("no planar face with that normal");
    return best;
  }
  throw ApiError("'face' must be an index or {\"normal\": [x, y, z]}");
}

doc::BooleanKind booleanKind(const std::string& s) {
  if (s == "union") return doc::BooleanKind::Union;
  if (s == "subtract") return doc::BooleanKind::Subtract;
  if (s == "intersect") return doc::BooleanKind::Intersect;
  throw ApiError("operation must be union, subtract or intersect");
}

json idResult(const ObjectId& id) { return json{{"id", id}}; }
json idsResult(const std::vector<ObjectId>& v) { return json{{"ids", v}}; }

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
       "face facing that way (planar faces only; curved faces by index, e.g. a cylinder side changes its "
       "radius). Positive distance moves the face outwards. A profile lying on another "
       "solid's face cuts into it (negative) or joins it (positive); the merged object gets a new "
       "id, so always continue with the returned id.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"},
         "face": {"oneOf": [{"type": "integer"}, {"type": "object", "properties": {"normal": {"type": "array"}}}]},
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
      {"set_parameter", "Change a parameter (see get_object.parameters), e.g. op0.distance or width.",
       SCHEMA({"type": "object", "properties": {"id": {"type": "string"}, "key": {"type": "string"},
               "value": {"type": "number"}}, "required": ["id", "key", "value"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.setParameter(text(a, "id"), text(a, "key"), number(a, "value")));
         return idResult(text(a, "id"));
       }},
      {"offset", "Offset a profile's outline (new object; positive = outwards), or a solid's face "
                 "when 'face' is given (new profile on that face).",
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
      {"copy_array",
       "Copy objects 'count' times. Linear: give 'vector' (step). Polar: give axis_origin, "
       "axis_direction and 'angle' (step, degrees).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "count": {"type": "integer"}, "vector": {"type": "array"}, "axis_origin": {"type": "array"},
               "axis_direction": {"type": "array"}, "angle": {"type": "number"}}, "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         gp_Trsf step;
         if (a.contains("vector")) {
           step.SetTranslation(gp_Vec(xyz(a.at("vector"), "vector")));
         } else {
           step.SetRotation(gp_Ax1(point(a, "axis_origin"), direction(a, "axis_direction", gp::DZ())),
                            number(a, "angle") * kDegree);
         }
         return idsResult(must(m.copy(ids(a, "ids"), step, a.value("count", 1))));
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
       "SketchUp's Follow Me: sweep the profile face along 'path' (a curve object) or around the outline of "
       "a solid's face ('around': {\"id\": solid, \"face\": index or {\"normal\": [...]}}). The profile is "
       "consumed; its dimensions stay editable on the result (in0.<key>).",
       SCHEMA({"type": "object", "properties": {"profile": {"type": "string"}, "path": {"type": "string"},
               "around": {"type": "object"}}, "required": ["profile"]}),
       [](doc::Modeler& m, const json& a) {
         if (a.contains("path")) return idResult(must(m.followMe(text(a, "profile"), text(a, "path"))));
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
      {"paint", "Paint objects (groups paint their contents). color: [r,g,b] 0..1 or \"#rrggbb\".",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}},
               "color": {}}, "required": ["ids", "color"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.paint(ids(a, "ids"), color(a, "color")));
         return idsResult(ids(a, "ids"));
       }},
      {"delete", "Delete objects (with their children).",
       SCHEMA({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
               "required": ["ids"]}),
       [](doc::Modeler& m, const json& a) {
         must(m.remove(ids(a, "ids")));
         return json::object();
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
      {"import_3dm", "Import the surfaces and solids of a Rhino .3dm file (units converted to mm).",
       SCHEMA({"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}),
       [](doc::Modeler& m, const json& a) { return idsResult(must(m.import3dm(text(a, "path")))); }},
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
         name == "import_3dm";
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
  m.history().endGroup();
  return json{{"results", results}};
}

// Runs a batch on a scratch copy of the model and reports what it would
// change; the real model is untouched (spec 07 §9.2).
json runPreview(const Context& c, const json& args) {
  doc::Modeler scratch;
  scratch.copyFrom(c.modeler);
  CommandApi api(scratch, c.options);
  json batch = {{"command", "batch"}, {"args", {{"commands", arg(args, "commands")}}}};
  const json outcome = json::parse(api.execute(batch.dump()));
  json added = json::array(), removed = json::array(), changed = json::array();
  const doc::Document& before = c.modeler.document();
  const doc::Document& after = scratch.document();
  for (const doc::Object& o : after.objects()) {
    const doc::Object* old = before.find(o.id);
    if (!old) {
      added.push_back(describeObject(scratch, o, false));
    } else if (!old->shape.IsSame(o.shape) || doc::describe(old->recipe) != doc::describe(o.recipe) ||
               old->name != o.name || old->layer != o.layer || old->parent != o.parent ||
               old->color.r != o.color.r || old->color.g != o.color.g || old->color.b != o.color.b ||
               !old->transform.TranslationPart().IsEqual(o.transform.TranslationPart(), 1e-9)) {
      changed.push_back(describeObject(scratch, o, false));
    }
  }
  for (const doc::Object& o : before.objects()) {
    if (!after.find(o.id)) removed.push_back(o.id);
  }
  json result = {{"would_succeed", outcome.value("ok", false)},
                 {"added", added},
                 {"changed", changed},
                 {"removed", removed},
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

std::string CommandApi::schemaJson() const {
  json list = json::array();
  for (const CommandDef& c : commands()) {
    if (isFileCommand(c.name) && !options_.allowFileCommands) continue;
    list.push_back({{"name", c.name}, {"description", c.description}, {"input_schema", json::parse(c.schema)}});
  }
  for (const Extension& e : extensions_) {
    list.push_back({{"name", e.name}, {"description", e.description}, {"input_schema", json::parse(e.inputSchema)}});
  }
  list.push_back({{"name", "preview"},
                  {"description",
                   "Try 'commands' (as in batch) on a scratch copy and report what they would add, change "
                   "and remove, without touching the model. To apply, send the same commands to batch with "
                   "expected_revision = the returned revision (refused if the model changed meanwhile)."},
                  {"input_schema",
                   {{"type", "object"},
                    {"properties", {{"commands", {{"type", "array"}, {"items", {{"type", "object"}}}}}}},
                    {"required", {"commands"}}}}});
  list.push_back({{"name", "batch"},
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
       R"({"type": "object", "properties": {}})", nullptr},
      {"select", "Select objects in the app (shows the user what you mean).",
       R"({"type": "object", "properties": {"ids": {"type": "array", "items": {"type": "string"}}},
           "required": ["ids"]})",
       nullptr},
      {"set_view", "Change the camera: iso, top, front, right, back, left, or fit (optionally to 'ids').",
       R"({"type": "object", "properties": {
           "view": {"enum": ["iso", "top", "front", "right", "back", "left", "fit"]},
           "ids": {"type": "array", "items": {"type": "string"}}}, "required": ["view"]})",
       nullptr},
      {"capture_view", "Capture the current 3D view as a PNG image so you can look at the model.",
       R"({"type": "object", "properties": {}})", nullptr},
      {"list_scenes", "List the saved views (scenes) of the model.", R"({"type": "object", "properties": {}})",
       nullptr},
      {"propose",
       "Show the user a proposal instead of changing the model: 'commands' (as in batch) are tried on a "
       "copy, the result is drawn as a ghost with Apply / Reject buttons, and nothing changes until the user "
       "applies it. Returns at once; poll proposal_status. 'description' is shown to the user.",
       R"({"type": "object", "properties": {"commands": {"type": "array", "items": {"type": "object"}},
           "description": {"type": "string"}}, "required": ["commands"]})",
       nullptr},
      {"proposal_status", "State of the last proposal: pending, applied, rejected or failed (with error).",
       R"({"type": "object", "properties": {}})", nullptr},
      {"go_to_scene", "Show a saved view by name or 0-based index; with 'save': true, save the current view "
                      "as a new scene named 'name' instead.",
       R"({"type": "object", "properties": {"name": {"type": "string"}, "index": {"type": "integer"},
           "save": {"type": "boolean"}}})",
       nullptr},
  };
}

}  // namespace sbr::api
