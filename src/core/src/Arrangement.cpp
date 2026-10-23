#include "sbr/geom/Arrangement.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <gp_Ax3.hxx>
#include <gp_XY.hxx>

namespace sbr::geom {
namespace {

constexpr double kTol = 1e-4;  // mm: points closer than this are one point

double cross(const gp_XY& a, const gp_XY& b) { return a.X() * b.Y() - a.Y() * b.X(); }

}  // namespace

LineArrangement arrangeLines(const std::vector<std::pair<gp_Pnt, gp_Pnt>>& segments, const std::vector<bool>& fresh,
                             const gp_Pln& plane) {
  LineArrangement out;
  const gp_Ax3 frame = plane.Position();
  const gp_XYZ o = frame.Location().XYZ(), ex = frame.XDirection().XYZ(), ey = frame.YDirection().XYZ();
  auto to2d = [&](const gp_Pnt& p) {
    const gp_XYZ d = p.XYZ() - o;
    return gp_XY(d.Dot(ex), d.Dot(ey));
  };
  auto to3d = [&](const gp_XY& p) { return gp_Pnt(o + ex * p.X() + ey * p.Y()); };

  std::vector<std::pair<gp_XY, gp_XY>> segs;
  for (const auto& [a, b] : segments) segs.emplace_back(to2d(a), to2d(b));

  // Where each segment is cut: parameters along it (0 and 1 are its ends).
  std::vector<std::vector<double>> cuts(segs.size(), std::vector<double>{0.0, 1.0});
  auto paramOn = [](const std::pair<gp_XY, gp_XY>& s, const gp_XY& p) {
    const gp_XY d = s.second - s.first;
    return (p - s.first).Dot(d) / d.SquareModulus();
  };
  auto distanceTo = [](const std::pair<gp_XY, gp_XY>& s, const gp_XY& p) {
    const gp_XY d = s.second - s.first;
    return std::abs(cross(d, p - s.first)) / d.Modulus();
  };
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const gp_XY d1 = segs[i].second - segs[i].first;
    const double l1 = d1.Modulus();
    if (l1 < kTol) continue;
    for (std::size_t j = i + 1; j < segs.size(); ++j) {
      const gp_XY d2 = segs[j].second - segs[j].first;
      const double l2 = d2.Modulus();
      if (l2 < kTol) continue;
      const double den = cross(d1, d2);
      if (std::abs(den) > 1e-9 * l1 * l2) {
        const gp_XY w = segs[j].first - segs[i].first;
        const double t = cross(w, d2) / den, u = cross(w, d1) / den;
        const double ti = kTol / l1, tj = kTol / l2;
        if (t > -ti && t < 1 + ti && u > -tj && u < 1 + tj) {
          cuts[i].push_back(std::clamp(t, 0.0, 1.0));
          cuts[j].push_back(std::clamp(u, 0.0, 1.0));
        }
      } else if (distanceTo(segs[i], segs[j].first) < kTol) {
        // Collinear: each one's ends inside the other cut it.
        for (const gp_XY& p : {segs[j].first, segs[j].second}) {
          const double t = paramOn(segs[i], p);
          if (t > 0 && t < 1) cuts[i].push_back(t);
        }
        for (const gp_XY& p : {segs[i].first, segs[i].second}) {
          const double u = paramOn(segs[j], p);
          if (u > 0 && u < 1) cuts[j].push_back(u);
        }
      }
    }
  }

  // Vertices (merged within the tolerance) and undirected edges.
  std::vector<gp_XY> vertices;
  auto vertexAt = [&](const gp_XY& p) {
    for (std::size_t k = 0; k < vertices.size(); ++k) {
      if ((vertices[k] - p).Modulus() < kTol) return static_cast<int>(k);
    }
    vertices.push_back(p);
    return static_cast<int>(vertices.size() - 1);
  };
  std::map<std::pair<int, int>, std::vector<std::size_t>> edgePieces;  // (lo, hi) -> pieces
  std::set<std::pair<int, int>> freshEdges;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    std::vector<double>& c = cuts[i];
    std::sort(c.begin(), c.end());
    const gp_XY d = segs[i].second - segs[i].first;
    for (std::size_t k = 1; k < c.size(); ++k) {
      const gp_XY a = segs[i].first + d * c[k - 1], b = segs[i].first + d * c[k];
      if ((b - a).Modulus() < kTol) continue;
      const int va = vertexAt(a), vb = vertexAt(b);
      if (va == vb) continue;
      out.pieces.push_back({static_cast<int>(i), to3d(a), to3d(b), false});
      const std::pair<int, int> key{std::min(va, vb), std::max(va, vb)};
      edgePieces[key].push_back(out.pieces.size() - 1);
      if (i < fresh.size() && fresh[i]) freshEdges.insert(key);
    }
  }

  // Dangling tails bound nothing: drop ends of degree one until none is left.
  std::map<int, std::set<int>> adjacent;
  for (const auto& [key, pieces] : edgePieces) {
    adjacent[key.first].insert(key.second);
    adjacent[key.second].insert(key.first);
  }
  for (bool pruned = true; pruned;) {
    pruned = false;
    for (auto& [v, near] : adjacent) {
      if (near.size() != 1) continue;
      const int other = *near.begin();
      adjacent[other].erase(v);
      near.clear();
      pruned = true;
    }
  }

  // Around each vertex, its neighbours by angle (counter-clockwise).
  std::map<int, std::vector<int>> around;
  for (const auto& [v, near] : adjacent) {
    std::vector<int> sorted(near.begin(), near.end());
    std::sort(sorted.begin(), sorted.end(), [&](int a, int b) {
      const gp_XY da = vertices[a] - vertices[v], db = vertices[b] - vertices[v];
      return std::atan2(da.Y(), da.X()) < std::atan2(db.Y(), db.X());
    });
    around[v] = std::move(sorted);
  }

  // Walk every directed edge, always turning to the next edge clockwise:
  // each walk goes once round one area (enclosed ones counter-clockwise).
  std::set<std::pair<int, int>> walked;
  for (const auto& [v0, near] : around) {
    for (int v1 : near) {
      if (walked.count({v0, v1})) continue;
      std::vector<int> cycle;
      std::pair<int, int> edge{v0, v1};
      bool closed = false;
      for (std::size_t steps = 0; steps < 4 * edgePieces.size() + 4; ++steps) {
        walked.insert(edge);
        cycle.push_back(edge.first);
        const std::vector<int>& list = around[edge.second];
        const auto at = std::find(list.begin(), list.end(), edge.first);
        if (at == list.end()) break;
        const std::size_t idx = static_cast<std::size_t>(at - list.begin());
        const int next = list[(idx + list.size() - 1) % list.size()];
        edge = {edge.second, next};
        if (edge == std::make_pair(v0, v1)) {
          closed = true;
          break;
        }
      }
      if (!closed || cycle.size() < 3) continue;
      double area = 0;
      for (std::size_t k = 0; k < cycle.size(); ++k) {
        area += cross(vertices[cycle[k]], vertices[cycle[(k + 1) % cycle.size()]]);
      }
      if (area / 2 < 1e-2) continue;  // the outside, or nothing
      const std::set<int> distinct(cycle.begin(), cycle.end());
      if (distinct.size() != cycle.size()) continue;  // pinched by a bridge: not one simple area
      bool touchesFresh = false;
      for (std::size_t k = 0; k < cycle.size(); ++k) {
        const int a = cycle[k], b = cycle[(k + 1) % cycle.size()];
        touchesFresh = touchesFresh || freshEdges.count({std::min(a, b), std::max(a, b)});
      }
      if (!touchesFresh) continue;
      std::vector<gp_Pnt> region;
      for (std::size_t k = 0; k < cycle.size(); ++k) {
        const int a = cycle[k], b = cycle[(k + 1) % cycle.size()];
        for (std::size_t piece : edgePieces[{std::min(a, b), std::max(a, b)}]) out.pieces[piece].bounding = true;
        // Corners only: a straight run through a crossing is one side.
        const gp_XY prev = vertices[cycle[(k + cycle.size() - 1) % cycle.size()]];
        if (std::abs(cross(vertices[a] - prev, vertices[b] - vertices[a])) >
            1e-9 * (vertices[a] - prev).Modulus() * (vertices[b] - vertices[a]).Modulus()) {
          region.push_back(to3d(vertices[a]));
        }
      }
      if (region.size() >= 3) out.regions.push_back(std::move(region));
    }
  }
  return out;
}

}  // namespace sbr::geom
