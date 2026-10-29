#include "sbr/geom/Section.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Standard_Failure.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

namespace sbr::geom {

namespace {

bool finitePositive(double v) { return std::isfinite(v) && v >= tolerance::kMinDimension; }

std::string number(double v) {
  char text[32];
  std::snprintf(text, sizeof text, "%g", v);
  return text;
}

}  // namespace

const char* toString(Section::Kind kind) {
  switch (kind) {
    case Section::Kind::Rectangle: return "rectangle";
    case Section::Kind::H: return "h";
  }
  return "rectangle";
}

Status validateSection(const Section& s) {
  if (!finitePositive(s.width) || !finitePositive(s.depth)) return Status::failure("Width and depth must be positive");
  if (s.kind == Section::Kind::H) {
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
  }
  return Status::success();
}

std::string sectionName(const Section& s) {
  std::string name;
  if (s.kind == Section::Kind::H) {
    name = "H-" + number(s.depth) + "×" + number(s.width) + "×" + number(s.web) + "×" + number(s.flange);
  } else {
    name = "□ " + number(s.width) + "×" + number(s.depth);
  }
  if (s.catalog.empty() && s.kind == Section::Kind::H) name += "（独自寸法）";
  return name;
}

const std::vector<CatalogSection>& sectionCatalog() {
  // JIS G 3192 rolled H sections (r as tabulated there). Stage A: two sizes;
  // the Steel Member Generator catalog comes in later (docs/40).
  static const std::vector<CatalogSection> catalog = [] {
    std::vector<CatalogSection> c;
    auto h = [&](const char* id, double depth, double width, double web, double flange, double radius) {
      Section s;
      s.kind = Section::Kind::H;
      s.depth = depth;
      s.width = width;
      s.web = web;
      s.flange = flange;
      s.radius = radius;
      s.catalog = id;
      c.push_back({id, "H", "steel", "JIS G 3192", s});
    };
    h("H-200x100x5.5x8", 200, 100, 5.5, 8, 8);
    h("H-400x200x8x13", 400, 200, 8, 13, 13);
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

Result<TopoDS_Face> makeSectionFace(const Section& s, const gp_Ax3& frame) {
  using R = Result<TopoDS_Face>;
  if (const Status valid = validateSection(s); !valid) return R::failure(valid.message());
  const gp_Vec x(frame.XDirection()), y(frame.YDirection());
  auto at = [&](double u, double v) { return frame.Location().Translated(x * u + y * v); };
  try {
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
