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
  // AW1, AD2, WD1, WW1, O1 (docs/176): the same for openings of one kind, fitting, size and side
  // (A: in an outer wall, W: in an inner one); or one's own.
  std::string mark;
  bool ownMark = false; // given by hand (Modeler::setOpeningMarks): it stays whatever is added or changed
  std::string product;  // product number given by hand, if any
  std::string kind;     // window | door | opening (no fitting)
  bool inner = false;   // in a wall of an inner build-up (interior_*): WD / WW; else AD / AW
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
  // docs/203: given by a person (Modeler::setOpeningDaylight); below 0: not given.
  double daylightFactor = -1.0;  // 採光補正係数 (令 20 条)
  std::string daylightBasis;
  double ventilationM2 = -1.0;   // area open for ventilation (法 28 条 2 項), m2
};

// One line of the door and window schedule: a mark and how many there are.
struct OpeningTypeRow {
  std::string mark, kind, fitting;
  bool inner = false;
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
  std::string ratioPart;                   // docs/162: a code of floorRatioParts(), or empty
  std::string daylightKind;  // docs/203: dwelling / other / none / empty (Modeler::setRoomDaylight)
  std::string ventilation;   // window / equipment / empty
};

struct RoomSchedule {
  std::vector<RoomRow> rows;
  double centreTotal = 0.0, innerTotal = 0.0;  // of the counted rooms (inner: of those whose inner area is known)
  int innerMissing = 0;  // counted rooms whose inner area is not evaluated: innerTotal is not the whole
  int notCounted = 0;
};
RoomSchedule roomSchedule(const Document& document);

struct RoomOpeningInput {
  RoomRow room;
  std::vector<std::size_t> openingIndices;  // in RoomOpeningInputs::openings; provisional geometry match
  // docs/203: from the candidates and what was given for them. Areas in mm2.
  double daylightArea = 0.0;      // Σ width × height × daylight factor
  int factorsMissing = 0;         // candidates with no daylight factor given
  double ventilationArea = 0.0;   // Σ areas open for ventilation
  int ventilationMissing = 0;     // candidates with no such area given
};

struct RoomOpeningInputs {
  std::vector<RoomOpeningInput> rooms;
  std::vector<OpeningRow> openings;
  std::vector<std::size_t> unmatched;
};
RoomOpeningInputs roomOpeningInputs(const Document& document);

// docs/203: a room's daylight and ventilation against 法 28 条 1・2 項 and 令 19 条 3 項 (住宅の居住
// のための居室 1/7; ventilation 1/20), read on e-Gov 2026-11-20, from the factors and areas a person
// gave for its candidate openings. state: 参考値内 / 参考値を外れる / 未評価 / 未入力 / 対象外 /
// 換気設備. A comparison, never a pass; the floor area is the room's within the wall lines (壁芯).
struct RoomDaylightCheck {
  std::string daylightState, daylightText;
  std::string ventilationState, ventilationText;
};
RoomDaylightCheck roomDaylightCheck(const RoomOpeningInput& input);

// docs/205: the daylight factor (採光補正係数, 令 20 条 2 項 1-3 号) of each wall opening worked out
// from the model: outwards is the side where a level ray from its centre meets no other wall; the
// horizontal distance runs from each part of the building over the opening to the site boundary it
// faces (across the road to its far side); the vertical one from that part's top to the opening's
// centre; the smallest ratio is used, and the zone's formula with its floors and cap. Skylights,
// verandas, parks and rivers, and canopies that do not hinder daylight are not looked at. A value
// to check and give (set_opening_daylight), not a pass. factor below 0: see problem.
struct DaylightFactorEstimate {
  ObjectId host;
  std::string openingId, mark;
  std::size_t index = 0;
  double horizontal = 0.0, vertical = 0.0, ratio = 0.0;  // mm, mm
  double factor = -1.0;
  bool facesRoad = false;
  std::string over;   // the part of the building that sets the ratio
  std::string basis;  // how it was worked out, for set_opening_daylight's basis
  std::string problem;
};
std::vector<DaylightFactorEstimate> daylightFactorEstimates(const Document& document);
class Modeler;
// Gives the openings (`openings`: opening ids, all that can be worked out when empty) the factors
// worked out, with their basis, in one undo step; their areas for ventilation stay. Returns how
// many were given (0: nothing changed).
Result<int> applyDaylightFactors(Modeler& modeler, const std::vector<std::string>& openings);

// docs/194: rows of the permit workflow (申請図の工程) read from the model as it is. A row says what
// was found and what is missing; never a legal judgement. state: 未入力 / 入力済み / 要確認 / 要修正 /
// 階待ち / 通り芯待ち. `targets`: the objects the row is about (to select them).
struct PermitCheck {
  std::string item, state, details;
  std::vector<ObjectId> targets;
  bool attention = false;
};
// Stage 2 (通り芯): levels, grids, walls on levels and on the grid, columns and beams on the grid.
std::vector<PermitCheck> permitGridChecks(const Document& document);
// Stage 3 (平面): per storey the enclosed regions and rooms, rooms that do not close or share,
// walls reshaped by hand, openings without a fitting.
std::vector<PermitCheck> permitPlanChecks(const Document& document);
// Stage 5 (階段, docs/197): each stair made by the stair tool compared with the sizes for a
// house's own stair (令 23-25, read on e-Gov 2026-11-20: 令和8年11月1日施行版), the basis, and
// what is not compared. A comparison, never a pass.
std::vector<PermitCheck> permitStairChecks(const Document& document);
// Stage 6 (屋根・庇・バルコニー, docs/198): each roof's shape, pitch, eaves and the walls standing up
// to it; canopies and balconies and how they count; the building area as worked out (docs/141),
// and what is not looked at.
std::vector<PermitCheck> permitRoofChecks(const Document& document);
// Stage 7 (耐力壁, docs/199): the timber wall-quantity comparison and quarter-division balance per
// storey and direction as construction::checkWallQuantity works them out (its pinned table and
// date), the walls counted, and what it does not assess. A comparison, never a pass.
std::vector<PermitCheck> permitBraceChecks(const Document& document);
// Stage 8 (基礎, docs/200): the foundation plans made from the sills and whether they are current,
// the sills' and columns' bases found on supports (geometry only), and what is not assessed.
std::vector<PermitCheck> permitFoundationChecks(const Document& document);
// Stage 9 (省エネ・設備, docs/201): what the model has for the envelope (wall build-ups and whether
// one of their layers is insulation, typed floors and roofs, openings) and what it does not have
// yet (the route and region, performance values, equipment): mostly 未入力, never a pass.
std::vector<PermitCheck> permitEnergyChecks(const Document& document);
// Stage 10 (立面・断面, docs/201): the level GL, the model's highest point above it (not the legal
// height), where sections are cut, the elevations' orientation, and what is not checked.
std::vector<PermitCheck> permitElevationChecks(const Document& document);
// Stage 11 (一式の突合せ, docs/202): per stage read from the model (2, 3, 5-10) how many rows are
// left to look at, marks given by hand to unlike openings, rooms of a storey with one name.
std::vector<PermitCheck> permitSetChecks(const Document& document);
// Stage 12 (申請用出力, docs/202): the permit output stays on hold (with the stages left); study
// drawings can always be made.
std::vector<PermitCheck> permitOutputChecks(const Document& document);

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
  // 床面積 (令 2 条 1 項 3 号, docs/141): within the wall lines, where there is a floor (the slabs of
  // this storey and the stairs arriving at it); every enclosed part when the storey has no slab.
  bool legalKnown = false;
  double legalFloor = 0.0;  // mm2
  // docs/166: a basement (令 1 条 2 号: its floor below the ground, the ground 1/3 or more of its
  // ceiling height above the floor), the ground being the level named GL. `ceiling`: the rooms'
  // ceiling heights averaged by area when all have one, else the height up to the next level.
  bool basement = false;
  double ceiling = 0.0;           // mm, 0: not known
  bool ceilingFromRooms = false;
  double top = 0.0;               // z of the next level (the top of the storey), when there is one
  bool hasTop = false;
};

struct AreaTable {
  std::vector<AreaRow> rows;  // bottom to top
  double enclosedTotal = 0.0, roomCentreTotal = 0.0, slabTotal = 0.0;
  // 延べ面積 (令 2 条 1 項 4 号): the floor areas added (the exclusions for the floor area ratio not
  // looked at). 建築面積 (同 2 号): the plan of every storey within the wall lines, and the eaves
  // and the like beyond 1 m from them (back 1 m from their ends). docs/141.
  double legalTotal = 0.0;
  bool buildingKnown = false;
  double buildingArea = 0.0;
  std::vector<std::string> legalNotes;  // what was assumed and what is not evaluated, with the basis
  // docs/162: the total the floor area ratio (容積率) is worked out from: legalTotal less the rooms
  // marked as parts that are left out, each kind up to its share of legalTotal.
  struct RatioPart {
    std::string code, label, basis;
    int rooms = 0;
    double area = 0.0;      // mm2 of the rooms marked so (within the wall lines)
    double limit = -1.0;    // mm2 at most left out; below 0: no limit
    double excluded = 0.0;  // mm2 left out
  };
  // docs/163: balconies (kind = balcony). Counted as floor area only where the person said so.
  int balconies = 0, balconiesCounted = 0, balconiesExcluded = 0;
  int balconiesByNotice = 0;      // docs/175: worked out by the notice (floor area left to it)
  double balconyFloorArea = 0.0;  // mm2 of balconies counted as floor area
  double balconyArea = 0.0;  // mm2 in plan of all of them (width x depth)
  // docs/166: the ground (the level named GL), when there is one.
  bool groundKnown = false;
  double ground = 0.0;
  std::vector<RatioPart> ratioParts;  // the kinds that have rooms
  bool ratioKnown = false;            // false: a floor area or a marked room cannot be read
  double ratioTotal = 0.0;            // mm2
};
// Areas measured from the model, and the floor and building areas by the Building Standards Act
// Enforcement Order art. 2 (1) ii-iv as read on e-Gov (version in force 2026-11-01) — docs/141 for
// what is and is not looked at.
AreaTable areaTable(const Document& document);

// What a room's tag says on screen and on drawings: its name and its area within the wall
// lines (壁芯) as the walls are now; a room no longer enclosed says so. The unit is written
// "m²": the view's font has no glyph for the one-character square metre sign.
std::string roomTagText(const RoomRow& room);

// The tables for a sheet of their own (docs/113): 0 the door and window schedule (one line per
// mark), 1 the room schedule, 2 the area table, 3 the finish schedule (docs/115), 4 the types of
// floors and roofs (docs/136: one line per type name and part, its count and sizes), 5 the
// quantities of the layers (docs/172), 6 daylight and ventilation of the rooms given a kind
// (docs/207), 7 the wall quantity table, 8 the list of bracing walls, 9 the quarter-division side
// parts (docs/208, from check_wall_quantity), 10 the outline of the project (docs/209: site, zone,
// road, areas and ratios for reference, storeys, highest point, structure); the same values and
// notes as the CSV.
draw::Table scheduleTable(const Document& document, int which);

// The quantities of the layers of walls, floors and roofs (docs/172): per part and layer name,
// how many elements have it, its area (each element's volume over its thickness: openings out,
// a roof's along its slope) and its volume (that area times the layer's thickness).
struct LayerQuantityRow {
  std::string part;   // 壁 / 床 / 屋根
  std::string name;   // the layer's name
  double thickness = 0.0;  // mm (the same name may come with several: one row each)
  int elements = 0;
  double area = 0.0;    // mm2
  double volume = 0.0;  // mm3
};
struct LayerQuantities {
  std::vector<LayerQuantityRow> rows;
  int withoutLayers = 0;  // walls / floors / roofs with no layers, or layers that no longer add up
  std::vector<std::string> notes;
};
LayerQuantities layerQuantities(const Document& document);
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
