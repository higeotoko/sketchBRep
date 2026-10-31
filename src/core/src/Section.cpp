#include "sbr/geom/Section.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <TopoDS.hxx>
#include <Standard_Failure.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <nlohmann/json.hpp>

#include "sbr/geom/Profile.h"

namespace sbr::embedded {
extern const char* const kSteelCatalog[];
extern const unsigned kSteelCatalog_parts;
}  // namespace sbr::embedded

namespace sbr::geom {

namespace {

bool finitePositive(double v) { return std::isfinite(v) && v >= tolerance::kMinDimension; }

std::string number(double v) {
  char text[32];
  std::snprintf(text, sizeof text, "%g", v);
  return text;
}

// Exact edges of one closed loop in `frame`.
Result<TopoDS_Wire> loopWire(const std::vector<std::array<double, 3>>& loop, const gp_Ax3& frame) {
  const gp_Vec x(frame.XDirection()), y(frame.YDirection());
  auto at = [&](double u, double v) { return frame.Location().Translated(x * u + y * v); };
  BRepBuilderAPI_MakeWire wire;
  const std::size_t n = loop.size();
  for (std::size_t i = 0; i < n; ++i) {
    const auto& a = loop[i];
    const auto& b = loop[(i + 1) % n];
    if (std::hypot(b[0] - a[0], b[1] - a[1]) < 1e-9) continue;
    if (a[2] == 0) {
      wire.Add(BRepBuilderAPI_MakeEdge(at(a[0], a[1]), at(b[0], b[1])).Edge());
    } else {
      const auto [mu, mv] = bulgeArcMiddle(a[0], a[1], b[0], b[1], a[2]);
      GC_MakeArcOfCircle arc(at(a[0], a[1]), at(mu, mv), at(b[0], b[1]));
      if (!arc.IsDone()) return Result<TopoDS_Wire>::failure("A section arc is invalid");
      wire.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
    }
  }
  if (!wire.IsDone() || !wire.Wire().Closed()) return Result<TopoDS_Wire>::failure("The section outline does not close");
  return wire.Wire();
}

// Signed area of a loop (counter-clockwise positive), arcs included: the
// polygon of its points plus each arc's segment beyond its chord.
double loopArea(const std::vector<std::array<double, 3>>& loop) {
  double area = 0;
  const std::size_t n = loop.size();
  for (std::size_t i = 0; i < n; ++i) {
    const auto& a = loop[i];
    const auto& b = loop[(i + 1) % n];
    area += (a[0] * b[1] - b[0] * a[1]) / 2;
    if (a[2] != 0) {
      const double sweep = 4 * std::atan(a[2]);
      const double chord = std::hypot(b[0] - a[0], b[1] - a[1]);
      const double r = chord / (2 * std::abs(std::sin(sweep / 2)));
      area += (sweep > 0 ? 1 : -1) * r * r * (std::abs(sweep) - std::sin(std::abs(sweep))) / 2;
    }
  }
  return area;
}

std::vector<CatalogSection> readCatalog(std::string* source) {
  std::string text;
  for (unsigned i = 0; i < embedded::kSteelCatalog_parts; ++i) text += embedded::kSteelCatalog[i];
  const nlohmann::json data = nlohmann::json::parse(text);
  if (source) {
    const auto& s = data.at("source");
    *source = s.value("repository", std::string()) + " " + s.value("version", std::string()) + " (" +
              s.value("commit", std::string()).substr(0, 7) + ")";
  }
  std::vector<CatalogSection> out;
  out.reserve(data.at("sections").size());
  for (const auto& j : data.at("sections")) {
    CatalogSection c;
    c.id = j.at("id").get<std::string>();
    c.name = j.value("name", c.id);
    c.family = j.value("family", std::string());
    c.standard = j.value("standard", std::string());
    c.subtype = j.value("subtype", std::string());
    c.material = "steel";
    for (const auto& [key, value] : j.at("dims").items()) c.dimensions.emplace_back(key, value.get<double>());
    auto outline = std::make_shared<SectionOutline>();
    for (const auto& loop : j.at("loops")) {
      std::vector<std::array<double, 3>> points;
      for (const auto& p : loop) points.push_back({p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>()});
      outline->loops.push_back(std::move(points));
    }
    c.section.kind = Section::Kind::Profile;
    c.section.width = j.at("width").get<double>();
    c.section.depth = j.at("depth").get<double>();
    c.section.outline = std::move(outline);
    c.section.catalog = c.id;
    c.section.label = c.name;
    out.push_back(std::move(c));
  }
  return out;
}

std::string& catalogSource() {
  static std::string source;
  return source;
}

}  // namespace

bool Section::operator==(const Section& o) const {
  if (kind != o.kind || width != o.width || depth != o.depth || web != o.web || flange != o.flange ||
      radius != o.radius || catalog != o.catalog || label != o.label) {
    return false;
  }
  if (outline == o.outline) return true;
  return outline && o.outline && outline->loops == o.outline->loops;
}

const char* toString(Section::Kind kind) {
  switch (kind) {
    case Section::Kind::Rectangle: return "rectangle";
    case Section::Kind::H: return "h";
    case Section::Kind::Circle: return "circle";
    case Section::Kind::Profile: return "profile";
  }
  return "rectangle";
}

Status validateSection(const Section& s) {
  if (!finitePositive(s.width) || !finitePositive(s.depth)) return Status::failure("Width and depth must be positive");
  switch (s.kind) {
    case Section::Kind::Rectangle: break;
    case Section::Kind::Circle:
      if (std::abs(s.width - s.depth) > 1e-9) return Status::failure("A round section has one diameter");
      break;
    case Section::Kind::H:
      if (!finitePositive(s.web) || !finitePositive(s.flange)) {
        return Status::failure("Web and flange thicknesses must be positive");
      }
      if (!std::isfinite(s.radius) || s.radius < 0) return Status::failure("The fillet radius must not be negative");
      if (s.web + 2 * s.radius >= s.width - tolerance::kMinDimension) {
        return Status::failure("The web and its fillets must be narrower than the flanges");
      }
      if (2 * (s.flange + s.radius) >= s.depth - tolerance::kMinDimension) {
        return Status::failure("The flanges and fillets must leave some web between them");
      }
      break;
    case Section::Kind::Profile:
      if (!s.outline || s.outline->loops.empty() || s.outline->loops.front().size() < 2) {
        return Status::failure("The section has no outline");
      }
      for (const auto& loop : s.outline->loops) {
        for (const auto& p : loop) {
          if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
            return Status::failure("The section outline has a point that is not finite");
          }
        }
      }
      break;
  }
  return Status::success();
}

std::string sectionName(const Section& s) {
  switch (s.kind) {
    case Section::Kind::Profile: return s.label.empty() ? s.catalog : s.label;
    case Section::Kind::Circle: return "○ φ" + number(s.width);
    case Section::Kind::H:
      return "H-" + number(s.depth) + "×" + number(s.width) + "×" + number(s.web) + "×" + number(s.flange) +
             (s.catalog.empty() ? "（独自寸法）" : "");
    case Section::Kind::Rectangle: break;
  }
  return "□ " + number(s.width) + "×" + number(s.depth);
}

const std::vector<CatalogSection>& sectionCatalog() {
  static const std::vector<CatalogSection> catalog = [] {
    std::vector<CatalogSection> c = readCatalog(&catalogSource());
    for (const CatalogSection& entry : c) {
      // Checked once here, never for each member placed.
      if (!validateSection(entry.section)) std::abort();
    }
    return c;
  }();
  return catalog;
}

const CatalogSection* findSection(const std::string& id) {
  for (const CatalogSection& entry : sectionCatalog()) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

std::string sectionCatalogSource() {
  sectionCatalog();
  return catalogSource();
}

Result<TopoDS_Face> makeSectionFace(const Section& s, const gp_Ax3& frame) {
  using R = Result<TopoDS_Face>;
  if (const Status valid = validateSection(s); !valid) return R::failure(valid.message());
  const gp_Vec x(frame.XDirection()), y(frame.YDirection());
  auto at = [&](double u, double v) { return frame.Location().Translated(x * u + y * v); };
  try {
    if (s.kind == Section::Kind::Circle) {
      const gp_Circ circle(gp_Ax2(frame.Location(), frame.Direction(), frame.XDirection()), s.width / 2);
      BRepBuilderAPI_MakeFace face(gp_Pln(frame),
                                   BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circle).Edge()).Wire(), Standard_True);
      if (!face.IsDone()) return R::failure("The section face could not be made");
      return face.Face();
    }
    if (s.kind == Section::Kind::Profile) {
      // The outside counter-clockwise about the frame's normal, holes the
      // other way, whichever way the loops were drawn.
      auto wire = [&](std::size_t i) -> Result<TopoDS_Wire> {
        auto w = loopWire(s.outline->loops[i], frame);
        if (!w) return w;
        const bool ccw = loopArea(s.outline->loops[i]) > 0;
        return (ccw == (i == 0)) ? w.value() : TopoDS::Wire(w.value().Reversed());
      };
      auto outer = wire(0);
      if (!outer) return R::failure(outer.error());
      BRepBuilderAPI_MakeFace make(gp_Pln(frame), outer.value(), Standard_True);
      if (!make.IsDone()) return R::failure("The section face could not be made");
      for (std::size_t i = 1; i < s.outline->loops.size(); ++i) {
        auto hole = wire(i);
        if (!hole) return R::failure(hole.error());
        make.Add(hole.value());
      }
      const TopoDS_Face face = make.Face();
      int state = s.outline->checked.load();
      if (state == 0) {
        state = BRepCheck_Analyzer(face).IsValid() ? 1 : 2;
        s.outline->checked.store(state);
      }
      if (state == 2) return R::failure("The section outline is not a valid face");
      return face;
    }
    BRepBuilderAPI_MakeWire wire;
    auto line = [&](double u0, double v0, double u1, double v1) {
      wire.Add(BRepBuilderAPI_MakeEdge(at(u0, v0), at(u1, v1)).Edge());
    };
    const double b = s.width / 2, h = s.depth / 2;
    if (s.kind == Section::Kind::Rectangle) {
      line(-b, -h, b, -h);
      line(b, -h, b, h);
      line(b, h, -b, h);
      line(-b, h, -b, -h);
    } else {
      const double w = s.web / 2, f = s.flange, r = s.radius;
      // Quarter arc from (u0, v0) to (u1, v1) about (cu, cv).
      auto fillet = [&](double u0, double v0, double u1, double v1, double cu, double cv) {
        if (r <= 0) {
          line(u0, v0, u1, v1);
          return;
        }
        const double mu = (u0 - cu) + (u1 - cu), mv = (v0 - cv) + (v1 - cv), len = std::hypot(mu, mv);
        const Handle(Geom_TrimmedCurve) arc =
            GC_MakeArcOfCircle(at(u0, v0), at(cu + r * mu / len, cv + r * mv / len), at(u1, v1)).Value();
        wire.Add(BRepBuilderAPI_MakeEdge(arc).Edge());
      };
      line(-b, -h, b, -h);
      line(b, -h, b, -h + f);
      line(b, -h + f, w + r, -h + f);
      fillet(w + r, -h + f, w, -h + f + r, w + r, -h + f + r);
      line(w, -h + f + r, w, h - f - r);
      fillet(w, h - f - r, w + r, h - f, w + r, h - f - r);
      line(w + r, h - f, b, h - f);
      line(b, h - f, b, h);
      line(b, h, -b, h);
      line(-b, h, -b, h - f);
      line(-b, h - f, -w - r, h - f);
      fillet(-w - r, h - f, -w, h - f - r, -w - r, h - f - r);
      line(-w, h - f - r, -w, -h + f + r);
      fillet(-w, -h + f + r, -w - r, -h + f, -w - r, -h + f + r);
      line(-w - r, -h + f, -b, -h + f);
      line(-b, -h + f, -b, -h);
    }
    if (!wire.IsDone()) return R::failure("The section outline does not close");
    BRepBuilderAPI_MakeFace face(gp_Pln(frame), wire.Wire(), Standard_True);
    if (!face.IsDone()) return R::failure("The section face could not be made");
    return face.Face();
  } catch (const Standard_Failure& e) {
    return R::failure(std::string("The section could not be made: ") + e.GetMessageString());
  }
}

Result<gp_Dir> memberUp(const gp_Dir& axis, const gp_Dir& hint) {
  gp_Vec up = gp_Vec(hint) - gp_Vec(axis) * gp_Vec(hint).Dot(gp_Vec(axis));
  if (up.Magnitude() < 1e-3) {
    // Along the hint: a deterministic other choice (world +Y, then +X).
    const gp_Vec fallback = std::abs(axis.Y()) < 0.999 ? gp_Vec(0, 1, 0) : gp_Vec(1, 0, 0);
    up = fallback - gp_Vec(axis) * fallback.Dot(gp_Vec(axis));
  }
  if (up.Magnitude() < 1e-9) return Result<gp_Dir>::failure("No section direction for this member");
  return gp_Dir(up);
}

Result<gp_Ax3> memberFrame(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& up, double roll) {
  using R = Result<gp_Ax3>;
  const gp_Vec along(start, end);
  if (!std::isfinite(along.X()) || !std::isfinite(along.Y()) || !std::isfinite(along.Z())) {
    return R::failure("The member's points must be finite");
  }
  if (along.Magnitude() < tolerance::kMinDimension) return R::failure("The member's start and end are the same point");
  if (!std::isfinite(roll)) return R::failure("The member's rotation must be finite");
  const gp_Dir z(along);
  gp_Vec y = gp_Vec(up) - gp_Vec(z) * gp_Vec(up).Dot(gp_Vec(z));
  if (y.Magnitude() < 1e-9) return R::failure("The section direction runs along the member");
  y.Normalize();
  y.Rotate(gp_Ax1(gp::Origin(), z), roll);
  const gp_Dir x = gp_Dir(y.Crossed(gp_Vec(z)));  // left, looking from the start to the end
  return gp_Ax3(start, z, x);
}

std::pair<double, double> anchorOffset(const Section& section, const SectionAnchor& anchor) {
  return {anchor.across * section.width / 2, anchor.up * section.depth / 2};
}

Result<TopoDS_Shape> makeMember(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& up, double roll,
                                const Section& section, const SectionAnchor& anchor) {
  using R = Result<TopoDS_Shape>;
  if (std::abs(anchor.across) > 1 || std::abs(anchor.up) > 1) return R::failure("Unknown section anchor");
  auto frame = memberFrame(start, end, up, roll);
  if (!frame) return R::failure(frame.error());
  // The section's middle sits off the axis line by the anchor offset.
  const auto [du, dv] = anchorOffset(section, anchor);
  gp_Ax3 placed = *frame;
  placed.SetLocation(start.Translated(-gp_Vec(frame->XDirection()) * du - gp_Vec(frame->YDirection()) * dv));
  auto face = makeSectionFace(section, placed);
  if (!face) return R::failure(face.error());
  try {
    BRepPrimAPI_MakePrism prism(face.value(), gp_Vec(start, end));
    if (!prism.IsDone()) return R::failure("The member could not be made");
    return prism.Shape();
  } catch (const Standard_Failure& e) {
    return R::failure(std::string("The member could not be made: ") + e.GetMessageString());
  }
}

}  // namespace sbr::geom
