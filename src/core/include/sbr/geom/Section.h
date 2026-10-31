#pragma once
// Cross sections of linear members (beams, columns, braces) and the member
// solid made of one: the section swept straight from a start to an end
// point (docs/40). The catalog is where a member starts from, never a rule
// on its shape afterwards.

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include "sbr/Types.h"

namespace sbr::geom {

// An exact outline: closed loops of (x, y, bulge) - the first the outside,
// the others holes - each segment straight (bulge 0) or a circular arc
// (bulge = tan(sweep / 4), positive counter-clockwise, as in DXF).
struct SectionOutline {
  std::vector<std::vector<std::array<double, 3>>> loops;
  // Checked as a face once (0 not yet, 1 valid, 2 invalid), not for every
  // member made of it.
  mutable std::atomic<int> checked{0};
};

// A section in its own plane: x across (width), y up (depth), centred on
// the middle of its bounding box. Rectangles (timber, RC), circles (RC
// columns, round bars), own H dimensions, and catalog outlines (every
// steel family: H, I, channels, angles, box and round tubes, ...).
struct Section {
  enum class Kind { Rectangle, H, Circle, Profile };
  Kind kind = Kind::Rectangle;
  double width = 0.0;   // B: across (x); circle: diameter; profile: its bounding box
  double depth = 0.0;   // H: up (y); circle: diameter; profile: its bounding box
  double web = 0.0;     // H: web thickness tw
  double flange = 0.0;  // H: flange thickness tf
  double radius = 0.0;  // H: fillet radius between web and flanges
  // Profile: the outline (shared with the catalog entry it came from).
  std::shared_ptr<const SectionOutline> outline;
  // Catalog entry the section is (empty: own dimensions). Changing a
  // dimension by hand makes it the member's own section.
  std::string catalog;
  std::string label;  // the catalog's display name ("BCP-350x350x12")

  bool operator==(const Section& o) const;
};

const char* toString(Section::Kind kind);
// Why these dimensions make no section (empty: they do).
Status validateSection(const Section& section);
// Display name: "H-200×100×5.5×8", "□ 105×105", own dimensions marked.
std::string sectionName(const Section& section);

struct CatalogSection {
  std::string id;        // "H-200x100x5.5x8", "BOX/BCP/SHS/350x350x12"
  std::string name;      // "BCP-350x350x12"
  std::string family;    // H, I, BOX, PIPE, ANGLE, CHANNEL, C, CT, FB, SB, RB, HB
  std::string standard;  // BCP, STK, JIS_G_3194, general, ...
  std::string subtype;   // SHS, CHS, Equal, Lipped, ...
  std::string material;  // material class it is made of ("steel")
  std::vector<std::pair<std::string, double>> dimensions;  // nominal (H, B, t, ...)
  Section section;
};
// The built-in catalog: the Steel Member Generator's sections (docs/41),
// read and checked once, when first used.
const std::vector<CatalogSection>& sectionCatalog();
const CatalogSection* findSection(const std::string& id);
// Where the catalog comes from (repository, version, commit).
std::string sectionCatalogSource();

// The section as a face in `frame` (its x / y axes are the section's).
// Straight edges and exact arcs only.
Result<TopoDS_Face> makeSectionFace(const Section& section, const gp_Ax3& frame);

// Where a member's axis line meets its section: -1 / 0 / 1 across (right,
// centre, left looking from the start to the end) and up (bottom, centre,
// top). A beam drawn on its top edge keeps that edge when it gets deeper.
struct SectionAnchor {
  int across = 0;
  int up = 0;
  bool operator==(const SectionAnchor& o) const { return across == o.across && up == o.up; }
};

// The direction a section's y axis takes for a member along `axis`: the
// hint made square to the axis. Members along the hint (columns, with the
// default world-up hint) take world +Y instead, so the section never
// flips while the direction is dragged near vertical.
Result<gp_Dir> memberUp(const gp_Dir& axis, const gp_Dir& hint = gp_Dir(0, 0, 1));
// The member's section frame at its start: z along the member, y = up
// turned by `roll` (radians) about it, origin on the axis line.
Result<gp_Ax3> memberFrame(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& up, double roll);
// The member solid.
Result<TopoDS_Shape> makeMember(const gp_Pnt& start, const gp_Pnt& end, const gp_Dir& up, double roll,
                                const Section& section, const SectionAnchor& anchor);
// The anchor point's offset from the section's middle, in the section's
// own (x, y).
std::pair<double, double> anchorOffset(const Section& section, const SectionAnchor& anchor);

}  // namespace sbr::geom
