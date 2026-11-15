#include "sbr/geom/Stair.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Vec.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::geom {
namespace {

using R = Result<StairShape>;
constexpr double kPi = 3.14159265358979323846;
constexpr double kMeasureAt = 300.0;  // from a winder's narrow end

struct V2 {
  double x = 0, y = 0;
  V2 operator+(const V2& o) const { return {x + o.x, y + o.y}; }
  V2 operator-(const V2& o) const { return {x - o.x, y - o.y}; }
  V2 operator*(double s) const { return {x * s, y * s}; }
  double dot(const V2& o) const { return x * o.x + y * o.y; }
  double length() const { return std::hypot(x, y); }
};

struct Tread {
  std::vector<V2> outline;
  std::string kind;
  double going = 0.0;
  int step = 0;  // 1 = the first tread
};

struct Layout {
  int risers = 0;
  double riser = 0.0;
  std::vector<Tread> treads;
  double walk = 0.0;
  int flights = 0, landings = 0, winders = 0;
};

Result<Layout> layout(const StairSpec& spec) {
  using L = Result<Layout>;
  if (!(spec.width >= 300.0) || !std::isfinite(spec.width)) return L::failure("A stair is at least 300 wide");
  if (!(spec.rise >= 100.0) || !std::isfinite(spec.rise)) return L::failure("A stair climbs at least 100");
  if (!(spec.riserLimit >= 50.0)) return L::failure("The riser limit is at least 50");
  if (spec.risers < 0 || spec.risers > 200) return L::failure("The number of risers is 0 (automatic) to 200");
  if (spec.style != "block" && spec.style != "boards") return L::failure("Stair style: block or boards");
  if (spec.handrail != "none" && spec.handrail != "left" && spec.handrail != "right" && spec.handrail != "both")
    return L::failure("Handrail: none, left, right or both");
  if (spec.handrail != "none" && !(spec.handrailHeight >= 600.0 && spec.handrailHeight <= 1200.0))
    return L::failure("A handrail is 600 to 1200 above the treads");
  if (!(spec.thickness >= 5.0)) return L::failure("The tread thickness is at least 5");
  if (spec.stringer != "none" && spec.stringer != "left" && spec.stringer != "right" && spec.stringer != "both")
    return L::failure("Side boards: none, left, right or both");
  if (spec.stringer != "none" && !(spec.stringerThickness >= 15.0 && spec.stringerThickness <= 100.0))
    return L::failure("A side board is 15 to 100 thick");
  if (spec.stringerStyle != "side" && spec.stringerStyle != "cut") return L::failure("Side boards: side or cut");
  if (spec.riserBoards && spec.style != "boards") return L::failure("Riser boards are for a stair of tread boards");
  if (spec.riserBoards && !(spec.riserThickness >= 5.0 && spec.riserThickness <= 60.0)) return L::failure("A riser board is 5 to 60 thick");
  if (spec.newels && spec.stringer == "none") return L::failure("Newel posts stand with side boards");
  if (spec.newels && !(spec.newelSize >= 60.0 && spec.newelSize <= 240.0)) return L::failure("A newel post is 60 to 240 square");
  // The line: no repeated points, no points on a straight part.
  std::vector<V2> pts;
  for (const gp_Pnt& p : spec.path) {
    const V2 q{p.X(), p.Y()};
    if (!std::isfinite(q.x) || !std::isfinite(q.y)) return L::failure("The stair's line has a point that is not a number");
    if (!pts.empty() && (q - pts.back()).length() < 0.01) continue;
    if (pts.size() >= 2) {
      const V2 a = pts.back() - pts[pts.size() - 2], b = q - pts.back();
      const double cross = a.x * b.y - a.y * b.x;
      if (std::abs(cross) < 1e-6 * a.length() * b.length() && a.dot(b) > 0) pts.pop_back();
    }
    pts.push_back(q);
  }
  if (pts.size() < 2) return L::failure("A stair needs a line: where it starts and where it arrives");
  const std::size_t segments = pts.size() - 1, corners = segments - 1;
  std::vector<V2> dir(segments);
  std::vector<double> len(segments);
  Layout out;
  for (std::size_t i = 0; i < segments; ++i) {
    const V2 d = pts[i + 1] - pts[i];
    len[i] = d.length();
    dir[i] = d * (1.0 / len[i]);
    out.walk += len[i];
    if (i > 0 && std::abs(dir[i - 1].dot(dir[i])) > 1e-6)
      return L::failure("A stair turns at right angles only (turn " + std::to_string(i) + " is not one)");
  }
  std::vector<int> turns(corners, 3);
  for (std::size_t j = 0; j < corners; ++j) {
    if (!spec.turns.empty()) turns[j] = spec.turns[std::min(j, spec.turns.size() - 1)];
    if (turns[j] < 1 || turns[j] > 6) return L::failure("A turn is cut into 1 (a landing) to 6 treads");
  }
  const double w = spec.width, half = w / 2;
  // Straight flights: what is left of each part of the line between the corner squares.
  std::vector<double> flight(segments);
  double flightTotal = 0.0;
  for (std::size_t i = 0; i < segments; ++i) {
    flight[i] = len[i] - (i > 0 ? half : 0.0) - (i < corners ? half : 0.0);
    if (flight[i] < -0.01)
      return L::failure("Part " + std::to_string(i + 1) + " of the stair's line is shorter than the turns at its ends need (" +
                        std::to_string(static_cast<int>(std::lround((i > 0 ? half : 0.0) + (i < corners ? half : 0.0)))) + " for this width)");
    if (flight[i] < 1.0) flight[i] = 0.0;
    else ++out.flights;
    flightTotal += flight[i];
  }
  // Two landings with nothing between them are one landing (a U-turn's half landing).
  auto joined = [&](std::size_t j) { return j > 0 && turns[j] == 1 && turns[j - 1] == 1 && flight[j] == 0.0; };
  int cornerTreads = 0;
  for (std::size_t j = 0; j < corners; ++j) cornerTreads += joined(j) ? 0 : turns[j];
  const int least = cornerTreads + out.flights + 1;
  int risers = spec.risers;
  if (risers == 0) risers = std::max(static_cast<int>(std::ceil(spec.rise / spec.riserLimit - 1e-9)), std::max(least, 2));
  if (risers < least || risers < 2)
    return L::failure("This line needs at least " + std::to_string(std::max(least, 2)) + " risers (its turns and flights each take a tread)");
  out.risers = risers;
  out.riser = spec.rise / risers;
  if (spec.style == "block" && spec.thickness >= out.riser)
    return L::failure("A block stair's thickness is less than its riser (" + std::to_string(static_cast<int>(out.riser)) + ")");
  // Straight treads over the flights: one each, then always to the flight whose going is longest.
  int straight = risers - 1 - cornerTreads;
  std::vector<int> count(segments, 0);
  for (std::size_t i = 0; i < segments; ++i)
    if (flight[i] > 0) count[i] = 1, --straight;
  if (out.flights == 0 && straight > 0)
    return L::failure("This line has no straight part for " + std::to_string(straight) + " of its treads: fewer risers, or a longer line");
  for (; straight > 0; --straight) {
    std::size_t best = segments;
    for (std::size_t i = 0; i < segments; ++i)
      if (count[i] > 0 && (best == segments || flight[i] / count[i] > flight[best] / count[best] + 1e-9)) best = i;
    ++count[best];
  }
  // The treads, bottom to top.
  int step = 0;
  for (std::size_t i = 0; i < segments; ++i) {
    const V2 d = dir[i], left{-d.y, d.x};
    const V2 start = pts[i] + d * (i > 0 ? half : 0.0);
    const double going = count[i] ? flight[i] / count[i] : 0.0;
    for (int k = 0; k < count[i]; ++k) {
      const V2 a = start + d * (going * k), b = start + d * (going * (k + 1));
      out.treads.push_back({{a - left * half, b - left * half, b + left * half, a + left * half}, "tread", going, ++step});
    }
    if (i >= corners) break;
    const V2 next = dir[i + 1], v = pts[i + 1];
    if (turns[i] == 1) {
      if (joined(i)) {
        // One landing over both squares.
        const V2 c = (pts[i] + v) * 0.5;  // the squares touch: their centres are a width apart
        Tread& landing = out.treads.back();
        landing.outline = {c - d * w - left * half, c + d * w - left * half, c + d * w + left * half, c - d * w + left * half};
        continue;
      }
      out.treads.push_back({{v - d * half - left * half, v + d * half - left * half, v + d * half + left * half, v - d * half + left * half},
                            "landing", w, ++step});
      ++out.landings;
      continue;
    }
    // Winders: the square cut from its inner corner, from the side the flight comes in by
    // (along a) to the side the next one leaves by (along b).
    const V2 inner = v + (next - d) * half, a = next * -1.0, b = d;
    const int n = turns[i];
    auto hit = [&](double degrees) {
      if (degrees <= 45.0) return inner + a * w + b * (w * std::tan(degrees * kPi / 180.0));
      return inner + a * (w * std::tan((90.0 - degrees) * kPi / 180.0)) + b * w;
    };
    const double wedge = 90.0 / n;
    const double measured = w > kMeasureAt ? 2.0 * kMeasureAt * std::sin(wedge / 2 * kPi / 180.0) : 0.0;
    for (int k = 0; k < n; ++k) {
      const double from = wedge * k, to = wedge * (k + 1);
      Tread t{{inner, hit(from)}, "winder", measured, ++step};
      if (from < 45.0 - 1e-9 && to > 45.0 + 1e-9) t.outline.push_back(inner + a * w + b * w);
      t.outline.push_back(hit(to));
      out.treads.push_back(std::move(t));
      ++out.winders;
    }
  }
  return out;
}

Result<TopoDS_Shape> prism(const std::vector<V2>& outline, double z0, double z1) {
  BRepBuilderAPI_MakePolygon polygon;
  for (const V2& p : outline) polygon.Add(gp_Pnt(p.x, p.y, z0));
  polygon.Close();
  if (!polygon.IsDone()) return Result<TopoDS_Shape>::failure("A tread could not be built");
  BRepBuilderAPI_MakeFace face(polygon.Wire(), Standard_True);
  if (!face.IsDone()) return Result<TopoDS_Shape>::failure("A tread could not be built");
  BRepPrimAPI_MakePrism solid(face.Face(), gp_Vec(0, 0, z1 - z0));
  if (!solid.IsDone()) return Result<TopoDS_Shape>::failure("A tread could not be built");
  return solid.Shape();
}

constexpr double kRailRadius = 17.5, kRailInset = 60.0;

// The axis of a handrail on `side` (+1 left, -1 right, looking up): over the front corner of
// each tread on that side, 60 in, `height` above the tread; then over the top edge.
std::vector<gp_Pnt> railAxis(const StairSpec& spec, const Layout& l, int side) {
  std::vector<V2> pts;
  for (const gp_Pnt& p : spec.path) {
    const V2 q{p.X(), p.Y()};
    if (pts.empty() || (q - pts.back()).length() > 0.01) pts.push_back(q);
  }
  // The nearest place on the walking line: its station, the point and the direction there.
  struct Near {
    double station;
    V2 at, dir;
  };
  auto nearest = [&](const V2& q) {
    Near best{0, pts.front(), V2{1, 0}};
    double gap = 1e300, s0 = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const V2 d = pts[i] - pts[i - 1];
      const double len = d.length();
      const double t = std::clamp((q - pts[i - 1]).dot(d) / (len * len), 0.0, 1.0);
      const V2 on = pts[i - 1] + d * t;
      const double g = (q - on).length();
      if (g < gap - 1e-9) gap = g, best = {s0 + t * len, on, d * (1.0 / len)};
      s0 += len;
    }
    return best;
  };
  std::vector<gp_Pnt> axis;
  for (const Tread& t : l.treads) {
    // The corner on that side nearest the foot.
    std::optional<V2> corner;
    double first = 1e300;
    for (const V2& c : t.outline) {
      const Near n = nearest(c);
      const V2 left{-n.dir.y, n.dir.x};
      if ((c - n.at).dot(left) * side < spec.width / 2 - 1.0) continue;
      if (n.station < first - 1e-6) first = n.station, corner = c;
    }
    if (!corner) continue;
    const Near n = nearest(*corner);
    const V2 left{-n.dir.y, n.dir.x};
    const V2 in = *corner - left * (side * kRailInset);
    axis.emplace_back(in.x, in.y, spec.base + l.riser * t.step + spec.handrailHeight);
  }
  // Over the top edge: the end of the walking line, on that side.
  const V2 end = pts.back(), d = (pts.back() - pts[pts.size() - 2]) * (1.0 / (pts.back() - pts[pts.size() - 2]).length());
  const V2 left{-d.y, d.x};
  const V2 top = end + left * (side * (spec.width / 2 - kRailInset));
  axis.emplace_back(top.x, top.y, spec.base + spec.rise + spec.handrailHeight);
  return axis;
}

constexpr double kStringerOver = 50.0;  // its top over the line of the nosings

// The side boards on `side` (+1 left, -1 right, looking up): per tread, a board standing on the
// part of its outline on that side, its top rising one riser along it. Added to `into`.
Status sideBoards(const StairSpec& spec, const Layout& l, int side, double depth, BRep_Builder& builder, TopoDS_Compound& into,
                  double& volume, int& newels) {
  const bool cut = spec.stringerStyle == "cut";
  std::vector<V2> pts;
  for (const gp_Pnt& p : spec.path) {
    const V2 q{p.X(), p.Y()};
    if (pts.empty() || (q - pts.back()).length() > 0.01) pts.push_back(q);
  }
  // On that side: at least half the width out from the walking line (its nearest part), and
  // how far along the line it is.
  auto onSide = [&](const V2& q, double& station) {
    double gap = 1e300, s0 = 0, lateral = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const V2 d = pts[i] - pts[i - 1];
      const double len = d.length();
      const double t = std::clamp((q - pts[i - 1]).dot(d) / (len * len), 0.0, 1.0);
      const V2 on = pts[i - 1] + d * t;
      const double g = (q - on).length();
      if (g < gap - 1e-9) {
        gap = g;
        station = s0 + t * len;
        const V2 u = d * (1.0 / len);
        lateral = (q - on).dot(V2{-u.y, u.x});
      }
      s0 += len;
    }
    return lateral * side > spec.width / 2 - 1.0;
  };
  // Each tread's run of edges on that side, in walking order.
  struct Run {
    std::vector<V2> points;
    double z0 = 0.0;  // the top of the board at its start
  };
  std::vector<Run> runs;
  for (const Tread& t : l.treads) {
    const std::size_t n = t.outline.size();
    std::vector<bool> in(n);
    std::vector<double> station(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = onSide(t.outline[i], station[i]);
    // Start after a point not on that side: the run is contiguous (the treads are convex).
    std::size_t start = n;
    for (std::size_t i = 0; i < n; ++i)
      if (!in[i] && in[(i + 1) % n]) start = (i + 1) % n;
    if (start == n) continue;
    std::vector<std::size_t> run;
    for (std::size_t k = 0; k < n && in[(start + k) % n]; ++k) run.push_back((start + k) % n);
    if (run.size() < 2) continue;  // the inside of a turn: a point only
    if (station[run.front()] > station[run.back()]) std::reverse(run.begin(), run.end());
    Run r;
    for (std::size_t i : run) r.points.push_back(t.outline[i]);
    r.z0 = spec.base + l.riser * t.step + kStringerOver;
    runs.push_back(std::move(r));
  }
  std::vector<TopoDS_Shape> pieces;
  double piecesVolume = 0.0;
  for (std::size_t k = 0; k < runs.size(); ++k) {
    const Run& r = runs[k];
    double total = 0.0;
    for (std::size_t i = 1; i < r.points.size(); ++i) total += (r.points[i] - r.points[i - 1]).length();
    if (total < 1.0) continue;
    double along = 0.0;
    for (std::size_t i = 1; i < r.points.size(); ++i) {
      V2 a = r.points[i - 1], b = r.points[i];
      const double len = (b - a).length();
      if (len < 1.0) continue;
      const V2 e = (b - a) * (1.0 / len), out = V2{-e.y, e.x} * static_cast<double>(side);
      const double za = r.z0 + l.riser * along / total;
      along += len;
      const double zb = r.z0 + l.riser * along / total;
      // An outside corner (the next edge turns in): this board runs on over the next one's end.
      std::optional<V2> next;
      if (i + 1 < r.points.size()) next = r.points[i + 1] - b;
      else if (k + 1 < runs.size() && (runs[k + 1].points.front() - b).length() < 1.0) next = runs[k + 1].points[1] - runs[k + 1].points[0];
      if (!cut && next && (e.x * next->y - e.y * next->x) * side < -1e-6 * next->length()) b = b + e * spec.stringerThickness;
      // The top: over the nosings (side boards), or flat under this tread (cut, the step).
      const double topA = cut ? r.z0 - kStringerOver - spec.thickness : za, topB = cut ? topA : zb;
      BRepBuilderAPI_MakePolygon polygon;
      polygon.Add(gp_Pnt(a.x, a.y, std::max(spec.base, za - depth)));
      // The bottom leaves the floor part way along: a corner there.
      if (za - depth < spec.base - 1e-6 && zb - depth > spec.base + 1e-6) {
        const V2 c = a + e * (len * (spec.base - (za - depth)) / (zb - za));
        polygon.Add(gp_Pnt(c.x, c.y, spec.base));
      }
      polygon.Add(gp_Pnt(b.x, b.y, std::max(spec.base, zb - depth)));
      polygon.Add(gp_Pnt(b.x, b.y, topB));
      polygon.Add(gp_Pnt(a.x, a.y, topA));
      polygon.Close();
      if (!polygon.IsDone()) return Status::failure("A side board could not be built");
      BRepBuilderAPI_MakeFace face(polygon.Wire(), Standard_True);
      if (!face.IsDone()) return Status::failure("A side board could not be built");
      // Side boards stand outside the width, cut ones inside it (under the treads).
      BRepPrimAPI_MakePrism board(face.Face(), gp_Vec(out.x, out.y, 0) * (cut ? -spec.stringerThickness : spec.stringerThickness));
      if (!board.IsDone()) return Status::failure("A side board could not be built");
      pieces.push_back(board.Shape());
      piecesVolume += sbr::geom::volume(board.Shape());
    }
  }
  // Newel posts: at the foot of the board, and where it stops at the inside of a turn (the next
  // board starts at the same place, higher).
  if (spec.newels && !runs.empty()) {
    auto post = [&](const V2& at, const V2& along, double top) -> Status {
      const V2 e = along * (1.0 / along.length()), o = V2{-e.y, e.x} * static_cast<double>(side);
      const V2 c = at + o * ((cut ? -1.0 : 1.0) * spec.stringerThickness / 2);
      const double h = spec.newelSize / 2;
      std::vector<V2> square = {c - e * h - o * h, c + e * h - o * h, c + e * h + o * h, c - e * h + o * h};
      auto made = prism(square, spec.base, top);
      if (!made) return Status::failure("A newel post could not be built");
      pieces.push_back(made.value());
      piecesVolume += sbr::geom::volume(made.value());
      ++newels;
      return Status::success();
    };
    const Run& first = runs.front();
    if (first.points.size() >= 2)
      if (const Status s = post(first.points[0], first.points[1] - first.points[0], first.z0 + 100.0); !s) return s;
    for (std::size_t k = 0; k + 1 < runs.size(); ++k) {
      const Run& r = runs[k];
      const Run& n = runs[k + 1];
      if ((n.points.front() - r.points.back()).length() > 1.0 || n.z0 < r.z0 + 1.5 * l.riser || n.points.size() < 2) continue;
      if (const Status s = post(n.points[0], n.points[1] - n.points[0], n.z0 + 100.0); !s) return s;
    }
  }
  // One board per side where the pieces meet (no joint at every tread in a drawing).
  if (pieces.size() > 1)
    if (const auto fused = booleanCombine(BooleanOp::Union, pieces)) {
      builder.Add(into, fused.value());
      volume += sbr::geom::volume(fused.value());  // (pieces overlap at outside corners)
      return Status::success();
    }
  for (const TopoDS_Shape& p : pieces) builder.Add(into, p);
  volume += piecesVolume;
  return Status::success();
}

// Riser boards: at the front edge of each tread (its edge nearest the foot along the walking
// line), from the tread below (or the floor) up to this tread's underside, set back into it.
Status riserBoards(const StairSpec& spec, const Layout& l, BRep_Builder& builder, TopoDS_Compound& into, int& count) {
  std::vector<V2> pts;
  for (const gp_Pnt& p : spec.path) {
    const V2 q{p.X(), p.Y()};
    if (pts.empty() || (q - pts.back()).length() > 0.01) pts.push_back(q);
  }
  auto station = [&](const V2& q) {
    double gap = 1e300, s0 = 0, best = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const V2 d = pts[i] - pts[i - 1];
      const double len = d.length();
      const double t = std::clamp((q - pts[i - 1]).dot(d) / (len * len), 0.0, 1.0);
      const double g = (q - (pts[i - 1] + d * t)).length();
      if (g < gap - 1e-9) gap = g, best = s0 + t * len;
      s0 += len;
    }
    return best;
  };
  for (const Tread& t : l.treads) {
    const double top = spec.base + l.riser * (t.step - 1), under = spec.base + l.riser * t.step - spec.thickness;
    if (under - top < 1.0) continue;
    const std::size_t n = t.outline.size();
    std::size_t front = 0;
    double lowest = 1e300;
    for (std::size_t i = 0; i < n; ++i) {
      const V2 mid = (t.outline[i] + t.outline[(i + 1) % n]) * 0.5;
      const double s = station(mid);
      if (s < lowest - 1e-6) lowest = s, front = i;
    }
    const V2 a = t.outline[front], b = t.outline[(front + 1) % n];
    const double len = (b - a).length();
    if (len < 1.0) continue;
    V2 centroid{0, 0};
    for (const V2& p : t.outline) centroid = centroid + p * (1.0 / n);
    const V2 e = (b - a) * (1.0 / len);
    V2 in{-e.y, e.x};
    if ((centroid - a).dot(in) < 0) in = in * -1.0;
    auto board = prism({a, b, b + in * spec.riserThickness, a + in * spec.riserThickness}, top, under);
    if (!board) return Status::failure("A riser board could not be built");
    builder.Add(into, board.value());
    ++count;
  }
  return Status::success();
}

StairCheck check(const std::string& item, double value, double limit, bool atMost, bool measurable = true) {
  StairCheck c{item, value, limit, "not_evaluated"};
  if (measurable) c.state = (atMost ? value <= limit + 1e-6 : value >= limit - 1e-6) ? "ok" : "ng";
  return c;
}
}  // namespace

Result<StairShape> makeStair(const StairSpec& spec) {
  const auto made = layout(spec);
  if (!made) return R::failure(made.error());
  const Layout& l = made.value();
  StairShape out;
  out.risers = l.risers;
  out.riser = l.riser;
  out.walkLength = l.walk;
  out.flights = l.flights;
  out.landings = l.landings;
  out.winders = l.winders;
  std::vector<TopoDS_Shape> bodies;
  bool winderUnmeasured = false;
  for (const Tread& t : l.treads) {
    const double top = spec.base + l.riser * t.step;
    const double bottom = spec.style == "block" ? std::max(spec.base, top - l.riser - spec.thickness) : top - spec.thickness;
    auto body = prism(t.outline, bottom, top);
    if (!body) return R::failure(body.error());
    bodies.push_back(body.value());
    StairTread tread;
    for (const V2& p : t.outline) tread.outline.emplace_back(p.x, p.y, top);
    tread.kind = t.kind;
    tread.going = t.going;
    out.treads.push_back(std::move(tread));
    if (t.kind == "tread") {
      out.goingMin = out.goingMin == 0.0 ? t.going : std::min(out.goingMin, t.going);
      out.goingMax = std::max(out.goingMax, t.going);
    } else if (t.kind == "winder") {
      if (t.going == 0.0) winderUnmeasured = true;
      else out.winderGoing = out.winderGoing == 0.0 ? t.going : std::min(out.winderGoing, t.going);
    }
  }
  if (spec.style == "block" && bodies.size() > 1) {
    auto all = booleanCombine(BooleanOp::Union, bodies);
    if (!all) return R::failure("The stair could not be built: " + all.error());
    int solids = 0;
    for (TopExp_Explorer e(all.value(), TopAbs_SOLID); e.More(); e.Next()) ++solids;
    if (solids != 1 || !BRepCheck_Analyzer(all.value()).IsValid()) return R::failure("The stair could not be built as one solid");
    out.solid = all.value();
  } else if (bodies.size() == 1) {
    out.solid = bodies.front();
  } else {
    TopoDS_Compound boards;
    BRep_Builder builder;
    builder.MakeCompound(boards);
    for (const TopoDS_Shape& b : bodies) builder.Add(boards, b);
    out.solid = boards;
  }
  // Handrails (docs/116) and side boards (docs/128): with the stair in one compound.
  if (spec.handrail != "none" || spec.stringer != "none" || spec.riserBoards) {
    TopoDS_Compound all;
    BRep_Builder builder;
    builder.MakeCompound(all);
    builder.Add(all, out.solid);
    if (spec.stringer != "none") {
      out.stringerDepth = std::max(240.0, l.riser + spec.thickness + 100.0);
      for (int side : {1, -1}) {
        if ((side > 0 && spec.stringer == "right") || (side < 0 && spec.stringer == "left")) continue;
        if (const Status s = sideBoards(spec, l, side, out.stringerDepth, builder, all, out.stringerVolume, out.newels); !s) return R::failure(s.message());
      }
    }
    if (spec.riserBoards)
      if (const Status s = riserBoards(spec, l, builder, all, out.riserBoards); !s) return R::failure(s.message());
    for (int side : {1, -1}) {
      if (spec.handrail == "none") break;
      if ((side > 0 && spec.handrail == "right") || (side < 0 && spec.handrail == "left")) continue;
      const std::vector<gp_Pnt> axis = railAxis(spec, l, side);
      for (std::size_t i = 1; i < axis.size(); ++i) {
        const gp_Vec v(axis[i - 1], axis[i]);
        if (v.Magnitude() < 1.0) continue;
        BRepPrimAPI_MakeCylinder bar(gp_Ax2(axis[i - 1], gp_Dir(v)), kRailRadius, v.Magnitude());
        bar.Build();  // (made on demand: IsDone is false until built)
        if (!bar.IsDone()) return R::failure("A handrail could not be built");
        builder.Add(all, bar.Shape());
        out.handrailLength += v.Magnitude();
      }
    }
    out.solid = all;
  }
  // Against the sizes for a house's stair (docs/103): what cannot be measured here is not judged.
  out.checks.push_back(check("riser", out.riser, 230.0, true));
  if (out.goingMin > 0.0) out.checks.push_back(check("going", out.goingMin, 150.0, false));
  if (l.winders > 0) out.checks.push_back(check("winder_going", out.winderGoing, 150.0, false, !winderUnmeasured));
  out.checks.push_back(check("width", spec.width, 750.0, false));
  out.checks.push_back(check("landing", spec.rise, 4000.0, true, spec.rise <= 4000.0 + 1e-6));
  // A handrail on at least one side (令 第 25 条第 1 項, docs/116). None made here: it may be
  // made otherwise (on a wall), so not judged.
  if (spec.handrail == "none") out.checks.push_back(check("handrail", 0.0, 1.0, false, false));
  else out.checks.push_back(check("handrail", spec.handrail == "both" ? 2.0 : 1.0, 1.0, false));
  return out;
}

Result<StairPlanMarks> stairPlanMarks(const StairSpec& spec, double cutZ) {
  using M = Result<StairPlanMarks>;
  const auto made = layout(spec);
  if (!made) return M::failure(made.error());
  if (cutZ <= spec.base) return M::failure("The plan cuts below the stair");
  // The walking line as stations: points with their distance from the foot.
  std::vector<V2> pts;
  for (const gp_Pnt& p : spec.path) {
    const V2 q{p.X(), p.Y()};
    if (pts.empty() || (q - pts.back()).length() > 0.01) pts.push_back(q);
  }
  std::vector<double> at(pts.size(), 0.0);
  for (std::size_t i = 1; i < pts.size(); ++i) at[i] = at[i - 1] + (pts[i] - pts[i - 1]).length();
  auto point = [&](double s) {
    for (std::size_t i = 1; i < pts.size(); ++i)
      if (s <= at[i] + 1e-9 || i + 1 == pts.size()) {
        const double t = at[i] - at[i - 1] > 0 ? std::clamp((s - at[i - 1]) / (at[i] - at[i - 1]), 0.0, 1.0) : 0.0;
        return std::make_pair(pts[i - 1] + (pts[i] - pts[i - 1]) * t, (pts[i] - pts[i - 1]) * (1.0 / (at[i] - at[i - 1])));
      }
    return std::make_pair(pts.back(), V2{1, 0});
  };
  auto station = [&](const V2& q) {  // the nearest station on the walking line
    double best = 1e300, s = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const V2 d = pts[i] - pts[i - 1];
      const double len = d.length();
      const double t = std::clamp((q - pts[i - 1]).dot(d) / (len * len), 0.0, 1.0);
      const double gap = (q - (pts[i - 1] + d * t)).length();
      if (gap < best) best = gap, s = at[i - 1] + t * len;
    }
    return s;
  };
  StairPlanMarks out;
  double end = at.back();
  // The first tread whose top is above the cut: the break goes at its front edge.
  for (const Tread& t : made->treads) {
    if (spec.base + made->riser * t.step <= cutZ) continue;
    // Its front edge: the station of the tread's nearest corner to the foot side.
    double front = 1e300;
    for (const V2& c : t.outline) front = std::min(front, station(c));
    end = std::max(0.0, front);
    out.cut = true;
    break;
  }
  if (!out.cut && spec.base + spec.rise <= cutZ) end = at.back();
  auto flat = [](const V2& v) { return gp_Pnt(v.x, v.y, 0.0); };
  for (std::size_t i = 0; i < pts.size() && at[i] < end - 1e-6; ++i) out.walk.push_back(flat(pts[i]));
  const auto [tip, dir] = point(end);
  out.walk.push_back(flat(tip));
  // The arrow head: two strokes back from the tip at 30 degrees.
  const double head = std::min(200.0, spec.width / 3);
  const V2 left{-dir.y, dir.x};
  const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
  out.arrow.push_back({flat(tip), flat(tip - dir * (head * c) + left * (head * s))});
  out.arrow.push_back({flat(tip), flat(tip - dir * (head * c) - left * (head * s))});
  if (out.cut) {
    // Across the width, slanted, with a zigzag in the middle (the usual break line).
    const double half = spec.width / 2 + 60.0, slant = 0.3 * half, zig = 60.0;  // a little past the sides, not into the walls
    out.breakLine = {flat(tip - left * half - dir * slant), flat(tip - left * zig - dir * (slant * zig / half)),
                     flat(tip + dir * (zig * 1.5)), flat(tip - dir * (zig * 1.5)),
                     flat(tip + left * zig + dir * (slant * zig / half)), flat(tip + left * half + dir * slant)};
  }
  const auto [foot, firstDir] = point(0.0);
  out.label = flat(foot - firstDir * 250.0);
  return out;
}

Result<TopoDS_Shape> stairOpening(const StairSpec& spec, double underside, double top, double headroom) {
  using T = Result<TopoDS_Shape>;
  const auto made = layout(spec);
  if (!made) return T::failure(made.error());
  if (!(top > underside)) return T::failure("The floor has no thickness");
  std::vector<TopoDS_Shape> bodies;
  for (const Tread& t : made->treads) {
    const double tread = spec.base + made->riser * t.step;
    if (tread > top + 1.0 || underside - tread >= headroom) continue;
    auto body = prism(t.outline, underside - 10.0, top + 10.0);
    if (!body) return body;
    bodies.push_back(body.value());
  }
  if (bodies.empty()) return T::failure("No tread of the stair needs this floor open");
  if (bodies.size() == 1) return bodies.front();
  auto all = booleanCombine(BooleanOp::Union, bodies);
  if (!all) return T::failure("The stair's opening could not be built: " + all.error());
  return all;
}

}  // namespace sbr::geom
