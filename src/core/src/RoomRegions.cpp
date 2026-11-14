#include "sbr/geom/RoomRegions.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace sbr::geom {
namespace {

constexpr double kMerge = 1.0;  // mm: points this near are one node
constexpr double kPi = 3.14159265358979323846;

struct V {
  double x, y;
};
V operator-(const PlanPoint& a, const PlanPoint& b) { return {a.x - b.x, a.y - b.y}; }
double cross(const V& a, const V& b) { return a.x * b.y - a.y * b.x; }
double dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y; }
double length(const V& a) { return std::hypot(a.x, a.y); }
double distance(const PlanPoint& a, const PlanPoint& b) { return length(a - b); }

// The lines p + t d and q + s e: (t, s), or nothing when parallel.
std::optional<std::pair<double, double>> meet(const PlanPoint& p, const V& d, const PlanPoint& q, const V& e) {
  const double den = cross(d, e);
  if (std::abs(den) < 1e-9) return std::nullopt;
  const V pq = q - p;
  return std::make_pair(cross(pq, e) / den, cross(pq, d) / den);
}

double signedArea(const std::vector<PlanPoint>& poly) {
  double a = 0.0;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const PlanPoint& p = poly[i];
    const PlanPoint& q = poly[(i + 1) % poly.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return a / 2;
}

bool inside(const std::vector<PlanPoint>& poly, const PlanPoint& p) {
  bool in = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const PlanPoint& a = poly[i];
    const PlanPoint& b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

// Does the closed outline cross itself? (Neighbouring sides share a corner: not a crossing.)
bool crossesItself(const std::vector<PlanPoint>& poly) {
  const std::size_t n = poly.size();
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      if (j == i + 1 || (i == 0 && j == n - 1)) continue;
      const PlanPoint& a = poly[i];
      const PlanPoint& b = poly[(i + 1) % n];
      const PlanPoint& c = poly[j];
      const PlanPoint& d = poly[(j + 1) % n];
      const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
      const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
      if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    }
  }
  return false;
}

struct HalfEdge {
  std::size_t from, to;
  std::size_t wall;
  bool forward;  // runs the way the wall was drawn
  bool used = false;
};

// One traced loop: its nodes in order and the half-edges between them.
struct Loop {
  std::vector<std::size_t> edges;  // half-edge indices
  std::vector<PlanPoint> points;
  double area = 0.0;  // signed
};

// The loop moved to the faces of its walls, on the side the loop goes round (its left).
std::optional<std::vector<PlanPoint>> toFaces(const Loop& loop, const std::vector<HalfEdge>& half,
                                              const std::vector<PlanWall>& walls) {
  const std::size_t n = loop.edges.size();
  if (n < 3) return std::nullopt;
  struct Side {
    PlanPoint p;  // a point of the face line
    V d;          // unit direction of travel
  };
  std::vector<Side> sides;
  for (std::size_t k = 0; k < n; ++k) {
    const HalfEdge& h = half[loop.edges[k]];
    const PlanPoint& a = loop.points[k];
    const PlanPoint& b = loop.points[(k + 1) % n];
    V d = b - a;
    const double len = length(d);
    if (len < 1e-9) return std::nullopt;
    d = {d.x / len, d.y / len};
    const double reach = h.forward ? walls[h.wall].left : walls[h.wall].right;
    sides.push_back({{a.x - d.y * reach, a.y + d.x * reach}, d});
  }
  std::vector<PlanPoint> out;
  for (std::size_t k = 0; k < n; ++k) {
    const Side& in = sides[(k + n - 1) % n];
    const Side& next = sides[k];
    const double turn = cross(in.d, next.d);
    if (std::abs(turn) < 1e-9) {
      if (dot(in.d, next.d) < 0) return std::nullopt;  // doubling back: no outline here
      // Straight on. The same face: no corner. Another thickness: a step at the node.
      const PlanPoint& node = loop.points[k];
      const V toIn = in.p - node, toNext = next.p - node;
      const double offIn = cross(in.d, toIn), offNext = cross(next.d, toNext);
      if (std::abs(offIn - offNext) < 1e-6) continue;
      out.push_back({node.x - in.d.y * offIn, node.y + in.d.x * offIn});
      out.push_back({node.x - next.d.y * offNext, node.y + next.d.x * offNext});
      continue;
    }
    const auto m = meet(in.p, in.d, next.p, next.d);
    if (!m) return std::nullopt;
    out.push_back({in.p.x + in.d.x * m->first, in.p.y + in.d.y * m->first});
  }
  if (out.size() < 3 || crossesItself(out)) return std::nullopt;
  return out;
}

}  // namespace

RoomRegions roomRegions(const std::vector<PlanWall>& walls) {
  RoomRegions result;
  const std::size_t count = walls.size();
  // 1. Each wall's line, run on at its ends to the line of the wall it reaches.
  struct Line {
    PlanPoint a, b;
    bool looseA = true, looseB = true;
    bool valid = false;
  };
  std::vector<Line> lines(count);
  for (std::size_t i = 0; i < count; ++i) {
    const PlanWall& w = walls[i];
    const V d = w.b - w.a;
    const double len = length(d);
    if (len < kMerge) continue;
    const V u{d.x / len, d.y / len};
    lines[i] = {w.a, w.b, true, true, true};
    for (int end = 0; end < 2; ++end) {
      const double s = end == 0 ? 0.0 : len;
      double best = 1e300, at = s;
      bool found = false;
      for (std::size_t j = 0; j < count; ++j) {
        if (j == i) continue;
        const PlanWall& o = walls[j];
        const V e = o.b - o.a;
        const double olen = length(e);
        if (olen < kMerge) continue;
        const V v{e.x / olen, e.y / olen};
        const auto m = meet(w.a, u, o.a, v);
        if (!m) continue;
        // Reaches the other's band: within its thickness of its line (plus this wall's own, for
        // corners where each stops at the other's face), and beside it.
        const double reach = o.right + o.left + std::max(w.right + w.left, 1.0) + 1.0;
        const double gap = std::abs(m->first - s);
        if (gap > reach || m->second < -reach || m->second > olen + reach || gap >= best) continue;
        best = gap;
        at = m->first;
        found = true;
      }
      if (!found) continue;
      const PlanPoint p{w.a.x + u.x * at, w.a.y + u.y * at};
      if (end == 0) {
        lines[i].a = p;
        lines[i].looseA = false;
      } else {
        lines[i].b = p;
        lines[i].looseB = false;
      }
    }
    if (distance(lines[i].a, lines[i].b) < kMerge) lines[i].valid = false;
  }
  // 2. Nodes: the ends, and wherever two lines meet or cross.
  std::vector<PlanPoint> nodes;
  auto nodeOf = [&](const PlanPoint& p) {
    for (std::size_t k = 0; k < nodes.size(); ++k)
      if (distance(nodes[k], p) <= kMerge) return k;
    nodes.push_back(p);
    return nodes.size() - 1;
  };
  std::vector<std::vector<std::pair<double, std::size_t>>> along(count);  // (parameter, node) per line
  for (std::size_t i = 0; i < count; ++i) {
    if (!lines[i].valid) continue;
    const double len = distance(lines[i].a, lines[i].b);
    along[i].push_back({0.0, nodeOf(lines[i].a)});
    along[i].push_back({len, nodeOf(lines[i].b)});
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (!lines[i].valid) continue;
    const V d = lines[i].b - lines[i].a;
    const double len = length(d);
    const V u{d.x / len, d.y / len};
    for (std::size_t j = i + 1; j < count; ++j) {
      if (!lines[j].valid) continue;
      const V e = lines[j].b - lines[j].a;
      const double olen = length(e);
      const V v{e.x / olen, e.y / olen};
      const auto m = meet(lines[i].a, u, lines[j].a, v);
      if (!m) continue;
      if (m->first < -kMerge || m->first > len + kMerge || m->second < -kMerge || m->second > olen + kMerge) continue;
      const PlanPoint p{lines[i].a.x + u.x * m->first, lines[i].a.y + u.y * m->first};
      const std::size_t node = nodeOf(p);
      along[i].push_back({std::clamp(m->first, 0.0, len), node});
      along[j].push_back({std::clamp(m->second, 0.0, olen), node});
    }
  }
  // 3. Edges between neighbouring nodes of each line.
  std::vector<HalfEdge> half;
  std::set<std::pair<std::size_t, std::size_t>> seen;
  for (std::size_t i = 0; i < count; ++i) {
    auto& points = along[i];
    std::sort(points.begin(), points.end());
    for (std::size_t k = 1; k < points.size(); ++k) {
      const std::size_t a = points[k - 1].second, b = points[k].second;
      if (a == b || !seen.insert({std::min(a, b), std::max(a, b)}).second) continue;  // one edge per pair of nodes
      half.push_back({a, b, i, true});
      half.push_back({b, a, i, false});
    }
  }
  // Loose ends: nodes with one edge, at an end that met nothing.
  {
    std::vector<int> degree(nodes.size(), 0);
    for (const HalfEdge& h : half) ++degree[h.from];
    for (std::size_t i = 0; i < count; ++i) {
      if (!lines[i].valid) continue;
      for (int end = 0; end < 2; ++end) {
        const PlanPoint& p = end == 0 ? lines[i].a : lines[i].b;
        const std::size_t node = nodeOf(p);
        if (node < degree.size() && degree[node] == 1) result.looseEnds.push_back(end == 0 ? walls[i].a : walls[i].b);
      }
    }
  }
  // 4. Take away dead ends (a wall sticking into a room does not divide it), again and again.
  std::vector<bool> dead(half.size(), false);
  for (bool again = true; again;) {
    again = false;
    std::vector<int> degree(nodes.size(), 0);
    for (std::size_t h = 0; h < half.size(); ++h)
      if (!dead[h]) ++degree[half[h].from];
    for (std::size_t h = 0; h < half.size(); h += 2) {
      if (dead[h]) continue;
      if (degree[half[h].from] == 1 || degree[half[h].to] == 1) {
        dead[h] = dead[h + 1] = true;
        again = true;
      }
    }
  }
  std::vector<bool> inside_(half.size(), false);  // dead ends: remembered for the walls of a room
  // 5. Trace loops: at each node, turn to the next edge clockwise from the way back.
  std::vector<std::vector<std::size_t>> out(nodes.size());
  for (std::size_t h = 0; h < half.size(); ++h)
    if (!dead[h]) out[half[h].from].push_back(h);
  auto angle = [&](std::size_t h) {
    const V d = nodes[half[h].to] - nodes[half[h].from];
    return std::atan2(d.y, d.x);
  };
  for (auto& list : out) std::sort(list.begin(), list.end(), [&](std::size_t a, std::size_t b) { return angle(a) < angle(b); });
  auto next = [&](std::size_t h) {
    const std::size_t node = half[h].to;
    const std::size_t back = h ^ 1;  // the pair is stored together
    const auto& list = out[node];
    const auto it = std::find(list.begin(), list.end(), back);
    const std::size_t at = static_cast<std::size_t>(it - list.begin());
    return list[(at + list.size() - 1) % list.size()];  // the one before it by angle: clockwise
  };
  std::vector<Loop> loops;
  for (std::size_t start = 0; start < half.size(); ++start) {
    if (dead[start] || half[start].used) continue;
    Loop loop;
    std::size_t h = start;
    std::size_t guard = 0;
    do {
      half[h].used = true;
      loop.edges.push_back(h);
      loop.points.push_back(nodes[half[h].from]);
      h = next(h);
    } while (h != start && ++guard <= half.size());
    loop.area = signedArea(loop.points);
    loops.push_back(std::move(loop));
  }
  (void)inside_;
  (void)kPi;
  // 6. Counter-clockwise loops are regions; clockwise ones are the outside of a group of walls:
  // a hole of the smallest region that holds it, or the outside of everything.
  std::vector<std::size_t> regionLoop;
  for (std::size_t k = 0; k < loops.size(); ++k)
    if (loops[k].area > kMerge * kMerge) regionLoop.push_back(k);
  std::vector<std::vector<std::size_t>> holes(regionLoop.size());
  for (std::size_t k = 0; k < loops.size(); ++k) {
    if (loops[k].area >= -kMerge * kMerge) continue;
    std::optional<std::size_t> owner;
    for (std::size_t r = 0; r < regionLoop.size(); ++r) {
      const Loop& outer = loops[regionLoop[r]];
      if (outer.area <= -loops[k].area) continue;  // must be bigger than what it holds
      // Every point of the hole inside it (a shared node would be on its outline, not inside).
      bool all = true;
      for (const PlanPoint& p : loops[k].points) all &= inside(outer.points, p);
      if (!all) continue;
      if (!owner || outer.area < loops[regionLoop[*owner]].area) owner = r;
    }
    if (owner) holes[*owner].push_back(k);
  }
  for (std::size_t r = 0; r < regionLoop.size(); ++r) {
    const Loop& outer = loops[regionLoop[r]];
    RoomRegion region;
    region.centre = outer.points;
    region.centreArea = outer.area;
    std::set<std::size_t> around;
    for (std::size_t h : outer.edges) around.insert(half[h].wall);
    bool innerKnown = true;
    const auto innerOuter = toFaces(outer, half, walls);
    innerKnown = innerOuter.has_value() && signedArea(*innerOuter) > 0;
    double innerArea = innerKnown ? signedArea(*innerOuter) : 0.0;
    for (std::size_t k : holes[r]) {
      const Loop& hole = loops[k];
      region.centreHoles.push_back(hole.points);
      region.centreArea += hole.area;  // negative
      for (std::size_t h : hole.edges) around.insert(half[h].wall);
      const auto innerHole = toFaces(hole, half, walls);
      if (!innerHole || signedArea(*innerHole) >= 0) {
        innerKnown = false;
        continue;
      }
      region.innerHoles.push_back(*innerHole);
      innerArea += signedArea(*innerHole);
    }
    // Dead ends standing in this room: their thickness is not taken out of the inner area.
    // Then the inner area would be too big: not given.
    for (std::size_t h = 0; h < half.size(); h += 2) {
      if (!dead[h]) continue;
      const PlanPoint& a = nodes[half[h].from];
      const PlanPoint& b = nodes[half[h].to];
      const PlanPoint mid{(a.x + b.x) / 2, (a.y + b.y) / 2};
      if (!inside(outer.points, mid)) continue;
      bool inHole = false;
      for (const auto& hole : region.centreHoles) inHole |= inside(hole, mid);
      if (inHole) continue;
      around.insert(half[h].wall);
      innerKnown = false;
    }
    region.innerKnown = innerKnown && innerArea > 0;
    if (region.innerKnown) {
      region.inner = *innerOuter;
      region.innerArea = innerArea;
      for (std::size_t i = 0; i < region.inner.size(); ++i)
        region.innerPerimeter += distance(region.inner[i], region.inner[(i + 1) % region.inner.size()]);
    } else {
      region.innerHoles.clear();
    }
    region.walls.assign(around.begin(), around.end());
    result.regions.push_back(std::move(region));
  }
  return result;
}

std::optional<std::size_t> regionAt(const RoomRegions& regions, const PlanPoint& p) {
  std::optional<std::size_t> found;
  for (std::size_t r = 0; r < regions.regions.size(); ++r) {
    const RoomRegion& region = regions.regions[r];
    if (!inside(region.centre, p)) continue;
    bool inHole = false;
    for (const auto& hole : region.centreHoles) inHole |= inside(hole, p);
    if (inHole) continue;
    if (!found || region.centreArea < regions.regions[*found].centreArea) found = r;
  }
  return found;
}

}  // namespace sbr::geom
