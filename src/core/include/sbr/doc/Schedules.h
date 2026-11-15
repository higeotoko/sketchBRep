// Tables read from the parts of the model as they are now (docs/90 G-11, docs/97 N-10 / N-11,
// docs/104): doors and windows, rooms, areas by storey. Nothing is stored: the same model
// always gives the same table, and the marks drawn on plans come from the same function.
#pragma once

#include <string>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "sbr/doc/Document.h"
#include "sbr/draw/Drawing.h"

namespace sbr::doc {

// One opening of a wall.
struct OpeningRow {
  std::string mark;     // W1, D2, O1: the same for openings of one kind, fitting and size; or one's own
  bool ownMark = false; // given by hand (Modeler::setOpeningMarks): it stays whatever is added or changed
  std::string product;  // product number given by hand, if any
  std::string kind;     // window | door | opening (no fitting)
  std::string fitting;  // the fitting's name for people ("引違い"); empty for a plain opening
  double width = 0.0, height = 0.0, sill = 0.0;
  ObjectId host;
  std::string hostName;
  std::string openingId;
  std::size_t index = 0;  // its number in the host
  std::string level;      // name of the level the wall stands on, if any
  gp_Pnt center;          // world, on the face it was cut from
  double bottom = 0.0;    // world z of its lowest edge
  gp_Dir normal;          // of that face
  double depth = 0.0;     // through the wall
};

// One line of the door and window schedule: a mark and how many there are.
struct OpeningTypeRow {
  std::string mark, kind, fitting;
  double width = 0.0, height = 0.0;
  int count = 0;
  bool ownMark = false;
  std::string product;
  // Openings given this mark by hand differ in kind, fitting, size or product number: the line
  // shows the first one's, and says so (nothing is made the same silently).
  bool mixed = false;
  std::vector<double> sills;  // the different sill heights among them, ascending
};

struct OpeningSchedule {
  std::vector<OpeningRow> rows;       // by host, in the order they were cut
  std::vector<OpeningTypeRow> types;  // windows, doors, plain openings; each by width, height, fitting
};
// Openings through the vertical faces of walls (attribute kind = wall). A fitting with its
// sill on the floor is a door, any other fitting a window (as in the IFC export).
OpeningSchedule openingSchedule(const Document& document);

struct RoomRow {
  ObjectId id;
  std::string name, level;  // level: its name, empty when the marker is at no level's height
  bool closed = false;
  bool counted = false;     // closed and not sharing its region: in the totals
  std::string problem;
  double centreArea = 0.0;  // mm2 (壁芯)
  bool innerKnown = false;
  double innerArea = 0.0, innerPerimeter = 0.0;  // mm2, mm (内法)
  gp_Pnt at;
  std::string use, floor, wall, ceiling;  // docs/115: as given on the room
  double ceilingHeight = 0.0;              // 0: not given
};

struct RoomSchedule {
  std::vector<RoomRow> rows;
  double centreTotal = 0.0, innerTotal = 0.0;  // of the counted rooms (inner: of those whose inner area is known)
  int innerMissing = 0;  // counted rooms whose inner area is not evaluated: innerTotal is not the whole
  int notCounted = 0;
};
RoomSchedule roomSchedule(const Document& document);

// One storey.
struct AreaRow {
  std::string level;  // name; empty: rooms whose marker is at no level's height
  double elevation = 0.0;
  bool enclosedKnown = false;   // false: no level to cut at (the row of rooms at no level)
  double enclosedArea = 0.0;    // mm2 within wall lines (壁芯), every closed region, with or without a room
  int regions = 0;              // closed regions
  int rooms = 0;                // counted rooms
  int regionsWithoutRoom = 0;
  double roomCentre = 0.0, roomInner = 0.0;
  int innerMissing = 0;
  int unreadableWalls = 0;      // walls reshaped by hand at this storey: not read as walls
  double slabArea = 0.0;        // mm2 in plan of the slabs whose top lies on this level (openings taken out)
  int slabs = 0;
};

struct AreaTable {
  std::vector<AreaRow> rows;  // bottom to top
  double enclosedTotal = 0.0, roomCentreTotal = 0.0, slabTotal = 0.0;
};
// Areas measured from the model. Not the floor area or building area of the Building
// Standards Act (what counts and what does not is not looked at): those are not evaluated.
AreaTable areaTable(const Document& document);

// What a room's tag says on screen and on drawings: its name and its area within the wall
// lines (壁芯) as the walls are now; a room no longer enclosed says so. The unit is written
// "m²": the view's font has no glyph for the one-character square metre sign.
std::string roomTagText(const RoomRow& room);

// The tables for a sheet of their own (docs/113): 0 the door and window schedule (one line per
// mark), 1 the room schedule, 2 the area table, 3 the finish schedule (docs/115), 4 the types of
// floors and roofs (docs/136: one line per type name and part, its count and sizes); the same
// values and notes as the CSV.
draw::Table scheduleTable(const Document& document, int which);
// Whether a table made by scheduleTable(document, which) has anything in it: openings beyond the
// total line, rooms, areas, finishes given by hand (docs/125, docs/130).
bool tableHasContent(const draw::Table& table, int which);

// The tables as CSV for a spreadsheet: UTF-8 with a byte order mark, CRLF, Japanese headings,
// lengths in mm, areas in m2.
std::string openingScheduleCsv(const OpeningSchedule& schedule);
std::string roomScheduleCsv(const RoomSchedule& schedule);
std::string areaTableCsv(const AreaTable& table);
// A set of drawings with sheet numbers (docs/97 N-12, docs/125): a sheet list first, then a
// plan per storey (cut 1100 above its floor, the storeys below left out), the four elevations
// and the tables (doors and windows, rooms, areas, finishes when given). Each sheet's title
// block carries its number (prefix + 01, 02, ...) and name; the list names them all. Views with
// nothing to draw are left out (said in `notes`).
struct DrawingSetSpec {
  double scale = 100.0;
  std::string paper = "auto";
  std::string prefix = "A-";
  draw::TitleBlock title;  // project, author, date (the drawing name and number are set per sheet)
  // docs/131: the tables beside the lowest storey's plan on its sheet (draw::layoutSheetWithTables)
  // instead of sheets of their own; that sheet is named "1FL 平面図・建具表・室の表" and so on.
  bool tablesWithPlan = false;
  // docs/132: sections after the plans: one per active vertical section plane ("A-A 断面図", ...),
  // or, with none, two through the middle of the model (A-A cut east-west and seen from the south,
  // B-B cut north-south and seen from the east). The plans show where they are cut, with the
  // sections' sheet numbers.
  bool sections = true;
};
struct DrawingSet {
  std::vector<draw::Sheet> sheets;
  std::vector<std::string> numbers, names;  // per sheet
  std::vector<std::string> notes;
};
Result<DrawingSet> drawingSet(const Document& document, const DrawingSetSpec& spec);

// Any of the tables as CSV, in the same form (docs/115: the finish schedule).
std::string tableCsv(const draw::Table& table);

}  // namespace sbr::doc
