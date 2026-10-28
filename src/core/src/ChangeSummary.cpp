#include "sbr/doc/ChangeSummary.h"

#include <map>
#include <utility>

namespace sbr::doc {

namespace {

bool sameTransform(const gp_Trsf& a, const gp_Trsf& b) {
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 4; ++c) {
      if (a.Value(r, c) != b.Value(r, c)) return false;
    }
  }
  return true;
}

bool samePoint(const gp_Pnt& a, const gp_Pnt& b) { return a.Distance(b) < 1e-9; }

bool sameFaceMaterials(const std::vector<FaceMaterial>& a, const std::vector<FaceMaterial>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].material != b[i].material || a[i].plane.has_value() != b[i].plane.has_value()) return false;
    if (a[i].plane && !samePoint(a[i].plane->centroid, b[i].plane->centroid)) return false;
    if (a[i].surface && b[i].surface && !samePoint(a[i].surface->point, b[i].surface->point)) return false;
  }
  return true;
}

}  // namespace

unsigned compareObjects(const Object& before, const Object& after) {
  unsigned aspects = 0;
  if (!before.shape.IsSame(after.shape) || before.shape.Orientation() != after.shape.Orientation()) {
    aspects |= kGeometry;
  }
  if (!sameTransform(before.transform, after.transform)) aspects |= kPlacement;
  if (before.visible != after.visible) aspects |= kVisibility;
  if (before.name != after.name || before.layer != after.layer || before.color.r != after.color.r ||
      before.color.g != after.color.g || before.color.b != after.color.b || before.attributes != after.attributes) {
    aspects |= kAttributes;
  }
  if (before.parent != after.parent) aspects |= kHierarchy;
  if (before.material != after.material || !sameFaceMaterials(before.faceMaterials, after.faceMaterials)) {
    aspects |= kMaterial;
  }
  return aspects;
}

ChangeSummary summarize(const Command& command) {
  struct Net {
    const std::optional<Object>* before = nullptr;
    const std::optional<Object>* after = nullptr;
  };
  std::vector<ObjectId> order;
  std::map<ObjectId, Net> net;
  for (const ObjectDelta& d : command.deltas) {
    auto [it, inserted] = net.try_emplace(d.id);
    if (inserted) {
      order.push_back(d.id);
      it->second.before = &d.before;
    }
    it->second.after = &d.after;
  }
  ChangeSummary summary;
  for (const ObjectId& id : order) {
    const Net& n = net.at(id);
    const bool existed = n.before->has_value();
    const bool exists = n.after->has_value();
    if (!existed && exists) {
      summary.added.push_back(id);
    } else if (existed && !exists) {
      summary.removed.push_back(id);
    } else if (existed && exists) {
      if (const unsigned aspects = compareObjects(**n.before, **n.after)) summary.modified.push_back({id, aspects});
    }
  }
  for (const DefinitionDelta& d : command.definitionDeltas) {
    bool seen = false;
    for (const std::string& id : summary.definitions) seen |= id == d.id;
    if (!seen) summary.definitions.push_back(d.id);
  }
  for (const MaterialDelta& d : command.materialDeltas) {
    if (d.before && d.after && *d.before == *d.after) continue;
    bool seen = false;
    for (const std::string& id : summary.materials) seen |= id == d.id;
    if (!seen) summary.materials.push_back(d.id);
  }
  if (command.settings) {
    const PersistentSettings& a = command.settings->before;
    const PersistentSettings& b = command.settings->after;
    if (a.scenes != b.scenes) summary.settings.push_back("scenes");
    if (a.sections != b.sections || a.sectionDisplay != b.sectionDisplay) summary.settings.push_back("sections");
    if (a.hiddenTags != b.hiddenTags) summary.settings.push_back("tags");
  }
  return summary;
}

std::vector<std::string> aspectNames(unsigned aspects) {
  static const std::pair<unsigned, const char*> names[] = {{kGeometry, "geometry"},
                                                           {kPlacement, "placement"},
                                                           {kVisibility, "visibility"},
                                                           {kAttributes, "attributes"},
                                                           {kHierarchy, "hierarchy"},
                                                           {kMaterial, "material"}};
  std::vector<std::string> out;
  for (const auto& [bit, name] : names) {
    if (aspects & bit) out.push_back(name);
  }
  return out;
}

}  // namespace sbr::doc
