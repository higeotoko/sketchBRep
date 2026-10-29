#include "sbr/doc/Recipe.h"

#include <BRep_Builder.hxx>

#include <TopoDS_Shell.hxx>

#include <cmath>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <sstream>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopLoc_Location.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <TopoDS_Edge.hxx>
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
std::string scaleKey(std::size_t index) { return "op" + std::to_string(index) + ".scale"; }
std::string operandPrefix(std::size_t index) { return "in" + std::to_string(index) + "."; }

geom::BooleanOp toGeom(BooleanKind kind) {
  switch (kind) {
    case BooleanKind::Union: return geom::BooleanOp::Union;
    case BooleanKind::Subtract: return geom::BooleanOp::Subtract;
    case BooleanKind::Intersect: return geom::BooleanOp::Intersect;
  }
  return geom::BooleanOp::Union;
}

const std::vector<BooleanOperand>* operandsOf(const BaseFeature& base);

// Whether a recipe depends on more than itself (component definitions).
bool usesContext(const Recipe& recipe) {
  if (std::holds_alternative<ComponentInstance>(recipe.base)) return true;
  if (const auto* operands = operandsOf(recipe.base)) {
    for (const BooleanOperand& o : *operands) {
      if (o.recipe && usesContext(*o.recipe)) return true;
    }
  }
  for (const Modifier& m : recipe.modifiers) {
    const auto* hole = std::get_if<HoleOp>(&m);
    if (hole && hole->outline.recipe && usesContext(*hole->outline.recipe)) return true;
  }
  return false;
}

// Nested recipes are shared and never change once shared (const): their
// shapes are kept, so a wall cut again and again (each opening a Boolean
// on the one before) regenerates only the new step, not the whole chain
// every time (40 openings in one go took minutes).
struct Kept {
  std::shared_ptr<const Recipe> recipe;  // keeps the address from being reused
  TopoDS_Shape shape;
};
std::mutex keptMutex;
std::unordered_map<const Recipe*, Kept> kept;
thread_local bool keptOnThisThread = true;

void keep(const std::shared_ptr<const Recipe>& recipe, const TopoDS_Shape& shape) {
  if (!keptOnThisThread) return;
  const std::lock_guard<std::mutex> lock(keptMutex);
  constexpr std::size_t kMaxKept = 2048;
  if (kept.size() >= kMaxKept) kept.clear();
  kept.emplace(recipe.get(), Kept{recipe, shape});
}

// Booleans by their operands: kind, then each operand's (shared) recipe
// and placement. Empty when an operand depends on the context.
std::string booleanKey(const BooleanFeature& b) {
  if (!keptOnThisThread) return {};
  std::ostringstream key;
  key.precision(17);
  key << static_cast<int>(b.kind);
  for (const BooleanOperand& o : b.operands) {
    if (!o.recipe || usesContext(*o.recipe)) return {};
    key << '|' << static_cast<const void*>(o.recipe.get());
    for (int r = 1; r <= 3; ++r) {
      for (int c = 1; c <= 4; ++c) key << ',' << o.placement.Value(r, c);
    }
  }
  return key.str();
}

struct KeptBoolean {
  std::vector<std::shared_ptr<const Recipe>> operands;  // keeps the addresses from being reused
  TopoDS_Shape shape;
};
std::unordered_map<std::string, KeptBoolean> keptBooleans;

std::optional<TopoDS_Shape> keptBoolean(const std::string& key) {
  const std::lock_guard<std::mutex> lock(keptMutex);
  const auto it = keptBooleans.find(key);
  if (it == keptBooleans.end()) return std::nullopt;
  return it->second.shape;
}

void keepBoolean(const std::string& key, const BooleanFeature& b, const TopoDS_Shape& shape) {
  const std::lock_guard<std::mutex> lock(keptMutex);
  constexpr std::size_t kMaxKept = 512;
  if (keptBooleans.size() >= kMaxKept) keptBooleans.clear();
  KeptBoolean entry{{}, shape};
  for (const BooleanOperand& o : b.operands) entry.operands.push_back(o.recipe);
  keptBooleans.emplace(key, std::move(entry));
}

Result<TopoDS_Shape> regenerateShared(const std::shared_ptr<const Recipe>& recipe, const RegenContext& context) {
  if (keptOnThisThread) {
    const std::lock_guard<std::mutex> lock(keptMutex);
    if (const auto it = kept.find(recipe.get()); it != kept.end()) return it->second.shape;
  }
  auto shape = regenerate(*recipe, context);
  if (shape && !usesContext(*recipe)) keep(recipe, shape.value());
  return shape;
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
          [](const RegularPolygonProfile& p) -> Result<TopoDS_Shape> {
            if (p.sides < 3 || p.sides > 256) return Result<TopoDS_Shape>::failure("A polygon needs 3 to 256 sides");
            if (!(p.radius > tolerance::kMinDimension)) {
              return Result<TopoDS_Shape>::failure("Polygon radius must be positive");
            }
            std::vector<gp_Pnt> corners;
            const gp_Vec x(p.plane.xDir), y(p.plane.yDir());
            for (int k = 0; k < p.sides; ++k) {
              const double a = 2.0 * M_PI * k / p.sides;
              corners.push_back(p.plane.origin.Translated(p.radius * (std::cos(a) * x + std::sin(a) * y)));
            }
            auto face = geom::makePolygonFace(corners);
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
          [](const ImageUnderlay& u) -> Result<TopoDS_Shape> {
            auto face = geom::makeRectangleFace(u.plane, u.width, u.height);
            if (!face) return Result<TopoDS_Shape>::failure(face.error());
            return TopoDS_Shape(face.value());
          },
          [](const GuideLine& g) -> Result<TopoDS_Shape> {
            // "Infinite": 100 m each way is beyond any building.
            const gp_Vec reach = gp_Vec(g.direction) * 100000.0;
            return TopoDS_Shape(
                BRepBuilderAPI_MakeEdge(g.point.Translated(-reach), g.point.Translated(reach)).Edge());
          },
          [](const ImportedShape& s) -> Result<TopoDS_Shape> {
            if (s.shape.IsNull()) return Result<TopoDS_Shape>::failure("Imported shape is empty");
            return s.shape;
          },
          [&](const BooleanFeature& b) -> Result<TopoDS_Shape> {
            // The same operands (shared, never changed) placed the same way
            // give the same result: a wall cut by many openings is not cut
            // again for each later push / pull on it.
            const std::string key = booleanKey(b);
            if (!key.empty()) {
              if (auto kept = keptBoolean(key)) return *kept;
            }
            std::vector<TopoDS_Shape> shapes;
            for (std::size_t i = 0; i < b.operands.size(); ++i) {
              const BooleanOperand& operand = b.operands[i];
              if (!operand.recipe) return Result<TopoDS_Shape>::failure("Boolean operand missing");
              auto shape = regenerateShared(operand.recipe, context);
              if (!shape) {
                return Result<TopoDS_Shape>::failure("Operand '" + operand.name +
                                                     "': " + shape.error());
              }
              shapes.push_back(shape->Moved(TopLoc_Location(operand.placement)));
            }
            auto combined = geom::booleanCombine(toGeom(b.kind), shapes);
            if (combined && !key.empty()) keepBoolean(key, b, combined.value());
            return combined;
          },
          [&](const LoftFeature& l) -> Result<TopoDS_Shape> {
            std::vector<TopoDS_Wire> wires;
            for (const BooleanOperand& section : l.sections) {
              if (!section.recipe) return Result<TopoDS_Shape>::failure("Loft section missing");
              auto shape = regenerateShared(section.recipe, context);
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
          [&](const SewnFeature& f) -> Result<TopoDS_Shape> {
            std::vector<TopoDS_Shape> placed;
            for (const BooleanOperand& in : f.faces) {
              if (!in.recipe) return Result<TopoDS_Shape>::failure("Face missing");
              auto shape = regenerateShared(in.recipe, context);
              if (!shape) return Result<TopoDS_Shape>::failure("'" + in.name + "': " + shape.error());
              placed.push_back(shape->Moved(TopLoc_Location(in.placement)));
            }
            return geom::sewFaces(placed);
          },
          [&](const SweepFeature& f) -> Result<TopoDS_Shape> {
            if (f.inputs.size() != 2 || !f.inputs[0].recipe || !f.inputs[1].recipe) {
              return Result<TopoDS_Shape>::failure("Sweep needs a profile and a path");
            }
            std::vector<TopoDS_Shape> placed;
            for (const BooleanOperand& in : f.inputs) {
              auto shape = regenerateShared(in.recipe, context);
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
              auto shape = regenerateShared(c.recipe, context);
              if (!shape) return Result<TopoDS_Shape>::failure("'" + c.name + "': " + shape.error());
              curves.push_back(shape->Moved(TopLoc_Location(c.placement)));
            }
            auto face = geom::faceFromCurves(curves);
            if (!face) return Result<TopoDS_Shape>::failure(face.error());
            TopoDS_Face f = face.value();
            if (l.reversed) f.Reverse();
            return TopoDS_Shape(f);
          },
          [&](const FacePart& f) -> Result<TopoDS_Shape> {
            if (f.source.empty() || f.source.size()>2 || !f.source[0].recipe) return Result<TopoDS_Shape>::failure("Split face missing");
            auto shape = regenerateShared(f.source[0].recipe, context);
            if (!shape) return Result<TopoDS_Shape>::failure(shape.error());
            const TopoDS_Shape placed = shape->Moved(TopLoc_Location(f.source[0].placement));
            if (placed.ShapeType() != TopAbs_FACE) return Result<TopoDS_Shape>::failure("Split source is not a face");
            auto parts = [&]() -> Result<std::vector<TopoDS_Face>> {
              if(f.source.size()==1)return geom::splitPlanarFace(TopoDS::Face(placed), f.line);
              if(!f.source[1].recipe)return Result<std::vector<TopoDS_Face>>::failure("Split curve missing");
              auto curve=regenerateShared(f.source[1].recipe,context);
              if(!curve)return Result<std::vector<TopoDS_Face>>::failure(curve.error());
              return geom::splitPlanarFaceWithCurve(TopoDS::Face(placed),curve->Moved(TopLoc_Location(f.source[1].placement)));
            }();
            // The original face may have grown since (e.g. a larger width):
            // the split line then reaches on to its new edges.
            if (!parts && f.source.size()==1) parts = geom::splitPlanarFaceExtended(TopoDS::Face(placed), f.line);
            if (!parts) return Result<TopoDS_Shape>::failure(parts.error());
            for (const TopoDS_Face& part : parts.value()) {
              if (geom::pointOnFace(part, f.inside)) return TopoDS_Shape(part);
            }
            return Result<TopoDS_Shape>::failure("This part of the split face no longer exists");
          },
          [](const GroupNode&) -> Result<TopoDS_Shape> { return TopoDS_Shape(); },
          [&](const ComponentInstance& c) -> Result<TopoDS_Shape> {
            if (!c.plain()) {
              if (!context.instanceShape) {
                return Result<TopoDS_Shape>::failure("Component definitions are not available");
              }
              return context.instanceShape(c);
            }
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
  if (const auto* s = std::get_if<SewnFeature>(&base)) return &s->faces;
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

void useKeptShapes(bool on) { keptOnThisThread = on; }

void rememberShape(const std::shared_ptr<const Recipe>& recipe, const TopoDS_Shape& shape) {
  if (recipe && !shape.IsNull() && !usesContext(*recipe)) keep(recipe, shape);
}

Result<TopoDS_Shape> regenerate(const Recipe& recipe, const RegenContext& context) {
  auto current = buildBase(recipe.base, context);
  if (!current) return current;
  TopoDS_Shape shape = current.value();
  if (isContainer(recipe.base) && !recipe.modifiers.empty()) {
    return Result<TopoDS_Shape>::failure("Groups and components cannot have modifiers");
  }

  std::vector<geom::Segment> splitLines;  // kept through later push/pulls
  std::vector<TopoDS_Edge> imprinted;     // the same for lines drawn into curved faces
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
            [&](const HoleOp& op) -> Result<TopoDS_Shape> {
              if (!isFace) return Result<TopoDS_Shape>::failure("A hole requires a profile face");
              if (!op.outline.recipe) return Result<TopoDS_Shape>::failure("Hole outline missing");
              auto hole = regenerateShared(op.outline.recipe, context);
              if (!hole) return Result<TopoDS_Shape>::failure("Hole: " + hole.error());
              const TopoDS_Shape placed = hole->Moved(TopLoc_Location(op.outline.placement));
              if (placed.ShapeType() != TopAbs_FACE) return Result<TopoDS_Shape>::failure("Hole outline is not a face");
              auto cut = geom::cutHole(TopoDS::Face(shape), TopoDS::Face(placed));
              if (!cut) return Result<TopoDS_Shape>::failure(cut.error());
              return TopoDS_Shape(cut.value());
            },
            [&](const OffsetFaceOp& op) -> Result<TopoDS_Shape> {
              auto face = op.face.resolve(shape);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              if (op.part) return geom::pushPullCurvedPart(shape, face.value(), op.distance, splitLines, imprinted);
              return geom::offsetSolidFace(shape, face.value(), op.distance);
            },
            [&](const InsetOp& op) -> Result<TopoDS_Shape> {
              auto face = op.face.resolve(shape, splitLines);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              return geom::imprintOffset(shape, face.value(), op.distance);
            },
            [&](const SplitOp& op) -> Result<TopoDS_Shape> {
              auto face = op.face.resolve(shape, splitLines);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              if(op.curve) {
                if(!op.curve->recipe)return Result<TopoDS_Shape>::failure("Split curve missing");
                auto curve=regenerateShared(op.curve->recipe,context);
                if(!curve)return Result<TopoDS_Shape>::failure(curve.error());
                return geom::splitFaceWithCurve(shape,*face,curve->Moved(TopLoc_Location(op.curve->placement)));
              }
              for (std::size_t k = 1; k < op.points.size(); ++k) splitLines.emplace_back(op.points[k - 1], op.points[k]);
              return geom::splitFace(shape, face.value(), op.points);
            },
            [&](const StretchOp& op) -> Result<TopoDS_Shape> {
              if (!(op.fx > 1e-6 && op.fy > 1e-6 && op.fz > 1e-6) || !std::isfinite(op.fx) ||
                  !std::isfinite(op.fy) || !std::isfinite(op.fz)) {
                return Result<TopoDS_Shape>::failure("Scale factors must be positive");
              }
              const gp_GTrsf t = geom::stretchTransform(op.center, op.xDir, op.yDir, op.fx, op.fy, op.fz);
              for (geom::Segment& seg : splitLines) {
                gp_XYZ a = seg.first.XYZ(), b = seg.second.XYZ();
                t.Transforms(a);
                t.Transforms(b);
                seg = {gp_Pnt(a), gp_Pnt(b)};
              }
              // Stretched unevenly, a circle is no longer one: those lines
              // are no longer kept apart by later push/pulls.
              imprinted.clear();
              return geom::stretchShape(shape, t);
            },
            [&](const ScaleOp& op) -> Result<TopoDS_Shape> {
              if (!std::isfinite(op.factor) || op.factor < 1e-6) {
                return Result<TopoDS_Shape>::failure("Scale factor must be positive");
              }
              gp_Trsf t;
              t.SetScale(op.center, op.factor);
              BRepBuilderAPI_Transform scaled(shape, t, true);
              if (!scaled.IsDone()) return Result<TopoDS_Shape>::failure("Scaling failed");
              for (geom::Segment& s : splitLines) {
                s.first.Transform(t);
                s.second.Transform(t);
              }
              for (TopoDS_Edge& e : imprinted) e = TopoDS::Edge(BRepBuilderAPI_Transform(e, t, true).Shape());
              return scaled.Shape();
            },
            [&](const RemoveFaceOp& op) -> Result<TopoDS_Shape> {
              auto face = op.plane ? op.plane->resolve(shape, splitLines)
                          : op.surface ? op.surface->resolve(shape)
                                       : Result<TopoDS_Face>::failure("No face to delete");
              if (!face && op.plane) {
                // The deleted face moved with an earlier parameter (e.g. the
                // height): the one face carrying it on, if unambiguous.
                const std::vector<TopoDS_Face> moved = op.plane->continuations(shape);
                if (moved.size() == 1) face = moved.front();
              }
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              BRep_Builder builder;
              TopoDS_Shell shell;
              builder.MakeShell(shell);
              int kept = 0;
              for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
                if (e.Current().IsSame(face.value())) continue;
                builder.Add(shell, e.Current());
                ++kept;
              }
              if (kept == 0) return Result<TopoDS_Shape>::failure("Nothing would be left");
              shell.Closed(Standard_False);
              return TopoDS_Shape(shell);
            },
            [&](const CapHolesOp&) -> Result<TopoDS_Shape> { return geom::capHoles(shape); },
            [&](const MergeAcrossOp& op) -> Result<TopoDS_Shape> {
              // Gone with an earlier change (a parameter edited): nothing
              // to merge, the shape as it is.
              return geom::mergeFacesAcross(shape, op.point, op.direction);
            },
            [&](const ImprintOp&) -> Result<TopoDS_Shape> {
              // A run of them divides the faces in one go: one by one, each
              // would go through every face the ones before made.
              std::vector<TopoDS_Edge> edges;
              std::size_t j = i;
              for (; j < recipe.modifiers.size(); ++j) {
                const auto* op = std::get_if<ImprintOp>(&recipe.modifiers[j]);
                if (!op) break;
                if (!op->edges.recipe) return Result<TopoDS_Shape>::failure("Curved-face line missing");
                auto drawn = regenerateShared(op->edges.recipe, context);
                if (!drawn) return Result<TopoDS_Shape>::failure(drawn.error());
                const TopoDS_Shape placed = drawn->Moved(TopLoc_Location(op->edges.placement));
                for (TopExp_Explorer e(placed, TopAbs_EDGE); e.More(); e.Next()) edges.push_back(TopoDS::Edge(e.Current()));
              }
              i = j - 1;
              imprinted.insert(imprinted.end(), edges.begin(), edges.end());
              return geom::imprintEdges(shape, edges);
            },
            [&](const CurveIntervalOp& op) -> Result<TopoDS_Shape> {
              TopTools_IndexedMapOfShape edges;
              TopExp::MapShapes(shape, TopAbs_EDGE, edges);
              if (edges.Extent() != op.edgeCount || op.edge < 1 || op.edge > edges.Extent() ||
                  !std::isfinite(op.first) || !std::isfinite(op.last) ||
                  op.first < 0 || op.last > 1 || op.first >= op.last)
                return Result<TopoDS_Shape>::failure("The curve interval no longer identifies an edge");
              const auto edge = TopoDS::Edge(edges(op.edge));
              TopoDS_Vertex a, b; TopExp::Vertices(edge, a, b);
              if (a.IsNull() || b.IsNull()) return Result<TopoDS_Shape>::failure("The curve interval lost its ends");
              const gp_Pnt p = BRep_Tool::Pnt(a), q = BRep_Tool::Pnt(b);
              if (!((p.Distance(op.start) < tolerance::kLinear && q.Distance(op.end) < tolerance::kLinear) ||
                    (p.Distance(op.end) < tolerance::kLinear && q.Distance(op.start) < tolerance::kLinear)))
                return Result<TopoDS_Shape>::failure("The curve interval endpoints changed; select the outline again");
              double first, last;
              const auto curve = BRep_Tool::Curve(edge, first, last);
              if (curve.IsNull()) return Result<TopoDS_Shape>::failure("The curve interval has no curve");
              BRepBuilderAPI_MakeEdge part(curve, first + (last - first) * op.first,
                                                first + (last - first) * op.last);
              if (!part.IsDone()) return Result<TopoDS_Shape>::failure("The curve interval cannot be built");
              return TopoDS_Shape(BRepBuilderAPI_MakeWire(part.Edge()).Wire());
            },
            [&](const ReverseOp&) -> Result<TopoDS_Shape> { return shape.Reversed(); },
            [&](const MoveVerticesOp& op) -> Result<TopoDS_Shape> {
              return geom::moveVertices(shape, op.points, gp_Vec(op.direction) * op.distance, op.autofold);
            },
            [&](const PushPullOp& op) -> Result<TopoDS_Shape> {
              const bool solid = TopExp_Explorer(shape, TopAbs_SOLID).More();
              if (solid && !geom::validateShape(shape, true).valid) {
                return Result<TopoDS_Shape>::failure("Push/pull requires a valid solid");
              }
              auto face = op.face.resolve(shape, splitLines);
              if (!face) return Result<TopoDS_Shape>::failure(face.error());
              // An open shape: pushed / pulled as a surface (SketchUp).
              if (!solid) return geom::pushPullOpenFace(shape, face.value(), op.distance);
              return geom::pushPullFace(shape, face.value(), op.distance, splitLines, imprinted);
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
  if (const auto* u = std::get_if<ImageUnderlay>(&recipe.base)) {
    out.push_back({"width", "Width", u->width, true});  // the height follows (same proportions)
  }
  if (const auto* p = std::get_if<RegularPolygonProfile>(&recipe.base)) {
    out.push_back({"radius", "Radius", p->radius, true});
    out.push_back({"sides", "Sides", static_cast<double>(p->sides), false});
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
                   [&](const InsetOp& op) {
                     out.push_back({opKey(i), "Offset " + std::to_string(i) + " distance", op.distance, true});
                   },
                   [&](const RevolveOp& op) {
                     // Exposed in degrees for editing.
                     out.push_back({angleKey(i), "Revolve angle", op.angle * 180.0 / M_PI, false});
                   },
                   [&](const SplitOp&) {},
                   [&](const HoleOp&) {},
                   [&](const RemoveFaceOp&) {},
                   [&](const CapHolesOp&) {},
                   [&](const MergeAcrossOp&) {},
                   [&](const ImprintOp&) {},
                   [&](const CurveIntervalOp&) {},
                   [&](const ReverseOp&) {},
                   [&](const MoveVerticesOp& op) {
                     out.push_back({opKey(i), "Move " + std::to_string(i) + " distance", op.distance, true});
                   },
                   [&](const ScaleOp& op) { out.push_back({scaleKey(i), "Scale", op.factor, false}); },
                   [&](const StretchOp&) {},
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
  if (auto* u = std::get_if<ImageUnderlay>(&recipe.base); u && key == "width") {
    if (!(value > 0)) return Status::failure("Width must be positive");
    u->height *= value / u->width;
    u->width = value;
    return Status::success();
  }
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
  if (auto* p = std::get_if<RegularPolygonProfile>(&recipe.base)) {
    if (key == "radius") {
      p->radius = value;
      return Status::success();
    }
    if (key == "sides") {
      if (std::abs(value - std::round(value)) > 1e-9 || value < 3 || value > 256) {
        return Status::failure("A polygon needs 3 to 256 sides");
      }
      p->sides = static_cast<int>(std::lround(value));
      return Status::success();
    }
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
    if (auto* scale = std::get_if<ScaleOp>(&recipe.modifiers[i])) {
      if (key != scaleKey(i)) continue;
      if (!(value > 1e-6)) return Status::failure("Scale factor must be positive");
      scale->factor = value;
      return Status::success();
    }
    if (key != opKey(i)) continue;
    bool found = false;
    std::visit(Overloaded{[&](RevolveOp&) {}, [&](SplitOp&) {}, [&](HoleOp&) {}, [&](ScaleOp&) {}, [&](StretchOp&) {},
                          [&](RemoveFaceOp&) {}, [&](CapHolesOp&) {}, [&](ReverseOp&) {}, [&](MergeAcrossOp&) {}, [&](CurveIntervalOp&) {}, [&](ImprintOp&) {},
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
                 [&](const RegularPolygonProfile& p) { out << p.sides << "-gon r=" << p.radius; },
                 [&](const PolylineCurve& p) { out << "Line (" << p.points.size() << " pts)"; },
                 [&](const InterpolatedCurve& c) {
                   out << "NURBS curve (" << c.points.size() << " pts" << (c.closed ? ", closed" : "") << ")";
                 },
                 [&](const ArcCurve& a) {
                   out << "Arc " << a.start.Distance(a.end) << " bulge " << a.bulge << (a.closed ? " (segment)" : "");
                 },
                 [&](const LinearDimension& d) { out << "Dimension " << d.start.Distance(d.end); },
                 [&](const TextNote& t) { out << "Text \"" << t.text << "\""; },
                 [&](const GuideLine&) { out << "Guide"; },
                 [&](const ImageUnderlay&) { out << "Image"; },
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
                 [&](const SewnFeature& s) { out << "Solid from " << s.faces.size() << " faces"; },
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
                   [&](const ScaleOp& op) { out << " > Scale x" << op.factor; },
                   [&](const StretchOp& op) { out << " > Scale " << op.fx << " x " << op.fy << " x " << op.fz; },
                   [&](const HoleOp&) { out << " > Hole"; },
                   [&](const RemoveFaceOp&) { out << " > Delete face"; },
                   [&](const CapHolesOp&) { out << " > Fill holes"; },
                   [&](const MergeAcrossOp&) { out << " > Erase line"; },
                   [&](const ImprintOp&) { out << " > Divide curved face"; },
                   [&](const CurveIntervalOp&) { out << " > Curve interval"; },
                   [&](const ReverseOp&) { out << " > Reverse faces"; },
                   [&](const MoveVerticesOp& op) {
                     out << " > Move " << op.points.size() << (op.points.size() == 1 ? " corner " : " corners ")
                         << op.distance;
                   },
                   [&](const OffsetFaceOp& op) { out << " > Push/Pull (curved) " << op.distance; },
                   [&](const OffsetOp& op) { out << " > Offset " << op.distance; },
                   [&](const InsetOp& op) { out << " > Offset on face " << op.distance; },
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
