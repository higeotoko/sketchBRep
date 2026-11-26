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

#include <memory>
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
  TitleBlock,  // the title block's lines and text
  HeightLimit,  // section of the governing height envelope
  HeightReview, // unconfirmed height envelope in a section
  FireLimit3,
  FireLimit5,
  FireCompartment
};
constexpr int kPenCount = 16;
const char* penLayerName(Pen pen);  // layer names in DXF ("CUT", ...)

struct Item {
  enum class Kind { Line, Arc, Polyline, Text, Fill, Image };
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
  // docs/233: Image: pixels (RGBA, 4 bytes each, rows from the top) stretched over the rectangle from
  // points[0] (lower left) to points[1] (upper right). Drawn on screen and in PDF; DXF / JWW / LayOut
  // leave it out.
  int imageWidth = 0, imageHeight = 0;
  std::shared_ptr<const std::vector<unsigned char>> rgba;
};

enum class ViewKind { Plan, Section, Elevation, SitePlan };

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
  // Sections: how far behind the cut to see (docs/290: a frame elevation along one grid line); 0 = all.
  double sectionDepth = 0.0;
  std::vector<std::string> tags;  // empty: all leaf tags
  std::string within;            // optional ancestor group
  bool includeHidden = false;
  bool memberLabels = false;
  std::string framingPreset;     // named framing plan, empty for ordinary views
  // docs/208: a plan of the bracing walls of this storey of the wall calculation (its floor id):
  // the walls counted, thick, with their mark, ratio and effective length, and the side parts of
  // the quarter division, dashed, with what they need and have. Empty: none.
  std::string bracingFloor;
  // docs/216: plans: the walls with insulation given drawn with the insulation mark (a zigzag along
  // the middle of the wall), and a legend of the insulations and of the floors' and roofs'.
  bool insulation = false;
  // docs/219: the scale the view is laid out at (1:paperScale); the insulation zigzags are made no
  // finer than a peak every 1.2 mm on paper. 0: not known (the screen): by the band's width only.
  double paperScale = 0.0;
  // Plans (docs/132): where sections are cut, drawn as a cut line across the drawing with arrows
  // the way the section looks and its label and sheet number at both ends.
  struct CutMark {
    gp_Pnt point;            // on the cut
    gp_Dir towardsViewer;    // as sectionView's
    std::string label;       // "A"
    std::string sheet;       // "A-08" (empty: none)
  };
  std::vector<CutMark> cutMarks;
  // docs/173: > 0: the cut layers of walls and floors hatched (structural timber crossed, plywood
  // with single lines), spaced on paper at this scale (1:hatchScale); 0: no hatching (the screen).
  double hatchScale = 0.0;
};

// The usual views. Plans: cut at `cutZ`, seen from above. Sections: cut on
// the plane through `point` across `towardsViewer` (horizontal), seen from
// that side. Elevations: from 0 south (looking north, +Y), 1 east (looking
// west), 2 north, 3 west, before the model's bounds.
ViewSpec planView(double cutZ);
ViewSpec sitePlanView();
ViewSpec sectionView(const gp_Pnt& point, const gp_Dir& towardsViewer);
ViewSpec elevationView(const doc::Document& document, int side);
Result<ViewSpec> framingView(const doc::Document& document, const std::string& preset,
                             const std::string& within = "", bool includeHidden = false,
                             const std::vector<std::string>& tags = {});

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

// Cells are text as shown; the first row of rows comes under headers.
struct Table {
  std::string title;
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> notes;
};

struct View {
  std::vector<Item> items;  // view coordinates (real mm)
  std::vector<GridMark> grids;
  struct CutLine {
    gp_Pnt2d point;
    gp_Dir2d along{1, 0}, look{0, 1};  // the line; the way the section looks
    std::string label, sheet;
  };
  std::vector<CutLine> cuts;  // plans: spec.cutMarks on the view (docs/132)
  std::vector<LevelMark> levels;
  std::vector<DimensionMark> dimensions;
  struct MemberMark { gp_Pnt2d at; std::string text; };
  std::vector<MemberMark> members;
  struct BracingMark { gp_Pnt2d at; std::string text; double height; };
  std::vector<BracingMark> bracingMarks;
  struct RoomTag { std::size_t item; std::vector<gp_Pnt2d> outline; };
  std::vector<RoomTag> roomTags;  // room tags movable within their room on a bracing plan
  double minU = 0, minV = 0, maxU = 0, maxV = 0;  // extent of the model's lines
  ViewSpec spec;
  int objects = 0;            // objects drawn
  std::vector<std::string> notes;  // what was left out, with the reason
  std::vector<std::string> calculationNotes;  // provenance only; no wall-quantity verdict on a geometry sheet
  std::optional<gp_Dir2d> trueNorth;
  std::optional<Table> siteGroundTable;
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
  bool north = true;          // north mark on plans
  bool compactDiagram = false;  // embedded plan: north beside the drawing and a graphic scale
};

struct Sheet {
  std::vector<Item> items;  // sheet coordinates (real mm, paper's lower left at 0, 0)
  double scale = 100.0;
  std::string paper;        // "A3", ...
  double paperWidth = 420.0, paperHeight = 297.0;  // mm on paper
  std::string title;        // the drawing's name
  std::vector<std::string> notes;  // e.g. "does not fit A1 at 1:50"
};

// The table on the smallest paper A4..A1 that holds it (or the paper asked for), in the frame
// with the title block, at the top left of the drawing area. Sheet units are real mm at
// spec.scale like a view's (so the writers treat it the same); text 3 mm on paper.
Result<Sheet> tableSheet(const Table& table, const SheetSpec& spec);

// The paper the view fits at that scale ("auto": the smallest A4..A1 that
// holds it with the title block, else A1 with a note).
Result<Sheet> layoutSheet(const View& view, const SheetSpec& spec);

// The view with tables beside it on one sheet (docs/130): the drawing (its caption, grids,
// levels; a plan's north mark at its top right) at the top left of the frame, the tables one
// under another to its right, or under the drawing when that takes a smaller paper. The paper:
// the smallest A4..A1 that holds both ("auto"), else the one asked for (with a note when they
// run off). No tables: as layoutSheet.
Result<Sheet> layoutSheetWithTables(const View& view, const std::vector<Table>& tables, const SheetSpec& spec);

// Writers. DXF: R12 (AC1009), real size, layers per pen, Japanese text in
// Shift_JIS ($DWGCODEPAGE ANSI_932) as Jw_cad and AutoCAD read it. JWW:
// Jw_cad's own format (version 6.00 data), real size with the scale on the
// layer group, pens as Jw line colours / types, layers per pen.
Status writeDxf(const Sheet& sheet, const std::string& path);
Status writeJww(const Sheet& sheet, const std::string& path);
// LayOut (docs/185): the sheets as the pages of one document (named `names`, else the sheets'
// titles), lines, arcs, fills and text on paper with the pens' widths and line types, through
// the LayOut part of the SketchUp SDK (loaded when needed; without it: refused, nothing written).
// One page size for the document: the largest paper of the sheets.
bool layoutAvailable(std::string* why = nullptr);
Status writeLayout(const std::vector<Sheet>& sheets, const std::vector<std::string>& names, const std::string& path);
// The pages of a LayOut file (to check what was written).
Result<int> layoutPageCount(const std::string& path);
// A LayOut file printed to PDF by LayOut itself (to look at what was written).
Status layoutToPdf(const std::string& layout, const std::string& pdf);

// Line weights on paper (mm) and dash patterns (paper mm, on / off, empty:
// solid) per pen, shared by every writer.
double penWidth(Pen pen);
std::vector<double> penDashes(Pen pen);

}  // namespace sbr::draw
