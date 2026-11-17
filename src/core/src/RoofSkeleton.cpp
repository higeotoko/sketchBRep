#include "sbr/geom/RoofSkeleton.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace sbr::geom {
namespace {

struct V {
  double x = 0.0, y = 0.0;
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y}; }
V operator*(V a, double k) { return {a.x * k, a.y * k}; }
double dot(V a, V b) { return a.x * b.x + a.y * b.y; }
double cross(V a, V b) { return a.x * b.y - a.y * b.x; }
double norm(V a) { return std::sqrt(dot(a, a)); }

const double kEps = 1e-3;  // mm: what counts as the same place

struct Line {
  V p, d, n;  // a point of the side, along it (unit), inwards (unit)
  bool moving = true;
  double slope = 0.4, out = 0.0, start = 0.0;
};

// One side as it is in a loop of the front. `pos`: where it starts (the corner with the side
// before it in the loop) now. `anchored`: that corner is between two sides in one line: it stays
// on the line across them through `anchor`.
struct Node {
  int side = 0;
  int face = 0;  // the part of the side's face this stretch of it sweeps
  V pos;
  bool anchored = false;
  V anchor;
};
using Loop = std::vector<Node>;

// What one stretch of a side sweeps: the paths of its two ends upwards; split in two when an
// inward corner runs into it.
struct Face {
  int side = 0;
  std::vector<V> left, right;
  int first = -1, second = -1;  // after a split: the stretch that keeps the left end, and the other
};

struct Work {
  std::vector<Line> lines;
  std::vector<Face> faces;
  std::vector<Loop> loops;
  double t = 0.0;
  std::string why;
  bool failed = false;

  void fail(const std::string& reason) {
    if (!failed) why = reason;
    failed = true;
  }
  // How far inside its side a line stands at height `at`, when its speed is the one it has just
  // after `t` (so that what is worked out from two heights is straight).
  double offset(const Line& l, double at) const {
    if (!l.moving || t < l.start - 1e-9) return -l.out;
    return -l.out + (at - l.start) / l.slope;
  }
  V point(const Line& l, double at) const { return l.p + l.n * offset(l, at); }
  // The corner at the start of node i of the loop at height `at`.
  std::optional<V> corner(const Loop& loop, std::size_t i, double at) const {
    const Node& node = loop[i];
    const Line& b = lines[static_cast<std::size_t>(node.side)];
    const V pb = point(b, at);
    if (node.anchored) return pb + b.d * dot(node.anchor - pb, b.d);
    const Line& a = lines[static_cast<std::size_t>(loop[(i + loop.size() - 1) % loop.size()].side)];
    const double den = cross(a.d, b.d);
    if (std::abs(den) < 1e-9) return std::nullopt;
    const V pa = point(a, at);
    return pa + a.d * (cross(pb - pa, b.d) / den);
  }
  void put(std::vector<V>& chain, V p) {
    if (chain.empty() || norm(chain.back() - p) > 1e-7) chain.push_back(p);
  }
  void record(const Loop& loop) {
    for (std::size_t i = 0; i < loop.size(); ++i) {
      Face& f = faces[static_cast<std::size_t>(loop[i].face)];
      put(f.left, loop[i].pos);
      put(f.right, loop[(i + 1) % loop.size()].pos);
    }
  }
  bool sameMotion(const Line& a, const Line& b) const {
    if (a.moving != b.moving || std::abs(a.out - b.out) > 1e-9) return false;
    return !a.moving || (std::abs(a.slope - b.slope) < 1e-12 && std::abs(a.start - b.start) < 1e-9);
  }
  // Sides before and at node i run the same way: one line, or it cannot be worked out here.
  bool anchorIfStraight(Loop& loop, std::size_t i) {
    Node& node = loop[i];
    const Line& a = lines[static_cast<std::size_t>(loop[(i + loop.size() - 1) % loop.size()].side)];
    const Line& b = lines[static_cast<std::size_t>(node.side)];
    if (std::abs(cross(a.d, b.d)) >= 1e-9 || dot(a.d, b.d) < 0) return true;
    if (std::abs(dot(point(b, t) - point(a, t), a.n)) > kEps || !sameMotion(a, b)) {
      fail("two sides in one direction that move differently meet");
      return false;
    }
    node.anchored = true;
    node.anchor = node.pos;
    return true;
  }
  // Takes out what has shrunk to nothing at this height: sides of no length, and sides folded
  // back onto each other (a ridge). False when the loop is finished.
  bool tidy(Loop& loop) {
    for (int guard = 0; guard < 10000; ++guard) {
      record(loop);
      if (loop.size() <= 2) return false;
      const std::size_t n = loop.size();
      bool changed = false;
      for (std::size_t i = 0; i < n && !changed; ++i) {
        const std::size_t j = (i + 1) % n;
        if (norm(loop[j].pos - loop[i].pos) > kEps) continue;
        // Side i has no length: its neighbours meet where it was.
        const V at = loop[i].pos;
        loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(i));
        const std::size_t k = i % loop.size();
        loop[k].pos = at;
        loop[k].anchored = false;
        if (loop.size() > 2 && !anchorIfStraight(loop, k)) return false;
        changed = true;
      }
      for (std::size_t i = 0; i < n && !changed; ++i) {
        const std::size_t h = (i + n - 1) % n, j = (i + 1) % n;
        const Line& a = lines[static_cast<std::size_t>(loop[h].side)];
        const Line& b = lines[static_cast<std::size_t>(loop[i].side)];
        if (std::abs(cross(a.d, b.d)) >= 1e-9 || dot(a.d, b.d) > 0) continue;
        if (std::abs(dot(point(b, t) - point(a, t), a.n)) > kEps) continue;
        // Side h runs to the corner and side i back along it: the shorter is used up.
        const double lenA = norm(loop[i].pos - loop[h].pos), lenB = norm(loop[j].pos - loop[i].pos);
        const V startA = loop[h].pos;
        if (lenA < lenB - kEps) {
          loop[i].pos = startA;
          loop[i].anchored = false;
          record(loop);
          loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(h));
          const std::size_t k = (h < i ? i - 1 : i) % loop.size();
          if (loop.size() > 2 && !anchorIfStraight(loop, k)) return false;
        } else if (lenB < lenA - kEps) {
          loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(i));
          const std::size_t k = i % loop.size();
          loop[k].anchored = false;
          if (loop.size() > 2 && !anchorIfStraight(loop, k)) return false;
        } else {
          // Both: the higher index first.
          const std::size_t first = std::max(h, i), second = std::min(h, i);
          loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(first));
          loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(second));
          if (!loop.empty()) {
            const std::size_t k = second % loop.size();
            loop[k].anchored = false;
            if (loop.size() > 2 && !anchorIfStraight(loop, k)) return false;
          }
        }
        changed = true;
      }
      if (failed) return false;
      if (!changed) return true;
    }
    fail("the front does not settle");
    return false;
  }
};

double area2(const std::vector<V>& p) {
  double a = 0.0;
  for (std::size_t i = 0; i < p.size(); ++i) a += cross(p[i], p[(i + 1) % p.size()]);
  return a;
}

bool crosses(V a, V b, V c, V d) {
  const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
  return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
}

}  // namespace

std::optional<std::vector<std::vector<SkeletonPoint>>> roofSkeletonFaces(const std::vector<SkeletonSide>& sides, std::string* why) {
  auto refuse = [&](const std::string& reason) -> std::optional<std::vector<std::vector<SkeletonPoint>>> {
    if (why) *why = reason;
    return std::nullopt;
  };
  const std::size_t n = sides.size();
  if (n < 3) return refuse("fewer than three sides");
  Work w;
  double first = std::numeric_limits<double>::infinity();
  for (const SkeletonSide& s : sides) {
    Line l;
    l.p = {s.a.x, s.a.y};
    const V along = V{s.b.x, s.b.y} - l.p;
    if (norm(along) < kEps) return refuse("a side has no length");
    l.d = along * (1.0 / norm(along));
    l.n = {-l.d.y, l.d.x};
    l.moving = s.moving;
    l.slope = s.slope;
    l.out = s.out;
    l.start = s.start;
    if (l.moving && !(l.slope > 1e-9)) return refuse("a side's slope is not above 0");
    if (l.moving) first = std::min(first, l.start);
    w.lines.push_back(l);
  }
  if (!std::isfinite(first)) return refuse("no side moves");
  // The front at the lowest eave: the outline grown by its overhangs.
  w.t = first - 1.0;  // before any side moves
  Loop loop;
  for (std::size_t i = 0; i < n; ++i) {
    Node node;
    node.side = node.face = static_cast<int>(i);
    Face face;
    face.side = static_cast<int>(i);
    w.faces.push_back(face);
    loop.push_back(node);
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Line& a = w.lines[(i + n - 1) % n];
    const Line& b = w.lines[i];
    if (std::abs(cross(a.d, b.d)) < 1e-9) {
      if (dot(a.d, b.d) < 0) return refuse("a side folds back onto the one before");
      if (!w.sameMotion(a, b)) return refuse("two sides in one line have different pitches or overhangs");
      loop[i].anchored = true;
      loop[i].anchor = b.p;
      loop[i].pos = b.p + b.n * -b.out;
      continue;
    }
    const auto at = w.corner(loop, i, w.t);
    if (!at) return refuse("a corner could not be placed");
    loop[i].pos = *at;
  }
  w.t = first;
  std::vector<V> outline;
  for (const Node& node : loop) outline.push_back(node.pos);
  const double whole = area2(outline);
  if (whole < 2.0) return refuse("the outline grown by its overhangs encloses nothing");
  for (std::size_t i = 0; i < n; ++i) {
    if (dot(outline[(i + 1) % n] - outline[i], w.lines[i].d) < kEps) return refuse("an overhang turns a side round");
    for (std::size_t j = i + 2; j < n; ++j) {
      if (i == 0 && j == n - 1) continue;
      if (crosses(outline[i], outline[(i + 1) % n], outline[j], outline[(j + 1) % n])) return refuse("the overhangs cross each other");
    }
  }
  w.loops.push_back(loop);
  if (!w.tidy(w.loops.back())) w.loops.pop_back();
  if (w.failed) return refuse(w.why);

  const int limit = static_cast<int>(40 * n + 200);
  for (int step = 0; step < limit && !w.loops.empty(); ++step) {
    // How every corner moves from here, and what happens first.
    std::vector<std::vector<V>> speed(w.loops.size());
    double dt = std::numeric_limits<double>::infinity();
    for (const Line& l : w.lines)
      if (l.moving && l.start > w.t + 1e-9) dt = std::min(dt, l.start - w.t);  // a side begins to move
    for (std::size_t q = 0; q < w.loops.size(); ++q) {
      const Loop& lp = w.loops[q];
      const std::size_t m = lp.size();
      for (std::size_t i = 0; i < m; ++i) {
        const auto next = w.corner(lp, i, w.t + 1.0);
        if (!next) return refuse("a corner between two sides in one direction could not be followed");
        speed[q].push_back(*next - lp[i].pos);
      }
      for (std::size_t i = 0; i < m; ++i) {
        const std::size_t j = (i + 1) % m;
        const Line& e = w.lines[static_cast<std::size_t>(lp[i].side)];
        // The side shrinks to nothing.
        const double len = dot(lp[j].pos - lp[i].pos, e.d), rate = dot(speed[q][j] - speed[q][i], e.d);
        if (rate < -1e-12) dt = std::min(dt, std::max(0.0, len) / -rate);
        // The inward corner at its start runs into another side.
        const Line& before = w.lines[static_cast<std::size_t>(lp[(i + m - 1) % m].side)];
        if (lp[i].anchored || cross(before.d, e.d) > -1e-9) continue;
        for (std::size_t k = 0; k < m; ++k) {
          if (k == i || k == (i + m - 1) % m) continue;
          const Line& c = w.lines[static_cast<std::size_t>(lp[k].side)];
          const std::size_t k1 = (k + 1) % m;
          const double d0 = dot(lp[i].pos - lp[k].pos, c.n), closing = dot(speed[q][i] - speed[q][k], c.n);
          if (d0 <= kEps || closing > -1e-12) continue;
          const double when = d0 / -closing;
          if (when >= dt) continue;
          const V v = lp[i].pos + speed[q][i] * when, cs = lp[k].pos + speed[q][k] * when, ce = lp[k1].pos + speed[q][k1] * when;
          const double along = dot(v - cs, c.d), length = dot(ce - cs, c.d);
          if (length > kEps && along >= -kEps && along <= length + kEps) dt = when;
        }
      }
    }
    if (!std::isfinite(dt)) return refuse("the roof does not close (no side reaches another)");
    for (std::size_t q = 0; q < w.loops.size(); ++q)
      for (std::size_t i = 0; i < w.loops[q].size(); ++i) w.loops[q][i].pos = w.loops[q][i].pos + speed[q][i] * dt;
    w.t += dt;
    // What has happened at this height: shrunk sides out, then corners that reached a side.
    for (std::size_t q = 0; q < w.loops.size();) {
      if (!w.tidy(w.loops[q])) {
        if (w.failed) return refuse(w.why);
        w.loops.erase(w.loops.begin() + static_cast<std::ptrdiff_t>(q));
        continue;
      }
      Loop& lp = w.loops[q];
      const std::size_t m = lp.size();
      bool split = false;
      for (std::size_t i = 0; i < m && !split; ++i) {
        const Line& before = w.lines[static_cast<std::size_t>(lp[(i + m - 1) % m].side)];
        const Line& e = w.lines[static_cast<std::size_t>(lp[i].side)];
        if (lp[i].anchored || cross(before.d, e.d) > -1e-9) continue;
        for (std::size_t k = 0; k < m && !split; ++k) {
          if (k == i || k == (i + m - 1) % m) continue;
          const Line& c = w.lines[static_cast<std::size_t>(lp[k].side)];
          const std::size_t k1 = (k + 1) % m;
          if (std::abs(dot(lp[i].pos - lp[k].pos, c.n)) > kEps) continue;
          const double along = dot(lp[i].pos - lp[k].pos, c.d), length = dot(lp[k1].pos - lp[k].pos, c.d);
          if (length <= kEps || along < -kEps || along > length + kEps) continue;
          // Coming towards it (not sliding along it).
          const auto vi = w.corner(lp, i, w.t + 1.0), vk = w.corner(lp, k, w.t + 1.0);
          if (!vi || !vk) return refuse("a corner could not be followed");
          if (dot((*vi - lp[i].pos) - (*vk - lp[k].pos), c.n) > -1e-12) continue;
          // The loop falls in two at the corner: [i .. k] and [k .. i - 1], side k in both.
          const V at = lp[i].pos;
          Loop one, two;
          for (std::size_t x = i;; x = (x + 1) % m) {
            one.push_back(lp[x]);
            if (x == k) break;
          }
          for (std::size_t x = k;; x = (x + 1) % m) {
            two.push_back(lp[x]);
            if (x == (i + m - 1) % m) break;
          }
          Face& whole = w.faces[static_cast<std::size_t>(lp[k].face)];
          Face a, b;
          a.side = b.side = whole.side;
          const int ia = static_cast<int>(w.faces.size()), ib = ia + 1;
          whole.first = ia;   // keeps the side's start: in `one`
          whole.second = ib;  // keeps its end: in `two`
          w.faces.push_back(a);
          w.faces.push_back(b);
          one.back().face = ia;
          one.front().pos = at;
          one.front().anchored = false;
          two.front().face = ib;
          two.front().pos = at;
          two.front().anchored = false;
          if (one.size() > 2 && !w.anchorIfStraight(one, 0)) return refuse(w.why);
          if (two.size() > 2 && !w.anchorIfStraight(two, 0)) return refuse(w.why);
          w.loops[q] = one;
          w.loops.push_back(two);
          split = true;
        }
      }
      if (!split) ++q;  // a split loop is looked at again
      if (w.loops.size() > 4 * n + 8) return refuse("the front falls into too many parts");
    }
  }
  if (!w.loops.empty()) return refuse("the roof does not close in the steps allowed");

  // Each side's face: up the path of its end, round what it was split into, down the path of its start.
  std::function<void(int, std::vector<V>&)> path = [&](int f, std::vector<V>& out) {
    const Face& face = w.faces[static_cast<std::size_t>(f)];
    for (const V& p : face.right) out.push_back(p);
    if (face.second >= 0) path(face.second, out);
    if (face.first >= 0) path(face.first, out);
    for (auto p = face.left.rbegin(); p != face.left.rend(); ++p) out.push_back(*p);
  };
  std::vector<std::vector<SkeletonPoint>> result(n);
  double total = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    std::vector<V> raw, clean;
    path(static_cast<int>(i), raw);
    for (const V& p : raw)
      if (clean.empty() || norm(clean.back() - p) > kEps) clean.push_back(p);
    while (clean.size() > 1 && norm(clean.front() - clean.back()) <= kEps) clean.pop_back();
    // Points on a straight run are left out (they only mark heights where something else
    // happened), and so are the tips of spikes of no width (a corner that slid along a side
    // still standing at its eave: that strip is the moving neighbour's).
    for (bool again = true; again && clean.size() >= 3;) {
      again = false;
      for (std::size_t k = 0; k < clean.size(); ++k) {
        const V &a = clean[(k + clean.size() - 1) % clean.size()], &b = clean[k], &c = clean[(k + 1) % clean.size()];
        if (norm(b - a) <= kEps || std::abs(cross(b - a, c - b)) <= 1e-6 * std::max(1.0, norm(b - a) * norm(c - b))) {
          clean.erase(clean.begin() + static_cast<std::ptrdiff_t>(k));
          again = true;
          break;
        }
      }
    }
    if (clean.size() < 3) continue;
    const double a = area2(clean);
    if (std::abs(a) < 2.0) continue;
    if (a < 0) return refuse("a face came out turned over");
    total += a;
    for (const V& p : clean) result[i].push_back({p.x, p.y});
  }
  if (std::abs(total - whole) > 1e-7 * whole + 2.0) return refuse("the faces do not add up to the outline");
  return result;
}

}  // namespace sbr::geom
