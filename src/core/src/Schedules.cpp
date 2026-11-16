#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Trsf.hxx>
#include <nlohmann/json.hpp>

#include "sbr/OpeningSymbols.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
#include "sbr/geom/Stair.h"

namespace sbr::doc {
namespace {

std::string attribute(const Object& o, const char* key) {
  const auto it = o.attributes.find(key);
  return it == o.attributes.end() ? std::string() : it->second;
}

std::string levelName(const Document& document, const std::string& id) {
  for (const FloorLevel& l : document.settings().levels)
    if (l.id == id) return l.name;
  return {};
}

// The name people know a fitting by (OpeningSymbols.h), or its type when it is not on the list.
std::string fittingName(const nlohmann::json& fitting) {
  // The choices read once (docs/118: read again for every opening, they were most of the time).
  static const std::vector<std::pair<nlohmann::json, std::string>> choices = [] {
    std::vector<std::pair<nlohmann::json, std::string>> out;
    for (const draw::FittingChoice& c : draw::fittingChoices()) {
      nlohmann::json choice = nlohmann::json::parse(c.fitting, nullptr, false);
      if (!choice.is_discarded()) out.emplace_back(std::move(choice), c.label);
    }
    return out;
  }();
  for (const auto& [choice, label] : choices) {
    bool same = true;
    for (const auto& [key, value] : choice.items()) same &= fitting.contains(key) && fitting.at(key) == value;
    if (same) return label;
  }
  return fitting.value("type", std::string());
}

long tenths(double v) { return std::lround(v * 10.0); }

std::string number(double v, int decimals) {
  char text[48];
  std::snprintf(text, sizeof text, "%.*f", decimals, v);
  return text;
}

std::string cell(const std::string& text) {
  if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
  std::string quoted = "\"";
  for (char c : text) {
    if (c == '"') quoted += '"';
    quoted += c;
  }
  return quoted + "\"";
}

struct Csv {
  std::ostringstream out;
  Csv() { out << "\xef\xbb\xbf"; }
  void line(const std::vector<std::string>& cells) {
    for (std::size_t i = 0; i < cells.size(); ++i) out << (i ? "," : "") << cell(cells[i]);
    out << "\r\n";
  }
};

std::string kindName(const std::string& kind) { return kind == "window" ? "窓" : kind == "door" ? "扉" : "開口"; }
}  // namespace

OpeningSchedule openingSchedule(const Document& document) {
  OpeningSchedule out;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "wall" || o.shape.IsNull()) continue;
    const auto openings = Modeler::openingsIn(document, o.id);
    if (!openings) continue;
    nlohmann::json fittings = nlohmann::json::object();
    if (const std::string stored = attribute(o, "opening_fittings"); !stored.empty()) {
      const nlohmann::json parsed = nlohmann::json::parse(stored, nullptr, false);
      if (parsed.is_object()) fittings = parsed;
    }
    nlohmann::json own = nlohmann::json::object();
    if (const std::string stored = attribute(o, "opening_marks"); !stored.empty()) {
      const nlohmann::json parsed = nlohmann::json::parse(stored, nullptr, false);
      if (parsed.is_object()) own = parsed;
    }
    for (const Modeler::OpeningInfo& op : *openings) {
      if (std::abs(op.normal.Z()) > 1e-6 || op.depth < 1.0) continue;
      OpeningRow row;
      const auto fitting = fittings.find(op.id);
      const std::string type = fitting != fittings.end() && fitting->is_object() ? fitting->value("type", std::string()) : std::string();
      if (type.empty() || type == "none") {
        row.kind = "opening";
      } else {
        row.kind = type != "window" && op.sill < 1.0 ? "door" : "window";
        row.fitting = fittingName(*fitting);
      }
      if (const auto given = own.find(op.id); given != own.end() && given->is_object()) {
        row.mark = given->value("mark", std::string());
        row.ownMark = !row.mark.empty();
        row.product = given->value("product", std::string());
      }
      row.width = op.width;
      row.height = op.height;
      row.sill = op.sill;
      row.host = o.id;
      row.hostName = o.name;
      row.openingId = op.id;
      row.index = op.index;
      row.level = levelName(document, attribute(o, "base_level"));
      row.center = op.center;
      row.bottom = op.bottom;
      row.normal = op.normal;
      row.depth = op.depth;
      out.rows.push_back(std::move(row));
    }
  }
  // Marks: one per kind, size and fitting; numbered in a fixed order so that the same model
  // always gives the same marks.
  using Key = std::tuple<int, long, long, std::string>;
  auto keyOf = [](const OpeningRow& r) {
    return Key{r.kind == "window" ? 0 : r.kind == "door" ? 1 : 2, tenths(r.width), tenths(r.height), r.fitting};
  };
  auto sill = [](OpeningTypeRow& t, double value) {
    if (std::none_of(t.sills.begin(), t.sills.end(), [&](double s) { return tenths(s) == tenths(value); })) t.sills.push_back(value);
  };
  // Marks given by hand first: one line per mark, whatever the openings are.
  std::map<std::string, OpeningTypeRow> given;
  for (const OpeningRow& r : out.rows) {
    if (!r.ownMark) continue;
    const bool first = !given.count(r.mark);
    OpeningTypeRow& t = given[r.mark];
    if (first) {
      t.mark = r.mark;
      t.ownMark = true;
      t.kind = r.kind;
      t.fitting = r.fitting;
      t.width = r.width;
      t.height = r.height;
      t.product = r.product;
    } else if (t.kind != r.kind || t.fitting != r.fitting || tenths(t.width) != tenths(r.width) ||
               tenths(t.height) != tenths(r.height) || t.product != r.product) {
      t.mixed = true;
    }
    ++t.count;
    sill(t, r.sill);
  }
  // The others: worked out, skipping numbers taken by hand.
  std::map<Key, OpeningTypeRow> types;
  for (const OpeningRow& r : out.rows) {
    if (r.ownMark) continue;
    OpeningTypeRow& t = types[keyOf(r)];
    t.kind = r.kind;
    t.fitting = r.fitting;
    t.width = r.width;
    t.height = r.height;
    ++t.count;
    sill(t, r.sill);
  }
  int numbers[3] = {0, 0, 0};
  std::map<Key, std::string> marks;
  std::vector<OpeningTypeRow> lines;
  for (auto& [key, t] : types) {
    const int k = std::get<0>(key);
    do t.mark = std::string(k == 0 ? "W" : k == 1 ? "D" : "O") + std::to_string(++numbers[k]);
    while (given.count(t.mark));
    marks[key] = t.mark;
    lines.push_back(t);
  }
  for (auto& [mark, t] : given) lines.push_back(t);
  auto order = [](const OpeningTypeRow& t) { return t.kind == "window" ? 0 : t.kind == "door" ? 1 : 2; };
  std::stable_sort(lines.begin(), lines.end(), [&](const OpeningTypeRow& a, const OpeningTypeRow& b) { return order(a) < order(b); });
  for (OpeningTypeRow& t : lines) {
    std::sort(t.sills.begin(), t.sills.end());
    out.types.push_back(std::move(t));
  }
  for (OpeningRow& r : out.rows)
    if (!r.ownMark) r.mark = marks[keyOf(r)];
  return out;
}

RoomSchedule roomSchedule(const Document& document) {
  RoomSchedule out;
  for (const RoomInfo& r : Modeler::roomsIn(document)) {
    RoomRow row;
    row.id = r.id;
    row.name = r.name;
    row.level = levelName(document, r.level);
    row.closed = r.closed;
    row.counted = r.closed && r.problem.empty();
    row.problem = r.problem;
    row.centreArea = r.centreArea;
    row.innerKnown = r.innerKnown;
    row.innerArea = r.innerArea;
    row.innerPerimeter = r.innerPerimeter;
    row.at = r.at;
    if (const Object* marker = document.find(r.id)) {
      const RoomFinish finish = roomFinishOf(*marker);
      row.use = finish.use;
      row.floor = finish.floor;
      row.wall = finish.wall;
      row.ceiling = finish.ceiling;
      row.ceilingHeight = finish.ceilingHeight;
    }
    if (row.counted) {
      out.centreTotal += r.centreArea;
      if (r.innerKnown) out.innerTotal += r.innerArea;
      else ++out.innerMissing;
    } else {
      ++out.notCounted;
    }
    out.rows.push_back(std::move(row));
  }
  return out;
}

namespace {
// docs/141: areas in plan as 1 mm thick slices (z 0 to 1), so that unions and overlaps are solids
// and their volume is the area (mm2).
std::optional<TopoDS_Shape> slice(const std::vector<gp_Pnt>& outline) {
  if (outline.size() < 3) return std::nullopt;
  BRepBuilderAPI_MakePolygon polygon;
  for (const gp_Pnt& p : outline) polygon.Add(gp_Pnt(p.X(), p.Y(), 0.0));
  polygon.Close();
  if (!polygon.IsDone()) return std::nullopt;
  BRepBuilderAPI_MakeFace face(polygon.Wire(), Standard_True);
  if (!face.IsDone()) return std::nullopt;
  BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(0, 0, 1));
  if (!prism.IsDone()) return std::nullopt;
  return prism.Shape();
}

// A solid's plan at height z, as such a slice.
std::optional<TopoDS_Shape> sliceOf(const TopoDS_Shape& solid, double z) {
  const Bnd_Box box = geom::exactBounds(solid);
  if (box.IsVoid()) return std::nullopt;
  const gp_Pnt lo(box.CornerMin().X() - 10, box.CornerMin().Y() - 10, z - 0.5), hi(box.CornerMax().X() + 10, box.CornerMax().Y() + 10, z + 0.5);
  BRepAlgoAPI_Common cut(solid, BRepPrimAPI_MakeBox(lo, hi).Shape());
  if (!cut.IsDone() || cut.HasErrors()) return std::nullopt;
  gp_Trsf down;
  down.SetTranslation(gp_Vec(0, 0, -(z - 0.5)));
  const TopoDS_Shape moved = BRepBuilderAPI_Transform(cut.Shape(), down, Standard_True).Shape();
  if (!TopExp_Explorer(moved, TopAbs_SOLID).More()) return std::nullopt;
  return moved;
}

std::optional<TopoDS_Shape> joined(const std::vector<TopoDS_Shape>& parts) {
  if (parts.empty()) return std::nullopt;
  if (parts.size() == 1) return parts.front();
  const auto all = geom::booleanCombine(geom::BooleanOp::Union, parts);
  if (!all) return std::nullopt;
  return all.value();
}

double areaOf(const std::optional<TopoDS_Shape>& slice) { return slice ? geom::volume(*slice) : 0.0; }

// The outline with each side moved out by its offset (corners where the moved sides meet).
std::vector<gp_Pnt> grownOutline(std::vector<gp_Pnt> p, std::vector<double> offsets) {
  double twice = 0.0;
  for (std::size_t i = 0; i < p.size(); ++i) twice += p[i].X() * p[(i + 1) % p.size()].Y() - p[(i + 1) % p.size()].X() * p[i].Y();
  if (twice < 0) {
    std::reverse(p.begin(), p.end());
    std::vector<double> o(offsets.size());
    for (std::size_t k = 0; k < offsets.size(); ++k) o[k] = offsets[(2 * offsets.size() - 2 - k) % offsets.size()];
    offsets = o;
  }
  const std::size_t n = p.size();
  std::vector<gp_Pnt> out;
  for (std::size_t i = 0; i < n; ++i) {
    auto line = [&](std::size_t k) {
      const gp_Vec d = gp_Vec(p[k], p[(k + 1) % n]).Normalized();
      return std::make_pair(p[k].Translated(gp_Vec(d.Y(), -d.X(), 0) * offsets[k]), d);
    };
    const auto [a, da] = line((i + n - 1) % n);
    const auto [b, db] = line(i);
    const double den = da.X() * db.Y() - da.Y() * db.X();
    if (std::abs(den) < 1e-9) {
      out.push_back(b);
      continue;
    }
    const gp_Vec w(a, b);
    out.push_back(a.Translated(da * ((w.X() * db.Y() - w.Y() * db.X()) / den)));
  }
  return out;
}
}  // namespace

AreaTable areaTable(const Document& document) {
  AreaTable out;
  const RoomSchedule rooms = roomSchedule(document);
  std::vector<FloorLevel> levels = document.settings().levels;
  std::stable_sort(levels.begin(), levels.end(), [](const FloorLevel& a, const FloorLevel& b) { return a.elevation < b.elevation; });
  auto addRooms = [&](AreaRow& row, const std::string& level) {
    for (const RoomRow& r : rooms.rows) {
      if (!r.counted || r.level != level) continue;
      ++row.rooms;
      row.roomCentre += r.centreArea;
      if (r.innerKnown) row.roomInner += r.innerArea;
      else ++row.innerMissing;
    }
  };
  // Two levels at one height (GL and 1FL) are one storey: the one named "...FL", else the first.
  auto isFloor = [](const FloorLevel& l) { return l.name.size() >= 2 && l.name.compare(l.name.size() - 2, 2, "FL") == 0; };
  auto storeyOf = [&](const FloorLevel& level) -> const FloorLevel& {
    const FloorLevel* chosen = nullptr;
    for (const FloorLevel& l : levels) {
      if (std::abs(l.elevation - level.elevation) > 1e-6) continue;
      if (!chosen || (isFloor(l) && !isFloor(*chosen))) chosen = &l;
    }
    return *chosen;
  };
  std::vector<TopoDS_Shape> storeyPlans;  // within the wall lines, per storey (for the building area)
  std::vector<std::string> noSlab, voids, noWalls;
  bool stairsArriving = false;
  for (const FloorLevel& level : levels) {
    if (&storeyOf(level) != &level) continue;
    AreaRow row;
    row.level = level.name;
    row.elevation = level.elevation;
    const Modeler::EnclosedArea enclosed = Modeler::enclosedAt(document, level.elevation);
    row.enclosedKnown = true;
    row.enclosedArea = enclosed.area;
    row.regions = enclosed.regions;
    row.unreadableWalls = enclosed.unreadableWalls;
    addRooms(row, level.name);
    for (const Object& o : document.objects()) {
      if (attribute(o, "kind") != "slab" || o.shape.IsNull()) continue;
      const std::string top = attribute(o, "top_level");
      bool here = false;
      for (const FloorLevel& l : levels) here |= l.id == top && &storeyOf(l) == &level;
      if (!here) continue;
      const Bnd_Box box = geom::exactBounds(o.shape);
      if (box.IsVoid()) continue;
      const double thickness = box.CornerMax().Z() - box.CornerMin().Z();
      if (thickness < 1e-6) continue;
      row.slabArea += geom::volume(o.shape) / thickness;
      ++row.slabs;
    }
    if (row.regions == 0 && row.rooms == 0 && row.slabs == 0 && row.unreadableWalls == 0) continue;
    row.regionsWithoutRoom = std::max(0, row.regions - row.rooms);
    // 床面積 (令 2 条 1 項 3 号, docs/141): within the wall lines, where there is a floor.
    std::vector<TopoDS_Shape> enclosedParts;
    for (const auto& outline : Modeler::enclosedOutlines(document, level.elevation))
      if (const auto s = slice(outline)) enclosedParts.push_back(*s);
    const auto enclosedSlice = joined(enclosedParts);
    if (enclosedSlice) storeyPlans.push_back(*enclosedSlice);
    std::vector<TopoDS_Shape> floorParts;
    for (const Object& o : document.objects()) {
      const std::string kind = attribute(o, "kind");
      if (kind == "slab" && !o.shape.IsNull()) {
        const std::string top = attribute(o, "top_level");
        bool here = false;
        for (const FloorLevel& l : levels) here |= l.id == top && &storeyOf(l) == &level;
        if (!here) continue;
        const TopoDS_Shape world = document.worldShape(o);
        const Bnd_Box box = geom::exactBounds(world);
        if (box.IsVoid()) continue;
        if (const auto s = sliceOf(world, (box.CornerMin().Z() + box.CornerMax().Z()) / 2)) floorParts.push_back(*s);
      } else if (kind == "stair") {
        // A stair arriving at this floor: its plan counts here too (the opening it needs is floor).
        const auto spec = stairInWorld(document, o);
        if (!spec || std::abs(spec->base + spec->rise - level.elevation) > 1.0) continue;
        if (const auto made = geom::makeStair(*spec))
          for (const auto& t : made->treads)
            if (const auto s = slice(t.outline)) floorParts.push_back(*s);
        stairsArriving = true;
      }
    }
    if (enclosedSlice) {
      row.legalKnown = true;
      if (row.slabs == 0) {
        row.legalFloor = areaOf(enclosedSlice);
        noSlab.push_back(level.name);
      } else if (const auto floor = joined(floorParts)) {
        const auto both = geom::booleanCombine(geom::BooleanOp::Intersect, {*enclosedSlice, *floor});
        if (both) row.legalFloor = geom::volume(both.value());
        else row.legalKnown = false;
        if (both && areaOf(enclosedSlice) - row.legalFloor > 1e4) voids.push_back(level.name);  // more than 0.01 m2 without floor
      }
    } else if (row.unreadableWalls == 0) {
      // No walls enclose anything on this storey (a floor only, a roof deck): no floor area.
      row.legalKnown = true;
      row.legalFloor = 0.0;
      if (row.slabs > 0) noWalls.push_back(level.name);
    }
    if (row.unreadableWalls > 0) row.legalKnown = false;
    out.rows.push_back(std::move(row));
  }
  AreaRow loose;  // rooms whose marker is at no level's height
  addRooms(loose, "");
  if (loose.rooms > 0) out.rows.push_back(std::move(loose));
  bool everyKnown = !out.rows.empty();
  for (const AreaRow& r : out.rows) {
    out.enclosedTotal += r.enclosedArea;
    out.roomCentreTotal += r.roomCentre;
    out.slabTotal += r.slabArea;
    if (!r.enclosedKnown) continue;  // rooms at no level's height: no storey
    out.legalTotal += r.legalFloor;
    everyKnown = everyKnown && r.legalKnown;
  }
  // 建築面積 (令 2 条 1 項 2 号): every storey's plan within the wall lines, and roofs' eaves beyond
  // 1 m from the line they bear on, back 1 m from their ends.
  std::vector<TopoDS_Shape> plan = storeyPlans;
  bool eavesUnknown = false;
  int eaves = 0;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "roof") continue;
    const nlohmann::json spec = nlohmann::json::parse(attribute(o, "roof_spec"), nullptr, false);
    if (!spec.is_object() || !spec.contains("outline")) {
      eavesUnknown = true;
      continue;
    }
    const gp_Trsf world = document.worldTransform(o);
    std::vector<gp_Pnt> outline;
    for (const auto& p : spec.at("outline")) {
      const gp_Pnt q = gp_Pnt(p.at(0).get<double>(), p.at(1).get<double>(), 0.0).Transformed(world);
      if (outline.empty() || outline.back().Distance(q) > 0.01) outline.emplace_back(q.X(), q.Y(), 0.0);
    }
    if (outline.size() > 1 && outline.front().Distance(outline.back()) <= 0.01) outline.pop_back();
    if (outline.size() < 3) continue;
    const std::string kind = spec.value("kind", std::string("gable"));
    const double eave = spec.value("eave", 600.0), verge = spec.value("gable_eave", 300.0);
    if (std::max(eave, verge) < 1000.0) continue;  // nothing 1 m out or more
    const std::size_t n = outline.size();
    auto dir = [&](std::size_t i) { return gp_Vec(outline[i], outline[(i + 1) % n]).Normalized(); };
    std::vector<double> overhang(n, eave);
    bool slanted = false;
    for (std::size_t i = 0; i < n; ++i) slanted |= std::abs(dir(i).X()) > 1e-6 && std::abs(dir(i).Y()) > 1e-6 &&
                                                   std::abs(dir(i).Dot(dir(0))) > 1e-6 && std::abs(dir(i).Dot(dir(0))) < 1 - 1e-6;
    if (kind == "shed") {
      gp_Vec fall(spec.value("fall", nlohmann::json::array({0.0, -1.0}))[0].get<double>(),
                  spec.value("fall", nlohmann::json::array({0.0, -1.0}))[1].get<double>(), 0);
      fall.Transform(world);
      if (fall.Magnitude() < 1e-9) {
        eavesUnknown = true;
        continue;
      }
      fall.Normalize();
      for (std::size_t i = 0; i < n; ++i) overhang[i] = std::abs(gp_Vec(dir(i).Y(), -dir(i).X(), 0).Dot(fall)) >= 0.5 ? eave : verge;
    } else if (kind == "gable") {
      const std::vector<int> sides = spec.value("gable_sides", std::vector<int>{});
      if (!sides.empty()) {
        for (const int g : sides)
          if (g >= 0 && g < static_cast<int>(n)) overhang[static_cast<std::size_t>(g)] = verge;
      } else if (n == 4 && !slanted) {
        // One rectangle: the gables at the ends of the ridge (along the longer side, or the shorter one).
        const double a = outline[0].Distance(outline[1]), b = outline[1].Distance(outline[2]);
        const bool ridgeShort = spec.value("ridge", std::string("long")) == "short";
        const bool firstAlong = (a >= b) != ridgeShort;  // side 0 runs along the ridge
        for (std::size_t i = 0; i < n; ++i) overhang[i] = (i % 2 == 0) == firstAlong ? eave : verge;
      } else {
        eavesUnknown = true;  // which sides end in gables is the roof's working-out: not read here
        continue;
      }
    }
    std::vector<double> beyond(n);
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
      beyond[i] = std::max(0.0, overhang[i] - 1000.0);
      any |= beyond[i] > 0.0;
    }
    if (!any) continue;
    if (const auto s = slice(grownOutline(outline, beyond))) {
      plan.push_back(*s);
      ++eaves;
    }
  }
  if (const auto all = joined(plan)) {
    out.buildingArea = areaOf(all);
    out.buildingKnown = !eavesUnknown;
  }
  // What was taken as given, and what was not looked at.
  out.legalNotes.push_back("根拠：建築基準法施行令 第 2 条 第 1 項 第 2 号（建築面積）・第 3 号（床面積）・第 4 号（延べ面積）、e-Gov 法令検索で 2026-11-16 に読んだ版（令和 8 年 11 月 1 日施行）。");
  out.legalNotes.push_back("床面積：壁（壁ツールの壁）の中心線で囲まれた所のうち、床（その階のスラブ）のある所。吹抜けは入れない。階段は、着く上の階でも入れた（一般的な扱い。特定行政庁の取扱いを確かめてください）。");
  if (!noSlab.empty()) {
    std::string names;
    for (const auto& n : noSlab) names += (names.empty() ? "" : "・") + n;
    out.legalNotes.push_back(names + "：床（スラブ）が無いので、壁で囲まれた所を全部床として数えた。");
  }
  if (!noWalls.empty()) {
    std::string names;
    for (const auto& n : noWalls) names += (names.empty() ? "" : "・") + n;
    out.legalNotes.push_back(names + "：床（スラブ）はあるが、壁で囲まれた所が無いので、床面積は 0 とした（屋上・バルコニーなどなら、その扱いは未評価）。");
  }
  if (!voids.empty()) {
    std::string names;
    for (const auto& n : voids) names += (names.empty() ? "" : "・") + n;
    out.legalNotes.push_back(names + "：壁の中に床の無い所（吹抜けなど）があり、入れていない。");
  }
  out.legalNotes.push_back("未評価：壁で囲まれていない所（バルコニー・ポーチ・ピロティなど、開放性による扱い）、柱だけで囲まれた部分、地階（地盤面上 1 m 以下の部分の建築面積）、特例軒等、高い開放性の指定（同 2 号ただし書）。");
  out.legalNotes.push_back("延べ面積は各階の床面積の合計。容積率の算定で入れない部分（自動車車庫等 1/5 など、同 4 号ただし書・第 3 項）は見ていない（未評価）。");
  if (eaves > 0) out.legalNotes.push_back("建築面積：1 m 以上出た軒は、その先から 1 m 下がった線まで入れた（屋根を載せた線から測った）。");
  if (eavesUnknown) out.legalNotes.push_back("建築面積は未評価：1 m 以上出た軒のどこが妻かを読めない屋根がある（L 字などの切妻）。");
  if (!everyKnown) out.legalNotes.push_back("床面積を出せない階がある（形を直接変えて壁として読めない壁がある）：その階と延べ面積は未評価。");
  (void)stairsArriving;
  return out;
}

std::string roomTagText(const RoomRow& room) {
  if (!room.closed) return room.name + "（閉じていない）";
  if (!room.counted) return room.name;
  return room.name + "  " + number(room.centreArea / 1e6, 2) + " m²";
}

bool tableHasContent(const draw::Table& table, int which) {
  if (table.rows.empty() || (which == 0 && table.rows.size() <= 1)) return false;  // the openings' total only
  if (which == 3) {
    bool any = false;
    for (const auto& row : table.rows)
      for (std::size_t c = 2; c < row.size(); ++c) any |= !row[c].empty();
    return any;  // no finishes given: nothing
  }
  return true;
}

draw::Table scheduleTable(const Document& document, int which) {
  draw::Table t;
  if (which == 0) {
    const OpeningSchedule s = openingSchedule(document);
    t.title = "建具表";
    t.headers = {"符号", "種類", "建具", "幅", "高さ", "数", "腰高", "品番", "注"};
    int total = 0;
    for (const OpeningTypeRow& r : s.types) {
      std::string sills;
      for (double v : r.sills) sills += (sills.empty() ? "" : " / ") + number(v, 0);
      t.rows.push_back({r.mark, kindName(r.kind), r.fitting, number(r.width, 0), number(r.height, 0), std::to_string(r.count), sills,
                        r.product, r.mixed ? "大きさ・建具・品番の違う開口が混ざっています" : ""});
      total += r.count;
    }
    t.rows.push_back({"合計", "", "", "", "", std::to_string(total), "", "", ""});
    t.notes = {"寸法は mm。符号は、手で決めたものはそのまま、ほかは種類・建具・幅・高さが同じ開口に同じものを付けています。",
               "平面図の開口の横の符号と同じです。モデルから読んだ今の値です。"};
  } else if (which == 1) {
    const RoomSchedule s = roomSchedule(document);
    t.title = "室の表";
    t.headers = {"室名", "階", "壁芯 (m2)", "内法 (m2)", "内法の周長 (m)", "状態"};
    for (const RoomRow& r : s.rows)
      t.rows.push_back({r.name, r.level, r.closed ? number(r.centreArea / 1e6, 2) : "",
                        r.closed ? (r.innerKnown ? number(r.innerArea / 1e6, 2) : "未評価") : "",
                        r.closed && r.innerKnown ? number(r.innerPerimeter / 1e3, 2) : "",
                        r.counted ? "閉じている" : !r.closed ? "閉じていない" : "ほかの室と同じ場所"});
    t.rows.push_back({"合計（閉じている室だけ）", "", number(s.centreTotal / 1e6, 2), s.innerMissing == 0 ? number(s.innerTotal / 1e6, 2) : "未評価", "", ""});
    t.notes = {"壁芯＝壁を描いた線の内側、内法＝壁の面の内側。法の床面積（算入・不算入の決まり）ではありません。"};
  } else if (which == 4) {
    // Types of floors and roofs (docs/134, docs/136): read from the objects as they are now.
    t.title = "種類の表";
    t.headers = {"種類名", "部位", "数", "厚さ", "勾配", "軒の出", "けらばの出", "注"};
    struct Values {
      int count = 0;
      bool mixed = false;
      std::vector<double> v;  // thickness[, pitch, eave, gable eave]
    };
    std::map<std::pair<int, std::string>, Values> types;  // (0 floor / 1 roof, name)
    int unnamed = 0;
    for (const Object& o : document.objects()) {
      const auto kind = o.attributes.find("kind");
      if (kind == o.attributes.end() || (kind->second != "slab" && kind->second != "roof")) continue;
      const auto name = o.attributes.find("type_name");
      if (name == o.attributes.end() || name->second.empty()) {
        ++unnamed;
        continue;
      }
      std::vector<double> v;
      if (kind->second == "slab") {
        const Bnd_Box box = document.worldBounds(o);
        if (box.IsVoid()) continue;
        v = {box.CornerMax().Z() - box.CornerMin().Z()};
      } else {
        const auto spec = o.attributes.find("roof_spec");
        const nlohmann::json j = spec == o.attributes.end() ? nlohmann::json() : nlohmann::json::parse(spec->second, nullptr, false);
        if (!j.is_object()) continue;
        v = {j.value("thickness", 0.0), j.value("pitch", 0.0), j.value("eave", 0.0), j.value("gable_eave", 0.0)};
      }
      Values& at = types[{kind->second == "slab" ? 0 : 1, name->second}];
      if (at.count == 0) at.v = v;
      else
        for (std::size_t i = 0; i < v.size() && i < at.v.size(); ++i) at.mixed |= std::abs(v[i] - at.v[i]) > 0.5;
      ++at.count;
    }
    for (const auto& [key, at] : types) {
      const bool roof = key.first == 1;
      t.rows.push_back({key.second, roof ? "屋根" : "床", std::to_string(at.count), number(at.v[0], 0),
                        roof ? number(at.v[1], 1) + " 寸" : "", roof ? number(at.v[2], 0) : "", roof ? number(at.v[3], 0) : "",
                        at.mixed ? "値の違う物が混ざっています（1 つ目の値）" : ""});
    }
    t.notes = {"床（スラブ）・屋根に付けた種類名ごとです。寸法は mm。同じ種類名の物は「同じ種類の…にする」でそろえられます。"};
    if (unnamed > 0) t.notes.push_back("種類名の無い床・屋根が " + std::to_string(unnamed) + " 個あります（この表には入っていません）。");
  } else if (which == 3) {
    const RoomSchedule s = roomSchedule(document);
    t.title = "仕上げ表";
    t.headers = {"室名", "階", "用途", "床", "壁", "天井", "天井高"};
    for (const RoomRow& r : s.rows)
      t.rows.push_back({r.name, r.level, r.use, r.floor, r.wall, r.ceiling, r.ceilingHeight > 0 ? number(r.ceilingHeight, 0) : ""});
    t.notes = {"室ごとに入れた用途・仕上げ・天井高です（空欄：まだ入れていない）。天井高は床から mm。"};
  } else {
    const AreaTable a = areaTable(document);
    t.title = "面積の表";
    t.headers = {"階", "壁で囲まれた面積・壁芯 (m2)", "室の数", "室のない閉じた所", "室の合計・壁芯 (m2)", "室の合計・内法 (m2)",
                 "床スラブ (m2)", "法の床面積"};
    for (const AreaRow& r : a.rows)
      t.rows.push_back({r.level.empty() ? "（レベルなし）" : r.level, r.enclosedKnown ? number(r.enclosedArea / 1e6, 2) : "",
                        std::to_string(r.rooms), r.enclosedKnown ? std::to_string(r.regionsWithoutRoom) : "",
                        number(r.roomCentre / 1e6, 2), r.innerMissing == 0 ? number(r.roomInner / 1e6, 2) : "未評価",
                        number(r.slabArea / 1e6, 2), !r.enclosedKnown ? "" : r.legalKnown ? number(r.legalFloor / 1e6, 2) : "未評価"});
    bool allKnown = true;
    for (const AreaRow& r : a.rows) allKnown = allKnown && (!r.enclosedKnown || r.legalKnown);
    t.rows.push_back({"合計（延べ面積）", number(a.enclosedTotal / 1e6, 2), "", "", number(a.roomCentreTotal / 1e6, 2), "", number(a.slabTotal / 1e6, 2),
                      allKnown ? number(a.legalTotal / 1e6, 2) : "未評価"});
    t.rows.push_back({"建築面積", "", "", "", "", "", "", a.buildingKnown ? number(a.buildingArea / 1e6, 2) : "未評価"});
    t.notes = {"モデルから測った面積です。法の床面積・延べ面積・建築面積は、下の決まりで出しました（確認申請に使う時は、特定行政庁・確認検査機関の取扱いを確かめてください）。"};
    for (const std::string& n : a.legalNotes) t.notes.push_back(n);
  }
  return t;
}

Result<DrawingSet> drawingSet(const Document& document, const DrawingSetSpec& spec) {
  using R = Result<DrawingSet>;
  struct Planned {
    std::string name;
    std::optional<draw::ViewSpec> view;
    std::optional<draw::Table> table;
    std::vector<draw::Table> beside;  // tables on the same sheet (docs/131)
  };
  std::vector<Planned> planned;
  // Plans: one per storey (levels at one height are one storey: the "...FL" one).
  std::vector<FloorLevel> levels = document.settings().levels;
  std::stable_sort(levels.begin(), levels.end(), [](const FloorLevel& a, const FloorLevel& b) { return a.elevation < b.elevation; });
  auto isFloor = [](const FloorLevel& l) { return l.name.size() >= 2 && l.name.compare(l.name.size() - 2, 2, "FL") == 0; };
  for (std::size_t i = 0; i < levels.size(); ++i) {
    bool shadowed = false;
    for (std::size_t j = 0; j < levels.size(); ++j)
      if (j != i && std::abs(levels[j].elevation - levels[i].elevation) < 1e-6 && (isFloor(levels[j]) && !isFloor(levels[i]) || (isFloor(levels[j]) == isFloor(levels[i]) && j < i)))
        shadowed = true;
    if (shadowed) continue;
    draw::ViewSpec view = draw::planView(levels[i].elevation + 1100.0);
    view.depth = 1100.0 + 400.0;  // down to 400 under its floor: the storeys below are left out
    view.title = levels[i].name + " 平面図";
    planned.push_back({view.title, view, std::nullopt, {}});
  }
  if (levels.empty()) {
    draw::ViewSpec view = draw::planView(1100.0);
    view.title = "平面図";
    planned.push_back({view.title, view, std::nullopt, {}});
  }
  // Sections (docs/132).
  struct Cut {
    std::string label, name;
    gp_Pnt point;
    gp_Dir toward;
  };
  std::vector<Cut> cuts;
  if (spec.sections) {
    for (const SectionPlane& sp : document.settings().sections) {
      const gp_Vec n(sp.normal[0], sp.normal[1], 0);
      if (!sp.active || n.Magnitude() < 0.5 * std::hypot(n.Magnitude(), sp.normal[2])) continue;  // vertical planes only
      const std::string label(1, static_cast<char>('A' + std::min<std::size_t>(cuts.size(), 25)));
      cuts.push_back({label, label + "-" + label + " 断面図", gp_Pnt(sp.origin[0], sp.origin[1], sp.origin[2]), gp_Dir(n)});
    }
    if (cuts.empty()) {
      Bnd_Box all;
      for (const Object& o : document.objects())
        if (!o.shape.IsNull() && document.isVisible(o.id)) all.Add(document.worldBounds(o));
      if (!all.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        all.Get(x0, y0, z0, x1, y1, z1);
        const gp_Pnt c((x0 + x1) / 2, (y0 + y1) / 2, 0);
        cuts.push_back({"A", "A-A 断面図", c, gp_Dir(0, -1, 0)});
        cuts.push_back({"B", "B-B 断面図", c, gp_Dir(1, 0, 0)});
      }
    }
  }
  for (const Cut& c : cuts) {
    draw::ViewSpec view = draw::sectionView(c.point, c.toward);
    view.title = c.name;
    planned.push_back({view.title, view, std::nullopt, {}});
  }
  const char* sides[4] = {"南立面図", "東立面図", "北立面図", "西立面図"};
  for (int side = 0; side < 4; ++side) {
    draw::ViewSpec view = draw::elevationView(document, side);
    view.title = sides[side];
    planned.push_back({view.title, view, std::nullopt, {}});
  }
  // The first planned is the lowest storey's plan.
  for (int which : {0, 1, 2, 3, 4}) {
    draw::Table table = scheduleTable(document, which);
    if (!tableHasContent(table, which)) continue;
    if (spec.tablesWithPlan) {
      planned.front().name += "・" + table.title;
      planned.front().beside.push_back(std::move(table));
    } else {
      planned.push_back({table.title, std::nullopt, table, {}});
    }
  }
  DrawingSet out;
  std::vector<draw::Sheet> sheets;
  std::vector<std::string> names;
  std::vector<bool> tableOnly;
  for (const Planned& p : planned) {
    draw::SheetSpec sheetSpec;
    sheetSpec.scale = spec.scale;
    sheetSpec.paper = spec.paper;
    sheetSpec.title = spec.title;
    sheetSpec.title.drawing = p.name;
    if (p.view) {
      const auto view = draw::makeView(document, *p.view);
      if (!view) {
        out.notes.push_back(p.name + ": " + view.error());
        continue;
      }
      // The drawing's own name in its caption; the sheet's (with its tables) in the title block.
      draw::View named = *view;
      named.spec.title = p.view->title;
      auto laid = draw::layoutSheetWithTables(named, p.beside, sheetSpec);
      if (!laid) {
        out.notes.push_back(p.name + ": " + laid.error());
        continue;
      }
      sheets.push_back(std::move(*laid));
    } else {
      sheetSpec.paper = "auto";
      auto laid = draw::tableSheet(*p.table, sheetSpec);
      if (!laid) {
        out.notes.push_back(p.name + ": " + laid.error());
        continue;
      }
      sheets.push_back(std::move(*laid));
    }
    for (const std::string& n : sheets.back().notes) out.notes.push_back(p.name + ": " + n);
    names.push_back(p.name);
    tableOnly.push_back(!p.view);
  }
  if (sheets.empty()) return R::failure("Nothing to draw: the model is empty");
  // Numbers: the list is 01, the drawings 02, 03, ...
  auto number = [&](std::size_t k) {
    char text[16];
    std::snprintf(text, sizeof text, "%02zu", k);
    return spec.prefix + text;
  };
  draw::Table list;
  list.title = "図面リスト";
  list.headers = {"図番", "図面名", "縮尺", "用紙"};
  list.rows.push_back({number(1), "図面リスト", "", ""});
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    char scale[32];
    std::snprintf(scale, sizeof scale, "1:%g", sheets[i].scale);
    list.rows.push_back({number(i + 2), names[i], tableOnly[i] ? "" : scale, sheets[i].paper});
  }
  list.notes = {"モデルから作った図面です。図面を変えた時は、もう一度書き出してください。"};
  draw::SheetSpec listSpec;
  listSpec.title = spec.title;
  listSpec.title.drawing = list.title;
  listSpec.title.number = number(1);
  const auto listSheet = draw::tableSheet(list, listSpec);
  if (!listSheet) return R::failure(listSheet.error());
  out.sheets.push_back(*listSheet);
  out.numbers.push_back(number(1));
  out.names.push_back(list.title);
  // The drawings again with their numbers in the title block.
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    // Lay out again with the number (the title block is drawn in the layout).
    const Planned* p = nullptr;
    for (const Planned& q : planned)
      if (q.name == names[i]) p = &q;
    draw::SheetSpec sheetSpec;
    sheetSpec.scale = spec.scale;
    sheetSpec.paper = p && p->table ? "auto" : spec.paper;
    sheetSpec.title = spec.title;
    sheetSpec.title.drawing = names[i];
    sheetSpec.title.number = number(i + 2);
    Result<draw::Sheet> laid = Result<draw::Sheet>::failure("");
    if (p && p->view) {
      draw::ViewSpec viewSpec = *p->view;
      // Plans: where the sections are cut, with their sheet numbers.
      if (viewSpec.kind == draw::ViewKind::Plan)
        for (const Cut& c : cuts)
          for (std::size_t k = 0; k < names.size(); ++k)
            if (names[k] == c.name) viewSpec.cutMarks.push_back({c.point, c.toward, c.label, number(k + 2)});
      const auto view = draw::makeView(document, viewSpec);
      if (view) {
        draw::View named = *view;
        named.spec.title = p->view->title;
        laid = draw::layoutSheetWithTables(named, p->beside, sheetSpec);
      } else {
        laid = Result<draw::Sheet>::failure(view.error());
      }
    } else if (p) {
      laid = draw::tableSheet(*p->table, sheetSpec);
    }
    out.sheets.push_back(laid ? *laid : sheets[i]);
    out.numbers.push_back(number(i + 2));
    out.names.push_back(names[i]);
  }
  return out;
}

std::string tableCsv(const draw::Table& table) {
  Csv csv;
  csv.line(table.headers);
  for (const auto& row : table.rows) csv.line(row);
  if (!table.notes.empty()) {
    csv.line({});
    for (const std::string& n : table.notes) csv.line({"注", n});
  }
  return csv.out.str();
}

std::string openingScheduleCsv(const OpeningSchedule& schedule) {
  Csv csv;
  csv.line({"符号", "種類", "建具", "幅(mm)", "高さ(mm)", "数", "腰高(mm)", "品番", "注"});
  int total = 0;
  for (const OpeningTypeRow& t : schedule.types) {
    std::string sills;
    for (double s : t.sills) sills += (sills.empty() ? "" : " / ") + number(s, 0);
    csv.line({t.mark, kindName(t.kind), t.fitting, number(t.width, 0), number(t.height, 0), std::to_string(t.count), sills, t.product,
              t.mixed ? "同じ符号に、種類・建具・大きさ・品番の違う開口があります" : t.ownMark ? "手で決めた符号" : ""});
    total += t.count;
  }
  csv.line({"合計", "", "", "", "", std::to_string(total), "", "", ""});
  csv.line({});
  csv.line({"符号", "種類", "建具", "幅(mm)", "高さ(mm)", "腰高(mm)", "階", "壁", "壁の中の番号", "品番"});
  for (const OpeningRow& r : schedule.rows)
    csv.line({r.mark, kindName(r.kind), r.fitting, number(r.width, 0), number(r.height, 0), number(r.sill, 0), r.level, r.hostName,
              std::to_string(r.index), r.product});
  csv.line({});
  csv.line({"注", "符号は、手で決めたものはそのまま、ほかは種類・建具・幅・高さが同じ開口に同じものを付けています（モデルから毎回読み直します）。"});
  return csv.out.str();
}

std::string roomScheduleCsv(const RoomSchedule& schedule) {
  Csv csv;
  csv.line({"室名", "階", "壁芯(m2)", "内法(m2)", "内法の周長(m)", "状態", "用途", "床", "壁", "天井", "天井高(mm)"});
  for (const RoomRow& r : schedule.rows) {
    csv.line({r.name, r.level, r.closed ? number(r.centreArea / 1e6, 2) : "",
              r.closed ? (r.innerKnown ? number(r.innerArea / 1e6, 2) : "未評価") : "",
              r.closed && r.innerKnown ? number(r.innerPerimeter / 1e3, 2) : "",
              r.counted ? "閉じている" : !r.closed ? "閉じていない" : "ほかの室と同じ場所", r.use, r.floor, r.wall, r.ceiling,
              r.ceilingHeight > 0 ? number(r.ceilingHeight, 0) : ""});
  }
  csv.line({"合計（閉じている室だけ）", "", number(schedule.centreTotal / 1e6, 2),
            schedule.innerMissing == 0 ? number(schedule.innerTotal / 1e6, 2) : "未評価（内法が出ない室があります）", "", ""});
  csv.line({});
  csv.line({"注", "壁芯＝壁を描いた線の内側、内法＝壁の面の内側。法の床面積（算入・不算入の決まり）ではありません。"});
  return csv.out.str();
}

std::string areaTableCsv(const AreaTable& table) {
  Csv csv;
  csv.line({"階", "高さ(mm)", "壁で囲まれた面積・壁芯(m2)", "閉じた所の数", "室の数", "室のない閉じた所", "室の合計・壁芯(m2)",
            "室の合計・内法(m2)", "床スラブの面積(m2)", "床スラブの数", "読めない壁", "法の床面積", "建築面積"});
  for (const AreaRow& r : table.rows) {
    csv.line({r.level.empty() ? "（レベルなし）" : r.level, r.level.empty() ? "" : number(r.elevation, 0),
              r.enclosedKnown ? number(r.enclosedArea / 1e6, 2) : "", r.enclosedKnown ? std::to_string(r.regions) : "",
              std::to_string(r.rooms), r.enclosedKnown ? std::to_string(r.regionsWithoutRoom) : "", number(r.roomCentre / 1e6, 2),
              r.innerMissing == 0 ? number(r.roomInner / 1e6, 2) : "未評価", number(r.slabArea / 1e6, 2), std::to_string(r.slabs),
              std::to_string(r.unreadableWalls), !r.enclosedKnown ? "" : r.legalKnown ? number(r.legalFloor / 1e6, 2) : "未評価", ""});
  }
  bool allKnown = true;
  for (const AreaRow& r : table.rows) allKnown = allKnown && (!r.enclosedKnown || r.legalKnown);
  csv.line({"合計", "", number(table.enclosedTotal / 1e6, 2), "", "", "", number(table.roomCentreTotal / 1e6, 2), "",
            number(table.slabTotal / 1e6, 2), "", "", allKnown ? number(table.legalTotal / 1e6, 2) : "未評価",
            table.buildingKnown ? number(table.buildingArea / 1e6, 2) : "未評価"});
  csv.line({});
  csv.line({"注", "モデルから測った面積です。法の床面積・延べ面積・建築面積は、下の決まりで出しました（確認申請に使う時は、特定行政庁・確認検査機関の取扱いを確かめてください）。"});
  for (const std::string& n : table.legalNotes) csv.line({"", n});
  return csv.out.str();
}

}  // namespace sbr::doc
