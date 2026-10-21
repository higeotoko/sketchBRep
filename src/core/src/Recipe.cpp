#include "sbr/doc/Recipe.h"

#include <cmath>
#include <sstream>

#include <BRepTools.hxx>
#include <TopLoc_Location.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/geom/Annotation.h"
#include "sbr/geom/Freeform.h"
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
std::string angleKey(std::size_t index) { return "op" + std::to_string(index) + ".angle"; }
std::string operandPrefix(std::size_t index) { return "in" + std::to_string(index) + "."; }

geom::BooleanOp toGeom(BooleanKind kind) {
  switch (kind) {
    case BooleanKind::Union: return geom::BooleanOp::Union;
    case BooleanKind::Subtract: return geom::BooleanOp::Subtract;
    case BooleanKind::Intersect: return geom::BooleanOp::Intersect;
  }
  return geom::BooleanOp::Union;
}

Result<TopoDS_Shape> buildBase(const BaseFeature& base, const RegenContext& context) {
  return std::visit(
      Overloaded{
          [](const RectangleProfile& r) -> Result<TopoDS_Shape> {
            auto face = geom::makeRectangleFace(r.plane, r.width, r.height);
            if (!face) return Result<TopoDS_Shape>::failure(face.error());
            return TopoDS_Shape(face.value());
          },
          [](const CircleProfile& c) -> Result<TopoDS_Shape> {
            auto face = geom::makeCircleFace(c.plane, c.radius);
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
          [](const InterpolatedCurve& c) -> Result<TopoDS_Shape> {
            auto wire = geom::makeInterpolatedCurve(c.points, c.closed);
            if (!wire) return Result<TopoDS_Shape>::failure(wire.error());
            return TopoDS_Shape(wire.value());
          },
          [](const ArcCurve& a) { return geom::makeArc(a.start, a.end, a.normal, a.bulge, a.closed); },
          [](const LinearDimension& d) { return geom::makeDimensionShape(d.start, d.end, d.offset); },
          [](const TextNote& t) { return geom::makeTextNoteShape(t.position, t.target); },
          [](const ImportedShape& s) -> Result<TopoDS_Shape> {
            if (s.shape.IsNull()) return Result<TopoDS_Shape>::failure("Imported shape is empty");
            return s.shape;
          },
          [&](const BooleanFeature& b) -> Result<TopoDS_Shape> {
            std::vector<TopoDS_Shape> shapes;
            for (std::size_t i = 0; i < b.operands.size(); ++i) {
              const BooleanOperand& operand = b.operands[i];
              if (!operand.recipe) return Result<TopoDS_Shape>::failure("Boolean operand missing");
              auto shape = regenerate(*operand.recipe, context);
              if (!shape) {
                return Result<TopoDS_Shape>::failure("Operand '" + operand.name +
                                                     "': " + shape.error());
              }
              shapes.push_back(shape->Moved(TopLoc_Location(operand.placement)));
            }
            return geom::booleanCombine(toGeom(b.kind), shapes);
          },
          [&](const LoftFeature& l) -> Result<TopoDS_Shape> {
            std::vector<TopoDS_Wire> wires;
            for (const BooleanOperand& section : l.sections) {
              if (!section.recipe) return Result<TopoDS_Shape>::failure("Loft section missing");
              auto shape = regenerate(*section.recipe, context);
              if (!shape) {
                return Result<TopoDS_Shape>::failure("Section '" + section.name + "': " + shape.error());
              }
              const TopoDS_Shape placed = shape->Moved(TopLoc_Location(section.placement));
              if (placed.ShapeType() == TopAbs_FACE) {
                wires.push_back(BRepTools::OuterWire(TopoDS::Face(placed)));
              } else if (placed.ShapeType() == TopAbs_WIRE) {
                wires.push_back(TopoDS::Wire(placed));
              } else {
                return Result<TopoDS_Shape>::failure("Loft sections must be faces or curves");
              }
            }
            return geom::loft(wires, l.solid);
          },
          [&](const SweepFeature& f) -> Result<TopoDS_Shape> {
            if (f.inputs.size() != 2 || !f.inputs[0].recipe || !f.inputs[1].recipe) {
              return Result<TopoDS_Shape>::failure("Sweep needs a profile and a path");
            }
            std::vector<TopoDS_Shape> placed;
            for (const BooleanOperand& in : f.inputs) {
              auto shape = regenerate(*in.recipe, context);
              if (!shape) return Result<TopoDS_Shape>::failure("'" + in.name + "': " + shape.error());
              placed.push_back(shape->Moved(TopLoc_Location(in.placement)));
            }
            TopExp_Explorer path(placed[1], TopAbs_WIRE);
            if (!path.More()) return Result<TopoDS_Shape>::failure("Sweep path must be a curve");
            return geom::sweep(placed[0], TopoDS::Wire(path.Current()));
          },
          [&](const LoopProfile& l) -> Result<TopoDS_Shape> {
            std::vector<TopoDS_Shape> curves;
            for (const BooleanOperand& c : l.curves) {
              if (!c.recipe) return Result<TopoDS_Shape>::failure("Loop curve missing");
              auto shape = regenerate(*c.recipe, context);
              if (!shape) return Result<TopoDS_Shape>::failure("'" + c.name + "': " + shape.error());
              curves.push_back(shape->Moved(TopLoc_Location(c.placement)));
            }
            auto face = geom::faceFromCurves(curves);
            if (!face) return Result<TopoDS_Shape>::failure(face.error());
            return TopoDS_Shape(face.value());
          },
          [&](const FacePart& f) -> Result<TopoDS_Shape> {
            if (f.source.size() != 1 || !f.source[0].recipe) return Result<TopoDS_Shape>::failure("Split face missing");
            auto shape = regenerate(*f.source[0].recipe, context);
            if (!shape) return Result<TopoDS_Shape>::failure(shape.error());
            const TopoDS_Shape placed = shape->Moved(TopLoc_Location(f.source[0].placement));
            if (placed.ShapeType() != TopAbs_FACE) return Result<TopoDS_Shape>::failure("Split source is not a face");
            auto parts = geom::splitPlanarFace(TopoDS::Face(placed), f.line);
            // The original face may have grown since (e.g. a larger width):
            // the split line then reaches on to its new edges.
            if (!parts) parts = geom::splitPlanarFaceExtended(TopoDS::Face(placed), f.line);
            if (!parts) return Result<TopoDS_Shape>::failure(parts.error());
            for (const TopoDS_Face& part : parts.value()) {
              if (geom::pointOnFace(part, f.inside)) return TopoDS_Shape(part);
            }
            return Result<TopoDS_Shape>::failure("This part of the split face no longer exists");
          },
          [](const GroupNode&) -> Result<TopoDS_Shape> { return TopoDS_Shape(); },
          [&](const ComponentInstance& c) -> Result<TopoDS_Shape> {
            if (!context.definitionShape) {
              return Result<TopoDS_Shape>::failure("Component definitions are not available");
            }
            auto shape = context.definitionShape(c.definitionId);
            if (!shape) {
              return Result<TopoDS_Shape>::failure("Unknown component definition " +
                                                   c.definitionId);
            }
            return *shape;
          },
      },
      base);
}

// Nested recipes of Booleans and lofts.
const std::vector<BooleanOperand>* operandsOf(const BaseFeature& base) {
  if (const auto* b = std::get_if<BooleanFeature>(&base)) return &b->operands;
  if (const auto* l = std::get_if<LoftFeature>(&base)) return &l->sections;
  if (const auto* w = std::get_if<SweepFeature>(&base)) return &w->inputs;
  if (const auto* l = std::get_if<LoopProfile>(&base)) return &l->curves;
  if (const auto* p = std::get_if<FacePart>(&base)) return &p->source;
  return nullptr;
}
std::vector<BooleanOperand>* operandsOf(BaseFeature& base) {
  return const_cast<std::vector<BooleanOperand>*>(operandsOf(static_cast<const BaseFeature&>(base)));
}

bool isContainer(const BaseFeature& base) {
  return std::holds_alternative<GroupNode>(base) || std::holds_alternative<ComponentInstance>(base);
}

}  // namespace

const char* toString(BooleanKind kind) {
  switch (kind) {
    case BooleanKind::Union: return "Union";
    case BooleanKind::Subtract: return "Subtract";
    case BooleanKind::Intersect: return "Intersect";
  }
  return "Union";
}

Result<TopoDS_Shape> regenerate(const Recipe& recipe, const RegenContext& context) {
  auto current = buildBase(recipe.base, context);
  if (!current) return current;
  TopoDS_Shape shape = current.value();
  if (isContainer(recipe.base) && !recipe.modifiers.empty()) {
    return Result<TopoDS_Shape>::failure("Groups and components cannot have modifiers");
  }

  for (std::size_t i = 0; i < recipe.modifiers.size(); ++i) {
    const std::string prefix = "Step " + std::to_string(i + 1) + ": ";
    const bool isFace = !shape.IsNull() && shape.ShapeType() == TopAbs_FACE;
    Result<TopoDS_Shape> next = std::visit(
        Overloaded{
            [&](const ExtrudeOp& op) -> Result<TopoDS_Shape> {
              if (!isFace) return Result<TopoDS_Shape>::failure("Extrude requires a profile face");
              return geom::extrudeFace(TopoDS::Face(shape), op.distance);
            },
            [&](const OffsetOp& op) -> Result<TopoDS_Shape> {
              if (!isFace) return Result<TopoDS_Shape>::failure("Offset requires a profile face");
              auto face = geom::offsetFace(TopoDS::Face(shape), op.distance);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              return TopoDS_Shape(face.value());
            },
            [&](const RevolveOp& op) -> Result<TopoDS_Shape> {
              if (!isFace) return Result<TopoDS_Shape>::failure("Revolve requires a profile face");
              return geom::revolve(shape, op.axis, op.angle);
            },
            [&](const OffsetFaceOp& op) -> Result<TopoDS_Shape> {
              auto face = op.face.resolve(shape);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              return geom::offsetSolidFace(shape, face.value(), op.distance);
            },
            [&](const SplitOp& op) -> Result<TopoDS_Shape> {
              auto face = op.face.resolve(shape);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              return geom::splitFace(shape, face.value(), op.points);
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
  if (const auto* c = std::get_if<CircleProfile>(&recipe.base)) {
    out.push_back({"radius", "Radius", c->radius, true});
  }
  if (const auto* a = std::get_if<ArcCurve>(&recipe.base)) {
    out.push_back({"bulge", "Bulge", a->bulge, true});
  }
  if (const auto* operands = operandsOf(recipe.base)) {
    for (std::size_t i = 0; i < operands->size(); ++i) {
      if (!(*operands)[i].recipe) continue;
      for (Parameter p : parameters(*(*operands)[i].recipe)) {
        p.key = operandPrefix(i) + p.key;
        p.label = (*operands)[i].name + ": " + p.label;
        out.push_back(std::move(p));
      }
    }
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
                   [&](const OffsetOp& op) {
                     out.push_back({opKey(i), "Offset distance", op.distance, true});
                   },
                   [&](const RevolveOp& op) {
                     // Exposed in degrees for editing.
                     out.push_back({angleKey(i), "Revolve angle", op.angle * 180.0 / M_PI, false});
                   },
                   [&](const SplitOp&) {},
                   [&](const OffsetFaceOp& op) {
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
  if (auto* c = std::get_if<CircleProfile>(&recipe.base); c && key == "radius") {
    c->radius = value;
    return Status::success();
  }
  if (auto* a = std::get_if<ArcCurve>(&recipe.base); a && key == "bulge") {
    a->bulge = value;
    return Status::success();
  }
  if (auto* operands = operandsOf(recipe.base)) {
    for (std::size_t i = 0; i < operands->size(); ++i) {
      const std::string prefix = operandPrefix(i);
      if (key.rfind(prefix, 0) != 0 || !(*operands)[i].recipe) continue;
      // Operand recipes are shared immutable snapshots: copy on write.
      Recipe copy = *(*operands)[i].recipe;
      const Status s = setParameter(copy, key.substr(prefix.size()), value);
      if (!s) return s;
      (*operands)[i].recipe = std::make_shared<const Recipe>(std::move(copy));
      return Status::success();
    }
  }
  for (std::size_t i = 0; i < recipe.modifiers.size(); ++i) {
    if (auto* revolve = std::get_if<RevolveOp>(&recipe.modifiers[i])) {
      if (key != angleKey(i)) continue;
      revolve->angle = value * M_PI / 180.0;
      return Status::success();
    }
    if (key != opKey(i)) continue;
    bool found = false;
    std::visit(Overloaded{[&](RevolveOp&) {}, [&](SplitOp&) {},
                          [&](auto& op) {
                            op.distance = value;
                            found = true;
                          }},
               recipe.modifiers[i]);
    if (found) return Status::success();
  }
  return Status::failure("Unknown parameter: " + key);
}

std::string describe(const Recipe& recipe) {
  std::ostringstream out;
  std::visit(Overloaded{
                 [&](const RectangleProfile& r) {
                   out << "Rectangle " << r.width << " x " << r.height;
                 },
                 [&](const CircleProfile& c) { out << "Circle r=" << c.radius; },
                 [&](const PolygonProfile& p) { out << "Polygon (" << p.points.size() << " pts)"; },
                 [&](const PolylineCurve& p) { out << "Line (" << p.points.size() << " pts)"; },
                 [&](const InterpolatedCurve& c) {
                   out << "NURBS curve (" << c.points.size() << " pts" << (c.closed ? ", closed" : "") << ")";
                 },
                 [&](const ArcCurve& a) {
                   out << "Arc " << a.start.Distance(a.end) << " bulge " << a.bulge << (a.closed ? " (segment)" : "");
                 },
                 [&](const LinearDimension& d) { out << "Dimension " << d.start.Distance(d.end); },
                 [&](const TextNote& t) { out << "Text \"" << t.text << "\""; },
                 [&](const ImportedShape& s) { out << "Imported (" << s.source << ")"; },
                 [&](const BooleanFeature& b) {
                   out << toString(b.kind) << " (";
                   for (std::size_t i = 0; i < b.operands.size(); ++i) {
                     out << (i ? ", " : "") << b.operands[i].name;
                   }
                   out << ")";
                 },
                 [&](const LoftFeature& l) { out << "Loft (" << l.sections.size() << " sections)"; },
                 [&](const SweepFeature&) { out << "Sweep"; },
                 [&](const LoopProfile& l) { out << "Face (" << l.curves.size() << " edges)"; },
                 [&](const FacePart&) { out << "Face part"; },
                 [&](const GroupNode&) { out << "Group"; },
                 [&](const ComponentInstance&) { out << "Component instance"; },
             },
             recipe.base);
  for (const Modifier& m : recipe.modifiers) {
    std::visit(Overloaded{
                   [&](const ExtrudeOp& op) { out << " > Extrude " << op.distance; },
                   [&](const PushPullOp& op) { out << " > Push/Pull " << op.distance; },
                   [&](const SplitOp&) { out << " > Split face"; },
                   [&](const OffsetFaceOp& op) { out << " > Push/Pull (curved) " << op.distance; },
                   [&](const OffsetOp& op) { out << " > Offset " << op.distance; },
                   [&](const RevolveOp& op) { out << " > Revolve " << op.angle * 180.0 / M_PI << " deg"; },
               },
               m);
  }
  return out.str();
}

namespace {

// Architectural style: millimetres without a unit, thousands separated
// ("3,000"); one decimal only when needed.
std::string dimensionText(double mm) {
  const double rounded = std::round(mm * 10.0) / 10.0;
  const bool whole = std::abs(rounded - std::round(rounded)) < 1e-9;
  const long long integer = static_cast<long long>(std::floor(std::abs(rounded) + (whole ? 0.5 : 0.0)));
  std::string digits = std::to_string(integer);
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<std::size_t>(i), ",");
  if (!whole) digits += "." + std::to_string(static_cast<int>(std::round((std::abs(rounded) - integer) * 10.0)));
  return (rounded < 0 ? "-" : "") + digits;
}

}  // namespace

bool isAnnotation(const Recipe& recipe) {
  return std::holds_alternative<LinearDimension>(recipe.base) || std::holds_alternative<TextNote>(recipe.base);
}

std::optional<AnnotationLabel> annotationLabel(const Recipe& recipe) {
  if (const auto* d = std::get_if<LinearDimension>(&recipe.base)) {
    const std::string text = d->text.empty() ? dimensionText(d->start.Distance(d->end)) : d->text;
    return AnnotationLabel{geom::dimensionTextPoint(d->start, d->end, d->offset), text};
  }
  if (const auto* t = std::get_if<TextNote>(&recipe.base)) return AnnotationLabel{t->position, t->text};
  return std::nullopt;
}

}  // namespace sbr::doc
