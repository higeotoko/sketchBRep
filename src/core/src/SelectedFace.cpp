#include "sbr/doc/Modeler.h"

#include <map>
#include <set>
#include <array>
#include <cmath>
#include <unordered_map>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include "sbr/geom/Freeform.h"
#include "sbr/geom/Measure.h"

namespace sbr::doc {
Result<ObjectId> Modeler::faceFromSelection(const std::vector<DeleteTarget>& targets) {
  using R = Result<ObjectId>;
  if (targets.empty()) return R::failure("Select curve edges for a face");
  LoopProfile loop;
  std::set<std::pair<ObjectId, int>> used;
  std::vector<std::pair<gp_Pnt, gp_Pnt>> ends;
  for (const auto& target : targets) {
    const Object* owner = document_.find(target.id);
    if (!owner || owner->kind() != ObjectKind::Curve || !target.faces.empty())
      return R::failure("Select curve edges for a face");
    bool locked = false;
    for (const Object* p = owner; p; p = p->parent.empty() ? nullptr : document_.find(p->parent)) locked |= p->locked;
    if (locked || !document_.isVisible(owner->id))
      return R::failure("Face boundary is hidden or locked");
    if (owner->parent != activeGroup_) return R::failure("Face boundary is outside the editing context");
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(owner->shape, TopAbs_EDGE, edges);
    const gp_Trsf world = document_.worldTransform(*owner);
    std::vector<int> selected;
    if (target.whole) {
      for (int i = 1; i <= edges.Extent(); ++i) selected.push_back(i);
    } else {
      std::unordered_map<const void*, std::vector<int>> partners;
      for (int i = 1; i <= edges.Extent(); ++i) partners[edges(i).TShape().get()].push_back(i);
      for (const auto& picked : target.edges) {
        int match = 0;
        // TopLoc datum handles can differ for equal world transforms. Keep
        // topology identity and compare the placement values, not handles.
        const auto found = partners.find(picked.TShape().get());
        if (found != partners.end()) for (int i : found->second) {
          const auto expected = world * edges(i).Location().Transformation();
          const auto actual = picked.Location().Transformation();
          bool same = true;
          for (int r = 1; r <= 3; ++r) for (int c = 1; c <= 4; ++c)
            same &= std::abs(expected.Value(r,c) - actual.Value(r,c)) < 1e-8;
          if (same) { match = i; break; }
        }
        if (!match) return R::failure("Face boundary changed; select the edges again");
        selected.push_back(match);
      }
    }
    const auto* imported = owner->recipe.modifiers.empty() ? std::get_if<ImportedShape>(&owner->recipe.base) : nullptr;
    BRep_Builder builder;
    TopoDS_Compound importedSubset;
    if (imported) builder.MakeCompound(importedSubset);
    for (int i : selected) {
      if (!used.emplace(owner->id, i).second) continue;
      if (used.size() > 4096) return R::failure("Select at most 4096 boundary edges");
      const auto edge = TopoDS::Edge(edges(i));
      TopoDS_Vertex a, b; TopExp::Vertices(edge, a, b);
      if (a.IsNull() || b.IsNull()) return R::failure("Invalid face boundary edge");
      if (imported) builder.Add(importedSubset, edge);
      else {
        Recipe recipe = owner->recipe;
        recipe.modifiers.push_back(CurveIntervalOp{i, edges.Extent(), 0.0, 1.0,
            BRep_Tool::Pnt(a), BRep_Tool::Pnt(b)});
        loop.curves.push_back({std::make_shared<const Recipe>(std::move(recipe)), owner->transform, owner->name});
      }
      const auto placed = TopoDS::Edge(edge.Moved(TopLoc_Location(owner->transform)));
      TopExp::Vertices(placed, a, b);
      ends.emplace_back(BRep_Tool::Pnt(a), BRep_Tool::Pnt(b));
    }
    // Imported B-reps have no editable construction parameters. Snapshot
    // only their selected edges, not a whole large layer once per edge.
    // The original imported object and all its data remain in the document.
    if (imported && !selected.empty()) {
      Recipe subset; subset.base = ImportedShape{importedSubset, imported->source};
      loop.curves.push_back({std::make_shared<const Recipe>(std::move(subset)), owner->transform, owner->name});
    }
  }
  if (ends.empty()) return R::failure("Select curve edges for a face");
  // A simple boundary has two incident edges at every vertex. Spatial cells
  // bound the work even when a drawing layer contains thousands of edges.
  constexpr double tol = 1e-3;
  using Cell = std::array<long long, 3>;
  std::map<Cell, std::vector<int>> cells;
  std::vector<gp_Pnt> nodes;
  std::vector<int> degree;
  auto node = [&](const gp_Pnt& p) {
    const Cell c{static_cast<long long>(std::floor(p.X() / tol)),
                 static_cast<long long>(std::floor(p.Y() / tol)),
                 static_cast<long long>(std::floor(p.Z() / tol))};
    for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
      auto it = cells.find({c[0] + x, c[1] + y, c[2] + z});
      if (it != cells.end()) for (int i : it->second) if (nodes[i].Distance(p) < tol) return i;
    }
    const int i = static_cast<int>(nodes.size());
    nodes.push_back(p); degree.push_back(0); cells[c].push_back(i); return i;
  };
  for (const auto& [a, b] : ends) { ++degree[node(a)]; ++degree[node(b)]; }
  for (int d : degree) {
    if (d < 2) return R::failure("The loop is not closed");
    if (d > 2) return R::failure("Face boundary branches or contains duplicate edges");
  }
  Object face;
  face.name = uniqueName("Face"); face.parent = activeGroup_;
  face.recipe.base = std::move(loop);
  if (auto s = rebuild(face, document_.regenContext()); !s) return R::failure(s.message());
  if (geom::area(face.shape) < tolerance::kMinDimension * tolerance::kMinDimension)
    return R::failure("Polygon has zero area");
  const auto worldFace = TopoDS::Face(face.shape.Moved(
      TopLoc_Location(document_.parentTransform(*document_.find(targets.front().id)))));
  if (facesIntoSolid(worldFace, face.parent)) std::get<LoopProfile>(face.recipe.base).reversed = true;
  history_.beginGroup("Face from selected edges");
  auto id = addObject(std::move(face), "Face from selected edges");
  if (!id) { history_.abortGroup(); return id; }
  punchHole(*id);
  history_.endGroup();
  return id;
}
}  // namespace sbr::doc
