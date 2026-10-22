#include "sbr/snap/Inference.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Lin.hxx>

#include "sbr/Types.h"

namespace sbr::snap {

const char* toString(SnapKind kind) {
  switch (kind) {
    case SnapKind::None: return "None";
    case SnapKind::Endpoint: return "Endpoint";
    case SnapKind::Midpoint: return "Midpoint";
    case SnapKind::OnEdge: return "On Edge";
    case SnapKind::OnFace: return "On Face";
    case SnapKind::Origin: return "Origin";
    case SnapKind::AxisX: return "On Red Axis";
    case SnapKind::AxisY: return "On Green Axis";
    case SnapKind::AxisZ: return "On Blue Axis";
    case SnapKind::Locked: return "Locked";
    case SnapKind::Parallel: return "Parallel";
    case SnapKind::Perpendicular: return "Perpendicular";
    case SnapKind::FromPoint: return "From Point";
    case SnapKind::WorkPlane: return "On Plane";
    case SnapKind::Intersection: return "Intersection";
  }
  return "None";
}

gp_Pnt closestPointOnLineToRay(const gp_Pnt& p, const gp_Dir& dir, const Ray& ray) {
  // Closest points between two lines; returns the point on (p, dir).
  const gp_Vec u(dir);
  const gp_Vec v(ray.direction);
  const gp_Vec w(ray.origin, p);
  const double b = u.Dot(v);
  const double d = u.Dot(w);
  const double e = v.Dot(w);
  const double denom = 1.0 - b * b;  // |u| = |v| = 1
  if (std::abs(denom) < 1e-12) return p;
  const double s = (b * e - d) / denom;
  return p.Translated(u * s);
}

struct InferenceEngine::EdgeEntry {
  int shapeIndex = 0;
  TopoDS_Edge edge;
  bool isLine = false;
  gp_Pnt a, b;                 // ends of straight edges
  std::vector<gp_Pnt> samples; // samples of curved edges
};

// Index data of one shape. The face intersector is built on first use, and
// only for shapes whose bounds the sight line or ray actually crosses.
struct InferenceEngine::Slot {
  struct PointEntry {
    gp_Pnt point;
    SnapKind kind;
    int shapeIndex;
  };
  bool alive = false;
  int shapeIndex = -1;
  TopoDS_Shape shape;
  Bnd_Box box;
  std::vector<PointEntry> points;
  std::vector<EdgeEntry> edges;
  mutable std::unique_ptr<IntCurvesFace_ShapeIntersector> intersector;

  IntCurvesFace_ShapeIntersector& faces() const {
    if (!intersector) {
      intersector = std::make_unique<IntCurvesFace_ShapeIntersector>();
      intersector->Load(shape, 1e-6);
    }
    return *intersector;
  }
};

struct InferenceEngine::Cache {
  std::vector<Slot> slots;
  std::vector<std::size_t> freeSlots;
  std::unordered_map<std::string, std::size_t> byKey;
};

namespace {

template <typename Slot>
void indexShape(Slot& slot, const TopoDS_Shape& shape, int shapeIndex) {
  slot = Slot{};
  slot.alive = true;
  slot.shapeIndex = shapeIndex;
  slot.shape = shape;
  BRepBndLib::Add(shape, slot.box);
  slot.box.Enlarge(1.0);
  TopTools_IndexedMapOfShape vertices;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
  for (int i = 1; i <= vertices.Extent(); ++i) {
    slot.points.push_back({BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))), SnapKind::Endpoint, shapeIndex});
  }
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge edge = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(edge)) continue;
    BRepAdaptor_Curve curve(edge);
    const double f = curve.FirstParameter();
    const double l = curve.LastParameter();
    slot.points.push_back({curve.Value(0.5 * (f + l)), SnapKind::Midpoint, shapeIndex});
    typename decltype(slot.edges)::value_type entry;
    entry.shapeIndex = shapeIndex;
    entry.edge = edge;
    entry.isLine = curve.GetType() == GeomAbs_Line;
    entry.a = curve.Value(f);
    entry.b = curve.Value(l);
    if (!entry.isLine) {
      constexpr int kSamples = 64;
      for (int k = 0; k <= kSamples; ++k) entry.samples.push_back(curve.Value(f + (l - f) * k / kSamples));
    }
    slot.edges.push_back(std::move(entry));
  }
}

}  // namespace

bool InferenceEngine::isClipped(const gp_Pnt& p) const {
  if (!clip_) return false;
  return gp_Vec(clip_->Location(), p).Dot(gp_Vec(clip_->Axis().Direction())) > 1e-3;
}

bool InferenceEngine::isVisible(const gp_Pnt& p, const Ray& ray) const {
  if (isClipped(p)) return false;
  if (!cache_ || seeThrough_) return true;
  // Sight line from the viewer (ray origin: eye, or far behind the near
  // plane for orthographic views) to the point.
  const gp_Vec v(ray.origin, p);
  const double length = v.Magnitude();
  if (length < 1e-9) return true;
  const gp_Lin sight(ray.origin, gp_Dir(v));
  constexpr double kDepthTolerance = 0.5;  // mm: faces through p do not hide it
  for (const Slot& slot : cache_->slots) {
    if (!slot.alive || slot.box.IsOut(sight)) continue;
    IntCurvesFace_ShapeIntersector& intersector = slot.faces();
    if (!clip_) {
      intersector.PerformNearest(sight, 0.0, length - kDepthTolerance);
      if (intersector.IsDone() && intersector.NbPnt() > 0) return false;
      continue;
    }
    // With a section, faces cut away do not hide anything.
    intersector.Perform(sight, 0.0, length - kDepthTolerance);
    if (!intersector.IsDone()) continue;
    for (int i = 1; i <= intersector.NbPnt(); ++i) {
      if (!isClipped(intersector.Pnt(i))) return false;
    }
  }
  return true;
}

void InferenceEngine::setShapes(const std::vector<TopoDS_Shape>& shapes) {
  cache_ = std::make_shared<Cache>();
  cache_->slots.resize(shapes.size());
  for (std::size_t i = 0; i < shapes.size(); ++i) {
    if (!shapes[i].IsNull()) indexShape(cache_->slots[i], shapes[i], static_cast<int>(i));
  }
  guidePointsDirty_ = true;
}

void InferenceEngine::setShape(const std::string& key, const TopoDS_Shape& shape) {
  if (shape.IsNull()) {
    removeShape(key);
    return;
  }
  if (!cache_) cache_ = std::make_shared<Cache>();
  Cache& c = *cache_;
  std::size_t slot;
  if (const auto it = c.byKey.find(key); it != c.byKey.end()) {
    slot = it->second;
  } else if (!c.freeSlots.empty()) {
    slot = c.freeSlots.back();
    c.freeSlots.pop_back();
  } else {
    slot = c.slots.size();
    c.slots.emplace_back();
  }
  c.byKey[key] = slot;
  indexShape(c.slots[slot], shape, static_cast<int>(slot));
  guidePointsDirty_ = true;
}

void InferenceEngine::removeShape(const std::string& key) {
  if (!cache_) return;
  Cache& c = *cache_;
  const auto it = c.byKey.find(key);
  if (it == c.byKey.end()) return;
  c.slots[it->second] = Slot{};
  c.freeSlots.push_back(it->second);
  c.byKey.erase(it);
  guidePointsDirty_ = true;
}

std::size_t InferenceEngine::shapeCount() const {
  if (!cache_) return 0;
  std::size_t n = 0;
  for (const Slot& slot : cache_->slots) n += slot.alive;
  return n;
}

void InferenceEngine::setGuides(const std::vector<gp_Lin>& guides) {
  if (!cache_) cache_ = std::make_shared<Cache>();  // guides snap without any shape
  guides_ = guides;
  guidePointsDirty_ = true;
}

namespace {
// Crossing point of the line (p, u) with (q, v) as parameters along each,
// or nothing when they are parallel or skew.
std::optional<std::pair<double, double>> crossing(const gp_Pnt& p, const gp_Dir& u, const gp_Pnt& q,
                                                  const gp_Dir& v) {
  const gp_Vec uv(u), vv(v), w(q, p);
  const double b = uv.Dot(vv);
  const double denom = 1.0 - b * b;
  if (std::abs(denom) < 1e-12) return std::nullopt;
  const double d = uv.Dot(w), e = vv.Dot(w);
  const double s = (b * e - d) / denom;
  const double t = (e - b * d) / denom;
  if (p.Translated(uv * s).Distance(q.Translated(vv * t)) > tolerance::kLinear) return std::nullopt;
  return std::make_pair(s, t);
}
}  // namespace

void InferenceEngine::rebuildGuidePoints() const {
  guidePointsDirty_ = false;
  guidePoints_.clear();
  for (std::size_t i = 0; i < guides_.size(); ++i) {
    const gp_Lin& g = guides_[i];
    for (std::size_t j = i + 1; j < guides_.size(); ++j) {
      const gp_Lin& h = guides_[j];
      if (auto c = crossing(g.Location(), g.Direction(), h.Location(), h.Direction())) {
        guidePoints_.push_back(g.Location().Translated(gp_Vec(g.Direction()) * c->first));
      }
    }
    if (!cache_) continue;
    for (const Slot& slot : cache_->slots) {
      if (!slot.alive) continue;
      for (const EdgeEntry& e : slot.edges) {
      if (!e.isLine) continue;
      const gp_Vec ab(e.a, e.b);
      const double len = ab.Magnitude();
      if (len < 1e-9) continue;
      auto c = crossing(g.Location(), g.Direction(), e.a, gp_Dir(ab));
      if (!c || c->second < -tolerance::kLinear || c->second > len + tolerance::kLinear) continue;
      guidePoints_.push_back(g.Location().Translated(gp_Vec(g.Direction()) * c->first));
      }
    }
  }
}

namespace {

double screenDistance(const Projector& project, const gp_Pnt& p, const ScreenPoint& cursor) {
  const ScreenPoint s = project(p);
  return std::hypot(s.x - cursor.x, s.y - cursor.y);
}

SnapKind axisKind(int axis) {
  return axis == 0 ? SnapKind::AxisX : (axis == 1 ? SnapKind::AxisY : SnapKind::AxisZ);
}

std::optional<gp_Pnt> intersectPlane(const Ray& ray, const gp_Pln& plane) {
  const gp_Dir n = plane.Axis().Direction();
  const double denom = gp_Vec(ray.direction).Dot(gp_Vec(n));
  if (std::abs(denom) < 1e-9) return std::nullopt;
  const double t = gp_Vec(ray.origin, plane.Location()).Dot(gp_Vec(n)) / denom;
  if (t < 0) return std::nullopt;
  return ray.origin.Translated(gp_Vec(ray.direction) * t);
}

}  // namespace

SnapResult InferenceEngine::snap(const SnapQuery& q) const {
  if (guidePointsDirty_) rebuildGuidePoints();
  SnapResult result;
  const double tol = q.pixelTolerance;

  // 1. Exact points: endpoints, midpoints, origin. Points hidden behind
  //    faces are skipped (they may project right next to visible ones).
  std::optional<SnapResult> pointSnap;
  if (q.snapToGeometry && cache_) {
    std::vector<std::pair<double, SnapResult>> candidates;
    for (const Slot& slot : cache_->slots)
    for (const auto& entry : slot.points) {
      const double d = screenDistance(q.project, entry.point, q.cursor);
      const bool sticky = q.stickyPoint && q.stickyPoint->Distance(entry.point) < 1e-6;
      if (sticky && d < 1.5 * tol) {
        SnapResult r;
        r.kind = entry.kind;
        r.point = entry.point;
        r.shapeIndex = entry.shapeIndex;
        candidates.emplace_back(-1.0, r);  // keeps first place
        continue;
      }
      // Endpoints win over midpoints at equal distance.
      const double bias = entry.kind == SnapKind::Endpoint ? 0.0 : 0.5;
      if (d + bias >= tol) continue;
      SnapResult r;
      r.kind = entry.kind;
      r.point = entry.point;
      r.shapeIndex = entry.shapeIndex;
      candidates.emplace_back(d + bias, r);
    }
    for (const gp_Pnt& p : guidePoints_) {
      const double d = screenDistance(q.project, p, q.cursor) + 0.25;
      if (d >= tol) continue;
      SnapResult r;
      r.kind = SnapKind::Intersection;
      r.point = p;
      candidates.emplace_back(d, r);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [score, r] : candidates) {
      if (isVisible(r.point, q.ray)) {
        pointSnap = r;
        break;
      }
    }
  }
  if (!pointSnap && q.snapToGeometry) {
    const gp_Pnt origin(0, 0, 0);
    if (screenDistance(q.project, origin, q.cursor) < tol && isVisible(origin, q.ray)) {
      SnapResult r;
      r.kind = SnapKind::Origin;
      r.point = origin;
      pointSnap = r;
    }
  }

  // Shift lock: constrain to the locked line, using a point snap if present.
  if (q.lockedDirection && q.anchor) {
    const gp_Lin line(*q.anchor, *q.lockedDirection);
    result.kind = SnapKind::Locked;
    result.axis = *q.lockedDirection;
    if (pointSnap) {
      const gp_Vec v(*q.anchor, pointSnap->point);
      result.point = q.anchor->Translated(gp_Vec(*q.lockedDirection) *
                                          v.Dot(gp_Vec(*q.lockedDirection)));
    } else {
      result.point = closestPointOnLineToRay(*q.anchor, *q.lockedDirection, q.ray);
    }
    return result;
  }
  if (pointSnap) return *pointSnap;

  // 2. On edge (visible edges only).
  if (q.snapToGeometry && cache_) {
    std::vector<std::pair<double, SnapResult>> candidates;
    for (const Slot& slot : cache_->slots)
    for (const EdgeEntry& e : slot.edges) {
      gp_Pnt candidate;
      if (e.isLine) {
        const gp_Vec ab(e.a, e.b);
        const double len = ab.Magnitude();
        if (len < 1e-9) continue;
        const gp_Dir dir(ab);
        gp_Pnt p = closestPointOnLineToRay(e.a, dir, q.ray);
        double t = gp_Vec(e.a, p).Dot(gp_Vec(dir));
        t = std::clamp(t, 0.0, len);
        candidate = e.a.Translated(gp_Vec(dir) * t);
      } else {
        double bestSample = std::numeric_limits<double>::max();
        for (const gp_Pnt& s : e.samples) {
          const double d = screenDistance(q.project, s, q.cursor);
          if (d < bestSample) {
            bestSample = d;
            candidate = s;
          }
        }
      }
      const double d = screenDistance(q.project, candidate, q.cursor);
      if (d >= tol) continue;
      SnapResult r;
      r.kind = SnapKind::OnEdge;
      r.point = candidate;
      r.shapeIndex = e.shapeIndex;
      if (e.isLine && e.a.Distance(e.b) > 1e-9) r.edgeDirection = gp_Dir(gp_Vec(e.a, e.b));
      candidates.emplace_back(d, r);
    }
    for (const gp_Lin& g : guides_) {
      const gp_Pnt p = closestPointOnLineToRay(g.Location(), g.Direction(), q.ray);
      const double d = screenDistance(q.project, p, q.cursor);
      if (d >= tol) continue;
      SnapResult r;
      r.kind = SnapKind::OnEdge;
      r.point = p;
      r.edgeDirection = g.Direction();
      candidates.emplace_back(d, r);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [score, r] : candidates) {
      if (isVisible(r.point, q.ray)) return r;
    }
  }

  const gp_Dir axes[3] = {gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)};

  // 3. Axis inference from the anchor, preferring points where an anchor
  //    axis crosses an axis line through a tracked point.
  if (q.anchor) {
    double best = tol;
    std::optional<SnapResult> axisSnap;
    for (int a = 0; a < 3; ++a) {
      for (const gp_Pnt& tracked : q.trackedPoints) {
        for (int b = 0; b < 3; ++b) {
          if (a == b) continue;
          // Intersection of (anchor, axes[a]) and (tracked, axes[b]).
          const gp_Vec w(*q.anchor, tracked);
          if (std::abs(w.Dot(gp_Vec(axes[a]).Crossed(gp_Vec(axes[b])))) > tolerance::kLinear) continue;
          const double t = w.Dot(gp_Vec(axes[a]));
          const gp_Pnt p = q.anchor->Translated(gp_Vec(axes[a]) * t);
          if (std::abs(t) < 1e-6) continue;
          const double d = screenDistance(q.project, p, q.cursor);
          if (d < best) {
            best = d;
            SnapResult r;
            r.kind = axisKind(a);
            r.point = p;
            r.axis = axes[a];
            r.referencePoint = tracked;
            axisSnap = r;
          }
        }
      }
    }
    if (axisSnap) return *axisSnap;
    for (int a = 0; a < 3; ++a) {
      const gp_Pnt p = closestPointOnLineToRay(*q.anchor, axes[a], q.ray);
      if (p.Distance(*q.anchor) < 1e-6) continue;
      const double d = screenDistance(q.project, p, q.cursor);
      if (d < best) {
        best = d;
        SnapResult r;
        r.kind = axisKind(a);
        r.point = p;
        r.axis = axes[a];
        axisSnap = r;
      }
    }
    if (axisSnap) return *axisSnap;

    // Parallel / perpendicular to tracked edges.
    const gp_Dir planeNormal = q.workPlane.Axis().Direction();
    for (const gp_Dir& dir : q.trackedDirections) {
      std::vector<std::pair<gp_Dir, SnapKind>> candidates = {{dir, SnapKind::Parallel}};
      const gp_Vec perp = gp_Vec(dir).Crossed(gp_Vec(planeNormal));
      if (perp.Magnitude() > 1e-9) candidates.push_back({gp_Dir(perp), SnapKind::Perpendicular});
      for (const auto& [d, kind] : candidates) {
        bool isAxis = false;
        for (const gp_Dir& ax : axes) isAxis |= d.IsParallel(ax, 1e-9);
        if (isAxis) continue;  // already covered by axis inference
        const gp_Pnt p = closestPointOnLineToRay(*q.anchor, d, q.ray);
        if (p.Distance(*q.anchor) < 1e-6) continue;
        const double dist = screenDistance(q.project, p, q.cursor);
        if (dist < best) {
          best = dist;
          SnapResult r;
          r.kind = kind;
          r.point = p;
          r.axis = d;
          axisSnap = r;
        }
      }
    }
    if (axisSnap) return *axisSnap;
  }

  // 3b. Axis lines through tracked points ("from point").
  if (!q.trackedPoints.empty()) {
    double best = tol;
    std::optional<SnapResult> fromPoint;
    for (const gp_Pnt& tracked : q.trackedPoints) {
      for (int a = 0; a < 3; ++a) {
        const gp_Pnt p = closestPointOnLineToRay(tracked, axes[a], q.ray);
        if (p.Distance(tracked) < 1e-6) continue;
        const double d = screenDistance(q.project, p, q.cursor);
        if (d < best) {
          best = d;
          SnapResult r;
          r.kind = SnapKind::FromPoint;
          r.point = p;
          r.axis = axes[a];
          r.referencePoint = tracked;
          fromPoint = r;
        }
      }
    }
    if (fromPoint) return *fromPoint;
  }

  // 4. On face (nearest hit along the ray).
  if (q.snapToGeometry && cache_) {
    double bestParam = std::numeric_limits<double>::max();
    std::optional<SnapResult> faceSnap;
    const gp_Lin line(q.ray.origin, q.ray.direction);
    for (const Slot& slot : cache_->slots) {
      if (!slot.alive || slot.box.IsOut(line)) continue;
      IntCurvesFace_ShapeIntersector* intersector = &slot.faces();
      int hit = 0;
      if (!clip_) {
        intersector->PerformNearest(line, -1e12, 1e12);
        if (!intersector->IsDone() || intersector->NbPnt() < 1) continue;
        hit = 1;
      } else {
        // Nearest hit that the section does not cut away.
        intersector->Perform(line, -1e12, 1e12);
        if (!intersector->IsDone()) continue;
        double nearest = std::numeric_limits<double>::max();
        for (int i = 1; i <= intersector->NbPnt(); ++i) {
          if (isClipped(intersector->Pnt(i)) || intersector->WParameter(i) >= nearest) continue;
          nearest = intersector->WParameter(i);
          hit = i;
        }
        if (hit == 0) continue;
      }
      const double w = intersector->WParameter(hit);
      if (w < bestParam) {
        bestParam = w;
        SnapResult r;
        r.kind = SnapKind::OnFace;
        r.point = intersector->Pnt(hit);
        r.shapeIndex = slot.shapeIndex;
        r.face = intersector->Face(hit);
        BRepGProp_Face prop(r.face);
        gp_Pnt p;
        gp_Vec n;
        prop.Normal(intersector->UParameter(hit), intersector->VParameter(hit), p, n);
        if (n.Magnitude() > 1e-12) r.faceNormal = gp_Dir(n);
        faceSnap = r;
      }
    }
    if (faceSnap) return *faceSnap;
  }

  // 5. Work plane.
  if (const auto p = intersectPlane(q.ray, q.workPlane)) {
    result.kind = SnapKind::WorkPlane;
    result.point = *p;
    result.faceNormal = q.workPlane.Axis().Direction();
  }
  return result;
}

}  // namespace sbr::snap
