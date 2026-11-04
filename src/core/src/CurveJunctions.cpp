#include "sbr/doc/Modeler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <set>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <NCollection_UBTree.hxx>
#include <NCollection_UBTreeFiller.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"

namespace sbr::doc {

// Endpoint-only loop search stays the fast/common path. After erasing a
// crossing piece, however, a remaining end can touch the middle of an arc.
// Split there for graph traversal, then keep every unused interval as a curve.
Result<ObjectId> Modeler::faceFromCurveJunctions(const ObjectId& curveId, bool* loopFound) {
  using R = Result<ObjectId>;
  constexpr double tol = 1e-3;  // same as the endpoint-only loop search
  const auto noLoop = [] { return R::failure("The curve does not close a loop"); };
  const auto* start = document_.find(curveId);
  if (!start) return noLoop();
  TopExp_Explorer wire(document_.worldShape(*start), TopAbs_WIRE);
  if (!wire.More()) return noLoop();
  TopoDS_Vertex a, b;
  TopExp::Vertices(TopoDS::Wire(wire.Current()), a, b);
  if (a.IsNull() || b.IsNull()) return noLoop();

  struct Edge {
    const Object* owner;
    TopoDS_Edge world;
    int index, count;
    std::vector<double> cuts{0.0, 1.0};
  };
  std::vector<Edge> edges;
  std::vector<gp_Pnt> endpoints{BRep_Tool::Pnt(a), BRep_Tool::Pnt(b)};
  bool curved = false;
  for (const auto& o : document_.objects()) {
    if (o.id == curveId || o.parent != start->parent || o.locked ||
        o.kind() != ObjectKind::Curve || !document_.isVisible(o.id)) continue;
    TopTools_IndexedMapOfShape local;
    TopExp::MapShapes(o.shape, TopAbs_EDGE, local);
    const auto placement = TopLoc_Location(document_.worldTransform(o));
    for (int i = 1; i <= local.Extent(); ++i) {
      const auto e = TopoDS::Edge(local(i).Moved(placement));
      TopoDS_Vertex first, last; TopExp::Vertices(e, first, last);
      if (first.IsNull() || last.IsNull()) continue;
      endpoints.push_back(BRep_Tool::Pnt(first)); endpoints.push_back(BRep_Tool::Pnt(last));
      curved |= BRepAdaptor_Curve(e).GetType() != GeomAbs_Line;
      edges.push_back({&o, e, i, local.Extent(), {0.0, 1.0}});
    }
  }
  if (!curved) return noLoop();  // straight crossings use the planar arrangement

  // Query nearby endpoints instead of projecting every drawing point onto
  // every edge: distant parts of a large drawing cannot make a junction here.
  NCollection_UBTree<int, Bnd_Box> points;
  NCollection_UBTreeFiller<int, Bnd_Box> filler(points);
  for (std::size_t i = 0; i < endpoints.size(); ++i) {
    Bnd_Box box; box.Add(endpoints[i]); filler.Add(static_cast<int>(i), box);
  }
  filler.Fill();
  struct Nearby : NCollection_UBTree<int, Bnd_Box>::Selector {
    Bnd_Box bounds;
    std::vector<int> found;
    Standard_Boolean Reject(const Bnd_Box& box) const override { return bounds.IsOut(box); }
    Standard_Boolean Accept(const int& index) override { found.push_back(index); return true; }
  };
  for (auto& edge : edges) {
    double first, last;
    const auto curve = BRep_Tool::Curve(edge.world, first, last);
    if (curve.IsNull() || last <= first) continue;
    Bnd_Box bounds; BRepBndLib::Add(edge.world, bounds); bounds.Enlarge(tol);
    Nearby nearby; nearby.bounds = bounds; points.Select(nearby);
    for (int index : nearby.found) {
      const auto& p = endpoints[index];
      if (bounds.IsOut(p) || p.Distance(curve->Value(first)) < tol || p.Distance(curve->Value(last)) < tol) continue;
      GeomAPI_ProjectPointOnCurve on(p, curve, first, last);
      if (on.NbPoints() == 0 || on.LowerDistance() > tol) continue;
      const double t = (on.LowerDistanceParameter() - first) / (last - first);
      if (t > 0 && t < 1) edge.cuts.push_back(t);
    }
    std::sort(edge.cuts.begin(), edge.cuts.end());
    edge.cuts.erase(std::unique(edge.cuts.begin(), edge.cuts.end(), [](double x, double y) {
      return std::abs(x - y) < 1e-12;
    }), edge.cuts.end());
  }

  std::vector<gp_Pnt> nodes;
  std::map<std::array<long long, 3>, std::vector<int>> cells;
  auto nodeOf = [&](const gp_Pnt& p) {
    const std::array<long long, 3> cell{static_cast<long long>(std::floor(p.X() / (4 * tol))),
                                       static_cast<long long>(std::floor(p.Y() / (4 * tol))),
                                       static_cast<long long>(std::floor(p.Z() / (4 * tol)))};
    for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
      const auto found = cells.find({cell[0] + x, cell[1] + y, cell[2] + z});
      if (found != cells.end()) for (int n : found->second) if (nodes[n].Distance(p) < tol) return n;
    }
    const int n = static_cast<int>(nodes.size()); nodes.push_back(p); cells[cell].push_back(n); return n;
  };
  struct Part { std::size_t edge; double first, last; int a, b; };
  std::vector<Part> parts;
  const int from = nodeOf(endpoints[1]), to = nodeOf(endpoints[0]);
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const auto& e = edges[i];
    BRepAdaptor_Curve curve(e.world);
    const double first = curve.FirstParameter(), span = curve.LastParameter() - first;
    for (std::size_t k = 1; k < e.cuts.size(); ++k) {
      const int a = nodeOf(curve.Value(first + span * e.cuts[k - 1]));
      const int b = nodeOf(curve.Value(first + span * e.cuts[k]));
      parts.push_back({i, e.cuts[k - 1], e.cuts[k], a, b});
    }
  }
  std::vector<std::vector<std::pair<int, int>>> links(nodes.size());
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto& p = parts[i];
    if (p.a == p.b) continue;
    links[p.a].push_back({static_cast<int>(i), p.b}); links[p.b].push_back({static_cast<int>(i), p.a});
  }
  std::vector<std::pair<int, int>> came(nodes.size(), {-1, -1});
  std::deque<int> queue{from}; came[from] = {-1, from};
  while (!queue.empty() && came[to].second == -1) {
    const int n = queue.front(); queue.pop_front();
    for (const auto& [part, next] : links[n]) {
      if (came[next].second != -1) continue;
      came[next] = {part, n}; queue.push_back(next);
    }
  }
  if (from == to || came[to].second == -1) return noLoop();
  if (loopFound) *loopFound = true;
  std::vector<int> path;
  for (int n = to; n != from; n = came[n].second) path.push_back(came[n].first);
  std::reverse(path.begin(), path.end());
  auto recipeFor = [&](const Part& p) {
    const auto& e = edges[p.edge];
    Recipe recipe = e.owner->recipe;
    const auto local = TopoDS::Edge(e.world.Moved(TopLoc_Location(document_.worldTransform(*e.owner).Inverted())));
    TopoDS_Vertex a, b; TopExp::Vertices(local, a, b);
    recipe.modifiers.push_back(CurveIntervalOp{e.index, e.count, p.first, p.last,
                                               BRep_Tool::Pnt(a), BRep_Tool::Pnt(b)});
    return recipe;
  };
  LoopProfile loop;
  loop.curves.push_back({std::make_shared<const Recipe>(start->recipe), start->transform, start->name});
  std::set<ObjectId> consumed{start->id};
  std::set<int> used;
  for (int i : path) {
    const auto& part = parts[i]; const auto* owner = edges[part.edge].owner;
    loop.curves.push_back({std::make_shared<const Recipe>(recipeFor(part)), owner->transform, owner->name});
    consumed.insert(owner->id); used.insert(i);
  }
  Object face;
  face.id = generateObjectId(); face.name = uniqueName("Face"); face.parent = start->parent;
  face.recipe.base = std::move(loop);
  if (auto built = rebuild(face, document_.regenContext()); !built) return R::failure(built.message());
  if (geom::area(face.shape) < tolerance::kMinDimension * tolerance::kMinDimension) return noLoop();
  const auto worldFace = TopoDS::Face(face.shape.Moved(TopLoc_Location(document_.parentTransform(*start))));
  if (facesIntoSolid(worldFace, face.parent)) {
    std::get<LoopProfile>(face.recipe.base).reversed = true;
    if (auto built = rebuild(face, document_.regenContext()); !built) return R::failure(built.message());
  }
  Command command; command.name = "Face";
  for (const auto& id : consumed) command.deltas.push_back({id, *document_.find(id), std::nullopt, document_.indexOf(id)});
  // Preserve overhangs, including a short arc past an erased line's true intersection.
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto& part = parts[i]; const auto* owner = edges[part.edge].owner;
    if (!consumed.count(owner->id) || used.count(static_cast<int>(i))) continue;
    Object rest = *owner;
    rest.id = generateObjectId(); rest.recipe = recipeFor(part);
    if (auto built = rebuild(rest, document_.regenContext()); !built) return R::failure(built.message());
    command.deltas.push_back({rest.id, std::nullopt, rest, std::nullopt});
  }
  command.deltas.push_back({face.id, std::nullopt, face, std::nullopt});
  history_.beginGroup("Face");
  history_.execute(std::move(command));
  punchHole(face.id);
  history_.endGroup();
  return face.id;
}
}  // namespace sbr::doc
