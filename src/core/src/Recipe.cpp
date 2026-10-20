#include "sbr/doc/Recipe.h"

#include <cmath>
#include <sstream>

#include <TopoDS.hxx>

#include "sbr/geom/Operations.h"
#include "sbr/geom/Validation.h"

namespace sbr::doc {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string opKey(std::size_t index) { return "op" + std::to_string(index) + ".distance"; }

Result<TopoDS_Shape> buildBase(const BaseFeature& base) {
  return std::visit(
      Overloaded{
          [](const RectangleProfile& r) -> Result<TopoDS_Shape> {
            auto face = geom::makeRectangleFace(r.plane, r.width, r.height);
            if (!face) return Result<TopoDS_Shape>::failure(face.error());
            return TopoDS_Shape(face.value());
          },
          [](const PolygonProfile& p) -> Result<TopoDS_Shape> {
            auto face = geom::makePolygonFace(p.points);
            if (!face) return Result<TopoDS_Shape>::failure(face.error());
            return TopoDS_Shape(face.value());
          },
          [](const PolylineCurve& p) -> Result<TopoDS_Shape> {
            auto wire = geom::makePolylineWire(p.points);
            if (!wire) return Result<TopoDS_Shape>::failure(wire.error());
            return TopoDS_Shape(wire.value());
          },
          [](const ImportedShape& s) -> Result<TopoDS_Shape> {
            if (s.shape.IsNull()) return Result<TopoDS_Shape>::failure("Imported shape is empty");
            return s.shape;
          },
      },
      base);
}

}  // namespace

Result<TopoDS_Shape> regenerate(const Recipe& recipe) {
  auto current = buildBase(recipe.base);
  if (!current) return current;
  TopoDS_Shape shape = current.value();

  for (std::size_t i = 0; i < recipe.modifiers.size(); ++i) {
    const std::string prefix = "Step " + std::to_string(i + 1) + ": ";
    Result<TopoDS_Shape> next = std::visit(
        Overloaded{
            [&](const ExtrudeOp& op) -> Result<TopoDS_Shape> {
              if (i != 0 || shape.ShapeType() != TopAbs_FACE) {
                return Result<TopoDS_Shape>::failure("Extrude requires a profile face");
              }
              return geom::extrudeFace(TopoDS::Face(shape), op.distance);
            },
            [&](const PushPullOp& op) -> Result<TopoDS_Shape> {
              const geom::ValidationReport report = geom::validateShape(shape, true);
              if (!report.valid) {
                return Result<TopoDS_Shape>::failure("Push/pull requires a valid solid");
              }
              auto face = op.face.resolve(shape);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              return geom::pushPullFace(shape, face.value(), op.distance);
            },
        },
        recipe.modifiers[i]);
    if (!next) return Result<TopoDS_Shape>::failure(prefix + next.error());
    shape = next.value();
  }
  return shape;
}

std::vector<Parameter> parameters(const Recipe& recipe) {
  std::vector<Parameter> out;
  if (const auto* r = std::get_if<RectangleProfile>(&recipe.base)) {
    out.push_back({"width", "Width", r->width, true});
    out.push_back({"height", "Height", r->height, true});
  }
  for (std::size_t i = 0; i < recipe.modifiers.size(); ++i) {
    std::visit(Overloaded{
                   [&](const ExtrudeOp& op) {
                     out.push_back({opKey(i), "Extrude distance", op.distance, true});
                   },
                   [&](const PushPullOp& op) {
                     out.push_back({opKey(i), "Push/Pull " + std::to_string(i) + " distance",
                                    op.distance, true});
                   },
               },
               recipe.modifiers[i]);
  }
  return out;
}

Status setParameter(Recipe& recipe, const std::string& key, double value) {
  if (!std::isfinite(value)) return Status::failure("Value must be finite");
  if (auto* r = std::get_if<RectangleProfile>(&recipe.base)) {
    if (key == "width") {
      r->width = value;
      return Status::success();
    }
    if (key == "height") {
      r->height = value;
      return Status::success();
    }
  }
  for (std::size_t i = 0; i < recipe.modifiers.size(); ++i) {
    if (key != opKey(i)) continue;
    std::visit([&](auto& op) { op.distance = value; }, recipe.modifiers[i]);
    return Status::success();
  }
  return Status::failure("Unknown parameter: " + key);
}

std::string describe(const Recipe& recipe) {
  std::ostringstream out;
  std::visit(Overloaded{
                 [&](const RectangleProfile& r) {
                   out << "Rectangle " << r.width << " x " << r.height;
                 },
                 [&](const PolygonProfile& p) { out << "Polygon (" << p.points.size() << " pts)"; },
                 [&](const PolylineCurve& p) { out << "Line (" << p.points.size() << " pts)"; },
                 [&](const ImportedShape& s) { out << "Imported (" << s.source << ")"; },
             },
             recipe.base);
  for (const Modifier& m : recipe.modifiers) {
    std::visit(Overloaded{
                   [&](const ExtrudeOp& op) { out << " > Extrude " << op.distance; },
                   [&](const PushPullOp& op) { out << " > Push/Pull " << op.distance; },
               },
               m);
  }
  return out.str();
}

}  // namespace sbr::doc
