#include "sbr/geom/Roof.h"

#include <algorithm>
#include <cmath>

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
    if (std::min(da, db) > 0.01)
      return R::failure("Roof outlines must have right-angled corners only (a slanted side is not supported yet)");
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
