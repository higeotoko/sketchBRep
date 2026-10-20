#include "sbr/doc/NativeFormat.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

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
          [](const PolygonProfile& p) {
            return json{{"type", "polygon"}, {"points", pointsToJson(p.points)}};
          },
          [](const PolylineCurve& p) {
            return json{{"type", "polyline"}, {"points", pointsToJson(p.points)}};
          },
          [](const ImportedShape& s) {
            return json{{"type", "imported"},
                        {"source", s.source},
                        {"brep", geom::shapeToBRepString(s.shape)}};
          },
          [](const BooleanFeature& b) {
            json operands = json::array();
            for (const BooleanOperand& o : b.operands) {
              operands.push_back({{"name", o.name},
                                  {"placement", toJson(o.placement)},
                                  {"recipe", recipeToJson(*o.recipe)}});
            }
            return json{{"type", "boolean"}, {"kind", toString(b.kind)}, {"operands", operands}};
          },
          [](const GroupNode&) { return json{{"type", "group"}}; },
          [](const ComponentInstance& c) {
            return json{{"type", "instance"}, {"definition", c.definitionId}};
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
  if (type == "polygon") return PolygonProfile{pointsFrom(j.at("points"))};
  if (type == "polyline") return PolylineCurve{pointsFrom(j.at("points"))};
  if (type == "imported") {
    auto shape = geom::shapeFromBRepString(j.at("brep").get<std::string>());
    if (!shape) throw std::runtime_error(shape.error());
    return ImportedShape{shape.value(), j.value("source", std::string())};
  }
  if (type == "boolean") {
    BooleanFeature b;
    b.kind = booleanKindFrom(j.at("kind").get<std::string>());
    for (const json& o : j.at("operands")) {
      b.operands.push_back(BooleanOperand{std::make_shared<const Recipe>(recipeFrom(o.at("recipe"))),
                                          trsfFrom(o.at("placement")),
                                          o.value("name", std::string())});
    }
    return b;
  }
  if (type == "group") return GroupNode{};
  if (type == "instance") return ComponentInstance{j.at("definition").get<std::string>()};
  throw std::runtime_error("unknown base feature type: " + type);
}

json modifierToJson(const Modifier& m) {
  return std::visit(
      Overloaded{
          [](const ExtrudeOp& op) { return json{{"type", "extrude"}, {"distance", op.distance}}; },
          [](const OffsetOp& op) { return json{{"type", "offset"}, {"distance", op.distance}}; },
          [](const PushPullOp& op) {
            return json{{"type", "pushpull"},
                        {"distance", op.distance},
                        {"face",
                         {{"normal", toJson(op.face.normal)},
                          {"centroid", toJson(op.face.centroid)},
                          {"area", op.face.area}}}};
          },
      },
      m);
}

Modifier modifierFrom(const json& j) {
  const std::string type = j.at("type").get<std::string>();
  if (type == "extrude") return ExtrudeOp{j.at("distance").get<double>()};
  if (type == "offset") return OffsetOp{j.at("distance").get<double>()};
  if (type == "pushpull") {
    PushPullOp op;
    op.distance = j.at("distance").get<double>();
    const json& f = j.at("face");
    op.face.normal = dirFrom(f.at("normal"));
    op.face.centroid = pointFrom(f.at("centroid"));
    op.face.area = f.value("area", 0.0);
    return op;
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

json objectToJson(const Object& o) {
  json j = {
      {"id", o.id},
      {"name", o.name},
      {"layer", o.layer},
      {"visible", o.visible},
      {"color", {o.color.r, o.color.g, o.color.b}},
      {"transform", toJson(o.transform)},
      {"recipe", recipeToJson(o.recipe)},
  };
  if (!o.parent.empty()) j["parent"] = o.parent;
  if (!o.shape.IsNull()) {
    j["geometry"] = {{"brep", geom::shapeToBRepString(o.shape)}, {"volume", geom::volume(o.shape)}};
  }
  return j;
}

json objectsToJson(const std::vector<Object>& objects) {
  json arr = json::array();
  for (const Object& o : objects) arr.push_back(objectToJson(o));
  return arr;
}

// Parses an object and resolves its geometry: the recipe is regenerated and
// compared with the stored BRep; stored geometry wins on any doubt.
Object objectFrom(const json& j, const RegenContext& context, std::vector<std::string>* warnings) {
  Object o;
  o.id = j.at("id").get<std::string>();
  o.parent = j.value("parent", std::string());
  o.name = j.value("name", std::string());
  o.layer = j.value("layer", std::string("Default"));
  o.visible = j.value("visible", true);
  if (j.contains("color")) {
    const json& c = j.at("color");
    o.color = Color{c.at(0).get<double>(), c.at(1).get<double>(), c.at(2).get<double>()};
  }
  if (j.contains("transform")) o.transform = trsfFrom(j.at("transform"));
  o.recipe = recipeFrom(j.at("recipe"));

  Result<TopoDS_Shape> stored = Result<TopoDS_Shape>::failure("no stored geometry");
  if (j.contains("geometry")) {
    stored = geom::shapeFromBRepString(j.at("geometry").at("brep").get<std::string>());
  }
  const Result<TopoDS_Shape> regenerated = regenerate(o.recipe, context);

  auto warn = [&](const std::string& message) {
    if (warnings) warnings->push_back("'" + o.name + "': " + message);
  };
  if (regenerated && stored) {
    o.shape = stored.value();
    if (!sameGeometry(regenerated.value(), stored.value())) {
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
  json definitions = json::array();
  for (const ComponentDefinition& d : document.definitions()) {
    definitions.push_back({{"id", d.id}, {"name", d.name}, {"objects", objectsToJson(d.objects)}});
  }
  const DocumentSettings& s = document.settings();
  json root = {
      {"format", kNativeFormatMagic},
      {"version", kNativeFormatVersion},
      {"units", s.units},
      {"linearTolerance", s.linearTolerance},
      {"camera", cameraToJson(s.camera)},
      {"definitions", definitions},
      {"objects", objectsToJson(document.objects())},
  };
  return root.dump(1);
}

Status deserializeDocument(const std::string& text, Document& document,
                           std::vector<std::string>* warnings) {
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

    // Definitions first: instances regenerate from their shapes.
    if (root.contains("definitions")) {
      for (const json& d : root.at("definitions")) {
        ComponentDefinition definition;
        definition.id = d.at("id").get<std::string>();
        definition.name = d.value("name", std::string());
        for (const json& o : d.at("objects")) {
          definition.objects.push_back(objectFrom(o, loaded.regenContext(), warnings));
        }
        auto shape = buildDefinitionShape(definition);
        if (!shape) return Status::failure("Component '" + definition.name + "': " + shape.error());
        definition.shape = shape.value();
        loaded.putDefinition(definition);
      }
    }
    for (const json& j : root.at("objects")) {
      loaded.insert(objectFrom(j, loaded.regenContext(), warnings));
    }
    for (const Object& o : loaded.objects()) {
      if (!o.parent.empty() && !loaded.find(o.parent)) {
        return Status::failure("Object '" + o.name + "' refers to a missing parent");
      }
    }

    document.clear();
    document.settings() = loaded.settings();
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
  const std::string text = serializeDocument(document);
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return Status::failure("Cannot write " + path);
    out << text;
    out.flush();
    if (!out) return Status::failure("Write failed for " + path);
  }
  // Verify what actually reached the disk before replacing the old file.
  {
    std::ifstream in(temp, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str() != text) {
      std::error_code ec;
      fs::remove(temp, ec);
      return Status::failure("Verification of saved data failed");
    }
    Document check;
    const Status parsed = deserializeDocument(buffer.str(), check);
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

Status loadDocument(Document& document, const std::string& path,
                    std::vector<std::string>* warnings) {
  std::ifstream in(fs::u8path(path), std::ios::binary);
  if (!in) return Status::failure("Cannot open " + path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return deserializeDocument(buffer.str(), document, warnings);
}

}  // namespace sbr::doc
