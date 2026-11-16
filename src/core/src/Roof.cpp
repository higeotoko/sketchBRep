#include "sbr/geom/Roof.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::geom {
namespace {

using R = Result<RoofShape>;

struct Rect {
  double a0, a1, b0, b1;  // in the outline's frame
};

struct Frame {
  gp_Vec u, v;  // unit, in plan; v = u turned left
  gp_Pnt at(double a, double b, double z) const { return gp_Pnt(u.X() * a + v.X() * b, u.Y() * a + v.Y() * b, z); }
};

bool insidePolygon(const std::vector<std::pair<double, double>>& poly, double a, double b) {
  bool in = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const auto& p = poly[i];
    const auto& q = poly[j];
    if ((p.second > b) != (q.second > b) && a < (q.first - p.first) * (b - p.second) / (q.second - p.second) + p.first)
      in = !in;
  }
  return in;
}

std::vector<double> distinct(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  std::vector<double> out;
  for (double x : values)
    if (out.empty() || x - out.back() > 0.01) out.push_back(x);
  return out;
}

// The solid swept by a closed section (points in the plane across `along`) for `length`.
Result<TopoDS_Shape> prism(const std::vector<gp_Pnt>& section, const gp_Vec& along) {
  BRepBuilderAPI_MakePolygon polygon;
  for (const gp_Pnt& p : section) polygon.Add(p);
  polygon.Close();
  if (!polygon.IsDone()) return Result<TopoDS_Shape>::failure("Roof section could not be built");
  BRepBuilderAPI_MakeFace face(polygon.Wire(), Standard_True);
  if (!face.IsDone()) return Result<TopoDS_Shape>::failure("Roof section could not be built");
  BRepPrimAPI_MakePrism solid(face.Face(), along);
  if (!solid.IsDone()) return Result<TopoDS_Shape>::failure("Roof body could not be built");
  return solid.Shape();
}

// The body under a two-sided roof over the rectangle: the ridge along `a` (alongA) or `b`.
// `edge`: z of the underside at the rectangle's sides the roof falls to; `base`: z of the bottom.
Result<TopoDS_Shape> ridgeBody(const Frame& f, const Rect& r, bool alongA, double slope, double edge, double base) {
  const double w0 = alongA ? r.b0 : r.a0, w1 = alongA ? r.b1 : r.a1;  // across the ridge
  const double l0 = alongA ? r.a0 : r.b0, l1 = alongA ? r.a1 : r.b1;  // along it
  const double top = edge + slope * (w1 - w0) / 2;
  auto p = [&](double across, double z) { return alongA ? f.at(l0, across, z) : f.at(across, l0, z); };
  const std::vector<gp_Pnt> section = {p(w0, base), p(w1, base), p(w1, edge), p((w0 + w1) / 2, top), p(w0, edge)};
  return prism(section, (alongA ? f.u : f.v) * (l1 - l0));
}

}  // namespace

namespace {
Result<RoofShape> build(const RoofSpec& spec, double riseGiven);

// Outlines with slanted sides (docs/108, docs/138). Polygons in plan (z 0), counter-clockwise.
using Poly = std::vector<gp_Pnt>;

double twiceArea(const Poly& p) {
  double a = 0.0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const gp_Pnt &u = p[i], &w = p[(i + 1) % p.size()];
    a += u.X() * w.Y() - w.X() * u.Y();
  }
  return a;
}

bool reflexAt(const Poly& p, std::size_t i) {
  const std::size_t n = p.size();
  const gp_Vec a(p[(i + n - 1) % n], p[i]), b(p[i], p[(i + 1) % n]);
  return a.Crossed(b).Z() < -1e-6 * a.Magnitude() * b.Magnitude();
}

// Cut at reflex corner i along the extension of its incoming side (incoming) or of its outgoing
// side backwards, up to where that line first meets the outline: two polygons.
std::optional<std::pair<Poly, Poly>> splitAt(const Poly& p, std::size_t i, bool incoming) {
  const std::size_t n = p.size();
  const gp_Pnt v = p[i];
  gp_Vec d = incoming ? gp_Vec(p[(i + n - 1) % n], v) : gp_Vec(p[(i + 1) % n], v);
  if (d.Magnitude() < 1e-9) return std::nullopt;
  d.Normalize();
  double best = 1e300, bestS = 0.0;
  std::size_t edge = n;
  for (std::size_t j = 0; j < n; ++j) {
    if (j == i || (j + 1) % n == i) continue;  // the sides at the corner
    const gp_Pnt &a = p[j], &b = p[(j + 1) % n];
    const gp_Vec e(a, b);
    const double den = d.X() * e.Y() - d.Y() * e.X();
    if (std::abs(den) < 1e-12) continue;
    const gp_Vec w(v, a);
    const double t = (w.X() * e.Y() - w.Y() * e.X()) / den;  // along the ray
    const double s = (w.X() * d.Y() - w.Y() * d.X()) / den;  // along the side
    if (t > 1.0 && s >= -1e-9 && s <= 1.0 + 1e-9 && t < best) best = t, bestS = s, edge = j;
  }
  if (edge == n) return std::nullopt;
  const gp_Pnt hit = v.Translated(d * best);
  Poly a, b;
  // a: from the corner forwards to the side hit, then the hit point.
  for (std::size_t k = i;; k = (k + 1) % n) {
    a.push_back(p[k]);
    if (k == edge) break;
  }
  if (hit.Distance(a.back()) > 0.5) a.push_back(hit);
  // b: from the hit point on to the corner.
  if (hit.Distance(p[(edge + 1) % n]) > 0.5) b.push_back(hit);
  for (std::size_t k = (edge + 1) % n;; k = (k + 1) % n) {
    b.push_back(p[k]);
    if (k == i) break;
  }
  if (a.size() < 3 || b.size() < 3 || twiceArea(a) < 2.0 || twiceArea(b) < 2.0) return std::nullopt;
  return std::make_pair(a, b);
}

std::string polyKey(Poly p) {
  std::vector<std::pair<long long, long long>> q;
  for (const gp_Pnt& v : p) q.push_back({std::llround(v.X()), std::llround(v.Y())});
  std::sort(q.begin(), q.end());
  std::string key;
  for (const auto& [x, y] : q) key += std::to_string(x) + "," + std::to_string(y) + ";";
  return key;
}

// Every convex piece any way of cutting at the reflex corners gives (the pieces of one way cover
// the outline; all of them, the largest kept, overlap the way the right-angled roof's largest
// rectangles do).
void convexPieces(const Poly& p, int depth, std::map<std::string, Poly>& out, int& work) {
  if (++work > 400) return;
  bool convex = true;
  for (std::size_t i = 0; i < p.size() && convex; ++i) convex = !reflexAt(p, i);
  if (convex) {
    out.emplace(polyKey(p), p);
    return;
  }
  if (depth > 8) return;
  for (std::size_t i = 0; i < p.size(); ++i) {
    if (!reflexAt(p, i)) continue;
    for (bool incoming : {true, false})
      if (const auto parts = splitAt(p, i, incoming)) {
        convexPieces(parts->first, depth + 1, out, work);
        convexPieces(parts->second, depth + 1, out, work);
      }
  }
}

bool insideConvex(const Poly& p, const gp_Pnt& q) {
  for (std::size_t i = 0; i < p.size(); ++i) {
    const gp_Vec e(p[i], p[(i + 1) % p.size()]), w(p[i], q);
    if (e.Crossed(w).Z() < -0.5 * e.Magnitude()) return false;
  }
  return true;
}

// The polygon with each side i moved out by offsets[i] (convex: the corners where the moved
// sides meet).
std::optional<Poly> grown(const Poly& p, const std::vector<double>& offsets) {
  const std::size_t n = p.size();
  Poly out;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t h = (i + n - 1) % n;
    auto line = [&](std::size_t k) {
      const gp_Vec d = gp_Vec(p[k], p[(k + 1) % n]).Normalized();
      const gp_Vec outward(d.Y(), -d.X(), 0);
      return std::make_pair(p[k].Translated(outward * offsets[k]), d);
    };
    const auto [a, da] = line(h);
    const auto [b, db] = line(i);
    const double den = da.X() * db.Y() - da.Y() * db.X();
    if (std::abs(den) < 1e-9) {
      out.push_back(b);  // straight on: the moved side's start
      continue;
    }
    const gp_Vec w(a, b);
    const double t = (w.X() * db.Y() - w.Y() * db.X()) / den;
    out.push_back(a.Translated(da * t));
  }
  return out;
}

Result<TopoDS_Shape> upright(const Poly& p, double z0, double z1) {
  Poly section;
  for (const gp_Pnt& v : p) section.emplace_back(v.X(), v.Y(), z0);
  return prism(section, gp_Vec(0, 0, z1 - z0));
}

// What lies under the plane rising inwards from side (p -> q) at `slope`, its eave `eave` out
// from the side at `low`, far enough along and across to cover everything.
Result<TopoDS_Shape> underSlope(const gp_Pnt& p, const gp_Pnt& q, double slope, double eave, double height, double base, double far) {
  const gp_Vec d = gp_Vec(p, q).Normalized();
  const gp_Vec in(-d.Y(), d.X(), 0);
  const gp_Pnt s = p.Translated(d * -far);
  auto at = [&](double inwards, double z) {
    const gp_Pnt o = s.Translated(in * inwards);
    return gp_Pnt(o.X(), o.Y(), z);
  };
  return prism({at(-eave, base), at(far, base), at(far, height + slope * far), at(-eave, height - slope * eave)},
               d * (p.Distance(q) + 2 * far));
}

// The roof from the body under it: the body lifted by `rise` less the body.
R fromBody(const TopoDS_Shape& under, double rise, double slope, int parts) {
  gp_Trsf lift;
  lift.SetTranslation(gp_Vec(0, 0, rise));
  const TopoDS_Shape lifted = BRepBuilderAPI_Transform(under, lift, Standard_True).Shape();
  auto roof = booleanCombine(BooleanOp::Subtract, {lifted, under});
  if (!roof) return R::failure("Roof could not be built: " + roof.error());
  int solids = 0;
  for (TopExp_Explorer e(roof.value(), TopAbs_SOLID); e.More(); e.Next()) ++solids;
  if (solids != 1 || !BRepCheck_Analyzer(roof.value()).IsValid()) return R::failure("Roof could not be built as one solid");
  RoofShape out;
  out.parts = parts;
  out.solid = roof.value();
  out.planArea = volume(out.solid) / rise;
  out.slopeArea = out.planArea * std::sqrt(1.0 + slope * slope);
  const Bnd_Box box = exactBounds(out.solid);
  out.ridgeHeight = box.CornerMax().Z();
  out.eaveHeight = box.CornerMin().Z();
  return out;
}

Result<RoofShape> slantedRoof(const RoofSpec& spec, Poly points, double riseGiven) {
  double around = 0.0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    around += points[i].Distance(points[(i + 1) % points.size()]);
    if (points[i].Distance(points[(i + 1) % points.size()]) < 1.0) return R::failure("Two roof outline corners coincide");
  }
  const double twice = twiceArea(points);
  if (std::abs(twice) < 2.0) return R::failure("The roof outline encloses nothing");
  const std::size_t n = points.size();
  // Side numbers as drawn; counter-clockwise from here on (inside on the left).
  std::vector<int> sideOf(n);
  for (std::size_t i = 0; i < n; ++i) sideOf[i] = static_cast<int>(i);
  if (twice < 0) {
    // Reversed: corner k was corner n-1-k; side k (k -> k+1) was side n-2-k (mod n).
    std::reverse(points.begin(), points.end());
    for (std::size_t k = 0; k < n; ++k) sideOf[k] = static_cast<int>((2 * n - 2 - k) % n);
  }
  int reflex = 0;
  for (std::size_t i = 0; i < n; ++i) reflex += reflexAt(points, i);
  const double slope = spec.pitch / 10.0;
  const double rise = riseGiven > 0 ? riseGiven : spec.thickness * std::sqrt(1.0 + slope * slope);
  const double lowest = spec.height - slope * spec.eave;
  const double far = around + std::max(spec.eave, spec.gableEave) + 1000.0;  // beyond the outline from any side
  const double base = lowest - rise - slope * far - 100.0;  // well below anything
  const double top = spec.height + slope * far + rise + 1000.0;
  auto dir = [&](std::size_t i) { return gp_Vec(points[i], points[(i + 1) % n]).Normalized(); };

  if (spec.kind == "hip") {
    // Each convex piece: under every one of its sides' planes; the pieces' roofs together
    // (valleys where they meet).
    std::map<std::string, Poly> pieces;
    int work = 0;
    if (reflex == 0) pieces.emplace(polyKey(points), points);
    else {
      if (reflex > 4) return R::failure("A roof outline with slanted sides may have at most four inward corners");
      convexPieces(points, 0, pieces, work);
      if (pieces.empty() || work > 400) return R::failure("The roof outline could not be cut into convex parts");
    }
    // Only the largest: a piece inside another adds nothing.
    std::vector<Poly> kept;
    for (const auto& [key, piece] : pieces) {
      bool within = false;
      for (const auto& [other, bigger] : pieces) {
        if (other == key) continue;
        bool all = true;
        for (const gp_Pnt& v : piece) all = all && insideConvex(bigger, v);
        if (all && std::abs(twiceArea(bigger)) > std::abs(twiceArea(piece)) + 1.0) within = true;
      }
      if (!within) kept.push_back(piece);
    }
    std::vector<TopoDS_Shape> bodies;
    for (const Poly& piece : kept) {
      std::vector<TopoDS_Shape> planes;
      for (std::size_t i = 0; i < piece.size(); ++i) {
        auto body = underSlope(piece[i], piece[(i + 1) % piece.size()], slope, spec.eave, spec.height, base, far);
        if (!body) return R::failure(body.error());
        planes.push_back(body.value());
      }
      auto under = booleanCombine(BooleanOp::Intersect, planes);
      if (!under) return R::failure("Hip roof could not be built: " + under.error());
      bodies.push_back(under.value());
    }
    TopoDS_Shape under = bodies.front();
    if (bodies.size() > 1) {
      auto all = booleanCombine(BooleanOp::Union, bodies);
      if (!all) return R::failure("Roof parts could not be joined: " + all.error());
      under = all.value();
    }
    return fromBody(under, rise, slope, static_cast<int>(kept.size()));
  }
  if (reflex > 0)
    return R::failure("A gable or shed roof over an outline with slanted sides needs one with no inward corners yet "
                      "(a hip roof can have them)");
  std::vector<double> offsets(n, spec.eave);
  std::vector<TopoDS_Shape> parts;
  if (spec.kind == "gable") {
    // The gable sides: as given, or the shorter pair of parallel opposite sides.
    std::vector<bool> gable(n, false);
    if (!spec.gableSides.empty()) {
      for (const int g : spec.gableSides) {
        if (g < 0 || g >= static_cast<int>(n)) return R::failure("A gable side is a number of a side of the outline");
        for (std::size_t k = 0; k < n; ++k)
          if (sideOf[k] == g) gable[k] = true;
      }
    } else {
      double best = 1e300;
      std::pair<std::size_t, std::size_t> pair{n, n};
      for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
          if (dir(i).Dot(dir(j)) > -1.0 + 1e-6) continue;  // opposite sides run opposite ways
          const double length = points[i].Distance(points[(i + 1) % n]) + points[j].Distance(points[(j + 1) % n]);
          const double score = spec.ridge == "short" ? -length : length;
          if (score < best) best = score, pair = {i, j};
        }
      if (pair.first == n) return R::failure("This outline has no parallel opposite sides: choose the gable sides");
      gable[pair.first] = gable[pair.second] = true;
    }
    int eaves = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (gable[i]) {
        offsets[i] = spec.gableEave;
        continue;
      }
      ++eaves;
      auto body = underSlope(points[i], points[(i + 1) % n], slope, spec.eave, spec.height, base, far);
      if (!body) return R::failure(body.error());
      parts.push_back(body.value());
    }
    if (eaves == 0) return R::failure("A gable roof needs sides that are not gables");
  } else {
    // Shed: one plane rising from the side the roof falls to (the side facing the fall most).
    const gp_Vec fall(spec.fallX, spec.fallY, 0);
    if (fall.Magnitude() < 1e-9) return R::failure("Give the direction the shed roof falls towards");
    std::size_t low = 0;
    double facing = -1e300;
    for (std::size_t i = 0; i < n; ++i) {
      const gp_Vec outward(dir(i).Y(), -dir(i).X(), 0);
      const double f = outward.Dot(fall.Normalized());
      if (f > facing + 1e-9) facing = f, low = i;
    }
    const gp_Vec lowOut(dir(low).Y(), -dir(low).X(), 0);
    for (std::size_t i = 0; i < n; ++i) {
      const gp_Vec outward(dir(i).Y(), -dir(i).X(), 0);
      offsets[i] = std::abs(outward.Dot(lowOut)) >= 0.5 ? spec.eave : spec.gableEave;  // eaves low and high, verges at the sides
    }
    auto body = underSlope(points[low], points[(low + 1) % n], slope, spec.eave, spec.height, base, far);
    if (!body) return R::failure(body.error());
    parts.push_back(body.value());
  }
  // Within the outline grown by its overhangs (the gables / verges stand up there).
  const auto outline = grown(points, offsets);
  if (!outline) return R::failure("The roof outline could not be grown by its overhangs");
  auto walls = upright(*outline, base, top);
  if (!walls) return R::failure(walls.error());
  parts.push_back(walls.value());
  auto under = booleanCombine(BooleanOp::Intersect, parts);
  if (!under) return R::failure("Roof could not be built: " + under.error());
  return fromBody(under.value(), rise, slope, 1);
}
}

Result<RoofShape> makeRoof(const RoofSpec& spec) { return build(spec, 0.0); }

Result<TopoDS_Shape> roofSpaceAbove(const RoofSpec& spec, double up) {
  if (!(up > 1.0)) return Result<TopoDS_Shape>::failure("The height above the roof must be positive");
  const auto space = build(spec, up);
  if (!space) return Result<TopoDS_Shape>::failure(space.error());
  return space->solid;
}

namespace {
// `riseGiven` > 0: that many mm upright instead of the roof's own thickness.
Result<RoofShape> build(const RoofSpec& spec, double riseGiven) {
  if (spec.kind != "gable" && spec.kind != "hip" && spec.kind != "shed") return R::failure("Roof kind: gable, hip or shed");
  if (!(spec.pitch > 0.0) || spec.pitch > 30.0 || !std::isfinite(spec.pitch))
    return R::failure("Roof pitch must be above 0 and at most 30 (rise per 10 of run)");
  if (!(spec.thickness >= 1.0) || !std::isfinite(spec.thickness)) return R::failure("Roof thickness must be at least 1");
  if (spec.eave < 0 || spec.gableEave < 0 || !std::isfinite(spec.eave) || !std::isfinite(spec.gableEave))
    return R::failure("Roof overhangs must not be negative");
  if (spec.ridge != "long" && spec.ridge != "short") return R::failure("Roof ridge: long or short");
  if (!std::isfinite(spec.height)) return R::failure("Roof height is not a number");
  // The outline without repeated points.
  std::vector<gp_Pnt> points;
  for (const gp_Pnt& p : spec.outline) {
    const gp_Pnt flat(p.X(), p.Y(), 0);
    if (points.empty() || points.back().Distance(flat) > 0.01) points.push_back(flat);
  }
  if (points.size() > 1 && points.front().Distance(points.back()) <= 0.01) points.pop_back();
  if (points.size() < 4) return R::failure("A roof outline needs at least four corners");
  // Its frame: along the first side. Every side must run along the frame.
  Frame frame;
  frame.u = gp_Vec(points[0], points[1]).Normalized();
  frame.v = gp_Vec(-frame.u.Y(), frame.u.X(), 0);
  std::vector<std::pair<double, double>> poly;
  for (const gp_Pnt& p : points) poly.push_back({gp_Vec(p.XYZ()).Dot(frame.u), gp_Vec(p.XYZ()).Dot(frame.v)});
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const auto& p = poly[i];
    const auto& q = poly[(i + 1) % poly.size()];
    const double da = std::abs(q.first - p.first), db = std::abs(q.second - p.second);
    if (std::min(da, db) > 0.01) return slantedRoof(spec, points, riseGiven);  // a slanted side
    if (std::max(da, db) < 1.0) return R::failure("Two roof outline corners coincide");
  }
  // Snap to exact lines (a side 0.005 off would make slivers).
  const std::vector<double> as = [&] {
    std::vector<double> v;
    for (const auto& p : poly) v.push_back(p.first);
    return distinct(v);
  }();
  const std::vector<double> bs = [&] {
    std::vector<double> v;
    for (const auto& p : poly) v.push_back(p.second);
    return distinct(v);
  }();
  if (as.size() < 2 || bs.size() < 2) return R::failure("The roof outline encloses nothing");
  // Cells of the grid through its corners that lie inside it.
  const std::size_t na = as.size() - 1, nb = bs.size() - 1;
  std::vector<std::vector<bool>> in(na, std::vector<bool>(nb, false));
  double cellArea = 0.0;
  for (std::size_t i = 0; i < na; ++i)
    for (std::size_t j = 0; j < nb; ++j) {
      in[i][j] = insidePolygon(poly, (as[i] + as[i + 1]) / 2, (bs[j] + bs[j + 1]) / 2);
      if (in[i][j]) cellArea += (as[i + 1] - as[i]) * (bs[j + 1] - bs[j]);
    }
  double outlineArea = 0.0;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const auto& p = poly[i];
    const auto& q = poly[(i + 1) % poly.size()];
    outlineArea += p.first * q.second - q.first * p.second;
  }
  outlineArea = std::abs(outlineArea) / 2;
  if (outlineArea < 1.0 || std::abs(outlineArea - cellArea) > 1e-6 * std::max(1.0, outlineArea))
    return R::failure("The roof outline crosses itself");
  // The largest rectangles in it (none inside another): each gets a simple roof, and together
  // they are the roof of the whole outline, valleys included.
  auto filled = [&](std::size_t i0, std::size_t i1, std::size_t j0, std::size_t j1) {
    for (std::size_t i = i0; i <= i1; ++i)
      for (std::size_t j = j0; j <= j1; ++j)
        if (!in[i][j]) return false;
    return true;
  };
  std::vector<Rect> rects;
  for (std::size_t i0 = 0; i0 < na; ++i0)
    for (std::size_t i1 = i0; i1 < na; ++i1)
      for (std::size_t j0 = 0; j0 < nb; ++j0)
        for (std::size_t j1 = j0; j1 < nb; ++j1) {
          if (!filled(i0, i1, j0, j1)) continue;
          const bool grows = (i0 > 0 && filled(i0 - 1, i1, j0, j1)) || (i1 + 1 < na && filled(i0, i1 + 1, j0, j1)) ||
                             (j0 > 0 && filled(i0, i1, j0 - 1, j1)) || (j1 + 1 < nb && filled(i0, i1, j0, j1 + 1));
          if (!grows) rects.push_back({as[i0], as[i1 + 1], bs[j0], bs[j1 + 1]});
        }
  if (rects.empty()) return R::failure("The roof outline encloses nothing");
  if (rects.size() > 40) return R::failure("The roof outline has too many steps");

  const double slope = spec.pitch / 10.0;
  const double rise = riseGiven > 0 ? riseGiven : spec.thickness * std::sqrt(1.0 + slope * slope);  // upright
  RoofShape out;
  out.parts = static_cast<int>(rects.size());
  std::vector<TopoDS_Shape> bodies;
  double lowest = spec.height;
  if (spec.kind == "shed") {
    // One plane over everything, falling towards the side nearest the given direction.
    const gp_Vec fall(spec.fallX, spec.fallY, 0);
    if (fall.Magnitude() < 1e-9) return R::failure("Give the direction the shed roof falls towards");
    const double fa = fall.Dot(frame.u), fb = fall.Dot(frame.v);
    const bool alongA = std::abs(fa) >= std::abs(fb);  // the fall runs along a
    const double sign = (alongA ? fa : fb) > 0 ? 1.0 : -1.0;
    // The low wall line: the outline's extreme on the fall side.
    const double lowLine = alongA ? (sign > 0 ? as.back() : as.front()) : (sign > 0 ? bs.back() : bs.front());
    auto under = [&](double t) { return spec.height + slope * (lowLine - t) * sign; };  // t: coordinate along the fall
    const double t0all = (alongA ? as.front() : bs.front()) - spec.eave, t1all = (alongA ? as.back() : bs.back()) + spec.eave;
    lowest = std::min(under(t0all), under(t1all));
    // Well below the lowest edge: the lifted copy of the body must still reach under it.
    const double base = lowest - rise - 100.0;
    for (const Rect& r : rects) {
      const double t0 = (alongA ? r.a0 : r.b0) - spec.eave, t1 = (alongA ? r.a1 : r.b1) + spec.eave;
      const double c0 = (alongA ? r.b0 : r.a0) - spec.gableEave, c1 = (alongA ? r.b1 : r.a1) + spec.gableEave;
      auto p = [&](double t, double z) { return alongA ? frame.at(t, c0, z) : frame.at(c0, t, z); };
      auto body = prism({p(t0, base), p(t1, base), p(t1, under(t1)), p(t0, under(t0))}, (alongA ? frame.v : frame.u) * (c1 - c0));
      if (!body) return R::failure(body.error());
      bodies.push_back(body.value());
    }
  } else {
    lowest = spec.height - slope * spec.eave;
    // Well below the lowest edge: the lifted copy of the body must still reach under it.
    const double base = lowest - rise - 100.0;
    for (const Rect& r : rects) {
      const double la = r.a1 - r.a0, lb = r.b1 - r.b0;
      bool alongA = la >= lb;  // the ridge along the longer side
      if (spec.kind == "gable") {
        if (rects.size() == 1 && spec.ridge == "short" && std::abs(la - lb) > 0.01) alongA = !alongA;
        // Eaves across the ridge, gable overhang along it.
        const Rect grown = alongA ? Rect{r.a0 - spec.gableEave, r.a1 + spec.gableEave, r.b0 - spec.eave, r.b1 + spec.eave}
                                  : Rect{r.a0 - spec.eave, r.a1 + spec.eave, r.b0 - spec.gableEave, r.b1 + spec.gableEave};
        auto body = ridgeBody(frame, grown, alongA, slope, lowest, base);
        if (!body) return R::failure(body.error());
        bodies.push_back(body.value());
      } else {
        // Hip: what is under both the roof ridged one way and the roof ridged the other way.
        const Rect grown{r.a0 - spec.eave, r.a1 + spec.eave, r.b0 - spec.eave, r.b1 + spec.eave};
        auto one = ridgeBody(frame, grown, true, slope, lowest, base);
        auto other = ridgeBody(frame, grown, false, slope, lowest, base);
        if (!one || !other) return R::failure(one ? other.error() : one.error());
        auto body = booleanCombine(BooleanOp::Intersect, {one.value(), other.value()});
        if (!body) return R::failure("Hip roof could not be built: " + body.error());
        bodies.push_back(body.value());
      }
    }
  }
  TopoDS_Shape under = bodies.front();
  if (bodies.size() > 1) {
    auto all = booleanCombine(BooleanOp::Union, bodies);
    if (!all) return R::failure("Roof parts could not be joined: " + all.error());
    under = all.value();
  }
  // The roof: the same body lifted by the thickness, less the body itself.
  gp_Trsf lift;
  lift.SetTranslation(gp_Vec(0, 0, rise));
  const TopoDS_Shape lifted = BRepBuilderAPI_Transform(under, lift, Standard_True).Shape();
  auto roof = booleanCombine(BooleanOp::Subtract, {lifted, under});
  if (!roof) return R::failure("Roof could not be built: " + roof.error());
  int solids = 0;
  for (TopExp_Explorer e(roof.value(), TopAbs_SOLID); e.More(); e.Next()) ++solids;
  if (solids != 1 || !BRepCheck_Analyzer(roof.value()).IsValid()) return R::failure("Roof could not be built as one solid");
  out.solid = roof.value();
  // Seen from above the roof covers the outline grown by its overhangs; every part of its
  // upper surface has the same slope.
  out.planArea = volume(out.solid) / rise;
  out.slopeArea = out.planArea * std::sqrt(1.0 + slope * slope);
  const Bnd_Box box = exactBounds(out.solid);
  out.ridgeHeight = box.CornerMax().Z();
  out.eaveHeight = box.CornerMin().Z();
  return out;
}
}  // namespace

}  // namespace sbr::geom
