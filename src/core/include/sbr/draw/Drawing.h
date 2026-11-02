#pragma once
// 2D drawings from the model (docs/50): plans, sections and elevations,
// laid out on a sheet with a frame and title block, written as PDF (app),
// DXF and JWW (Jw_cad).
//
// A view is made in three parts:
// - cut: the model cut by the view's plane, exactly (lines and arcs stay
//   lines and arcs) - drawn thick, the cut areas optionally filled;
// - seen beyond: what is behind the cut, with hidden lines removed (OCCT's
//   exact hidden line removal) - thin; hidden lines optionally dashed;
// - annotations: grid lines with their bubbles and spans, levels (sections
//   and elevations), dimensions and text lying in the view.
// Coordinates are real size (mm). A sheet holds a view placed on paper at a
// scale: sheet coordinates are real size too, the paper's lower left at
// (0, 0) and its upper right at paper size x scale; text heights are on
// paper (mm), as drawings and Jw_cad count them.

#include <optional>
#include <string>
#include <vector>

#include <gp_Ax3.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec2d.hxx>

#include "sbr/Types.h"

namespace sbr::doc {
class Document;
}

namespace sbr::draw {

// What a line is: its weight, line type and layer in every format.
enum class Pen {
  Cut,        // cut by the view's plane (thick)
  Visible,    // seen beyond the cut (thin)
  Hidden,     // hidden behind (dashed, optional)
  Fill,       // the cut areas (grey)
  Grid,       // grid lines (dash-dot)
  GridLabel,  // grid bubbles, names, spans
  Level,      // levels in sections / elevations (dash-dot) and their names
  Dimension,  // dimensions and their values
  Text,       // text notes
  Frame,      // the sheet's frame (thick)
  TitleBlock  // the title block's lines and text
};
constexpr int kPenCount = 11;
const char* penLayerName(Pen pen);  // layer names in DXF ("CUT", ...)

struct Item {
  enum class Kind { Line, Arc, Polyline, Text, Fill };
  Kind kind = Kind::Line;
  Pen pen = Pen::Visible;
  // Line: 2 points. Polyline: the points (closed: back to the first).
  // Fill: the outer loop; holes in `holes`.
  std::vector<gp_Pnt2d> points;
  bool closed = false;
  std::vector<std::vector<gp_Pnt2d>> holes;
  // Arc: centre, radius, start angle and sweep (radians, anticlockwise);
  // a full circle has sweep 2 pi.
  gp_Pnt2d centre;
  double radius = 0.0, start = 0.0, sweep = 0.0;
  // Text at points[0]: its height on paper (mm), angle (radians) and how it
  // sits on that point: 0 left, 1 centre, 2 right; 0 baseline, 1 middle, 2 top.
  std::string text;
  double height = 3.5;
  double angle = 0.0;
  int hAlign = 0, vAlign = 0;
};

enum class ViewKind { Plan, Section, Elevation };

struct ViewSpec {
  ViewKind kind = ViewKind::Plan;
  // On the cut plane (elevations: before the model); its direction points
  // to the viewer (the side taken away), its X direction to the sheet's
  // right; the sheet's up is direction x X.
  gp_Ax3 frame;
  bool hidden = false;      // hidden lines, dashed
  bool fill = true;         // cut areas filled
  bool grid = true;         // grid lines, bubbles and spans
  bool levels = true;       // levels (sections / elevations)
  bool annotations = true;  // dimensions and text
  std::string title;        // e.g. "1FL 平面図"
  // Plans: where to look down from (the cut) and how far down to see (the
  // level below the next): 0 = everything below.
  double depth = 0.0;
};

// The usual views. Plans: cut at `cutZ`, seen from above. Sections: cut on
// the plane through `point` across `towardsViewer` (horizontal), seen from
// that side. Elevations: from 0 south (looking north, +Y), 1 east (looking
// west), 2 north, 3 west, before the model's bounds.
ViewSpec planView(double cutZ);
ViewSpec sectionView(const gp_Pnt& point, const gp_Dir& towardsViewer);
ViewSpec elevationView(const doc::Document& document, int side);

// Annotations whose size is on paper: made into lines when laid out at a
// scale. Grid axes: a point on the line and its direction (plans), or the
// axes running towards the viewer (sections / elevations: vertical).
struct GridMark {
  std::string name;
  int family = 0;  // 0: X axes, 1: Y axes
  gp_Pnt2d point;
  gp_Dir2d direction{0, 1};
  bool inPlan = false;
};
struct LevelMark {
  double v = 0.0;  // height on the view
  std::string name;
  double elevation = 0.0;
};
struct DimensionMark {
  gp_Pnt2d a, b;    // measured points
  gp_Vec2d offset;  // to the dimension line
  std::string text;
};

struct View {
  std::vector<Item> items;  // view coordinates (real mm)
  std::vector<GridMark> grids;
  std::vector<LevelMark> levels;
  std::vector<DimensionMark> dimensions;
  double minU = 0, minV = 0, maxU = 0, maxV = 0;  // extent of the model's lines
  ViewSpec spec;
  int objects = 0;            // objects drawn
  std::vector<std::string> notes;  // what was left out, with the reason
};

// The view of the visible model. Heavy for large models (hidden lines):
// call once and lay it out again for other scales or papers.
Result<View> makeView(const doc::Document& document, const ViewSpec& spec);

struct TitleBlock {
  std::string project;  // 工事名
  std::string drawing;  // 図面名
  std::string number;   // 図番
  std::string author;   // 作成者
  std::string date;     // 日付 (e.g. 2026-11-02)
};

struct SheetSpec {
  double scale = 100.0;       // 1:scale
  std::string paper = "auto";  // "auto", "A4", "A3", "A2", "A1" (landscape)
  bool frame = true;          // frame and title block
  TitleBlock title;
  bool north = true;          // north mark on plans (+Y is north)
};

struct Sheet {
  std::vector<Item> items;  // sheet coordinates (real mm, paper's lower left at 0, 0)
  double scale = 100.0;
  std::string paper;        // "A3", ...
  double paperWidth = 420.0, paperHeight = 297.0;  // mm on paper
  std::string title;        // the drawing's name
  std::vector<std::string> notes;  // e.g. "does not fit A1 at 1:50"
};

// The paper the view fits at that scale ("auto": the smallest A4..A1 that
// holds it with the title block, else A1 with a note).
Result<Sheet> layoutSheet(const View& view, const SheetSpec& spec);

// Writers. DXF: R12 (AC1009), real size, layers per pen, Japanese text in
// Shift_JIS ($DWGCODEPAGE ANSI_932) as Jw_cad and AutoCAD read it. JWW:
// Jw_cad's own format (version 6.00 data), real size with the scale on the
// layer group, pens as Jw line colours / types, layers per pen.
Status writeDxf(const Sheet& sheet, const std::string& path);
Status writeJww(const Sheet& sheet, const std::string& path);

// Line weights on paper (mm) and dash patterns (paper mm, on / off, empty:
// solid) per pen, shared by every writer.
double penWidth(Pen pen);
std::vector<double> penDashes(Pen pen);

}  // namespace sbr::draw
