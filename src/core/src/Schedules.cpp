#include "sbr/doc/Schedules.h"
#include "sbr/doc/GroundLevel.h"
#include "sbr/doc/FoundationGround.h"
#include "sbr/doc/ColumnCheck.h"
#include "sbr/doc/ArchitectCheck.h"
#include "sbr/doc/FireSpread.h"
#include "sbr/doc/FireCompartments.h"
#include "AreaEvidence.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <gp_Lin.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>
#include <nlohmann/json.hpp>

#include "sbr/ConstructionFoundation.h"
#include "sbr/ConstructionWall.h"
#include "sbr/ConstructionQuantity.h"
#include "sbr/ConstructionGuide.h"
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
  // docs/260 (owner 2026-11-22 "wd にして"): a wall without a build-up with a closed room on both
  // sides of an opening is an inner wall (WD / WW). The rooms read once, when such a wall is met.
  std::optional<std::vector<RoomInfo>> rooms;
  const auto inRoom = [&](const gp_Pnt& p) {
    if (!rooms) rooms = Modeler::roomsIn(document);
    for (const RoomInfo& r : *rooms) {
      if (!r.closed || r.centre.size() < 3) continue;
      if (r.at.Z() > p.Z() + 1.0 || p.Z() - r.at.Z() > 4000.0) continue;  // the storey the opening is in
      bool inside = false;
      for (std::size_t i = 0, j = r.centre.size() - 1; i < r.centre.size(); j = i++) {
        const gp_Pnt& a = r.centre[i];
        const gp_Pnt& b = r.centre[j];
        if ((a.Y() > p.Y()) != (b.Y() > p.Y()) && p.X() < (b.X() - a.X()) * (p.Y() - a.Y()) / (b.Y() - a.Y()) + a.X()) inside = !inside;
      }
      if (inside) return true;
    }
    return false;
  };
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
    nlohmann::json performance = nlohmann::json::object();  // docs/210
    if (const std::string stored = attribute(o, "opening_performance"); !stored.empty()) {
      const nlohmann::json parsed = nlohmann::json::parse(stored, nullptr, false);
      if (parsed.is_object()) performance = parsed;
    }
    nlohmann::json daylight = nlohmann::json::object();  // docs/203
    if (const std::string stored = attribute(o, "opening_daylight"); !stored.empty()) {
      const nlohmann::json parsed = nlohmann::json::parse(stored, nullptr, false);
      if (parsed.is_object()) daylight = parsed;
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
      if (const auto given = performance.find(op.id); given != performance.end() && given->is_object()) {
        row.uValue = given->value("u", -1.0);
        row.solarGain = given->value("eta", -1.0);  // docs/227
        row.glass = given->value("glass", std::string());
        row.performanceSource = given->value("source", std::string());
      }
      if (const auto given = daylight.find(op.id); given != daylight.end() && given->is_object()) {
        row.daylightFactor = given->value("factor", -1.0);
        row.daylightBasis = given->value("basis", std::string());
        row.ventilationM2 = given->value("ventilation_m2", -1.0);
        row.veranda = given->value("veranda", false);
      }
      row.width = op.width;
      row.height = op.height;
      row.sill = op.sill;
      // docs/176: inner walls (an inner build-up) give WD / WW; outer ones AD / AW; docs/260: a wall
      // without a build-up by the rooms on its two sides.
      row.inner = attribute(o, "wall_type").rfind("interior", 0) == 0;
      if (attribute(o, "wall_type").empty()) {
        const gp_Vec away = gp_Vec(op.normal) * (op.depth / 2 + 200.0);
        row.inner = inRoom(op.center.Translated(away)) && inRoom(op.center.Translated(-away));
      }
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
  // Marks: one per kind and side, size and fitting; numbered in a fixed order so that the same
  // model always gives the same marks. The prefixes as drawings usually have them (docs/176):
  // AW / AD in outer walls, WW / WD in inner ones, O for a plain opening.
  using Key = std::tuple<int, long, long, std::string>;
  auto groupOf = [](const OpeningRow& r) {
    if (r.kind == "window") return r.inner ? 1 : 0;
    if (r.kind == "door") return r.inner ? 3 : 2;
    return 4;
  };
  auto keyOf = [&](const OpeningRow& r) { return Key{groupOf(r), tenths(r.width), tenths(r.height), r.fitting}; };
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
    t.inner = r.inner;
    t.fitting = r.fitting;
    t.width = r.width;
    t.height = r.height;
    ++t.count;
    sill(t, r.sill);
  }
  int numbers[5] = {0, 0, 0, 0, 0};
  static const char* const prefixes[5] = {"AW", "WW", "AD", "WD", "O"};
  std::map<Key, std::string> marks;
  std::vector<OpeningTypeRow> lines;
  for (auto& [key, t] : types) {
    const int k = std::get<0>(key);
    do t.mark = std::string(prefixes[k]) + std::to_string(++numbers[k]);
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

namespace {
RoomRow makeRoomRow(const Document& document, const RoomInfo& r) {
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
    row.ratioPart = finish.ratioPart;
    const nlohmann::json daylight = nlohmann::json::parse(attribute(*marker, "room_daylight"), nullptr, false);
    if (daylight.is_object()) {
      row.daylightKind = daylight.value("kind", std::string());
      row.ventilation = daylight.value("ventilation", std::string());
      row.joinedWith = daylight.value("joined_with", std::string());
      row.fanM3h = daylight.value("fan_m3h", 0.0);
      row.fanUnit = daylight.value("fan_unit", std::string());
    }
  }
  return row;
}

bool facesRoomSegment(const RoomInfo& room, const OpeningRow& opening, double scale) {
  const double halfWidth = opening.width * scale / 2;
  const double maxOffset = opening.depth * scale + tolerance::kLinear;
  const double nx = opening.normal.X(), ny = opening.normal.Y();
  for (std::size_t i = 0; i < room.centre.size(); ++i) {
    const auto& a = room.centre[i];
    const auto& b = room.centre[(i + 1) % room.centre.size()];
    const double dx = b.X() - a.X(), dy = b.Y() - a.Y();
    const double length = std::hypot(dx, dy);
    if (length <= tolerance::kLinear ||
        std::abs(dx * nx + dy * ny) > length * tolerance::kAngular) continue;
    const double px = opening.center.X() - a.X(), py = opening.center.Y() - a.Y();
    const double along = (px * dx + py * dy) / length;
    const double offset = std::abs(px * dy - py * dx) / length;
    if (offset <= maxOffset && along - halfWidth >= -tolerance::kLinear &&
        along + halfWidth <= length + tolerance::kLinear) return true;
  }
  return false;
}
}  // namespace

RoomSchedule roomSchedule(const Document& document) {
  RoomSchedule out;
  for (const RoomInfo& r : Modeler::roomsIn(document)) {
    RoomRow row = makeRoomRow(document, r);
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

RoomOpeningInputs roomOpeningInputs(const Document& document) {
  const auto rooms = Modeler::roomsIn(document);
  RoomOpeningInputs out;
  out.openings = openingSchedule(document).rows;
  std::vector<bool> matched(out.openings.size(), false);
  std::map<ObjectId, std::vector<std::size_t>> byWall;
  for (std::size_t i = 0; i < out.openings.size(); ++i) byWall[out.openings[i].host].push_back(i);
  for (const auto& room : rooms) {
    RoomOpeningInput row{makeRoomRow(document, room), {}};
    if (row.room.counted) {
      const double floor = room.at.Z();
      const auto& levels = document.settings().levels;
      const auto next = std::upper_bound(levels.begin(), levels.end(), floor + tolerance::kLinear,
          [](double z, const FloorLevel& level) { return z < level.elevation; });
      for (const auto& wall : room.walls) {
        const auto openings = byWall.find(wall);
        if (openings == byWall.end()) continue;
        const auto* host = document.find(wall);
        const double scale = std::abs(document.worldTransform(*host).ScaleFactor());
        for (std::size_t i : openings->second) {
          const auto& opening = out.openings[i];
          if (opening.bottom < floor - tolerance::kLinear ||
              (next != levels.end() && opening.bottom + opening.height * scale > next->elevation + tolerance::kLinear) ||
              !facesRoomSegment(room, opening, scale)) continue;
          row.openingIndices.push_back(i);
          matched[i] = true;
        }
      }
      std::sort(row.openingIndices.begin(), row.openingIndices.end());
      row.openingIndices.erase(std::unique(row.openingIndices.begin(), row.openingIndices.end()), row.openingIndices.end());
      for (std::size_t i : row.openingIndices) {
        const OpeningRow& o = out.openings[i];
        if (o.daylightFactor >= 0.0) row.daylightArea += o.width * o.height * o.daylightFactor;
        else ++row.factorsMissing;
        if (o.ventilationM2 >= 0.0) row.ventilationArea += o.ventilationM2 * 1e6;
        else ++row.ventilationMissing;
      }
    }
    out.rooms.push_back(std::move(row));
  }
  for (std::size_t i = 0; i < matched.size(); ++i)
    if (!matched[i]) out.unmatched.push_back(i);
  // docs/211: rooms joined by fusuma / shoji and the like are one (法 28 条 4 項).
  for (RoomOpeningInput& a : out.rooms) {
    if (a.room.joinedWith.empty()) continue;
    for (const RoomOpeningInput& b : out.rooms) {
      if (b.room.id != a.room.joinedWith) continue;
      a.pairedName = b.room.name;
      a.pairedFloor = b.room.centreArea;
      a.pairedCounted = b.room.counted;
      a.pairedDaylight = b.daylightArea;
      a.pairedVentilation = b.ventilationArea;
      a.pairedFactorsMissing = b.factorsMissing;
      a.pairedVentilationMissing = b.ventilationMissing;
    }
  }
  // docs/213: rooms served by one ventilation unit (令 20 条の 8 第 1 項第 1 号イ（2））.
  for (RoomOpeningInput& a : out.rooms) {
    if (a.room.fanUnit.empty()) continue;
    std::set<double> airflows;
    for (const RoomOpeningInput& b : out.rooms) {
      if (b.room.fanUnit != a.room.fanUnit) continue;
      if (b.room.id != a.room.id) a.fanUnitOthers.push_back(b.room.name);
      if (b.room.fanM3h > 0) airflows.insert(b.room.fanM3h);
      if (b.room.daylightKind == "none") continue;
      const auto required = requiredAirflow(b.room);
      if (required) a.fanUnitRequired += *required;
      else if (a.fanUnitProblem.empty()) a.fanUnitProblem = b.room.name + " の必要有効換気量を出せません（居室の区分・天井高・室の範囲）";
    }
    a.fanUnitAirflow = airflows.empty() ? 0.0 : airflows.size() > 1 ? -1.0 : *airflows.begin();
  }
  return out;
}

std::optional<double> requiredAirflow(const RoomRow& room) {
  if (room.daylightKind.empty() || room.daylightKind == "none" || !room.counted || room.ceilingHeight <= 0) return std::nullopt;
  const double n = room.daylightKind == "dwelling" ? 0.5 : 0.3;
  return n * room.centreArea / 1e6 * room.ceilingHeight / 1000;
}

RoomDaylightCheck roomDaylightCheck(const RoomOpeningInput& input) {
  RoomDaylightCheck out;
  const RoomRow& room = input.room;
  const auto m2 = [](double mm2) { return number(mm2 / 1e6, 2) + " m²"; };
  // docs/211: the mechanical ventilation (令 20 条の 8), whatever the windows give.
  const auto mechanical = [&] {
    if (room.daylightKind.empty()) {
      out.mechanicalState = "未入力";
      out.mechanicalText = "居室の区分を入れます。";
    } else if (room.daylightKind == "none") {
      out.mechanicalState = "対象外";
      out.mechanicalText = "居室でない室として入力されています。";
    } else if (!room.counted) {
      out.mechanicalState = "未評価";
      out.mechanicalText = "室の範囲（床面積）を読めません。";
    } else if (room.ceilingHeight <= 0) {
      out.mechanicalState = "未評価";
      out.mechanicalText = "天井高が無いので、必要有効換気量（床面積×天井高×回数）を出せません。室の天井高を入れます。";
    } else {
      const double n = room.daylightKind == "dwelling" ? 0.5 : 0.3;
      const double required = n * room.centreArea / 1e6 * room.ceilingHeight / 1000;
      const std::string need = "必要有効換気量 " + number(n, 1) + "×" + number(room.centreArea / 1e6, 2) + "×" +
                               number(room.ceilingHeight / 1000, 2) + " = " + number(required, 1) + " m³/h（令 20 条の 8 第 1 項第 1 号イ（1）、" +
                               (room.daylightKind == "dwelling" ? "住宅等の居室 0.5" : "その他の居室 0.3") + "。床面積は壁芯）";
      if (!input.fanUnitOthers.empty()) {
        // docs/213: one unit for several rooms: its airflow against the sum of theirs.
        std::string rooms = room.name;
        for (const auto& other : input.fanUnitOthers) rooms += "・" + other;
        const std::string unit = "換気設備「" + room.fanUnit + "」（" + rooms + "）";
        if (!input.fanUnitProblem.empty()) {
          out.mechanicalState = "未評価";
          out.mechanicalText = need + "。" + unit + "：" + input.fanUnitProblem + "。";
        } else if (input.fanUnitAirflow < 0) {
          out.mechanicalState = "要確認";
          out.mechanicalText = need + "。" + unit + "：室ごとに違う有効換気量が入っています。1台の量にそろえます。";
        } else if (input.fanUnitAirflow == 0) {
          out.mechanicalState = "未入力";
          out.mechanicalText = need + "。" + unit + "の必要有効換気量の合計 " + number(input.fanUnitRequired, 1) +
                               " m³/h。設備の有効換気量を入れます。";
        } else {
          out.mechanicalState = input.fanUnitAirflow + 1e-9 >= input.fanUnitRequired ? "参考値内" : "参考値を外れる";
          out.mechanicalText = "有効換気量 " + number(input.fanUnitAirflow, 1) + " m³/h／" + unit + "の必要有効換気量の合計 " +
                               number(input.fanUnitRequired, 1) + " m³/h（令 20 条の 8 第 1 項第 1 号イ（2））。この室の" + need;
        }
      } else if (room.fanM3h <= 0) {
        out.mechanicalState = "未入力";
        out.mechanicalText = need + "。換気設備の有効換気量を入れます（1台で複数の室をまかなう時は、室に同じ換気設備の名前を入れると合計と比べます）。";
      } else {
        out.mechanicalState = room.fanM3h + 1e-9 >= required ? "参考値内" : "参考値を外れる";
        out.mechanicalText = "有効換気量 " + number(room.fanM3h, 1) + " m³/h／" + need;
      }
    }
  };
  mechanical();
  if (!room.counted || (!room.joinedWith.empty() && !input.pairedCounted)) {
    out.daylightState = out.ventilationState = "未評価";
    out.daylightText = out.ventilationText = "室の範囲（床面積）を読めません。";
    return out;
  }
  // 法 28 条 4 項: a room joined to another by fusuma / shoji is one with it.
  const bool paired = !room.joinedWith.empty() && !input.pairedName.empty();
  const double floor = room.centreArea + (paired ? input.pairedFloor : 0.0);
  const double daylight = input.daylightArea + (paired ? input.pairedDaylight : 0.0);
  const double ventilationArea = input.ventilationArea + (paired ? input.pairedVentilation : 0.0);
  const int factorsMissing = input.factorsMissing + (paired ? input.pairedFactorsMissing : 0);
  const int ventilationMissing = input.ventilationMissing + (paired ? input.pairedVentilationMissing : 0);
  const std::string together = paired ? "（" + input.pairedName + " と一室として、法 28 条 4 項）" : std::string();
  const std::string candidates = "開口候補 " + std::to_string(input.openingIndices.size()) + (paired ? "＋一続きの室の候補" : "");
  if (room.daylightKind.empty()) {
    out.daylightState = out.ventilationState = "未入力";
    out.daylightText = out.ventilationText = "居室の区分（住宅の居住のための居室／その他の居室／居室でない）を入れます。";
    return out;
  }
  if (room.daylightKind == "none") {
    out.daylightState = out.ventilationState = "対象外";
    out.daylightText = out.ventilationText = "居室でない室として入力されています（法 28 条の対象外として扱う）。";
    return out;
  }
  // Daylight.
  if (room.daylightKind == "other") {
    out.daylightState = "未評価";
    out.daylightText = "住宅の居住のための居室以外の居室は、令 19 条 3 項の表の種類で割合が違います（1/5〜1/10）。ここでは照らしていません。";
  } else if (factorsMissing > 0) {
    out.daylightState = "未評価";
    out.daylightText = candidates + " のうち、採光補正係数が無い開口 " + std::to_string(factorsMissing) +
                       "。開口ごとに、隣地境界線などまでの距離から求めた係数（令 20 条）と根拠を入れます。";
  } else {
    const double required = floor / 7.0;
    out.daylightState = daylight + 1e-6 >= required ? "参考値内" : "参考値を外れる";
    out.daylightText = "採光に有効な面積 " + m2(daylight) + "／床面積 " + m2(floor) + " の 1/7 = " + m2(required) + together +
                       "（" + candidates + "、令 19 条 3 項 表（三）。大臣の定める措置による 1/10 までの緩和は見ていない）";
  }
  // Ventilation.
  if (room.ventilation == "equipment") {
    out.ventilationState = "換気設備";
    out.ventilationText = "換気設備を設ける室として入力されています（法 28 条 2 項ただし書き）。設備の技術的基準は未評価です。";
  } else if (ventilationMissing > 0) {
    out.ventilationState = "未評価";
    out.ventilationText = candidates + " のうち、換気に有効な面積が無い開口 " + std::to_string(ventilationMissing) +
                          "。開けられる部分の面積（引違いなら片側分など）を入れます。";
  } else {
    const double required = floor / 20.0;
    out.ventilationState = ventilationArea + 1e-6 >= required ? "参考値内" : "参考値を外れる";
    out.ventilationText = "換気に有効な面積 " + m2(ventilationArea) + "／床面積 " + m2(floor) + " の 1/20 = " + m2(required) + together +
                          "（法 28 条 2 項）";
  }
  return out;
}

// --- docs/205: the daylight factor (採光補正係数) worked out from the model ---------------------
namespace {
// Where a horizontal ray from `from` along `dir` (plan) first meets the outline: its distance and
// the edge (index of its first point); none when it does not leave through an edge.
std::optional<std::pair<double, std::size_t>> rayToOutline(const gp_Pnt& from, const gp_Dir& dir,
                                                           const std::vector<gp_Pnt>& outline) {
  std::optional<std::pair<double, std::size_t>> best;
  const std::size_t n = outline.size();
  for (std::size_t i = 0; i < n; ++i) {
    const gp_Pnt& a = outline[i];
    const gp_Pnt& b = outline[(i + 1) % n];
    const double ex = b.X() - a.X(), ey = b.Y() - a.Y();
    const double den = dir.X() * ey - dir.Y() * ex;
    if (std::abs(den) < 1e-12) continue;
    const double wx = a.X() - from.X(), wy = a.Y() - from.Y();
    const double t = (wx * ey - wy * ex) / den;              // along the ray
    const double s = (wx * dir.Y() - wy * dir.X()) / den;    // along the edge
    if (t <= 1e-6 || s < -1e-9 || s > 1 + 1e-9) continue;
    if (!best || t < best->first) best = std::make_pair(t, i);
  }
  return best;
}

// The objects that make up the building (what can stand over an opening).
bool buildingPart(const Object& o) {
  const std::string k = attribute(o, "kind");
  return k == "wall" || k == "roof" || k == "slab" || k == "balcony" || k == "canopy" || k == "stair" ||
         k == "column" || k == "beam" || k == "ceiling";
}

struct PartShape {
  const Object* object = nullptr;
  TopoDS_Shape shape;
  Bnd_Box box;
};

// The highest point of the building over plan point (x, y) above `zMin`, and what it is.
std::optional<std::pair<double, const Object*>> highestOver(const std::vector<PartShape>& parts, double x, double y,
                                                            double zMin) {
  std::optional<std::pair<double, const Object*>> top;
  for (const PartShape& p : parts) {
    double x0, y0, z0, x1, y1, z1;
    p.box.Get(x0, y0, z0, x1, y1, z1);
    if (x < x0 || x > x1 || y < y0 || y > y1 || z1 <= zMin) continue;
    if (top && z1 <= top->first) continue;
    IntCurvesFace_ShapeIntersector cut;
    cut.Load(p.shape, 1e-4);
    cut.Perform(gp_Lin(gp_Pnt(x, y, zMin), gp::DZ()), 0.0, z1 - zMin + 1.0);
    if (!cut.IsDone()) continue;
    for (int i = 1; i <= cut.NbPnt(); ++i) {
      const double z = cut.Pnt(i).Z();
      if (z > zMin + 1e-6 && (!top || z > top->first)) top = std::make_pair(z, p.object);
    }
  }
  return top;
}

// Does a horizontal ray from `from` along `dir` meet any wall but `host` (within `reach`)?
bool meetsAnotherWall(const std::vector<PartShape>& parts, const ObjectId& host, const gp_Pnt& from, const gp_Dir& dir,
                      double reach) {
  for (const PartShape& p : parts) {
    if (p.object->id == host || attribute(*p.object, "kind") != "wall") continue;
    IntCurvesFace_ShapeIntersector cut;
    cut.Load(p.shape, 1e-4);
    cut.Perform(gp_Lin(from, dir), 1.0, reach);
    if (cut.IsDone() && cut.NbPnt() > 0) return true;
  }
  return false;
}
}  // namespace

std::vector<DaylightFactorEstimate> daylightFactorEstimates(const Document& document) {
  std::vector<DaylightFactorEstimate> out;
  const auto& site = document.settings().siteBoundary;
  std::string zone;
  if (site)
    if (const Object* outlineObject = document.find(site->outline)) zone = attribute(*outlineObject, "use_zone");
  const auto geometry = site ? document.siteBoundaryGeometry(site->outline) : Result<SiteBoundaryGeometry>::failure("");
  std::optional<std::array<gp_Pnt, 2>> road;
  if (geometry && site->road) road = document.siteRoadEdge(*geometry);
  std::vector<PartShape> parts;
  for (const Object& o : document.objects()) {
    if (o.shape.IsNull() || !buildingPart(o) || !document.isVisible(o.id)) continue;
    PartShape p{&o, document.worldShape(o), document.worldBounds(o)};
    if (!p.box.IsVoid()) parts.push_back(std::move(p));
  }
  for (const OpeningRow& row : openingSchedule(document).rows) {
    DaylightFactorEstimate e;
    e.host = row.host;
    e.openingId = row.openingId;
    e.index = row.index;
    e.mark = row.mark;
    if (!site || !geometry) {
      e.problem = "敷地境界がありません（配置図の敷地境界を登録します）";
      out.push_back(e);
      continue;
    }
    if (zone.empty()) {
      e.problem = "用途地域の区分が未入力です（set_site_zone）";
      out.push_back(e);
      continue;
    }
    // Outwards: the side where a level ray from the opening's centre meets no other wall.
    const gp_Pnt centre = row.center;
    const gp_Dir n = row.normal;
    // (At the centre's height, and near the host's foot and top: a ray through another opening
    // straight across would meet nothing at one height only.)
    const Object* hostObject = document.find(row.host);
    const Bnd_Box hostBox = hostObject ? document.worldBounds(*hostObject) : Bnd_Box();
    std::vector<double> heights{centre.Z()};
    if (!hostBox.IsVoid()) {
      heights.push_back(hostBox.CornerMin().Z() + 50.0);
      heights.push_back(hostBox.CornerMax().Z() - 50.0);
    }
    const auto clear = [&](const gp_Dir& dir) {
      for (const double z : heights)
        if (meetsAnotherWall(parts, row.host, gp_Pnt(centre.X(), centre.Y(), z), dir, 1e6)) return false;
      return true;
    };
    const bool plus = clear(n);
    const bool minus = clear(n.Reversed());
    if (plus == minus) {
      e.problem = plus ? "外側を決められません（両側にほかの壁がありません）" : "内壁の開口です（両側にほかの壁があります）";
      out.push_back(e);
      continue;
    }
    const gp_Dir outward = plus ? n : n.Reversed();
    // The outer face of the wall at the opening (the centre is on the face it was cut from).
    const gp_Pnt face = plus ? centre : centre.Translated(gp_Vec(outward) * row.depth);
    const auto hit = rayToOutline(gp_Pnt(face.X(), face.Y(), 0), gp_Dir(outward.X(), outward.Y(), 0), geometry->vertices);
    if (!hit) {
      e.problem = "開口から外へ伸ばした線が敷地境界に当たりません（建物が敷地の外にある、など）";
      out.push_back(e);
      continue;
    }
    double reach = hit->first;
    if (road) {
      const gp_Pnt& a = geometry->vertices[hit->second];
      const gp_Pnt& b = geometry->vertices[(hit->second + 1) % geometry->vertices.size()];
      const auto same = [](const gp_Pnt& p, const gp_Pnt& q) { return std::hypot(p.X() - q.X(), p.Y() - q.Y()) < 1.0; };
      e.facesRoad = (same(a, (*road)[0]) && same(b, (*road)[1])) || (same(a, (*road)[1]) && same(b, (*road)[0]));
    }
    if (e.facesRoad) {
      if (site->road->widthMm <= 0) {
        e.problem = "道路に面しますが、道路の幅員が未入力です（反対側の境界線まで測れません）";
        out.push_back(e);
        continue;
      }
      reach += site->road->widthMm;  // to the far side of the road (令 20 条 2 項 1 号)
    }
    // Over the opening's width (its ends and middle), every 100 from the face out: the part of the
    // building highest above the opening's centre, and the smallest horizontal / vertical.
    const gp_Vec along(gp_Vec(0, 0, 1).Crossed(gp_Vec(outward)));
    const double half = std::max(0.0, row.width / 2 - 1.0);
    bool found = false;
    for (double s : {-half, 0.0, half}) {
      for (double d = -1.0; d < std::min(reach, 6000.0); d += 100.0) {  // -1: the host wall itself
        const gp_Pnt p = face.Translated(gp_Vec(outward) * d + along * s);
        const auto top = highestOver(parts, p.X(), p.Y(), centre.Z());
        if (!top) continue;
        const double horizontal = reach - d, vertical = top->first - centre.Z();
        if (vertical <= 1.0) continue;
        const double ratio = horizontal / vertical;
        if (!found || ratio < e.ratio) {
          found = true;
          e.ratio = ratio;
          e.horizontal = horizontal;
          e.vertical = vertical;
          e.over = top->second->name;
        }
      }
    }
    if (!found) {
      e.problem = "開口の上に建物の部分が見つかりません（採光関係比率を出せません）";
      out.push_back(e);
      continue;
    }
    // 令 20 条 2 項 1-3 号.
    const double a = zone == "residential" ? 6.0 : zone == "industrial" ? 8.0 : 10.0;
    const double b = zone == "residential" ? 1.4 : 1.0;
    const double far = zone == "residential" ? 7000.0 : zone == "industrial" ? 5000.0 : 4000.0;
    const std::string item = zone == "residential" ? "1 号" : zone == "industrial" ? "2 号" : "3 号";
    double f = e.ratio * a - b;
    std::string rule = "採光関係比率 " + number(e.horizontal, 0) + "÷" + number(e.vertical, 0) + "＝" + number(e.ratio, 3) +
                       "（" + e.over + "）×" + number(a, 0) + "−" + number(b, 1) + "＝" + number(f, 3);
    if (e.facesRoad && f < 1.0) {
      f = 1.0;
      rule += "、道に面するので 1.0（イ）";
    } else if (!e.facesRoad && e.horizontal >= far && f < 1.0) {
      f = 1.0;
      rule += "、水平距離 " + number(far / 1000, 0) + " m 以上なので 1.0（ロ）";
    } else if (!e.facesRoad && e.horizontal < far && f < 0.0) {
      f = 0.0;
      rule += "、負なので 0（ハ）";
    }
    if (row.veranda) {  // docs/211: a veranda 90 cm wide or more outside it
      f *= 0.7;
      rule += "、縁側（幅 90 cm 以上）の外なので ×0.7＝" + number(f, 3);
    }
    if (f > 3.0) {
      f = 3.0;
      rule += "、上限 3.0";
    }
    e.factor = f;
    e.basis = "令 20 条 2 項 " + item + "：" + rule + (e.facesRoad ? "。道の反対側の境界線まで" : "") +
              "。天窓・公園や川に面する場合は見ていない";
    out.push_back(e);
  }
  return out;
}

Result<int> applyDaylightFactors(Modeler& modeler, const std::vector<std::string>& openings) {
  const auto estimates = daylightFactorEstimates(modeler.document());
  int given = 0;
  modeler.history().beginGroup("Daylight Factors");
  for (const auto& e : estimates) {
    if (!openings.empty() && std::find(openings.begin(), openings.end(), e.openingId) == openings.end()) continue;
    if (e.factor < 0) continue;
    if (const Status s = modeler.setOpeningDaylight(e.host, {e.index}, e.factor, e.basis, std::nan("")); !s) {
      modeler.history().abortGroup();
      return Result<int>::failure(s.message());
    }
    ++given;
  }
  if (given == 0) modeler.history().abortGroup();
  else modeler.history().endGroup();
  return given;
}

// --- docs/194: permit workflow stages 2 and 3 ------------------------------------------------
namespace {
// "Wall 1、Wall 2、Wall 3 ほか 2" (the first few names of the objects a row is about).
std::string someNames(const Document& document, const std::vector<ObjectId>& ids) {
  std::string out;
  const std::size_t shown = std::min<std::size_t>(ids.size(), 5);
  for (std::size_t i = 0; i < shown; ++i) {
    const Object* o = document.find(ids[i]);
    if (!out.empty()) out += "、";
    out += o ? o->name : ids[i];
  }
  if (ids.size() > shown) out += " ほか " + std::to_string(ids.size() - shown);
  return out;
}

std::string m2(double mm2) { return number(mm2 / 1e6, 2) + " m²"; }
}  // namespace

std::vector<PermitCheck> permitGridChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto& levels = document.settings().levels;
  if (levels.empty()) {
    out.push_back({"階（レベル）", "未入力", "階の名前と床の高さ（1FL 0、2FL 2900 など）を設定します。", {}, true});
  } else {
    std::string list;
    for (const FloorLevel& l : levels)
      list += (list.empty() ? "" : "　") + l.name + " " + number(l.elevation, 0);
    out.push_back({"階（レベル）", "入力済み", std::to_string(levels.size()) + " 階　" + list + "（mm）", {}});
  }

  std::vector<ObjectId> grids, walls, wallsOffLevel, wallsOffGrid, members, membersOffGrid;
  std::string gridDetails;
  for (const Object& o : document.objects()) {
    if (const auto* node = std::get_if<GroupNode>(&o.recipe.base); node && node->grid) {
      grids.push_back(o.id);
      if (!gridDetails.empty()) gridDetails += "\n";
      gridDetails += o.name + "　X 方向 " + std::to_string(node->grid->x.size()) + " 本・Y 方向 " +
                     std::to_string(node->grid->y.size()) + " 本";
      continue;
    }
    const std::string kind = attribute(o, "kind");
    if (kind == "wall") {
      walls.push_back(o.id);
      if (attribute(o, "base_level").empty()) wallsOffLevel.push_back(o.id);
      if (attribute(o, "wall_grid_pin").empty()) wallsOffGrid.push_back(o.id);
    } else if (kind == "column" || kind == "beam") {
      members.push_back(o.id);
      if (!o.gridPin) membersOffGrid.push_back(o.id);
    }
  }
  if (grids.empty())
    out.push_back({"通り芯", "未入力", "通り芯（X・Y の通りと間隔）を作ります。壁・柱・梁は後から通り芯に付けられます。", {}, true});
  else
    out.push_back({"通り芯", "入力済み", gridDetails, grids});

  if (walls.empty()) {
    out.push_back({"壁と階", "未入力", "壁がまだありません。", {}});
  } else if (levels.empty()) {
    out.push_back({"壁と階", "階待ち", "階を設定すると、壁がどの階に立つかを照合できます。", {}});
  } else if (!wallsOffLevel.empty()) {
    out.push_back({"壁と階", "要確認",
                   "どの階の高さにも立っていない壁 " + std::to_string(wallsOffLevel.size()) + " 枚（全 " +
                       std::to_string(walls.size()) + " 枚）：" + someNames(document, wallsOffLevel) +
                       "\n階の高さに置き直すか、階の高さを見直します。",
                   wallsOffLevel, true});
  } else {
    out.push_back({"壁と階", "入力済み", "壁 " + std::to_string(walls.size()) + " 枚がどれも階の上に立っています。", {}});
  }

  const auto pinRow = [&](const char* item, const char* what, const std::vector<ObjectId>& all,
                          const std::vector<ObjectId>& off, const char* unit) {
    if (all.empty()) return;
    if (grids.empty()) {
      out.push_back({item, "通り芯待ち", "通り芯を作ると、通り芯に付いているかを照合できます。", {}});
    } else if (!off.empty()) {
      out.push_back({item, "要確認",
                     std::string("通り芯に付いていない") + what + " " + std::to_string(off.size()) + " " + unit +
                         "（全 " + std::to_string(all.size()) + " " + unit + "）：" + someNames(document, off) +
                         "\n通り芯の上にある物は、階のボタン →「柱・梁・壁を通り芯に付ける」で付けられます。"
                         "通りから外れた物はそのままで構いません。",
                     off, true});
    } else {
      out.push_back({item, "入力済み", std::string(what) + " " + std::to_string(all.size()) + " " + unit +
                                          "がどれも通り芯に付いています。", {}});
    }
  };
  pinRow("壁と通り芯", "壁", walls, wallsOffGrid, "枚");
  pinRow("柱・梁と通り芯", "柱・梁", members, membersOffGrid, "本");
  return out;
}

std::vector<PermitCheck> permitPlanChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const AreaTable areas = areaTable(document);
  for (const AreaRow& row : areas.rows) {
    if (!row.enclosedKnown) continue;
    std::string details = "壁で閉じた範囲 " + std::to_string(row.regions) + "（壁芯 " + m2(row.enclosedArea) +
                          "）、室 " + std::to_string(row.rooms) + "（壁芯 " + m2(row.roomCentre) + "、内法 " +
                          m2(row.roomInner) + "）";
    bool attention = false;
    if (row.regionsWithoutRoom > 0) {
      details += "\n室を置いていない範囲 " + std::to_string(row.regionsWithoutRoom) + "。室ツールで中をクリックします。";
      attention = true;
    }
    if (row.innerMissing > 0)
      details += "\n内法面積を出せない室 " + std::to_string(row.innerMissing) + "（壁の面が単純な形でない）。";
    if (row.unreadableWalls > 0) {
      details += "\n形を直接変えた壁 " + std::to_string(row.unreadableWalls) + " 枚は壁として読んでいません。";
      attention = true;
    }
    out.push_back({row.level + " の平面", attention ? "要確認" : "入力済み", details, {}, attention});
  }
  if (out.empty())
    out.push_back({"平面", "未入力", "階を設定し、壁で部屋を閉じてから室を置きます。", {}, true});

  std::vector<ObjectId> open, sharing;
  for (const RoomRow& r : roomSchedule(document).rows) {
    if (!r.closed) open.push_back(r.id);
    else if (!r.counted) sharing.push_back(r.id);
  }
  if (!open.empty() || !sharing.empty()) {
    std::string details;
    if (!open.empty()) details += "輪郭が閉じていない室 " + std::to_string(open.size()) + "：" + someNames(document, open);
    if (!sharing.empty())
      details += std::string(details.empty() ? "" : "\n") + "ほかの室と同じ範囲にある室 " +
                 std::to_string(sharing.size()) + "：" + someNames(document, sharing);
    std::vector<ObjectId> targets = open;
    targets.insert(targets.end(), sharing.begin(), sharing.end());
    out.push_back({"室の閉じ方", "要修正", details + "\n壁の端をつなぐか、重なった室の札を消します。", targets, true});
  }

  const auto openings = openingSchedule(document).rows;
  std::vector<ObjectId> bareHosts;
  int bare = 0;
  for (const OpeningRow& o : openings) {
    if (o.kind != "opening") continue;
    ++bare;
    if (std::find(bareHosts.begin(), bareHosts.end(), o.host) == bareHosts.end()) bareHosts.push_back(o.host);
  }
  if (openings.empty()) {
    out.push_back({"開口", "未入力", "壁に窓・扉の開口がまだありません。", {}});
  } else if (bare > 0) {
    out.push_back({"開口", "要確認",
                   "開口 " + std::to_string(openings.size()) + " 件のうち、建具（窓・扉）を決めていない開口 " +
                       std::to_string(bare) + " 件（壁：" + someNames(document, bareHosts) +
                       "）。\n建具表と平面の記号に出ません。穴のままでよい物はそのままで構いません。",
                   bareHosts, true});
  } else {
    out.push_back({"開口", "入力済み", "開口 " + std::to_string(openings.size()) + " 件。どれも建具が決まっています。", {}});
  }
  return out;
}

std::vector<PermitCheck> permitRoofChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto mm = [](double v) { return number(v, 0) + " mm"; };
  // Walls standing up to each roof (fitted to it, docs/100).
  std::map<ObjectId, std::vector<ObjectId>> wallsUp;
  for (const Object& o : document.objects())
    if (attribute(o, "kind") == "wall" && !attribute(o, "top_roof").empty()) wallsUp[attribute(o, "top_roof")].push_back(o.id);
  int roofs = 0;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "roof") continue;
    ++roofs;
    const nlohmann::json spec = nlohmann::json::parse(attribute(o, "roof_spec"), nullptr, false);
    if (!spec.is_object()) {
      out.push_back({o.name, "未評価", "屋根ツールの寸法を読めません（形を直接変えた屋根）。勾配・軒の出は図で確かめます。", {o.id}, true});
      continue;
    }
    const std::string kind = spec.value("kind", std::string("gable"));
    const std::string shape = kind == "gable" ? "切妻" : kind == "hip" ? "寄棟" : kind == "shed" ? "片流れ" : kind;
    const double eave = spec.value("eave", 600.0), verge = spec.value("gable_eave", 300.0);
    std::string details = shape + "　勾配 " + number(spec.value("pitch", 0.0), 1) + " 寸（" + number(spec.value("pitch", 0.0), 1) +
                          "/10）\n軒の出 " + mm(eave);
    if (kind != "hip") details += "　けらばの出 " + mm(verge);
    if (spec.contains("sides") && spec.at("sides").is_array() && !spec.at("sides").empty())
      details += "\n辺ごとの勾配・出あり " + std::to_string(spec.at("sides").size()) + " 辺";
    if (spec.contains("wall_sides") && spec.at("wall_sides").is_array() && !spec.at("wall_sides").empty())
      details += "\n壁に付く辺 " + std::to_string(spec.at("wall_sides").size()) + "（そこには軒が出ません）";
    if (const std::string type = attribute(o, "type_name"); !type.empty()) details += "\n種類 " + type;
    std::vector<ObjectId> targets{o.id};
    const auto up = wallsUp.find(o.id);
    if (up != wallsUp.end()) {
      details += "\n屋根まで立ち上げた壁 " + std::to_string(up->second.size()) + " 枚";
      targets.insert(targets.end(), up->second.begin(), up->second.end());
    } else {
      details += "\n屋根まで立ち上げた壁なし（妻壁が三角にならず、壁と屋根の間が空いて見えます。屋根のプロパティ「下の壁：合わせる」）";
    }
    double furthest = std::max(eave, kind == "hip" ? 0.0 : verge);
    if (spec.contains("sides") && spec.at("sides").is_array())
      for (const auto& one : spec.at("sides"))
        if (one.is_object()) furthest = std::max(furthest, one.value("eave", 0.0));
    if (furthest >= 1000.0) details += "\n出が 1 m 以上：先から 1 m を除いた部分が建築面積に入ります（令 2 条 1 項 2 号、docs/141）";
    out.push_back({o.name, up != wallsUp.end() ? "入力済み" : "要確認", details, targets, up == wallsUp.end()});
  }
  if (roofs == 0) out.push_back({"屋根", "未入力", "屋根ツールで外周を描き、勾配と軒の出を入れます。", {}, true});

  std::vector<ObjectId> canopies;
  std::string canopyDetails;
  int canopiesOut = 0;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "canopy") continue;
    const nlohmann::json c = nlohmann::json::parse(attribute(o, "canopy"), nullptr, false);
    canopies.push_back(o.id);
    if (!c.is_object()) continue;
    const double depth = c.value("depth", 0.0);
    if (depth >= 1000.0) ++canopiesOut;
    if (canopies.size() <= 5) canopyDetails += "\n" + o.name + "　出 " + mm(depth) + "　幅 " + mm(c.value("width", 0.0));
  }
  if (!canopies.empty()) {
    std::string details = "庇 " + std::to_string(canopies.size()) + " 枚" + canopyDetails;
    if (canopies.size() > 5) details += "\nほか " + std::to_string(canopies.size() - 5) + " 枚";
    if (canopiesOut > 0) details += "\n出が 1 m 以上の庇 " + std::to_string(canopiesOut) + " 枚：先から 1 m を除いた部分が建築面積に入ります";
    out.push_back({"庇", "入力済み", details, canopies});
  }

  std::vector<ObjectId> balconies;
  std::string balconyDetails;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "balcony") continue;
    const nlohmann::json b = nlohmann::json::parse(attribute(o, "balcony"), nullptr, false);
    balconies.push_back(o.id);
    if (!b.is_object() || balconies.size() > 5) continue;
    const std::string how = b.value("floor_area", std::string());
    const std::string floor = how == "count" ? "床面積に入れる（手で選択）" : how == "exclude" ? "床面積に入れない（手で選択）"
                                                                         : "床面積は通達どおり（自動）";
    balconyDetails += "\n" + o.name + "　出 " + mm(b.value("depth", 0.0)) + "　幅 " + mm(b.value("width", 0.0));
    if (b.value("posts", 0) > 0) balconyDetails += "　柱 " + std::to_string(b.value("posts", 0)) + " 本";
    balconyDetails += "　" + floor;
  }
  if (!balconies.empty()) {
    std::string details = "バルコニー " + std::to_string(balconies.size()) + " 個" + balconyDetails;
    if (balconies.size() > 5) details += "\nほか " + std::to_string(balconies.size() - 5) + " 個";
    details += "\n建築面積・床面積への入り方は面積の表の注にあります（docs/163・167・175）。";
    out.push_back({"バルコニー", "入力済み", details, balconies});
  }

  const AreaTable areas = areaTable(document);
  if (areas.buildingKnown) {
    std::string details = "建築面積 " + m2(areas.buildingArea) + "（壁芯の各階の平面と、1 m 以上出た軒・庇・バルコニーの先から 1 m を除いた部分を重ねた物）";
    for (std::size_t i = 0; i < std::min<std::size_t>(areas.legalNotes.size(), 4); ++i) details += "\n" + areas.legalNotes[i];
    out.push_back({"建築面積", "参考値", details, {}});
  } else {
    out.push_back({"建築面積", "未評価", "面積の表で建築面積を出せません。階と壁で閉じた平面を確かめます。", {}, true});
  }
  out.push_back({"まだ確かめていない事", "未評価",
                 "屋根の構造・材料（防火の区域による屋根の性能）、高さの制限（斜線など）、採光に効く軒・庇の出（第 4 工程の採光）、"
                 "屋根の排水。申請先と申請時点の法令で確かめます。",
                 {}});
  return out;
}

std::vector<PermitCheck> permitBraceChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto made = construction::checkWallQuantity(document);
  if (!made) {
    out.push_back({"壁量", "未評価", "壁量の比較を出せません：" + made.error(), {}, true});
    return out;
  }
  const nlohmann::json report = nlohmann::json::parse(*made, nullptr, false);
  if (!report.is_object()) {
    out.push_back({"壁量", "未評価", "壁量の比較を読めません。", {}, true});
    return out;
  }
  const auto stateOf = [](const std::string& status) -> std::string {
    return status == "pass" ? "参考値内" : status == "fail" ? "要確認" : "未評価";
  };
  const auto metres = [](const nlohmann::json& v) { return v.is_number() ? number(v.get<double>(), 2) + " m" : std::string("—"); };
  const auto directionName = [](const std::string& d) { return d.empty() ? std::string() : d + " 方向"; };
  // The walls counted for a storey and direction (to select them).
  const auto wallsOf = [&](const std::string& floor, const std::string& direction) {
    std::vector<ObjectId> ids;
    for (const auto& w : report.value("walls", nlohmann::json::array()))
      if (w.value("floor", std::string()) == floor && w.value("direction", std::string()) == direction && w.contains("id") &&
          w.at("id").is_string() && document.find(w.at("id").get<std::string>()))  // walls of the model only
        ids.push_back(w.at("id").get<std::string>());
    return ids;
  };
  const auto reasons = [](const nlohmann::json& row) {
    std::string text;
    for (const auto& r : row.value("reasons", nlohmann::json::array()))
      if (r.is_string()) text += "\n" + r.get<std::string>();
    return text;
  };
  for (const auto& row : report.value("rows", nlohmann::json::array())) {
    const std::string floor = row.value("floor", std::string()), direction = row.value("direction", std::string());
    const std::string status = row.value("status", std::string("not_evaluated"));
    if (floor.empty()) {
      out.push_back({"壁量の入力", "未入力",
                     "壁量の計算に使う階・荷重・耐力壁の仕様がまだありません（AI の set_wall_calculation、構造の入力）。" + reasons(row),
                     {}, true});
      continue;
    }
    const std::string details = "存在壁量 " + metres(row.value("existing_m", nlohmann::json())) + "　必要壁量 " +
                                metres(row.value("required_m", nlohmann::json())) + reasons(row);
    const std::string name = row.value("name", floor);  // the storey's name for people (floor: its id)
    out.push_back({name + " " + directionName(direction) + "の壁量", stateOf(status), details, wallsOf(floor, direction),
                   status != "pass"});
  }
  for (const auto& row : report.value("quarter_balance", nlohmann::json::array())) {
    const std::string floor = row.value("floor", std::string()), direction = row.value("direction", std::string());
    const std::string status = row.value("status", std::string("not_evaluated"));
    std::string details;
    if (row.contains("wall_ratio") && row.at("wall_ratio").is_number())
      details = "壁率比 " + number(row.at("wall_ratio").get<double>(), 2);
    if (row.contains("basis") && row.at("basis").is_string()) details += (details.empty() ? "" : "　") + row.at("basis").get<std::string>();
    for (const auto& s : row.value("strips", nlohmann::json::array()))
      if (s.contains("ratio") && s.at("ratio").is_number())
        details += "\n側端部分　存在 " + metres(s.value("existing_m", nlohmann::json())) + "／必要 " +
                   metres(s.value("required_m", nlohmann::json())) + "　充足率 " + number(s.at("ratio").get<double>(), 2);
    details += reasons(row);
    const std::string name = row.value("name", floor);
    out.push_back({name + " " + directionName(direction) + "の四分割", stateOf(status), details.empty() ? "—" : details,
                   wallsOf(floor, direction), status != "pass"});
  }
  if (report.value("stale_table", false))
    out.push_back({"係数表", "要確認", "保存した係数表が、今の版と違います。表を確かめて入れ直します。", {}, true});
  out.push_back({"照らした値", "参考", report.value("notice", std::string()), {}});
  std::string open;
  for (const auto& u : report.value("unassessed", nlohmann::json::array()))
    if (u.is_string()) open += (open.empty() ? "" : "、") + u.get<std::string>();
  out.push_back({"まだ確かめていない事", "未評価",
                 (open.empty() ? std::string() : open + "。") +
                     "耐力壁の配置図・壁量計算表・側端の図の書式は、申請先の様式で確かめます。",
                 {}});
  return out;
}

std::vector<PermitCheck> permitFoundationChecks(const Document& document) {
  std::vector<PermitCheck> out;
  std::vector<std::string> open;
  const auto addOpen = [&](const nlohmann::json& list) {
    for (const auto& u : list)
      if (u.is_string() && std::find(open.begin(), open.end(), u.get<std::string>()) == open.end()) open.push_back(u.get<std::string>());
  };
  const auto made = construction::checkFoundation(document);
  const nlohmann::json plans = made ? nlohmann::json::parse(*made, nullptr, false).value("plans", nlohmann::json::array())
                                    : nlohmann::json::array();
  for (const auto& plan : plans) {
    const std::string status = plan.value("status", std::string());
    const std::string id = plan.value("foundation", std::string());
    std::string details = status == "current" ? "土台から作った基礎で、今の土台と合っています。"
                          : status == "modified" ? "基礎の形・配置が手で直されています。"
                          : status == "needs_regeneration" ? "土台が変わりました。基礎を作り直します。"
                                                           : "基礎の元の土台を読めません。";
    for (const auto& r : plan.value("reasons", nlohmann::json::array()))
      if (r.is_string()) details += "\n" + r.get<std::string>();
    const nlohmann::json quantities = plan.value("quantities", nlohmann::json::object());
    for (const auto& [key, value] : quantities.items()) {
      const std::string part = key == "foundation_stem" ? "立上り" : key == "foundation_footing" ? "フーチング"
                               : key == "foundation_slab" ? "耐圧盤" : key == "foundation_pad" ? "束石" : key;
      if (value.is_object() && value.contains("volume_m3") && value.at("volume_m3").is_number())
        details += "\n" + part + "　" + number(value.at("volume_m3").get<double>(), 3) + " m³";
    }
    addOpen(plan.value("unassessed", nlohmann::json::array()));
    std::vector<ObjectId> targets;
    if (!id.empty() && document.find(id)) targets.push_back(id);
    out.push_back({plan.value("name", std::string("基礎")), status == "current" ? "入力済み" : "要確認", details, targets,
                   status != "current"});
  }
  if (plans.empty())
    out.push_back({"基礎", "未入力", "土台を置いてから、基礎を作ります（布基礎・べた基礎）。外部の基礎詳細図を使う時は、その図番を図書に付けます。",
                   {}, true});

  const auto supports = construction::foundationSupports(document);
  const nlohmann::json report = supports ? nlohmann::json::parse(*supports, nullptr, false) : nlohmann::json();
  const nlohmann::json rows = report.is_object() ? report.value("rows", nlohmann::json::array()) : nlohmann::json::array();
  if (rows.empty()) {
    out.push_back({"土台・柱の支え", "未評価", "下の面を照らす土台・柱がありません。", {}});
  } else {
    std::map<std::string, int> counts;
    std::vector<ObjectId> looseIds;
    std::string loose;
    for (const auto& row : rows) {
      const std::string status = row.value("status", std::string("not_evaluated"));
      ++counts[status];
      if (status == "detected") continue;
      if (row.contains("id") && row.at("id").is_string() && document.find(row.at("id").get<std::string>()))
        looseIds.push_back(row.at("id").get<std::string>());
      if (looseIds.size() <= 5) {
        loose += "\n" + row.value("name", std::string()) + "　" +
                 (status == "missing" ? "支えが見つからない" : status == "partial" ? "一部だけ支えに乗る" : status == "not_evaluated" ? "未評価" : status);
        if (const std::string why = row.value("reason", std::string()); !why.empty()) loose += "（" + why + "）";
      }
    }
    std::string details = "照らした土台・柱 " + std::to_string(rows.size()) + "：下の面が支えに乗る " +
                          std::to_string(counts["detected"]) + "、それ以外 " + std::to_string(rows.size() - counts["detected"]);
    details += loose;
    if (looseIds.size() > 5) details += "\nほか " + std::to_string(looseIds.size() - 5);
    const bool all = counts["detected"] == static_cast<int>(rows.size());
    out.push_back({"土台・柱の支え", all ? "形の上で支えあり" : "要確認", details, looseIds, !all});
    if (report.is_object()) {
      out.push_back({"照らした事", "参考", report.value("notice", std::string()), {}});
      addOpen(report.value("unassessed_supports", nlohmann::json::array()));
    }
  }
  std::string list;
  for (const auto& u : open) list += (list.empty() ? "" : "、") + u;
  out.push_back({"まだ確かめていない事", "未評価",
                 (list.empty() ? std::string("地盤・支持力、基礎の構造計算、配筋") : list) +
                     "。地盤の資料（地盤調査）と基礎伏図・配筋・人通口の記載、または外部の詳細図への参照を、申請先の様式で確かめます。",
                 {}});
  return out;
}

namespace {
// docs/210: the insulation given to a wall, slab or roof, read as people write it.
std::string insulationText(const Object& o) {
  const nlohmann::json j = nlohmann::json::parse(attribute(o, "insulation"), nullptr, false);
  if (!j.is_object()) return {};
  std::string text = j.value("material", std::string()) + " " + number(j.value("thickness_mm", 0.0), 0) + " mm";
  const std::string placement = j.value("placement", std::string());
  if (!placement.empty()) text += placement == "fill" ? "（充填）" : placement == "outside" ? "（外張り）" : "（内張り）";
  if (j.contains("resistance")) text += "　R " + number(j.at("resistance").get<double>(), 2);
  else if (j.contains("conductivity") && j.at("conductivity").get<double>() > 0)
    text += "　λ " + number(j.at("conductivity").get<double>(), 3) + "（R " +
            number(j.value("thickness_mm", 0.0) / 1000 / j.at("conductivity").get<double>(), 2) + "）";
  return text;
}

// docs/225: the openings of the outer walls (外皮) as the examples' 仕様表 (外皮基準 開口部): 部位
// (窓 / ドア / 開口（建具なし）), 符号, 仕様 (fitting, product number, glass), 性能値 (heat transmittance U), 数, 資料.
// The region given for the energy documents (敷地 > 省エネの適用ルート・地域, docs/210); 0: none.
int energyRegionOf(const Document& document) {
  if (const auto& site = document.settings().siteBoundary)
    if (const Object* outline = document.find(site->outline)) return std::atoi(attribute(*outline, "energy_region").c_str());
  return 0;
}

// docs/229, docs/230: the 仕様基準's values for openings, as 告示第266号 1(3) itself gives them (最終改正
// 令和6年告示第975号, read 2026-11-21): イ the heat transmittance of 開口部 (doors too) at most 2.3
// (regions 1-3), 3.5 (4), 4.7 (5-7), none (8); ロ for openings mostly of transparent material, in a
// detached house, in regions 5-7 an opening's solar gain at most 0.59 and in 8 at most 0.53 (or glass
// 0.73 / 0.66, fittings outside, eaves: not modelled).
double windowSpecU(int region) { return region <= 3 ? 2.3 : region == 4 ? 3.5 : region <= 7 ? 4.7 : -1; }
double windowSpecEta(int region) { return region >= 5 && region <= 7 ? 0.59 : region == 8 ? 0.53 : -1; }

draw::Table openingSpecTable(const Document& document) {
  draw::Table t;
  t.title = "開口部仕様表";
  t.headers = {"部位", "符号", "仕様又は使用材料等", "性能値", "数", "資料", "基準値(参考)", "照合(参考)"};
  const int region = energyRegionOf(document);
  struct Row {
    std::vector<std::string> cells;
    int count = 0;
  };
  std::map<std::string, Row> rows;
  bool anyRated = false;
  for (const OpeningRow& o : openingSchedule(document).rows) {
    if (o.inner) continue;  // the envelope only
    std::string spec = o.fitting;
    if (!o.product.empty()) spec += (spec.empty() ? "" : "　") + std::string("品番 ") + o.product;
    if (!o.glass.empty()) spec += (spec.empty() ? "" : "　") + o.glass;
    const std::string value = o.uValue < 0 ? "未入力"
                              : "U=" + number(o.uValue, 2) + (o.solarGain >= 0 ? "　η=" + number(o.solarGain, 2) : std::string());
    anyRated = anyRated || o.uValue >= 0;
    const std::string part = o.kind == "door" ? "ドア" : o.kind == "window" ? "窓" : "開口（建具なし）";
    Row& r = rows[(o.kind == "door" ? "1" : o.kind == "window" ? "0" : "2") + o.mark + "｜" + spec + "｜" + value];
    // docs/229: the reference values and how they compare. docs/230: doors by their U only (the solar
    // gain is for openings mostly of transparent material).
    std::string need, verdict;
    if (region < 1 || region > 8) {
      need = "地域未入力";
      verdict = "未評価";
    } else {
      const double u = windowSpecU(region), eta = o.kind == "door" ? -1.0 : windowSpecEta(region);
      std::vector<std::string> needs, verdicts;
      if (u > 0) {
        needs.push_back("U≦" + number(u, 1));
        verdicts.push_back(o.uValue < 0 ? "U 未評価" : o.uValue <= u + 1e-9 ? "U 満たす" : "U 不足");
      }
      if (eta > 0) {
        needs.push_back("η≦" + number(eta, 2));
        verdicts.push_back(o.solarGain < 0 ? "η 未入力" : o.solarGain <= eta + 1e-9 ? "η 満たす" : "η 他の方法で");
      }
      if (needs.empty()) {
        need = "基準なし";
        verdict = "対象外";
      } else {
        for (std::size_t i = 0; i < needs.size(); ++i) {
          need += (i ? "　" : "") + needs[i];
          verdict += (i ? "・" : "") + verdicts[i];
        }
      }
    }
    if (r.count == 0) r.cells = {part, o.mark, spec, value, "", o.performanceSource, need, verdict};
    ++r.count;
  }
  if (!anyRated) return t;  // no performance given: nothing to show yet (the door and window schedule has the rest)
  for (auto& [key, r] : rows) {
    r.cells[4] = std::to_string(r.count);
    t.rows.push_back(r.cells);
  }
  t.notes = {"外壁の窓・ドア・開口です。性能値は入れた熱貫流率 U（W/(m2K)）と日射熱取得率 η（入れた物）。",
             "基準値(参考)：仕様基準（平成28年国土交通省告示第266号、最終改正 令和6年告示第975号）1(3) の開口部の熱貫流率と"
             "日射遮蔽（5〜8 地域）の値。告示の本文から取った（2026-11-21）。ドアは熱貫流率だけ（日射遮蔽は透明な開口部が対象）。",
             "日射遮蔽は、窓の η のほか、ガラスの η（5〜7 地域 0.73 以下・8 地域 0.66 以下）・付属部材・ひさしや軒でも満たせる"
             "（「η 他の方法で」）。合否の判定ではありません。"};
  // docs/237: 告示 1(3)イ・ロ: windows whose total area is at most 2 % of the floor area may be left out
  // of the heat transmittance rule, and openings (not skylights) at most 4 % of the solar-gain rule.
  // Windows here: the outer walls' windows and plain openings (no skylights are modelled).
  double windows = 0.0;
  for (const OpeningRow& o : openingSchedule(document).rows)
    if (!o.inner && o.kind != "door") windows += o.width * o.height;
  const AreaTable areas = areaTable(document);
  const double floor = areas.legalTotal > 0.0 ? areas.legalTotal : areas.enclosedTotal;
  if (floor <= 0.0) {
    t.notes.push_back("小さい窓の除外（床面積の 2％・4％以下）：床面積が出ないので見ていません。");
  } else {
    const double share = windows / floor * 100.0;
    t.notes.push_back("窓の面積の合計 " + number(windows / 1e6, 2) + " m2（床面積 " + number(floor / 1e6, 2) + " m2 の " +
                      number(share, 1) + "％）：" +
                      (share <= 2.0 ? "2％以下なので、熱貫流率の基準から除くことができます（告示 1(3)イ）。"
                                    : "2％を超えるので、熱貫流率の基準から除くことはできません。") +
                      (share <= 4.0 ? "4％以下なので、日射遮蔽の基準から除くことができます（同ロ、天窓を除く）。"
                                    : "4％を超えるので、日射遮蔽の基準から除くことはできません。"));
  }
  return t;
}

draw::Table materialTableOf(const Document& document) {
  draw::Table t;
  t.title = "使用構造材料一覧表";
  t.headers = {"部位", "断面 (mm)", "数", "材料（樹種等）", "等級・規格", "資料"};
  const auto made = construction::takeOff(document);
  if (!made) return t;
  const auto report = nlohmann::json::parse(*made, nullptr, false);
  if (!report.is_object() || !report.contains("rows")) return t;
  const auto given = Modeler::structuralMaterialsOf(document);
  const auto lookup = [&](const std::string& role) -> const Modeler::StructuralMaterial* {
    for (const auto& m : given)
      if (m.role == role) return &m;
    for (const auto& m : given)
      if (m.role == "default") return &m;
    return nullptr;
  };
  // Rows of the take-off grouped by role and section (the lengths are left to the take-off table).
  std::vector<std::pair<std::string, std::string>> order;
  std::map<std::pair<std::string, std::string>, std::pair<std::string, int>> grouped;  // -> (role name, count)
  for (const auto& row : report.at("rows")) {
    const std::string role = row.value("role", std::string());
    if (role == "unclassified") continue;
    const std::string section = row.value("section", std::string());
    const auto key = std::make_pair(role, section);
    auto [it, fresh] = grouped.try_emplace(key, row.value("role_name", role), 0);
    if (fresh) order.push_back(key);
    it->second.second += row.value("count", 0);
  }
  // In the order of the guide's roles (sill, floor girder ... rafter).
  // (as roles.json lists them; the parsed object would sort the keys)
  std::map<std::string, std::size_t> rank;
  for (const char* role : {"sill", "floor_girder", "floor_post", "joist", "floor_sheathing", "through_column", "column",
                           "stud", "rim_beam", "floor_beam", "eave_beam", "roof_beam", "horizontal_brace", "brace",
                           "roof_post", "purlin", "ridge", "rafter", "lintel", "window_sill"})
    rank.emplace(role, rank.size());
  std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
    const auto ra = rank.count(a.first) ? rank.at(a.first) : rank.size();
    const auto rb = rank.count(b.first) ? rank.at(b.first) : rank.size();
    return ra < rb;
  });
  int missing = 0;
  for (const auto& key : order) {
    const auto& [name, count] = grouped.at(key);
    const auto* m = lookup(key.first);
    if (!m) ++missing;
    t.rows.push_back({name, key.second, std::to_string(count), m ? m->material : "未入力", m ? (m->grade.empty() ? "未入力" : m->grade) : "未入力",
                      m ? m->source : ""});
  }
  t.notes = {"部位・断面・数は木造架構の部材（拾い出し）から。材料・等級・規格は入力した値で、品質の確認はしていません。"};
  if (missing > 0) t.notes.push_back("材料が未入力の部位が " + std::to_string(missing) + " 行あります（set_structural_materials）。");
  return t;
}

// docs/224: the equipment table (機器表) as the examples of 省エネ drawings carry it: 部位 (kind),
// 仕様又は使用材料等 (name, model), 性能値, 数, 資料.
draw::Table equipmentTable(const Document& document) {
  draw::Table t;
  t.title = "機器表";
  t.headers = {"部位", "仕様又は使用材料等", "性能値", "数", "資料"};
  const auto& kinds = Modeler::equipmentKinds();
  const auto items = Modeler::equipmentOf(document);
  for (const auto& [key, label] : kinds)
    for (const auto& e : items)
      if (e.kind == key)
        t.rows.push_back({label, e.name + (e.model.empty() ? "" : "　型番：" + e.model), e.value.empty() ? "未入力" : e.value,
                          std::to_string(e.count), e.source});
  t.notes = {"入れた機器と資料の一覧です。基準値との照合・一次エネルギー消費量の計算はしていません。"};
  // docs/238: what the 仕様基準 asks of each kind of equipment in the table, as 告示第266号 2 gives it
  // (最終改正 令和6年告示第975号, read 2026-11-21), for the site's region. Only for reference: the kind of
  // machine (room air conditioner, gas boiler...) is not known, so nothing is judged.
  const int region = energyRegionOf(document);
  const auto has = [&](const std::string& kind) {
    return std::any_of(items.begin(), items.end(), [&](const Modeler::EquipmentItem& e) { return e.kind == kind; });
  };
  std::vector<std::string> spec;
  if (has("heating_cooling")) {
    if (region == 0) {
      spec.push_back("暖房：地域の区分が未入力なので出していません。");
    } else if (region == 8) {
      spec.push_back("暖房：8 地域は基準がありません。");
    } else {
      spec.push_back("暖房（全体）：ダクト式セントラル空調機（ヒートポンプ熱源）");
      if (region <= 4) {
        spec.push_back("暖房（居室のみ）：温水パネルラジエーター（石油熱源機 熱効率 83.0％以上、");
        spec.push_back("　ガス熱源機 78.9％以上、電気ヒートポンプ熱源機。配管に断熱被覆）、");
        spec.push_back("　強制対流式の密閉式石油ストーブ（熱効率 86.0％以上）、又はルームエアコン");
      } else {
        spec.push_back("暖房（居室のみ）：温水パネルラジエーター（石油熱源機 熱効率 87.8％以上、");
        spec.push_back("　ガス熱源機 82.5％以上、電気ヒートポンプ熱源機。配管に断熱被覆）、又はルームエアコン");
      }
      spec.push_back("　（暖房能力÷消費電力 ≧ −0.321×暖房能力(kW)＋6.16）");
    }
    spec.push_back("冷房（全体）：ダクト式セントラル空調機（ヒートポンプ熱源）");
    spec.push_back("冷房（居室のみ）：ルームエアコン（冷房能力÷消費電力 ≧ −0.504×冷房能力(kW)＋5.88）");
  }
  if (has("ventilation")) {
    spec.push_back("換気：比消費電力 0.3 W/(m3/h) 以下（熱交換型は有効換気量率で除した値）、");
    spec.push_back("　内径 75 mm 以上のダクトと直流電動機のダクト式第一種（熱交換なし）、");
    spec.push_back("　内径 75 mm 以上のダクト式第二種・第三種、又は壁付式第二種・第三種");
  }
  if (has("lighting")) spec.push_back("照明：非居室に白熱灯又はこれと同等以下の性能の照明を使わない");
  if (has("hot_water")) {
    if (region == 0) {
      spec.push_back("給湯：地域の区分が未入力なので出していません。");
    } else if (region <= 4) {
      static const char* kHeatPump[] = {"", "3.5", "3.2", "3.0", "2.9"};
      spec.push_back("給湯：石油給湯機 モード熱効率 81.3％以上、ガス給湯機 83.7％以上、");
      spec.push_back(std::string("　又は CO2 冷媒の電気ヒートポンプ給湯機（年間給湯保温効率又は年間給湯効率 ") + kHeatPump[region] +
                     " 以上）");
    } else {
      spec.push_back("給湯：石油給湯機 モード熱効率 77.8％以上、ガス給湯機 78.2％以上、");
      spec.push_back("　又は CO2 冷媒の電気ヒートポンプ給湯機");
    }
    // docs/239: 告示 2 ただし書き: with no 浴室等, 台所 and 洗面所, (5) does not apply. Only noted when
    // there are rooms and none of them is named (or used) as one of these; the designer decides.
    const auto rooms = roomSchedule(document).rows;
    static const char* kWet[] = {"浴室", "風呂", "バス", "シャワー", "台所", "キッチン", "洗面"};
    const bool wet = std::any_of(rooms.begin(), rooms.end(), [](const RoomRow& r) {
      return std::any_of(std::begin(kWet), std::end(kWet), [&](const char* w) {
        return r.name.find(w) != std::string::npos || r.use.find(w) != std::string::npos;
      });
    });
    if (!rooms.empty() && !wet) {
      spec.push_back("　室の名前に浴室・台所・洗面所が見当たりません。どれも無い住戸には、");
      spec.push_back("　給湯の基準は適用しません（告示第266号 2 ただし書き）。");
    }
  }
  if (!spec.empty()) {
    t.notes.push_back(std::string("仕様基準（告示第266号 2") + (region ? "、" + std::to_string(region) + " 地域" : std::string()) +
                      "）の参考。どれかに当たるか、計算で同等以上：");
    t.notes.insert(t.notes.end(), spec.begin(), spec.end());
  }
  return t;
}

// docs/223: the insulation specification table put beside the insulation drawings, as the examples
// of 省エネ drawings carry one on the same sheet (部位・仕様又は使用材料等・厚さ・性能値): one row per
// part and insulation, with how many walls / floors / roofs have it.
// docs/228: the thermal resistance the 仕様基準 asks of insulation in a timber house (木造軸組), by
// part, method and region: 告示第266号 1(2)ロ(イ)'s table for a detached house (最終改正 令和6年告示第
// 975号), checked against the notice's own text 2026-11-21 (docs/230; regions 4 and 5-7 have the same
// values for timber). Columns 1-2, 3, 4-7, 8; below 0: no value for that region. Floors: その他の部分
// (whether a floor faces outside air is not known).
struct SpecR {
  double fill[4], outside[4];
};
const SpecR kSpecRoof{{6.6, 4.6, 4.6, 0.96}, {5.7, 4.0, 4.0, 0.78}};
const SpecR kSpecCeiling{{5.7, 4.0, 4.0, 0.78}, {5.7, 4.0, 4.0, 0.78}};  // docs/236: 天井 (屋根又は天井 for 外張)
const SpecR kSpecWall{{3.3, 2.2, 2.2, -1}, {2.9, 1.7, 1.7, -1}};
const SpecR kSpecFloorOther{{3.3, 3.3, 2.2, -1}, {-1, -1, -1, -1}};
const SpecR kSpecFloorOutside{{5.2, 5.2, 3.3, -1}, {3.8, 3.8, 2.5, -1}};
int specColumn(int region) { return region <= 2 ? 0 : region == 3 ? 1 : region <= 7 ? 2 : 3; }

draw::Table insulationSpecTable(const Document& document) {
  draw::Table t;
  t.title = "断熱仕様表";
  t.headers = {"部位", "仕様又は使用材料等", "厚さ(mm)", "性能値", "数", "基準値(参考)", "照合(参考)"};
  const int region = energyRegionOf(document);
  struct Row {
    std::string material, thickness, value, need, verdict;
    int count = 0;
  };
  std::map<std::pair<int, std::string>, Row> rows;  // (part order, key) -> row
  for (const Object& o : document.objects()) {
    const std::string kind = attribute(o, "kind");
    const int order = kind == "roof" ? 0 : kind == "ceiling" ? 1 : kind == "wall" ? 2 : kind == "slab" ? 3 : -1;  // docs/236
    if (order < 0) continue;
    const nlohmann::json j = nlohmann::json::parse(attribute(o, "insulation"), nullptr, false);
    if (!j.is_object() || j.value("material", std::string()).empty()) continue;
    const std::string placement = j.value("placement", std::string());
    Row row;
    row.material = j.value("material", std::string()) +
                   (placement == "fill" ? "（充填）" : placement == "outside" ? "（外張り）" : placement == "inside" ? "（内張り）" : "");
    row.thickness = number(j.value("thickness_mm", 0.0), 0);
    double r = -1;
    if (j.contains("resistance") && j.at("resistance").get<double>() > 0) r = j.at("resistance").get<double>();
    else if (j.contains("conductivity") && j.at("conductivity").get<double>() > 0)
      r = j.value("thickness_mm", 0.0) / 1000 / j.at("conductivity").get<double>();
    row.value = r > 0 ? "R=" + number(r, 2) : "未入力";
    // docs/228: the reference value and how it compares.
    const SpecR& spec = order == 0 ? kSpecRoof : order == 1 ? kSpecCeiling : order == 2 ? kSpecWall : kSpecFloorOther;
    if (region < 1 || region > 8) {
      row.need = "地域未入力";
      row.verdict = "未評価";
    } else if (placement.empty()) {
      row.need = "工法未入力";
      row.verdict = "未評価";
    } else {
      const double need = (placement == "fill" ? spec.fill : spec.outside)[specColumn(region)];
      if (need < 0) {
        row.need = "基準なし";
        row.verdict = "対象外";
      } else {
        row.need = "R≧" + number(need, need < 1 ? 2 : 1);
        row.verdict = r <= 0 ? "未評価" : r + 1e-9 >= need ? "満たす" : "不足";
      }
    }
    Row& slot = rows[{order, row.material + "｜" + row.thickness + "｜" + row.value}];
    if (slot.count == 0) slot = row;
    ++slot.count;
  }
  const char* parts[] = {"屋根", "天井", "壁", "床"};
  for (const auto& [key, r] : rows)
    t.rows.push_back({parts[key.first], r.material, r.thickness, r.value, std::to_string(r.count), r.need, r.verdict});
  t.notes = {"性能値は、入れた熱抵抗 R、または厚さ÷熱伝導率（m2K/W）。",
             "基準値(参考)：仕様基準（平成28年国土交通省告示第266号、最終改正 令和6年告示第975号）1(2)ロ の断熱材の熱抵抗の"
             "基準値、一戸建て・木造軸組構法の値。告示の本文と照らした（2026-11-21）。",
             "床は「その他の部分」の値（外気に接する床は、充填 5.2・5.2・3.3、外張 3.8・3.8・2.5）。天井断熱・基礎・枠組壁工法・"
             "熱貫流率 U による確かめ方は見ていない。合否の判定ではありません。申請先と申請時点の告示で確かめます。"};
  return t;
}
}  // namespace

std::vector<PermitCheck> permitEnergyChecks(const Document& document) {
  std::vector<PermitCheck> out;
  std::string route, region, source;
  if (const auto& site = document.settings().siteBoundary)
    if (const Object* outline = document.find(site->outline)) {
      route = attribute(*outline, "energy_route");
      region = attribute(*outline, "energy_region");
      source = attribute(*outline, "energy_source");
    }
  if (route.empty() || region.empty()) {
    out.push_back({"適用ルート・地域", "未入力",
                   "どの基準・計算の方法で出すか（仕様基準・性能基準など）と、地域の区分（1〜8）を入れます（AI の set_energy_route、"
                   "敷地境界が要ります）。" +
                       (route.empty() ? std::string() : "\n適用ルート：入力済み") + (region.empty() ? std::string() : "\n地域：" + region + " 地域"),
                   {}, true});
  } else {
    out.push_back({"適用ルート・地域", "入力済み",
                   (route == "spec" ? std::string("仕様基準") : route == "performance" ? std::string("性能基準") : std::string("その他")) + "　" + region +
                       " 地域\n確認資料 " + source + "\n基準の値との比較はしていません。",
                   {}});
  }
  // Walls by build-up: the insulation given, or a layer named as insulation.
  struct BuildUp {
    std::string name, layers, insulation;
    int insulated = 0;
    std::vector<ObjectId> walls;
  };
  std::map<std::string, BuildUp> buildUps;
  std::vector<ObjectId> plain;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "wall") continue;
    const std::string type = attribute(o, "wall_type");
    if (type.empty() && attribute(o, "insulation").empty()) {
      plain.push_back(o.id);
      continue;
    }
    BuildUp& b = buildUps[type.empty() ? "(厚さだけ)" : type];
    b.walls.push_back(o.id);
    const std::string given = insulationText(o);
    bool layered = false;
    if (b.name.empty()) {
      b.name = type.empty() ? "厚さだけの壁" : attribute(o, "wall_type_name");
      const nlohmann::json layers = nlohmann::json::parse(attribute(o, "wall_layers"), nullptr, false);
      for (const auto& l : layers.is_array() ? layers : nlohmann::json::array()) {
        const std::string n = l.value("name", std::string());
        b.layers += (b.layers.empty() ? "" : "／") + n + " " + number(l.value("thickness", 0.0), 1);
      }
    }
    layered = b.layers.find("断熱") != std::string::npos;
    if (!given.empty() || layered) ++b.insulated;
    if (!given.empty() && b.insulation.find(given) == std::string::npos) b.insulation += (b.insulation.empty() ? "" : "、") + given;
  }
  for (const auto& [type, b] : buildUps) {
    const bool all = b.insulated == static_cast<int>(b.walls.size());
    std::string details = std::to_string(b.walls.size()) + " 枚" + (b.layers.empty() ? std::string() : "　外→内：" + b.layers);
    if (!b.insulation.empty()) details += "\n断熱：" + b.insulation;
    if (!all) details += "\n断熱の入っていない壁 " + std::to_string(b.walls.size() - b.insulated) + " 枚（AI の set_insulation で入れます）";
    out.push_back({"壁の仕様：" + b.name, all ? "入力済み" : "未入力", details, b.walls, !all});
  }
  if (!plain.empty())
    out.push_back({"仕様の無い壁", "未入力", "厚さだけで断熱も無い壁 " + std::to_string(plain.size()) + " 枚。仕様（層）か断熱を入れます。", plain, true});
  std::vector<ObjectId> bare;
  int parts = 0;
  for (const Object& o : document.objects()) {
    const std::string kind = attribute(o, "kind");
    if (kind != "slab" && kind != "roof") continue;
    ++parts;
    if (attribute(o, "insulation").empty()) bare.push_back(o.id);
  }
  if (parts > 0)
    out.push_back({"床・屋根の断熱", bare.empty() ? "入力済み" : "未入力",
                   "床・屋根 " + std::to_string(parts) + " のうち、断熱を入れた物 " + std::to_string(parts - static_cast<int>(bare.size())) + "。",
                   bare, !bare.empty()});
  const auto openings = openingSchedule(document).rows;
  int rated = 0;
  std::vector<ObjectId> hosts;
  for (const OpeningRow& o : openings) {
    if (o.uValue >= 0) ++rated;
    else if (std::find(hosts.begin(), hosts.end(), o.host) == hosts.end()) hosts.push_back(o.host);
  }
  if (!openings.empty())
    out.push_back({"建具の性能", rated == static_cast<int>(openings.size()) ? "入力済み" : "未入力",
                   "開口 " + std::to_string(openings.size()) + " 件のうち、熱貫流率を入れた物 " + std::to_string(rated) +
                       "（AI の set_opening_performance）。",
                   hosts, rated != static_cast<int>(openings.size())});
  {
    // docs/232: whether the energy plan (建築物エネルギー消費性能確保計画) is submitted, as given in the
    // project outline (第二面 8); the person's call.
    const auto outline = projectOutline(document);
    const auto plan = outline.find("energy_plan");
    if (plan == outline.end())
      out.push_back({"省エネ適判（確保計画の提出）", "未入力",
                     "確認申請書 第二面 8 の「提出済・未提出・提出不要」を、計画概要の項目で選びます（判断は申請先の案内で）。", {}, true});
    else
      out.push_back({"省エネ適判（確保計画の提出）", "入力済み", projectOutlineText("energy_plan", plan->second) + "（計画概要の項目から）",
                     {}, plan->second == "not_yet"});
  }
  {
    // docs/224: the equipment given (set_equipment): the four kinds the energy documents ask for.
    const auto items = Modeler::equipmentOf(document);
    std::vector<std::string> missing;
    for (const char* kind : {"heating_cooling", "ventilation", "hot_water", "lighting"})
      if (std::none_of(items.begin(), items.end(), [&](const Modeler::EquipmentItem& e) { return e.kind == kind; }))
        for (const auto& [key, label] : Modeler::equipmentKinds())
          if (key == kind) missing.push_back(label);
    std::string listed;
    for (const auto& e : items) listed += (listed.empty() ? "" : "、") + e.name;
    if (items.empty())
      out.push_back({"設備", "未入力", "暖冷房・換気・給湯・照明の機器を入れます（AI の set_equipment）。機器表になり、断熱範囲図の紙に載ります。", {}, true});
    else if (!missing.empty()) {
      std::string text = "入れた機器：" + listed + "\n入っていない種類：";
      for (std::size_t i = 0; i < missing.size(); ++i) text += (i ? "・" : "") + missing[i];
      out.push_back({"設備", "未入力", text, {}, true});
    } else {
      out.push_back({"設備", "入力済み", "入れた機器：" + listed + "\n性能値と基準値の照合・一次エネルギー消費量の計算はしていません。", {}, false});
    }
  }
  out.push_back({"まだ確かめていない事", "未評価",
                 "仕様基準の値（地域ごとの断熱材の熱抵抗・開口部の熱貫流率など）との比較、外皮の性能・一次エネルギー消費量の計算、"
                 "省エネ基準への適合。告示の原文を確かめてから照らします。",
                 {}});
  return out;
}

const std::vector<std::pair<std::string, std::string>>& heightZoneChoices() {
  static const std::vector<std::pair<std::string, std::string>> choices = {
      {"low_1", "第一種低層住居専用地域"}, {"low_2", "第二種低層住居専用地域"},
      {"mid_high_1", "第一種中高層住居専用地域"}, {"mid_high_2", "第二種中高層住居専用地域"},
      {"residential_1", "第一種住居地域"}, {"residential_2", "第二種住居地域"},
      {"quasi_residential", "準住居地域"}, {"rural_residential", "田園住居地域"},
      {"neighborhood_commercial", "近隣商業地域"}, {"commercial", "商業地域"},
      {"quasi_industrial", "準工業地域"}, {"industrial", "工業地域"},
      {"exclusive_industrial", "工業専用地域"}, {"none", "用途地域の指定なし"}};
  return choices;
}

const std::vector<std::pair<std::string, std::string>>& heightDistrictChoices() {
  static const std::vector<std::pair<std::string, std::string>> choices = {
      {"yokohama_1", "横浜市 第1種高度地区"}, {"yokohama_2", "横浜市 第2種高度地区"},
      {"yokohama_3", "横浜市 第3種高度地区"}, {"yokohama_4", "横浜市 第4種高度地区"},
      {"yokohama_5", "横浜市 第5種高度地区"}, {"yokohama_6", "横浜市 第6種高度地区"},
      {"yokohama_7", "横浜市 第7種高度地区"}, {"none", "高度地区の指定なし"}, {"other", "その他の高度地区"}};
  return choices;
}

SiteHeightConditions siteHeightConditions(const Document& document) {
  const auto& site = document.settings().siteBoundary;
  const auto* outline = site ? document.find(site->outline) : nullptr;
  if (!outline) return {};
  const auto data = nlohmann::json::parse(attribute(*outline, "height_conditions"), nullptr, false);
  if (!data.is_object()) return {};
  const auto read = [&](const char* key) {
    const auto it = data.find(key);
    return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  return {read("zone"), read("district"), read("source")};
}

std::optional<YokohamaHeightDistrictRule> yokohamaHeightDistrictRule(const std::string& district) {
  static const std::pair<std::string_view, YokohamaHeightDistrictRule> rules[] = {
      {"yokohama_1", {10000, 5000}}, {"yokohama_2", {12000, 5000}}, {"yokohama_3", {15000, 7000}},
      {"yokohama_4", {20000, 7500}}, {"yokohama_5", {20000, 10000}},
      {"yokohama_6", {20000, std::nullopt}}, {"yokohama_7", {31000, std::nullopt}}};
  for (const auto& [code, rule] : rules)
    if (district == code) return rule;
  return std::nullopt;
}

Result<RoadHeightDatum> roadHeightDatum(double roadCenterZMm, double siteGroundZMm, double roadWidthMm, double slope) {
  using R = Result<RoadHeightDatum>;
  if (!std::isfinite(roadCenterZMm) || !std::isfinite(siteGroundZMm))
    return R::failure("路面中心と敷地地盤面の高さを、同じモデルZの基準で入力してください");
  if (!std::isfinite(roadWidthMm) || roadWidthMm <= 0 || !std::isfinite(slope) || slope <= 0 || slope > 1000)
    return R::failure("正の道路幅員と、0より大きく1000以下の勾配を入力してください");

  // Order Art. 2(1)(vi)(a) and Art. 135-2(1): only a higher site can raise the road datum.
  const double difference = siteGroundZMm - roadCenterZMm;
  const double rise = std::max(0.0, (difference - 1000) / 2);
  const double adopted = roadCenterZMm + rise;
  const double edgeHeight = adopted + roadWidthMm * slope;
  if (!std::isfinite(difference) || !std::isfinite(edgeHeight))
    return R::failure("高さの差または斜線の高さを測れる範囲を超えています");
  return RoadHeightDatum{difference, rise, adopted, edgeHeight};
}

Result<RoadHeightTableValues> roadHeightTableValues(const std::string& zone, double floorAreaLimitPercent) {
  using R = Result<RoadHeightTableValues>;
  if (zone.empty()) return R::failure("先に敷地の「高さ制限の条件」で用途地域を登録してください");
  if (!std::isfinite(floorAreaLimitPercent) || floorAreaLimitPercent <= 0)
    return R::failure("法第52条による容積率の限度を0より大きい数値で入力してください");

  // Building Standards Act, Appendix 3 (items 1 through 3). The upper bounds are inclusive.
  if (zone == "neighborhood_commercial" || zone == "commercial") {
    constexpr double upperBounds[] = {400, 600, 800, 1000, 1100, 1200};
    const auto* band = std::lower_bound(std::begin(upperBounds), std::end(upperBounds), floorAreaLimitPercent);
    return RoadHeightTableValues{2, 1.5, 20000.0 + 5000.0 * (band - std::begin(upperBounds)), false};
  }

  constexpr double upperBounds[] = {200, 300, 400};
  const auto* band = std::lower_bound(std::begin(upperBounds), std::end(upperBounds), floorAreaLimitPercent);
  const double distance = 20000.0 + 5000.0 * (band - std::begin(upperBounds));
  if (zone == "quasi_industrial" || zone == "industrial" || zone == "exclusive_industrial")
    return RoadHeightTableValues{3, 1.5, distance, false};
  if (zone == "low_1" || zone == "low_2" || zone == "rural_residential")
    return RoadHeightTableValues{1, 1.25, distance, false};
  if (zone == "mid_high_1" || zone == "mid_high_2" || zone == "residential_1" || zone == "residential_2" ||
      zone == "quasi_residential")
    return RoadHeightTableValues{1, 1.25, distance, true};

  return R::failure("この用途地域には別表第三の一〜三の標準値を当てはめられません");
}

namespace {

struct RoadHeightReference {
  ObjectId outline;
  std::vector<gp_Pnt> vertices;
  gp_Pnt origin;
  gp_Dir inward;
  gp_Trsf frame;
};
Result<RoadHeightReference> roadHeightReference(const Document& document) {
  using R = Result<RoadHeightReference>;
  const auto& site = document.settings().siteBoundary;
  if (!site || !site->road) return R::failure("敷地境界と前面道路を登録してください");
  auto geometry = document.siteBoundaryGeometry(site->outline);
  if (!geometry) return R::failure(geometry.error());
  const auto road = document.siteRoadEdge(*geometry);
  if (!road) return R::failure("道路が接する敷地辺を登録し直してください");
  const gp_Pnt& from = (*road)[0];
  const gp_Pnt& to = (*road)[1];
  const gp_Dir along(to.X() - from.X(), to.Y() - from.Y(), 0);
  gp_Dir inward(-along.Y(), along.X(), 0);
  double low = 0, high = 0;
  for (const gp_Pnt& point : geometry->vertices) {
    const double d = gp_Vec(from, point).Dot(gp_Vec(inward));
    low = std::min(low, d);
    high = std::max(high, d);
  }
  constexpr double sideTolerance = 0.01;  // mm
  if (low < -sideTolerance && high > sideTolerance)
    return R::failure("敷地が道路境界線の両側にあるため、この斜線では試算できません");
  const bool flip = low < -sideTolerance;
  const double depth = flip ? -low : high;
  if (depth <= sideTolerance) return R::failure("敷地の奥行きを測れません");
  if (flip) inward.Reverse();
  gp_Trsf roadFrame;
  roadFrame.SetTransformation(gp_Ax3(from, gp::DZ(), inward));
  return RoadHeightReference{site->outline, std::move(geometry->vertices), from, inward, roadFrame};
}

// In this frame X is distance inward from the site-side road edge.
Result<TopoDS_Shape> clipRoadHeightRange(const TopoDS_Shape& world, const gp_Trsf& roadFrame,
                                       double lowerX, double upperX, bool includeBoundaryContacts = true) {
  using R = Result<TopoDS_Shape>;
  const TopoDS_Shape local = world.Moved(TopLoc_Location(roadFrame));
  const Bnd_Box box = geom::exactBounds(local);
  if (box.IsVoid() || box.IsOpen()) return R::failure("形状の範囲を測れません");
  const gp_Pnt lo = box.CornerMin(), hi = box.CornerMax();
  // Leave near-tangencies to OCCT's intersection tolerance.
  constexpr double toleranceMm = 1e-6;
  if (hi.X() < lowerX - toleranceMm || lo.X() > upperX + toleranceMm) return TopoDS_Shape();
  if (lo.X() >= lowerX && hi.X() <= upperX) return world;
  const TopoDS_Shape band = BRepPrimAPI_MakeBox(
      gp_Pnt(lowerX, lo.Y() - 1, lo.Z() - 1), gp_Pnt(upperX, hi.Y() + 1, hi.Z() + 1)).Shape();
  BRepAlgoAPI_Common common;
  TopTools_ListOfShape arguments, tools;
  arguments.Append(local);
  tools.Append(band);
  common.SetArguments(arguments);
  common.SetTools(tools);
  common.SetNonDestructive(true);
  common.Build();
  if (!common.IsDone() || common.HasErrors()) return R::failure("適用範囲で形状を切れません");
  const auto backToWorld = TopLoc_Location(roadFrame.Inverted());
  const bool hasCommon = !geom::exactBounds(common.Shape()).IsVoid();
  if (!includeBoundaryContacts) return hasCommon ? common.Shape().Moved(backToWorld) : TopoDS_Shape();

  // An exact point, edge or face contact at the inclusive distance boundary
  // may be absent from the volume common. Check both delimiting planes.
  BRep_Builder builder;
  TopoDS_Compound clipped;
  builder.MakeCompound(clipped);
  if (hasCommon) builder.Add(clipped, common.Shape());
  for (const double x : {lowerX, upperX}) {
    if (x < lo.X() - toleranceMm || x > hi.X() + toleranceMm) continue;
    BRepAlgoAPI_Section section(local, gp_Pln(gp_Pnt(x, 0, 0), gp::DX()), false);
    section.SetNonDestructive(true);
    section.Build();
    if (!section.IsDone() || section.HasErrors()) return R::failure("適用距離の境界で交わりを測れません");
    if (!geom::exactBounds(section.Shape()).IsVoid()) builder.Add(clipped, section.Shape());
  }
  if (geom::exactBounds(clipped).IsVoid()) return TopoDS_Shape();
  return clipped.Moved(backToWorld);
}

}  // namespace

Result<RoadBuildingSetback> measureRoadBuildingSetback(
    const Document& document, const std::vector<ObjectId>& ids) {
  using R = Result<RoadBuildingSetback>;
  if (ids.empty()) return R::failure("後退距離の算定に含める部材を選択してください");
  const auto road = roadHeightReference(document);
  if (!road) return R::failure(road.error());
  const TopLoc_Location toRoad(road->frame);
  std::optional<RoadBuildingSetback> nearest;
  for (const auto& id : std::set<ObjectId>(ids.begin(), ids.end())) {
    const auto* object = document.find(id);
    if (!object || id == road->outline) return R::failure("敷地境界以外の部材を選択してください");
    const TopoDS_Shape shape = document.worldShape(*object);
    if (shape.IsNull() || !TopExp_Explorer(shape, TopAbs_FACE).More())
      return R::failure("面を持つ部材を選択してください（" + object->name + "）");
    const Bnd_Box bounds = geom::exactBounds(shape.Moved(toRoad));
    if (bounds.IsVoid() || bounds.IsOpenXmin())
      return R::failure("道路までの距離を測れません（" + object->name + "）");
    const double distance = std::max(0.0, bounds.CornerMin().X());
    if (!nearest || distance < nearest->distanceMm) nearest = RoadBuildingSetback{id, object->name, distance};
  }
  return *nearest;
}

namespace {

struct RoadHeightParameters {
  double adjustedHeight, lowerX, upperX, rise;
};

Result<RoadHeightParameters> roadHeightParameters(double edgeHeightMm, double slope,
                                                   std::optional<RoadHeightPlaneRange> range) {
  using R = Result<RoadHeightParameters>;
  if (!std::isfinite(edgeHeightMm) || !std::isfinite(slope) || slope <= 0 || slope > 1000)
    return R::failure("有効な基準高さと、0より大きく1000以下の勾配を入力してください");
  if (range && (!std::isfinite(range->roadWidthMm) || !std::isfinite(range->applicationDistanceMm) ||
                range->roadWidthMm <= 0 || range->applicationDistanceMm <= range->roadWidthMm))
    return R::failure("道路幅員は正の値、適用距離は道路幅員より大きい値を入力してください");
  const double setback = range ? range->buildingSetbackMm : 0;
  const double rise = slope * setback;
  const double adjustedHeight = edgeHeightMm + rise;
  const double lowerX = range ? -(range->roadWidthMm + setback) : 0;
  const double upperX = range ? range->applicationDistanceMm + lowerX : 0;
  if (setback < 0 || !std::isfinite(adjustedHeight) || !std::isfinite(lowerX) ||
      (range && upperX <= lowerX))
    return R::failure("建物の後退距離は0以上の有効な値を入力してください");
  return RoadHeightParameters{adjustedHeight, lowerX, upperX, rise};
}

}  // namespace

Result<RoadHeightPlaneComparison> compareRoadHeightPlane(
    const Document& document, const std::vector<ObjectId>& ids, double edgeHeightMm, double slope,
    std::optional<RoadHeightPlaneRange> range) {
  using R = Result<RoadHeightPlaneComparison>;
  if (ids.empty()) return R::failure("照合する部材を選択してください");
  const auto parameters = roadHeightParameters(edgeHeightMm, slope, range);
  if (!parameters) return R::failure(parameters.error());
  const auto [adjustedHeight, lowerX, upperX, rise] = *parameters;
  const auto road = roadHeightReference(document);
  if (!road) return R::failure(road.error());
  const auto& from = road->origin;
  const auto& inward = road->inward;
  const auto& roadFrame = road->frame;

  gp_Trsf local;
  // In this frame +Z is normal to the plane. Convert the maximum normal distance
  // back to the vertical excess z - adjustedHeight - slope * inwardDistance.
  local.SetTransformation(gp_Ax3(gp_Pnt(from.X(), from.Y(), adjustedHeight),
                                gp_Dir(-slope * inward.X(), -slope * inward.Y(), 1)));
  const double verticalScale = std::hypot(1.0, slope);
  std::vector<RoadHeightPlaneRow> rows;
  for (const ObjectId& id : std::set<ObjectId>(ids.begin(), ids.end())) {
    const Object* object = document.find(id);
    if (!object || id == road->outline) return R::failure("敷地境界以外の部材を選択してください");
    const TopoDS_Shape shape = document.worldShape(*object);
    if (shape.IsNull() || !TopExp_Explorer(shape, TopAbs_FACE).More())
      return R::failure("面を持つ部材を選択してください（" + object->name + "）");
    TopoDS_Shape compared = shape;
    if (range) {
      const auto clipped = clipRoadHeightRange(shape, roadFrame, lowerX, upperX);
      if (!clipped) return R::failure(clipped.error() + "（" + object->name + "）");
      compared = *clipped;
      if (compared.IsNull()) {
        rows.push_back({id, object->name, std::nullopt});
        continue;
      }
    }
    const Bnd_Box bounds = geom::exactBounds(compared.Moved(TopLoc_Location(local)));
    if (bounds.IsVoid() || bounds.IsOpenZmax()) return R::failure("形状の高さを測れません（" + object->name + "）");
    rows.push_back({id, object->name, bounds.CornerMax().Z() * verticalScale});
  }
  std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.excessMm > b.excessMm; });
  const auto applicationEnd = range ? std::optional<double>(upperX) : std::nullopt;
  if (range && upperX <= 0) return RoadHeightPlaneComparison{std::move(rows), {}, applicationEnd, rise};
  BRepBuilderAPI_MakePolygon outline;
  for (const gp_Pnt& point : road->vertices) {
    const double distance = gp_Vec(from, point).Dot(gp_Vec(inward));
    outline.Add(gp_Pnt(point.X(), point.Y(), adjustedHeight + slope * distance));
  }
  outline.Close();
  if (!outline.IsDone()) return R::failure("斜線の表示範囲を作れません");
  BRepBuilderAPI_MakeFace face(outline.Wire(), Standard_True);
  if (!face.IsDone()) return R::failure("斜線の表示面を作れません");
  TopoDS_Shape preview = face.Face();
  if (range) {
    const auto clipped = clipRoadHeightRange(preview, roadFrame, lowerX, upperX, false);
    if (!clipped) return R::failure(clipped.error());
    preview = *clipped;
  }
  return RoadHeightPlaneComparison{std::move(rows), preview, applicationEnd, rise};
}

namespace {

Result<TopoDS_Face> heightSiteFootprint(const SiteBoundaryGeometry& geometry) {
  using R = Result<TopoDS_Face>;
  const double z = geometry.vertices.front().Z();
  BRepBuilderAPI_MakePolygon outline;
  for (const auto& p : geometry.vertices) outline.Add(gp_Pnt(p.X(), p.Y(), z));
  outline.Close();
  if (!outline.IsDone()) return R::failure("敷地輪郭から照合範囲を作れません");
  BRepBuilderAPI_MakeFace face(outline.Wire(), true);
  if (!face.IsDone()) return R::failure("敷地輪郭から照合範囲を作れません");
  return face.Face();
}

struct NeighborHeightField {
  TopoDS_Face footprint;
  std::vector<gp_Pnt> outline;
  double outlineZ;
  std::vector<std::array<gp_Pnt, 2>> boundaries;
  double baseZ;
  double slope;

  NeighborHeightSample sample(const gp_Pnt& xy) const {
    std::optional<NeighborHeightSample> nearest;
    for (const auto& boundary : boundaries) {
      const auto& from = boundary[0];
      const gp_Vec along(boundary[1].X() - from.X(), boundary[1].Y() - from.Y(), 0);
      const double length = along.Magnitude();
      // A registered nondegenerate straight edge has a positive horizontal length.
      const gp_Vec axis = along / length;
      const gp_Vec offset(xy.X() - from.X(), xy.Y() - from.Y(), 0);
      const double station = std::clamp(offset.Dot(axis), 0.0, length);
      const gp_Pnt closest(from.X() + station * axis.X(), from.Y() + station * axis.Y(), outlineZ);
      const double distance = xy.Distance(closest);
      if (!nearest || distance < nearest->distanceMm)
        nearest = NeighborHeightSample{boundary, closest, distance, baseZ + slope * distance};
    }
    return *nearest;
  }
};

Result<NeighborHeightField> neighborHeightField(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedEdges,
    double groundZMm, double baseHeightMm, double slope) {
  using R = Result<NeighborHeightField>;
  const double baseZ = groundZMm + baseHeightMm;
  if (!std::isfinite(baseZ) || baseHeightMm < 0 || !std::isfinite(slope) || slope <= 0 || slope > 1000)
    return R::failure("地盤面と立上りの高さ、および0より大きく1000以下の勾配を入力してください");
  if (confirmedEdges.empty()) return R::failure("確認した隣地の境界辺を指定してください");
  const auto& site = document.settings().siteBoundary;
  if (!site) return R::failure("先に敷地境界を登録してください");
  auto geometry = document.siteBoundaryGeometry(site->outline);
  if (!geometry) return R::failure(geometry.error());
  const auto road = document.siteRoadEdge(*geometry);
  if (site->road && !road) return R::failure("道路が接する敷地辺を登録し直してください");

  std::vector<std::array<gp_Pnt, 2>> boundaries;
  boundaries.reserve(confirmedEdges.size());
  for (const auto& edge : confirmedEdges) {
    const auto matched = geometry->matchEdge(edge[0], edge[1]);
    if (!matched) return R::failure("採用する隣地境界は、登録した敷地輪郭の辺を指定してください");
    // matchEdge returns both matches in the outline's order.
    if (road && (*matched)[0].Distance((*road)[0]) <= tolerance::kLinear &&
        (*matched)[1].Distance((*road)[1]) <= tolerance::kLinear)
      return R::failure("前面道路として登録した辺は隣地境界に指定できません");
    boundaries.push_back(*matched);
  }

  const auto face = heightSiteFootprint(*geometry);
  if (!face) return R::failure(face.error());
  const double outlineZ = geometry->vertices.front().Z();
  return NeighborHeightField{*face, std::move(geometry->vertices), outlineZ, std::move(boundaries), baseZ, slope};
}

}  // namespace

Result<std::vector<std::optional<NeighborHeightSample>>> neighborHeightAt(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedEdges,
    const std::vector<gp_Pnt>& points, double groundZMm, double baseHeightMm, double slope) {
  using R = Result<std::vector<std::optional<NeighborHeightSample>>>;
  if (points.empty()) return R::failure("照合する点を指定してください");
  const auto field = neighborHeightField(document, confirmedEdges, groundZMm, baseHeightMm, slope);
  if (!field) return R::failure(field.error());
  std::vector<std::optional<NeighborHeightSample>> samples;
  samples.reserve(points.size());
  for (const auto& point : points) {
    if (!std::isfinite(point.X()) || !std::isfinite(point.Y())) return R::failure("点のXY座標を確認してください");
    const gp_Pnt xy(point.X(), point.Y(), field->outlineZ);
    const auto inside = BRepClass_FaceClassifier(field->footprint, xy, tolerance::kLinear).State();
    if (inside != TopAbs_IN && inside != TopAbs_ON) {
      samples.emplace_back();
      continue;
    }
    const auto nearest = field->sample(xy);
    if (!std::isfinite(nearest.limitZMm)) return R::failure("この座標では隣地斜線の高さを測れません");
    samples.push_back(nearest);
  }
  return samples;
}

namespace {

bool planBoundsInsideSite(const NeighborHeightField& field, const std::array<gp_Pnt, 4>& corners) {
  for (const auto& p : corners)
    if (BRepClass_FaceClassifier(field.footprint, p, tolerance::kLinear).State() != TopAbs_IN) return false;
  // A re-entrant boundary may cross between the corners. Require the whole box to miss every edge.
  for (std::size_t i = 0; i < field.outline.size(); ++i) {
    const auto& a = field.outline[i];
    const auto& b = field.outline[(i + 1) % field.outline.size()];
    double first = 0, last = 1;
    const auto overlaps = [&](double origin, double delta, double low, double high) {
      if (delta == 0) return origin >= low && origin <= high;
      const double p = (low - origin) / delta, q = (high - origin) / delta;
      first = std::max(first, std::min(p, q));
      last = std::min(last, std::max(p, q));
      return first <= last;
    };
    if (overlaps(a.X(), b.X() - a.X(), corners[0].X(), corners[2].X()) &&
        overlaps(a.Y(), b.Y() - a.Y(), corners[0].Y(), corners[2].Y())) return false;
  }
  return true;
}

}  // namespace

Result<std::vector<NeighborHeightPartRow>> compareNeighborHeight(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedEdges,
    const std::vector<ObjectId>& ids, double groundZMm, double baseHeightMm, double slope) {
  using R = Result<std::vector<NeighborHeightPartRow>>;
  if (ids.empty()) return R::failure("モデルで照合する部材を選択してください");
  const auto field = neighborHeightField(document, confirmedEdges, groundZMm, baseHeightMm, slope);
  if (!field) return R::failure(field.error());
  const auto& site = document.settings().siteBoundary;
  std::vector<NeighborHeightPartRow> rows;
  constexpr double agreementMm = 0.01;
  for (const auto& id : std::set<ObjectId>(ids.begin(), ids.end())) {
    const auto* object = document.find(id);
    if (!object || id == site->outline) return R::failure("敷地境界以外の部材を選択してください");
    const auto shape = document.worldShape(*object);
    if (shape.IsNull() || !TopExp_Explorer(shape, TopAbs_FACE).More())
      return R::failure("面を持つ部材を選択してください（" + object->name + "）");
    const Bnd_Box bounds = geom::exactBounds(shape);
    if (bounds.IsVoid() || bounds.IsOpen())
      return R::failure("部材の範囲を測れません（" + object->name + "）");
    const auto lo = bounds.CornerMin(), hi = bounds.CornerMax();
    const auto z = field->outlineZ;
    const std::array<gp_Pnt, 4> corners{gp_Pnt(lo.X(), lo.Y(), z), gp_Pnt(hi.X(), lo.Y(), z),
                                        gp_Pnt(hi.X(), hi.Y(), z), gp_Pnt(lo.X(), hi.Y(), z)};
    const bool whollyInSite = planBoundsInsideSite(*field, corners);
    std::optional<double> lower;
    const auto addLower = [&](double v) { if (!lower || v > *lower) lower = v; };
    // A real B-rep vertex provides a lower bound, never a claim about intervening faces.
    for (TopExp_Explorer vertex(shape, TopAbs_VERTEX); vertex.More(); vertex.Next()) {
      const auto p = BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current()));
      const gp_Pnt xy(p.X(), p.Y(), z);
      const auto state = BRepClass_FaceClassifier(field->footprint, xy, tolerance::kLinear).State();
      if (state == TopAbs_ON || state == TopAbs_IN) addLower(p.Z() - field->sample(xy).limitZMm);
    }

    double upper = -std::numeric_limits<double>::infinity();
    for (const auto& edge : field->boundaries) {
      const auto& a = edge[0];
      const gp_Vec along(edge[1].X() - a.X(), edge[1].Y() - a.Y(), 0);
      const double length = along.Magnitude();
      const gp_Vec axis = along / length;
      const gp_Pnt centre((lo.X() + hi.X()) / 2, (lo.Y() + hi.Y()) / 2, z);
      const gp_Pnt origin(a.X(), a.Y(), z);
      const gp_Vec offset(origin, centre);
      const double station = std::clamp(offset.Dot(axis), 0.0, length);
      const gp_Vec away = offset - axis * station;
      const gp_Dir direction(away.Magnitude() > tolerance::kLinear ? away : gp_Vec(-axis.Y(), axis.X(), 0));
      const gp_Pnt anchor = direction.Dot(gp_Dir(axis)) > 0 ? origin.Translated(along) : origin;
      // A supporting line gives a lower bound on distance to the finite segment.
      // Hence the true signed excess is at most the excess above that affine slope.
      gp_Trsf local;
      local.SetTransformation(gp_Ax3(gp_Pnt(anchor.X(), anchor.Y(), field->baseZ),
                                    gp_Dir(-slope * direction.X(), -slope * direction.Y(), 1)));
      const auto localBounds = geom::exactBounds(shape.Moved(TopLoc_Location(local)));
      if (localBounds.IsVoid() || localBounds.IsOpenZmax())
        return R::failure("部材の高さを測れません（" + object->name + "）");
      const double cap = localBounds.CornerMax().Z() * std::hypot(1.0, slope);
      upper = std::max(upper, cap);
      const bool perpendicular = std::abs(direction.X() * axis.X() + direction.Y() * axis.Y()) <= 1e-12;
      if (whollyInSite && perpendicular && std::all_of(corners.begin(), corners.end(), [&](const gp_Pnt& p) {
            const gp_Vec v(origin, p);
            const double t = v.Dot(axis);
            return t >= 0 && t <= length && v.Dot(gp_Vec(direction)) >= 0;
          })) addLower(cap);
    }
    if (!std::isfinite(upper) || (lower && !std::isfinite(*lower)))
      return R::failure("部材と斜線の差を測れません（" + object->name + "）");
    if (lower) upper = std::max(upper, *lower);
    const bool exact = lower && upper - *lower <= agreementMm;
    std::string state = "要確認";
    if (lower && *lower > agreementMm) state = "超過";
    else if (lower && upper < -agreementMm) state = "余裕";
    else if (exact && upper <= agreementMm && *lower >= -agreementMm) state = "接触相当";
    rows.push_back({id, object->name, std::move(state), lower, upper, exact});
  }
  return rows;
}

namespace {

using HeightTriangle = std::array<gp_Pnt, 3>;

Result<std::vector<HeightTriangle>> heightStudyTriangles(const TopoDS_Face& footprint, bool refine) {
  using R = Result<std::vector<HeightTriangle>>;
  BRepMesh_IncrementalMesh mesh(footprint, 1.0);
  TopLoc_Location location;
  const auto triangulation = BRep_Tool::Triangulation(footprint, location);
  if (!mesh.IsDone() || triangulation.IsNull() || triangulation->NbTriangles() == 0)
    return R::failure("敷地輪郭を表示用の面に分けられません");

  std::vector<HeightTriangle> triangles;
  double maxSide = 0;
  constexpr std::size_t maxTriangles = 4096;
  if (static_cast<std::size_t>(triangulation->NbTriangles()) > maxTriangles)
    return R::failure("敷地輪郭の辺が多く、表示用の面を作れません");
  for (int t = 1; t <= triangulation->NbTriangles(); ++t) {
    int a, b, c;
    triangulation->Triangle(t).Get(a, b, c);
    const auto node = [&](int i) { return triangulation->Node(i).Transformed(location.Transformation()); };
    const HeightTriangle triangle{node(a), node(b), node(c)};
    maxSide = std::max({maxSide, triangle[0].Distance(triangle[1]), triangle[1].Distance(triangle[2]),
                        triangle[2].Distance(triangle[0])});
    triangles.push_back(triangle);
  }
  // Subdivide all triangles by the same number of levels, so shared edges have identical samples.
  const auto midpoint = [](const gp_Pnt& a, const gp_Pnt& b) {
    return gp_Pnt((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2, a.Z());
  };
  while (refine && maxSide > 1000 && triangles.size() <= maxTriangles / 4) {
    std::vector<HeightTriangle> refined;
    refined.reserve(triangles.size() * 4);
    for (const auto& t : triangles) {
      const auto ab = midpoint(t[0], t[1]), bc = midpoint(t[1], t[2]), ca = midpoint(t[2], t[0]);
      refined.insert(refined.end(), {{t[0], ab, ca}, {ab, t[1], bc}, {ca, bc, t[2]}, {ab, bc, ca}});
    }
    triangles = std::move(refined);
    maxSide /= 2;
  }

  return triangles;
}

}  // namespace

Result<NeighborHeightPreview> neighborHeightPreview(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedEdges,
    double groundZMm, double baseHeightMm, double slope) {
  using R = Result<NeighborHeightPreview>;
  const auto field = neighborHeightField(document, confirmedEdges, groundZMm, baseHeightMm, slope);
  if (!field) return R::failure(field.error());
  try {
    const auto plan = heightStudyTriangles(field->footprint, true);
    if (!plan) return R::failure(plan.error());
    auto triangles = *plan;

    BRep_Builder builder;
    TopoDS_Compound surface, boundaries;
    builder.MakeCompound(surface);
    builder.MakeCompound(boundaries);
    for (auto& triangle : triangles) {
      BRepBuilderAPI_MakePolygon wire;
      for (auto& p : triangle) {
        const double z = field->sample(p).limitZMm;
        if (!std::isfinite(z)) return R::failure("制限面の高さを測れません");
        p.SetZ(z);
        wire.Add(p);
      }
      wire.Close();
      if (!wire.IsDone()) return R::failure("隣地斜線の表示面を作れません");
      BRepBuilderAPI_MakeFace face(wire.Wire(), true);
      if (!face.IsDone()) return R::failure("隣地斜線の表示面を作れません");
      builder.Add(surface, face.Face());
    }
    for (const auto& edge : field->boundaries) {
      const gp_Vec lift(0, 0, 1);
      BRepBuilderAPI_MakeEdge line(edge[0].Translated(lift), edge[1].Translated(lift));
      if (!line.IsDone()) return R::failure("隣地境界を表示できません");
      builder.Add(boundaries, line.Edge());
    }
    return NeighborHeightPreview{surface, boundaries};
  } catch (const Standard_Failure&) {
    return R::failure("隣地斜線の表示面を作れません");
  }
}

namespace {

enum class NorthBoundaryKind { Unconfirmed, Neighbor, Road };

struct NorthRayHit {
  double station;
  std::size_t edge;
};

NorthHeightSample northHeightSample(const SiteBoundaryGeometry& geometry, const TopoDS_Face& footprint,
                                   const std::vector<NorthBoundaryKind>& kinds, const gp_Pnt& start,
                                   const gp_Vec& north, double width, double baseZ, double slope = 1.25) {
  const auto startState = BRepClass_FaceClassifier(footprint, start, tolerance::kLinear).State();
  if (startState == TopAbs_OUT) return NorthHeightSample{"outside_site"};
  if (startState != TopAbs_IN && startState != TopAbs_ON) return NorthHeightSample{"calculation_review"};

  std::vector<NorthRayHit> hits;
  const auto& vertices = geometry.vertices;
  const auto cross = [&](const gp_Vec& v) { return north.X() * v.Y() - north.Y() * v.X(); };
  const auto addHit = [&](double t, std::size_t edge) {
    if (t >= -tolerance::kLinear) hits.push_back({std::max(t, 0.0), edge});
  };
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    const gp_Vec a(start, vertices[i]), b(start, vertices[(i + 1) % vertices.size()]);
    const double sideA = cross(a), sideB = cross(b);
    const bool onA = std::abs(sideA) <= tolerance::kLinear;
    const bool onB = std::abs(sideB) <= tolerance::kLinear;
    if (onA) addHit(a.Dot(north), i);
    if (onB) addHit(b.Dot(north), i);
    if (!onA && !onB && (sideA > 0) != (sideB > 0)) {
      const double fraction = sideA / (sideA - sideB);
      addHit((a + (b - a) * fraction).Dot(north), i);
    }
    // Starting on a collinear edge also creates an event at the start of the ray.
    if (onA && onB && (a.Dot(north) < 0) != (b.Dot(north) < 0)) addHit(0, i);
  }
  std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.station < b.station; });

  for (std::size_t begin = 0; begin < hits.size();) {
    std::size_t end = begin + 1;
    while (end < hits.size() && hits[end].station - hits[begin].station <= tolerance::kLinear) ++end;
    const double exitDistance = hits[begin].station;
    const auto exit = start.Translated(north * exitDistance);
    const auto after = end == hits.size() ? TopAbs_OUT :
        BRepClass_FaceClassifier(footprint, start.Translated(north * ((hits[end - 1].station + hits[end].station) / 2)),
                                 tolerance::kLinear).State();
    if (after == TopAbs_IN) {
      begin = end;  // A tangency does not leave the site.
      continue;
    }
    NorthHeightSample sample;
    sample.exitPoint = exit;
    if (after != TopAbs_OUT) {
      sample.status = "parallel_review";
      return sample;
    }
    auto distance = std::numeric_limits<double>::infinity();
    bool hasRoad = false, hasNeighbor = false;
    for (std::size_t j = begin; j < end; ++j) {
      const auto i = hits[j].edge;
      const std::array<gp_Pnt, 2> edge{vertices[i], vertices[(i + 1) % vertices.size()]};
      const auto kind = kinds[i];
      if (kind == NorthBoundaryKind::Unconfirmed) {
        sample.status = "boundary_unconfirmed";
        sample.exitBoundary = edge;
        return sample;
      }
      double candidate = exitDistance;
      if (kind == NorthBoundaryKind::Road) {
        hasRoad = true;
        const gp_Vec tangent(edge[0], edge[1]);
        const double cosine = std::abs(cross(tangent)) / std::hypot(tangent.X(), tangent.Y());
        if (cosine <= 1e-8) {
          sample.status = "parallel_review";
          sample.exitBoundary = edge;
          sample.road = true;
          return sample;
        }
        if (!(width > 0) || !std::isfinite(width)) {
          sample.status = "road_width_missing";
          sample.exitBoundary = edge;
          sample.road = true;
          return sample;
        }
        candidate += width / cosine;
      } else {
        hasNeighbor = true;
      }
      if (candidate < distance) {
        distance = candidate;
        sample.exitBoundary = edge;
        sample.road = kind == NorthBoundaryKind::Road;
      }
    }
    const double limit = baseZ + slope * distance;
    if (!std::isfinite(limit)) return NorthHeightSample{"calculation_review"};
    sample.status = hasRoad && hasNeighbor ? "vertex_review" : "calculated";
    sample.distanceMm = distance;
    sample.referencePoint = start.Translated(north * distance);
    sample.limitZMm = limit;
    return sample;
  }
  return NorthHeightSample{"calculation_review"};
}

struct NorthHeightField {
  SiteBoundaryGeometry geometry;
  TopoDS_Face footprint;
  std::vector<NorthBoundaryKind> kinds;
  gp_Vec north;
  double width;
  double baseZ;
};

Result<NorthHeightField> northHeightField(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    double baseZ) {
  using R = Result<NorthHeightField>;
  const auto& settings = document.settings();
  if (!settings.siteNorth) return R::failure("先に敷地の真北を登録してください");
  if (!settings.siteBoundary) return R::failure("先に敷地境界を登録してください");
  auto geometry = document.siteBoundaryGeometry(settings.siteBoundary->outline);
  if (!geometry) return R::failure(geometry.error());
  const auto road = document.siteRoadEdge(*geometry);
  if (settings.siteBoundary->road && !road) return R::failure("道路が接する敷地辺を登録し直してください");
  const auto footprint = heightSiteFootprint(*geometry);
  if (!footprint) return R::failure(footprint.error());

  const auto& vertices = geometry->vertices;
  const auto setKind = [&](const std::array<gp_Pnt, 2>& edge, auto& kinds, NorthBoundaryKind value) {
    for (std::size_t i = 0; i < vertices.size(); ++i) {
      if (edge[0].Distance(vertices[i]) <= tolerance::kLinear &&
          edge[1].Distance(vertices[(i + 1) % vertices.size()]) <= tolerance::kLinear) {
        if (kinds[i] == NorthBoundaryKind::Road && value == NorthBoundaryKind::Neighbor) return false;
        kinds[i] = value;
        return true;
      }
    }
    return false;
  };
  std::vector<NorthBoundaryKind> kinds(vertices.size(), NorthBoundaryKind::Unconfirmed);
  if (road) setKind(*road, kinds, NorthBoundaryKind::Road);
  for (const auto& edge : confirmedNeighbors) {
    const auto matched = geometry->matchEdge(edge[0], edge[1]);
    if (!matched || !setKind(*matched, kinds, NorthBoundaryKind::Neighbor))
      return R::failure("確認した隣地境界には、前面道路以外の敷地輪郭の辺を指定してください");
  }
  const double angle = std::remainder(settings.siteNorth->angle, 360.0) * std::acos(-1.0) / 180.0;
  const gp_Vec north(std::sin(angle), std::cos(angle), 0);
  const double width = road ? settings.siteBoundary->road->widthMm : 0;
  return NorthHeightField{std::move(*geometry), *footprint, std::move(kinds), north, width, baseZ};
}

}  // namespace

Result<std::vector<NorthHeightSample>> northHeightAt(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    const std::vector<gp_Pnt>& points, double groundZMm, double baseHeightMm) {
  using R = Result<std::vector<NorthHeightSample>>;
  if (points.empty()) return R::failure("照合する点を指定してください");
  const double baseZ = groundZMm + baseHeightMm;
  if (!std::isfinite(baseZ) || (baseHeightMm != 5000 && baseHeightMm != 10000))
    return R::failure("地盤面と、法第56条で確認した立上り5 mまたは10 mを入力してください");
  const auto field = northHeightField(document, confirmedNeighbors, baseZ);
  if (!field) return R::failure(field.error());
  const double outlineZ = field->geometry.vertices.front().Z();
  std::vector<NorthHeightSample> rows;
  rows.reserve(points.size());
  for (const auto& point : points) {
    if (!std::isfinite(point.X()) || !std::isfinite(point.Y())) return R::failure("点のXY座標を確認してください");
    rows.push_back(northHeightSample(field->geometry, field->footprint, field->kinds,
                                    gp_Pnt(point.X(), point.Y(), outlineZ), field->north, field->width, field->baseZ));
  }
  return rows;
}

Result<std::vector<YokohamaHeightDistrictSample>> yokohamaHeightDistrictAt(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    const std::vector<gp_Pnt>& points, double groundZMm, bool singleNorthRoadConfirmed) {
  using R = Result<std::vector<YokohamaHeightDistrictSample>>;
  const auto rule = yokohamaHeightDistrictRule(siteHeightConditions(document).district);
  if (!rule) return R::failure("横浜市の最高限第1種〜第7種のいずれかを登録してください");
  if (points.empty()) return R::failure("照合する点を指定してください");
  const double capZ = groundZMm + rule->heightCapMm;
  if (!std::isfinite(capZ)) return R::failure("地盤面の高さを確認してください");

  TopoDS_Face footprint;
  double outlineZ = 0;
  std::optional<NorthHeightField> field;
  if (rule->northRiseMm) {
    auto north = northHeightField(document, confirmedNeighbors, groundZMm + *rule->northRiseMm);
    if (!north) return R::failure(north.error());
    field.emplace(std::move(*north));
    footprint = field->footprint;
    outlineZ = field->geometry.vertices.front().Z();
  } else {
    const auto& site = document.settings().siteBoundary;
    if (!site) return R::failure("先に敷地境界を登録してください");
    const auto geometry = document.siteBoundaryGeometry(site->outline);
    if (!geometry) return R::failure(geometry.error());
    const auto face = heightSiteFootprint(*geometry);
    if (!face) return R::failure(face.error());
    footprint = *face;
    outlineZ = geometry->vertices.front().Z();
  }

  std::vector<YokohamaHeightDistrictSample> rows;
  rows.reserve(points.size());
  for (const auto& point : points) {
    if (!std::isfinite(point.X()) || !std::isfinite(point.Y())) return R::failure("点のXY座標を確認してください");
    const gp_Pnt position(point.X(), point.Y(), outlineZ);
    if (!field) {
      const auto state = BRepClass_FaceClassifier(footprint, position, tolerance::kLinear).State();
      if (state == TopAbs_IN || state == TopAbs_ON) rows.push_back({"height_cap", std::nullopt, capZ});
      else rows.push_back({state == TopAbs_OUT ? "outside_site" : "calculation_review"});
      continue;
    }
    const auto sample = northHeightSample(field->geometry, footprint, field->kinds, position, field->north,
                                          singleNorthRoadConfirmed ? field->width / 2 : 0, field->baseZ,
                                          kYokohamaHeightDistrictSlope);
    if (sample.status != "calculated") {
      rows.push_back({sample.status == "road_width_missing" && !singleNorthRoadConfirmed
                          ? "north_road_unconfirmed" : sample.status});
      continue;
    }
    const double slantZ = *sample.limitZMm;
    rows.push_back({capZ <= slantZ ? "height_cap" : "north_slant", slantZ, std::min(capZ, slantZ)});
  }
  return rows;
}

namespace {

struct HeightPatch {
  TopoDS_Face footprint;
  TopoDS_Face surface;  // Null for an unconfirmed region.
  TopoDS_Shape reference;
  double areaMm2;
  bool heightCap = false;
  bool convexFootprint = true;
  std::optional<HeightLimitSource> source = std::nullopt;
};

using HeightPatches = Result<std::vector<HeightPatch>>;

TopoDS_Face planarHeightFace(const std::vector<gp_Pnt>& points) {
  BRepBuilderAPI_MakePolygon wire;
  for (const auto& point : points) wire.Add(point);
  wire.Close();
  if (!wire.IsDone()) return {};
  BRepBuilderAPI_MakeFace face(wire.Wire(), true);
  return face.IsDone() ? face.Face() : TopoDS_Face();
}

// Cut a convex patch on the line where the slant reaches the horizontal cap.
bool appendCappedHeightPatches(const std::vector<gp_Pnt>& slant, double capZ, double outlineZ,
                               std::vector<HeightPatch>& patches) {
  for (const bool cap : {false, true}) {
    std::vector<gp_Pnt> surface;
    const auto append = [&](gp_Pnt p) {
      if (cap) p.SetZ(capZ);
      if (surface.empty() || p.Distance(surface.back()) > tolerance::kLinear) surface.push_back(p);
    };
    const auto inside = [&](const gp_Pnt& p) { return cap ? p.Z() >= capZ : p.Z() <= capZ; };
    for (std::size_t i = 0; i < slant.size(); ++i) {
      const auto& a = slant[i];
      const auto& b = slant[(i + 1) % slant.size()];
      if (inside(a)) append(a);
      if (inside(a) != inside(b)) {
        const double t = (capZ - a.Z()) / (b.Z() - a.Z());
        auto crossing = a.Translated(gp_Vec(a, b) * t);
        crossing.SetZ(capZ);
        append(crossing);
      }
    }
    if (surface.size() > 1 && surface.front().Distance(surface.back()) <= tolerance::kLinear) surface.pop_back();
    if (surface.size() < 3) continue;
    auto plan = surface;
    for (auto& p : plan) p.SetZ(outlineZ);
    const auto footprint = planarHeightFace(plan);
    const auto face = planarHeightFace(surface);
    if (footprint.IsNull() || face.IsNull()) return false;
    patches.push_back({footprint, face, {}, 0, cap});
  }
  return true;
}

HeightPatches northHeightPatches(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    double baseZ, double slope, double roadFraction, std::optional<double> capZ = std::nullopt) {
  using R = HeightPatches;
  const auto field = northHeightField(document, confirmedNeighbors, baseZ);
  if (!field) return R::failure(field.error());
  const auto& vertices = field->geometry.vertices;
  const gp_Pnt origin = vertices.front();
  const gp_Vec east(field->north.Y(), -field->north.X(), 0);
  std::vector<gp_Pnt2d> plan;
  std::vector<double> cuts;
  for (const auto& vertex : vertices) {
    const gp_Vec offset(origin, vertex);
    plan.emplace_back(offset.Dot(east), offset.Dot(field->north));
    cuts.push_back(plan.back().X());
  }
  std::sort(cuts.begin(), cuts.end());
  const auto world = [&](double u, double v, double z) {
    const auto p = origin.Translated(east * u + field->north * v);
    return gp_Pnt(p.X(), p.Y(), z);
  };
  const auto northAt = [&](std::size_t i, double u) {
    const auto& a = plan[i];
    const auto& b = plan[(i + 1) % plan.size()];
    return a.Y() + (b.Y() - a.Y()) * ((u - a.X()) / (b.X() - a.X()));
  };

  try {
    std::vector<HeightPatch> patches;
    for (std::size_t column = 1; column < cuts.size(); ++column) {
      const double left = cuts[column - 1], right = cuts[column];
      if (right - left <= tolerance::kLinear) continue;
      const double mid = (left + right) / 2;
      std::vector<NorthRayHit> crossings;
      for (std::size_t i = 0; i < plan.size(); ++i) {
        const double a = plan[i].X(), b = plan[(i + 1) % plan.size()].X();
        if (mid > std::min(a, b) && mid < std::max(a, b)) crossings.push_back({northAt(i, mid), i});
      }
      std::sort(crossings.begin(), crossings.end(), [](const auto& a, const auto& b) { return a.station < b.station; });
      if (crossings.size() % 2 != 0) return R::failure("真北方向で敷地の表示範囲を分けられません");
      for (std::size_t i = 1; i < crossings.size(); i += 2) {
        if (patches.size() >= 4096) return R::failure("敷地の分割が多く、北側斜線を表示できません");
        const auto& south = crossings[i - 1];
        const auto& exit = crossings[i];
        const double v = (south.station + exit.station) / 2;
        const double sl = northAt(south.edge, left), sr = northAt(south.edge, right);
        const double nl = northAt(exit.edge, left), nr = northAt(exit.edge, right);
        const auto corners = [&](double southLeftZ, double southRightZ, double exitZ) {
          std::vector<gp_Pnt> points{world(left, sl, southLeftZ), world(right, sr, southRightZ)};
          if (nr - sr > tolerance::kLinear) points.push_back(world(right, nr, exitZ));
          if (nl - sl > tolerance::kLinear) points.push_back(world(left, nl, exitZ));
          return points;
        };
        HeightPatch patch{planarHeightFace(corners(origin.Z(), origin.Z(), origin.Z())), {}, {},
                               (right - left) * (exit.station - south.station)};
        if (patch.footprint.IsNull()) return R::failure("北側斜線の範囲を作れません");
        const auto sample = northHeightSample(field->geometry, field->footprint, field->kinds,
                                              world(mid, v, origin.Z()), field->north, field->width * roadFraction,
                                              field->baseZ, slope);
        if (sample.distanceMm && (!capZ || sample.status == "calculated")) {
          const double advance = *sample.distanceMm - (exit.station - v);
          const double zNearExit = field->baseZ + slope * advance;
          // Keep the two sides of a change in exit as separate faces.
          const auto slant = corners(zNearExit + slope * (nl - sl), zNearExit + slope * (nr - sr), zNearExit);
          if (capZ) {
            if (!appendCappedHeightPatches(slant, *capZ, origin.Z(), patches))
              return R::failure("高度地区の水平面と斜線を分けられません");
            continue;
          }
          patch.surface = planarHeightFace(slant);
          if (patch.surface.IsNull()) return R::failure("北側斜線の表示面を作れません");
          BRepBuilderAPI_MakeEdge line(world(left, nl + advance, origin.Z() + 1),
                                       world(right, nr + advance, origin.Z() + 1));
          if (line.IsDone()) patch.reference = line.Edge();
        }
        patches.push_back(std::move(patch));
      }
    }
    return patches;
  } catch (const Standard_Failure&) {
    return R::failure("北側斜線の表示面を作れません");
  }
}

HeightPatches statutoryHeightPatches(const Document& document,
                                            const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
                                            double groundZMm, double baseHeightMm) {
  const double baseZ = groundZMm + baseHeightMm;
  if (!std::isfinite(baseZ) || (baseHeightMm != 5000 && baseHeightMm != 10000))
    return HeightPatches::failure("地盤面と、法第56条で確認した立上り5 mまたは10 mを入力してください");
  return northHeightPatches(document, confirmedNeighbors, baseZ, 1.25, 1);
}

HeightPatches yokohamaHeightDistrictPatches(const Document& document,
                                           const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
                                           double groundZMm, bool singleNorthRoadConfirmed) {
  const auto rule = yokohamaHeightDistrictRule(siteHeightConditions(document).district);
  if (!rule) return HeightPatches::failure("横浜市の最高限第1種〜第7種のいずれかを登録してください");
  const double capZ = groundZMm + rule->heightCapMm;
  if (!std::isfinite(capZ)) return HeightPatches::failure("地盤面の高さを確認してください");
  if (rule->northRiseMm)
    return northHeightPatches(document, confirmedNeighbors, groundZMm + *rule->northRiseMm,
                              kYokohamaHeightDistrictSlope, singleNorthRoadConfirmed ? 0.5 : 0, capZ);

  const auto& site = document.settings().siteBoundary;
  if (!site) return HeightPatches::failure("先に敷地境界を登録してください");
  const auto geometry = document.siteBoundaryGeometry(site->outline);
  if (!geometry) return HeightPatches::failure(geometry.error());
  const auto footprint = heightSiteFootprint(*geometry);
  if (!footprint) return HeightPatches::failure(footprint.error());
  gp_Trsf translation;
  translation.SetTranslation(gp_Vec(0, 0, capZ - geometry->vertices.front().Z()));
  return std::vector<HeightPatch>{{*footprint, TopoDS::Face(footprint->Moved(TopLoc_Location(translation))),
                                       {}, 0, true, false}};
}

Result<TopoDS_Shape> clipPartToHeightPatch(const TopoDS_Shape& part, const Bnd_Box& bounds,
                                             const TopoDS_Face& footprint, double footprintZ) {
  using R = Result<TopoDS_Shape>;
  const auto lo = bounds.CornerMin(), hi = bounds.CornerMax();
  gp_Trsf down;
  down.SetTranslation(gp_Vec(0, 0, lo.Z() - 1 - footprintZ));
  BRepPrimAPI_MakePrism prism(footprint.Moved(TopLoc_Location(down)), gp_Vec(0, 0, hi.Z() - lo.Z() + 2));
  const auto solid = prism.Shape();
  if (!prism.IsDone() || solid.IsNull()) return R::failure("敷地内の照合範囲を作れません");

  TopTools_ListOfShape parts, tools;
  parts.Append(part);
  tools.Append(solid);
  BRepAlgoAPI_Common common;
  common.SetArguments(parts);
  common.SetTools(tools);
  common.SetNonDestructive(true);
  common.Build();
  if (!common.IsDone() || common.HasErrors()) return R::failure("斜線の範囲で部材を切れません");
  BRepAlgoAPI_Section contacts(part, solid, false);
  contacts.SetNonDestructive(true);
  contacts.Build();
  if (!contacts.IsDone() || contacts.HasErrors()) return R::failure("斜線の範囲端で部材を測れません");
  BRep_Builder builder;
  TopoDS_Compound clipped;
  builder.MakeCompound(clipped);
  if (!common.Shape().IsNull()) builder.Add(clipped, common.Shape());
  if (!contacts.Shape().IsNull()) builder.Add(clipped, contacts.Shape());
  return clipped;
}

}  // namespace

Result<NorthHeightPreview> northHeightPreview(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    double groundZMm, double baseHeightMm) {
  using R = Result<NorthHeightPreview>;
  const auto patches = statutoryHeightPatches(document, confirmedNeighbors, groundZMm, baseHeightMm);
  if (!patches) return R::failure(patches.error());
  BRep_Builder builder;
  TopoDS_Compound surface, references;
  builder.MakeCompound(surface);
  builder.MakeCompound(references);
  double omitted = 0;
  bool visible = false;
  for (const auto& patch : *patches) {
    if (patch.surface.IsNull()) {
      omitted += patch.areaMm2;
      continue;
    }
    visible = true;
    builder.Add(surface, patch.surface);
    if (!patch.reference.IsNull()) builder.Add(references, patch.reference);
  }
  if (!visible) return R::failure("表示できる範囲がありません。出口の隣地辺または道路幅員を確認してください");
  return NorthHeightPreview{surface, references, omitted};
}

Result<YokohamaHeightDistrictPreview> yokohamaHeightDistrictPreview(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    double groundZMm, bool singleNorthRoadConfirmed) {
  using R = Result<YokohamaHeightDistrictPreview>;
  const auto patches = yokohamaHeightDistrictPatches(document, confirmedNeighbors, groundZMm, singleNorthRoadConfirmed);
  if (!patches) return R::failure(patches.error());
  BRep_Builder builder;
  TopoDS_Compound cap, slant, outline;
  builder.MakeCompound(cap);
  builder.MakeCompound(slant);
  builder.MakeCompound(outline);
  gp_Trsf outlineOffset;
  outlineOffset.SetTranslation(gp_Vec(0, 0, 2));
  double unconfirmed = 0;
  for (const auto& patch : *patches) {
    if (patch.surface.IsNull()) {
      unconfirmed += patch.areaMm2;
      for (TopExp_Explorer edges(patch.footprint, TopAbs_EDGE); edges.More(); edges.Next())
        builder.Add(outline, edges.Current().Moved(TopLoc_Location(outlineOffset)));
    } else {
      builder.Add(patch.heightCap ? cap : slant, patch.surface);
    }
  }
  return YokohamaHeightDistrictPreview{cap.NbChildren() ? cap : TopoDS_Compound(),
                                       slant.NbChildren() ? slant : TopoDS_Compound(),
                                       outline.NbChildren() ? outline : TopoDS_Compound(), unconfirmed};
}

namespace {

struct HeightMeasurement {
  ObjectId id;
  std::string name;
  std::optional<double> excess;
  std::optional<std::size_t> patch;
  bool unknown;
  bool inSiteWithoutLimit;
};

Result<std::vector<HeightMeasurement>> measureHeightParts(
    const Document& document, const std::vector<ObjectId>& ids, const std::vector<HeightPatch>& patches,
    const TopoDS_Face& siteFootprint = {}) {
  using R = Result<std::vector<HeightMeasurement>>;
  if (ids.empty()) return R::failure("モデルで照合する部材を選択してください");

  struct Region {
    gp_Pnt lo, hi;
    std::optional<gp_Pln> plane;
  };
  std::vector<Region> regions;
  for (const auto& patch : patches) {
    const auto box = geom::exactBounds(patch.footprint);
    std::optional<gp_Pln> plane;
    if (!patch.surface.IsNull()) plane = BRepAdaptor_Surface(patch.surface).Plane();
    regions.push_back({box.CornerMin(), box.CornerMax(), plane});
  }
  std::vector<HeightMeasurement> rows;
  try {
    for (const auto& id : std::set<ObjectId>(ids.begin(), ids.end())) {
      const auto* object = document.find(id);
      if (!object || id == document.settings().siteBoundary->outline)
        return R::failure("敷地境界以外の部材を選択してください");
      const auto part = document.worldShape(*object);
      if (part.IsNull() || !TopExp_Explorer(part, TopAbs_FACE).More())
        return R::failure("面を持つ部材を選択してください（" + object->name + "）");
      const auto bounds = geom::exactBounds(part);
      if (bounds.IsVoid() || bounds.IsOpen()) return R::failure("部材の範囲を測れません（" + object->name + "）");
      const auto lo = bounds.CornerMin(), hi = bounds.CornerMax();
      std::optional<double> maximum;
      bool unknown = false;
      std::optional<std::size_t> controlling;
      for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& region = regions[i];
        if (hi.X() < region.lo.X() - tolerance::kLinear || lo.X() > region.hi.X() + tolerance::kLinear ||
            hi.Y() < region.lo.Y() - tolerance::kLinear || lo.Y() > region.hi.Y() + tolerance::kLinear) continue;
        if (!region.plane && unknown) continue;
        if (maximum && region.plane) {
          // The highest point of the part over this XY box is a conservative upper bound.
          const auto normal = region.plane->Axis().Direction();
          const auto& origin = region.plane->Location();
          const double dx = normal.X() / normal.Z(), dy = normal.Y() / normal.Z();
          const double x = dx >= 0 ? std::min(hi.X(), region.hi.X()) : std::max(lo.X(), region.lo.X());
          const double y = dy >= 0 ? std::min(hi.Y(), region.hi.Y()) : std::max(lo.Y(), region.lo.Y());
          const double upper = hi.Z() - origin.Z() + dx * (x - origin.X()) + dy * (y - origin.Y());
          if (upper < *maximum - tolerance::kLinear) continue;
        }
        const auto& patch = patches[i];
        const auto& footprint = patch.footprint;
        const double z = region.lo.Z();
        const std::array<gp_Pnt, 4> corners{gp_Pnt(lo.X(), lo.Y(), z), gp_Pnt(hi.X(), lo.Y(), z),
                                            gp_Pnt(hi.X(), hi.Y(), z), gp_Pnt(lo.X(), hi.Y(), z)};
        const bool whollyInside = patch.convexFootprint && std::all_of(corners.begin(), corners.end(), [&](const auto& p) {
          const auto state = BRepClass_FaceClassifier(footprint, p, tolerance::kLinear).State();
          return state == TopAbs_IN || state == TopAbs_ON;
        });
        TopoDS_Shape relevant = part;
        if (!whollyInside) {
          const auto clipped = clipPartToHeightPatch(part, bounds, footprint, z);
          if (!clipped) return R::failure(clipped.error() + "（" + object->name + "）");
          relevant = *clipped;
          if (geom::exactBounds(relevant).IsVoid()) continue;
        }
        if (!region.plane) {
          unknown = true;
          continue;
        }
        auto normal = region.plane->Axis().Direction();
        if (normal.Z() < 0) normal.Reverse();
        gp_Trsf frame;
        frame.SetTransformation(gp_Ax3(region.plane->Location(), normal));
        const auto measured = geom::exactBounds(relevant.Moved(TopLoc_Location(frame)));
        if (measured.IsVoid() || measured.IsOpenZmax() || normal.Z() <= 0)
          return R::failure("部材の鉛直差を測れません（" + object->name + "）");
        const double excess = measured.CornerMax().Z() / normal.Z();
        if (!maximum || excess > *maximum) {
          maximum = excess;
          controlling = i;
        }
      }
      bool inSiteWithoutLimit = false;
      if (!maximum && !unknown && !siteFootprint.IsNull()) {
        const auto clipped = clipPartToHeightPatch(part, bounds, siteFootprint,
                                                   geom::exactBounds(siteFootprint).CornerMin().Z());
        if (!clipped) return R::failure(clipped.error() + "（" + object->name + "）");
        inSiteWithoutLimit = !geom::exactBounds(*clipped).IsVoid();
      }
      rows.push_back({id, object->name, maximum, controlling, unknown, inSiteWithoutLimit});
    }
  } catch (const Standard_Failure&) {
    return R::failure("高さの制限面と部材を照合できません");
  }
  return rows;
}

std::string heightMeasurementState(const HeightMeasurement& row) {
  constexpr double agreementMm = 0.01;
  if (row.excess && *row.excess > agreementMm) return row.unknown ? "超過・要確認" : "超過";
  if (row.unknown) return "要確認";
  if (row.excess) return *row.excess < -agreementMm ? "余裕" : "接触相当";
  return row.inSiteWithoutLimit ? "適用面なし" : "敷地外";
}

Result<std::vector<NorthHeightPartRow>> compareHeightPatches(
    const Document& document, const std::vector<ObjectId>& ids, const HeightPatches& patches) {
  using R = Result<std::vector<NorthHeightPartRow>>;
  if (!patches) return R::failure(patches.error());
  const auto measurements = measureHeightParts(document, ids, *patches);
  if (!measurements) return R::failure(measurements.error());
  std::vector<NorthHeightPartRow> rows;
  for (const auto& row : *measurements)
    rows.push_back({row.id, row.name, heightMeasurementState(row), row.excess, row.unknown,
                    row.patch && (*patches)[*row.patch].heightCap});
  return rows;
}

}  // namespace

Result<std::vector<NorthHeightPartRow>> compareNorthHeight(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    const std::vector<ObjectId>& ids, double groundZMm, double baseHeightMm) {
  return compareHeightPatches(document, ids, statutoryHeightPatches(document, confirmedNeighbors, groundZMm, baseHeightMm));
}

Result<std::vector<NorthHeightPartRow>> compareYokohamaHeightDistrict(
    const Document& document, const std::vector<std::array<gp_Pnt, 2>>& confirmedNeighbors,
    const std::vector<ObjectId>& ids, double groundZMm, bool singleNorthRoadConfirmed) {
  return compareHeightPatches(document, ids,
      yokohamaHeightDistrictPatches(document, confirmedNeighbors, groundZMm, singleNorthRoadConfirmed));
}

namespace {

using HeightPolygon = std::vector<gp_Pnt>;

struct HeightConstraint {
  HeightLimitSource source;
  gp_Pln plane;

  double at(const gp_Pnt& point) const {
    const auto normal = plane.Axis().Direction();
    const auto& origin = plane.Location();
    return origin.Z() - (normal.X() * (point.X() - origin.X()) +
                         normal.Y() * (point.Y() - origin.Y())) / normal.Z();
  }
};

struct HeightCell {
  HeightPolygon plan;
  std::optional<HeightConstraint> limit;
  bool unconfirmed = false;
};

double heightPlanArea(const HeightPolygon& polygon) {
  double twice = 0;
  for (std::size_t i = 2; i < polygon.size(); ++i) {
    const gp_Vec a(polygon.front(), polygon[i - 1]), b(polygon.front(), polygon[i]);
    twice += a.X() * b.Y() - a.Y() * b.X();
  }
  return twice / 2;
}

template <typename Side>
HeightPolygon clipHeightPolygon(const HeightPolygon& plan, Side side) {
  HeightPolygon result;
  const auto append = [&](const gp_Pnt& point) {
    if (result.empty() || result.back().Distance(point) > tolerance::kLinear) result.push_back(point);
  };
  for (std::size_t i = 0; i < plan.size(); ++i) {
    const auto& a = plan[i];
    const auto& b = plan[(i + 1) % plan.size()];
    const double fa = side(a), fb = side(b);
    if (fa >= 0) append(a);
    if ((fa < 0) != (fb < 0)) append(a.Translated(gp_Vec(a, b) * (fa / (fa - fb))));
  }
  if (result.size() > 1 && result.front().Distance(result.back()) <= tolerance::kLinear) result.pop_back();
  if (std::abs(heightPlanArea(result)) <= tolerance::kLinear * tolerance::kLinear) result.clear();
  return result;
}

void applyHeightConstraint(HeightCell cell, const HeightConstraint& constraint, std::vector<HeightCell>& cells) {
  if (cell.plan.empty()) return;
  if (!cell.limit) {
    cell.limit = constraint;
    cells.push_back(std::move(cell));
    return;
  }
  const auto difference = [&](const gp_Pnt& p) { return cell.limit->at(p) - constraint.at(p); };
  const bool lowers = std::any_of(cell.plan.begin(), cell.plan.end(),
                                  [&](const auto& p) { return difference(p) > tolerance::kLinear; });
  if (!lowers) {
    cells.push_back(std::move(cell));
    return;
  }
  auto lower = clipHeightPolygon(cell.plan, difference);
  auto unchanged = clipHeightPolygon(cell.plan, [&](const auto& p) { return -difference(p); });
  if (!lower.empty()) cells.push_back({std::move(lower), constraint});
  if (!unchanged.empty()) cells.push_back({std::move(unchanged), cell.limit});
}

std::array<double, 4> heightPlanBounds(const HeightPolygon& plan) {
  std::array<double, 4> box{plan.front().X(), plan.front().Y(), plan.front().X(), plan.front().Y()};
  for (const auto& p : plan) {
    box[0] = std::min(box[0], p.X());
    box[1] = std::min(box[1], p.Y());
    box[2] = std::max(box[2], p.X());
    box[3] = std::max(box[3], p.Y());
  }
  return box;
}

constexpr std::size_t kHeightCellBudget = 32768;

bool overlayHeightPartition(std::vector<HeightCell>& cells, const std::vector<HeightPatch>& patches,
                            HeightLimitSource source) {
  if (patches.size() == 1 && !patches.front().convexFootprint) {
    std::vector<HeightCell> result;
    const HeightConstraint constraint{source, BRepAdaptor_Surface(patches.front().surface).Plane()};
    for (auto& cell : cells) {
      if (cell.unconfirmed) result.push_back(std::move(cell));
      else applyHeightConstraint(std::move(cell), constraint, result);
    }
    cells = std::move(result);
    return cells.size() <= kHeightCellBudget;
  }

  struct Region {
    HeightPolygon outline;
    std::array<double, 4> bounds;
    std::optional<HeightConstraint> constraint;
  };
  std::vector<Region> regions;
  for (const auto& patch : patches) {
    HeightPolygon outline;
    const auto wire = BRepTools::OuterWire(patch.footprint);
    for (BRepTools_WireExplorer edge(wire, patch.footprint); edge.More(); edge.Next())
      outline.push_back(BRep_Tool::Pnt(edge.CurrentVertex()));
    if (outline.size() < 3) return false;
    if (heightPlanArea(outline) < 0) std::reverse(outline.begin(), outline.end());
    const auto bounds = heightPlanBounds(outline);
    std::optional<HeightConstraint> constraint;
    if (!patch.surface.IsNull()) constraint = HeightConstraint{source, BRepAdaptor_Surface(patch.surface).Plane()};
    regions.push_back({std::move(outline), bounds, constraint});
  }

  std::vector<HeightCell> result;
  for (auto& cell : cells) {
    if (cell.unconfirmed) {
      result.push_back(std::move(cell));
      continue;
    }
    const auto bounds = heightPlanBounds(cell.plan);
    for (const auto& region : regions) {
      if (bounds[2] <= region.bounds[0] || bounds[0] >= region.bounds[2] ||
          bounds[3] <= region.bounds[1] || bounds[1] >= region.bounds[3]) continue;
      auto intersection = cell.plan;
      for (std::size_t i = 0; i < region.outline.size() && !intersection.empty(); ++i) {
        const auto& a = region.outline[i];
        const gp_Vec edge(a, region.outline[(i + 1) % region.outline.size()]);
        intersection = clipHeightPolygon(intersection, [&](const auto& p) {
          return edge.X() * (p.Y() - a.Y()) - edge.Y() * (p.X() - a.X());
        });
      }
      if (intersection.empty()) continue;
      if (region.constraint)
        applyHeightConstraint({std::move(intersection), cell.limit}, *region.constraint, result);
      else
        result.push_back({std::move(intersection), std::nullopt, true});
      if (result.size() > kHeightCellBudget) return false;
    }
  }
  cells = std::move(result);
  return true;
}

}  // namespace

Result<HeightLimitPreview> heightLimitPreview(const Document& document, const HeightLimitStudy& study) {
  using R = Result<HeightLimitPreview>;
  const auto& site = document.settings().siteBoundary;
  if (!site) return R::failure("先に敷地境界を登録してください");
  if (!study.road && !study.neighbor && !study.northRiseMm && !study.district)
    return R::failure("合成する制限を選んでください");
  if (!std::isfinite(study.groundZMm)) return R::failure("地盤面の高さを確認してください");
  const auto geometry = document.siteBoundaryGeometry(site->outline);
  if (!geometry) return R::failure(geometry.error());
  const auto footprint = heightSiteFootprint(*geometry);
  if (!footprint) return R::failure(footprint.error());

  try {
    std::optional<NeighborHeightField> neighbor;
    if (study.neighbor) {
      const auto field = neighborHeightField(document, study.boundaries, study.groundZMm,
                                            study.neighbor->riseMm, study.neighbor->slope);
      if (!field) return R::failure(field.error());
      neighbor = *field;
    }
    const auto triangles = heightStudyTriangles(*footprint, neighbor.has_value());
    if (!triangles) return R::failure(triangles.error());
    std::vector<HeightCell> cells;
    for (const auto& triangle : *triangles) {
      HeightCell cell{{triangle.begin(), triangle.end()}, std::nullopt};
      if (neighbor) {
        auto lifted = triangle;
        for (auto& p : lifted) p.SetZ(neighbor->sample(p).limitZMm);
        cell.limit = HeightConstraint{HeightLimitSource::Neighbor,
            gp_Pln(lifted[0], gp_Dir(gp_Vec(lifted[0], lifted[1]).Crossed(gp_Vec(lifted[0], lifted[2]))))};
      }
      cells.push_back(std::move(cell));
    }

    if (study.road) {
      const auto& rule = *study.road;
      const auto parameters = roadHeightParameters(rule.edgeHeightMm, rule.slope, rule.range);
      if (!parameters) return R::failure(parameters.error());
      const auto road = roadHeightReference(document);
      if (!road) return R::failure(road.error());
      const auto& [height, start, end, rise] = *parameters;
      const auto station = [&](const gp_Pnt& point) { return gp_Vec(road->origin, point).Dot(gp_Vec(road->inward)); };
      const HeightConstraint constraint{HeightLimitSource::Road,
          gp_Pln(gp_Pnt(road->origin.X(), road->origin.Y(), height),
                 gp_Dir(-rule.slope * road->inward.X(), -rule.slope * road->inward.Y(), 1))};
      std::vector<HeightCell> result;
      for (const auto& cell : cells) {
        const auto before = clipHeightPolygon(cell.plan, [&](const auto& p) { return start - station(p); });
        const auto after = clipHeightPolygon(cell.plan, [&](const auto& p) { return station(p) - end; });
        if (!before.empty()) result.push_back({before, cell.limit});
        if (!after.empty()) result.push_back({after, cell.limit});
        auto band = clipHeightPolygon(cell.plan, [&](const auto& p) { return station(p) - start; });
        band = clipHeightPolygon(band, [&](const auto& p) { return end - station(p); });
        applyHeightConstraint({std::move(band), cell.limit}, constraint, result);
      }
      cells = std::move(result);
    }

    const auto addPartition = [&](const HeightPatches& patches, HeightLimitSource source) -> Status {
      if (!patches) return Status::failure(patches.error());
      if (!overlayHeightPartition(cells, *patches, source))
        return Status::failure("制限面の分割が複雑で、合成面を作れません");
      return Status::success();
    };
    if (study.northRiseMm) {
      const auto added = addPartition(statutoryHeightPatches(document, study.boundaries, study.groundZMm,
                                                                  *study.northRiseMm), HeightLimitSource::North);
      if (!added) return R::failure(added.message());
    }
    if (study.district) {
      const auto added = addPartition(yokohamaHeightDistrictPatches(document, study.boundaries, study.groundZMm,
                                                                    study.singleNorthRoadConfirmed), HeightLimitSource::District);
      if (!added) return R::failure(added.message());
    }

    HeightLimitPreview preview;
    BRep_Builder builder;
    std::array<TopoDS_Compound, static_cast<std::size_t>(HeightLimitSource::Count)> surfaces;
    for (auto& surface : surfaces) builder.MakeCompound(surface);
    TopoDS_Compound unconfirmed;
    builder.MakeCompound(unconfirmed);
    for (auto& cell : cells) {
      const double area = std::abs(heightPlanArea(cell.plan));
      if (cell.unconfirmed) {
        preview.unconfirmedAreaMm2 += area;
        // Lift the fill above the site face to avoid coincident depth samples.
        for (auto& p : cell.plan) p.SetZ(p.Z() + 2);
      } else if (cell.limit) {
        for (auto& p : cell.plan) p.SetZ(cell.limit->at(p));
      } else {
        preview.noLimitAreaMm2 += area;
        continue;
      }
      const auto face = planarHeightFace(cell.plan);
      if (face.IsNull()) return R::failure("合成した制限面を作れません");
      if (cell.unconfirmed) builder.Add(unconfirmed, face);
      else builder.Add(surfaces[static_cast<std::size_t>(cell.limit->source)], face);
    }
    for (std::size_t i = 0; i < surfaces.size(); ++i)
      if (surfaces[i].NbChildren()) preview.surfaces[i] = surfaces[i];
    if (unconfirmed.NbChildren()) preview.unconfirmed = unconfirmed;
    return preview;
  } catch (const Standard_Failure&) {
    return R::failure("斜線と高度地区の合成面を作れません");
  }
}

Result<CombinedHeightComparison> compareCombinedHeight(
    const Document& document, const HeightLimitStudy& study, const std::vector<ObjectId>& ids) {
  using R = Result<CombinedHeightComparison>;
  if (ids.empty()) return R::failure("モデルで照合する部材を選択してください");
  auto preview = heightLimitPreview(document, study);
  if (!preview) return R::failure(preview.error());
  const auto& site = *document.settings().siteBoundary;
  const auto geometry = document.siteBoundaryGeometry(site.outline);
  if (!geometry) return R::failure(geometry.error());
  const auto footprint = heightSiteFootprint(*geometry);
  if (!footprint) return R::failure(footprint.error());
  try {
    std::vector<HeightPatch> patches;
    const double planZ = geometry->vertices.front().Z();
    const auto collect = [&](const TopoDS_Shape& shape, std::optional<HeightLimitSource> source) {
      for (TopExp_Explorer entry(shape, TopAbs_FACE); entry.More(); entry.Next()) {
        const auto face = TopoDS::Face(entry.Current());
        HeightPolygon plan;
        for (BRepTools_WireExplorer edge(BRepTools::OuterWire(face), face); edge.More(); edge.Next()) {
          auto point = BRep_Tool::Pnt(edge.CurrentVertex());
          point.SetZ(planZ);
          plan.push_back(point);
        }
        const auto area = planarHeightFace(plan);
        if (area.IsNull()) return false;
        patches.push_back({area, source ? face : TopoDS_Face(), {}, 0, false, true, source});
      }
      return true;
    };
    for (std::size_t i = 0; i < preview->surfaces.size(); ++i)
      if (!collect(preview->surfaces[i], static_cast<HeightLimitSource>(i)))
        return R::failure("合成面の照合範囲を作れません");
    if (!collect(preview->unconfirmed, std::nullopt)) return R::failure("未確認の範囲を作れません");

    const auto measurements = measureHeightParts(document, ids, patches, *footprint);
    if (!measurements) return R::failure(measurements.error());
    CombinedHeightComparison result{*preview, {}, study.neighbor.has_value()};
    for (const auto& row : *measurements)
      result.rows.push_back({row.id, row.name, heightMeasurementState(row), row.excess,
                             row.patch ? patches[*row.patch].source : std::nullopt, row.unknown});
    return result;
  } catch (const Standard_Failure&) {
    return R::failure("合成面と部材を照合できません");
  }
}

namespace {

const char* heightLimitSourceName(HeightLimitSource source) {
  static const std::array<const char*, 4> names{"道路斜線", "隣地斜線", "北側斜線", "高度地区"};
  return names[static_cast<std::size_t>(source)];
}

std::vector<std::string> combinedHeightBasis(const Document& document, const HeightLimitStudy& study, bool compact = false) {
  const auto value = [compact](double n) {
    auto text = number(n, 6);
    if (compact && text.find('.') != std::string::npos) {
      text.erase(text.find_last_not_of('0') + 1);
      if (text.back() == '.') text.pop_back();
    }
    return text;
  };
  std::vector<std::string> basis;
  if (study.neighbor || study.northRiseMm || study.district) basis.push_back("地盤Z " + value(study.groundZMm) + " mm");
  const auto append = [&](std::string entry) { basis.push_back(std::move(entry)); };
  if (study.road) {
    const auto& road = *study.road;
    append("道路斜線　境界Z " + value(road.edgeHeightMm) + " mm、勾配 " + value(road.slope) +
           "、幅員 " + value(road.range.roadWidthMm) + " mm、適用距離 " + value(road.range.applicationDistanceMm) +
           " mm、後退 " + value(road.range.buildingSetbackMm) + " mm");
  }
  if (study.neighbor)
    append("隣地斜線　立上り " + value(study.neighbor->riseMm) + " mm、勾配 " + value(study.neighbor->slope));
  if (study.northRiseMm) append("北側斜線　立上り " + value(*study.northRiseMm) + " mm");
  if (study.district) {
    const auto districtCode = siteHeightConditions(document).district;
    const auto& districts = heightDistrictChoices();
    const auto label = std::find_if(districts.begin(), districts.end(),
                                  [&](const auto& entry) { return entry.first == districtCode; });
    append("高度地区　" + (compact && label != districts.end() ? label->second : districtCode));
  }
  const auto& settings = document.settings();
  const auto district = study.district ? yokohamaHeightDistrictRule(siteHeightConditions(document).district) : std::nullopt;
  if (study.northRiseMm || (district && district->northRiseMm)) {
    if (settings.siteNorth) append("真北 +Y 時計回り " + value(settings.siteNorth->angle) + " 度");
    if (settings.siteBoundary && settings.siteBoundary->road)
      append("北側の算定に用いる登録幅員 " + value(settings.siteBoundary->road->widthMm) + " mm");
  }
  if (district && district->northRiseMm)
    append(study.singleNorthRoadConfirmed ? "高度地区　北側の単独平行道路を確認済み" : "高度地区　北側の単独平行道路の確認なし");
  for (const auto& edge : study.boundaries) {
    const auto xy = [&](const auto& p) { return "(" + value(p.X()) + " " + value(p.Y()) + ")"; };
    append("確認済みの隣地辺 XY " + xy(edge[0]) + "–" + xy(edge[1]) + " mm");
  }

  return basis;
}

}  // namespace

std::string combinedHeightCsv(const Document& document, const HeightLimitStudy& study,
                              const CombinedHeightComparison& comparison) {
  std::string basis;
  for (const auto& entry : combinedHeightBasis(document, study))
    basis += (basis.empty() ? "" : " ／ ") + entry;
  Csv csv;
  csv.line({"部材", "部材ID", "照合結果", "最大鉛直差（mm）", "差を決めた制限", "確認範囲",
            "照合面", "法規適合", "採用条件"});
  for (const auto& row : comparison.rows) {
    auto name = row.name;
    // Spreadsheet applications may evaluate a text field beginning with a formula prefix.
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && std::string_view("=+-@").find(name[first]) != std::string_view::npos)
      name.insert(0, "'");
    csv.line({name, row.id, row.state, row.excessMm ? number(*row.excessMm, 6) : "",
              row.controllingSource ? heightLimitSourceName(*row.controllingSource) : "",
              row.needsReview ? "未確認の範囲あり" : "選んだ条件",
              comparison.approximate ? "隣地斜線を含む近似面" : "合成面", "未評価", basis});
  }
  return csv.out.str();
}

namespace {

// Intersect a polygon boundary with the vertical section plane and project into section U, Z.
std::vector<gp_Pnt2d> heightCutPoints(const std::vector<gp_Pnt>& ring, const gp_Trsf& toSection) {
  std::vector<gp_Pnt2d> points;
  const auto add = [&](const gp_Pnt& p) {
    const gp_Pnt2d point(p.X(), p.Y());
    if (std::none_of(points.begin(), points.end(), [&](const auto& other) {
          return other.Distance(point) <= tolerance::kLinear;
        })) points.push_back(point);
  };
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const gp_Pnt a = ring[i].Transformed(toSection);
    const gp_Pnt b = ring[(i + 1) % ring.size()].Transformed(toSection);
    if (std::abs(a.Z()) <= tolerance::kLinear) add(a);
    if ((a.Z() < -tolerance::kLinear && b.Z() > tolerance::kLinear) ||
        (b.Z() < -tolerance::kLinear && a.Z() > tolerance::kLinear))
      add(a.Translated(gp_Vec(a, b) * (a.Z() / (a.Z() - b.Z()))));
  }
  std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.X() < b.X(); });
  return points;
}

struct HeightCutSpan {
  gp_Pnt2d a, b;
  std::optional<HeightLimitSource> source;
  double at(double u) const { return a.Y() + (b.Y() - a.Y()) * (u - a.X()) / (b.X() - a.X()); }
};

}  // namespace

Result<HeightLimitEvidence> heightLimitEvidence(
    const Document& document, const HeightLimitStudy& study, const CombinedHeightComparison& comparison,
    const ObjectId& selected, bool useActiveSection, std::optional<double> roadSurfaceZ) {
  using R = Result<HeightLimitEvidence>;
  const auto& site = document.settings().siteBoundary;
  const auto* target = document.find(selected);
  if (!site || !target) return R::failure("敷地と断面を通す部材を指定してください");
  if (roadSurfaceZ && !std::isfinite(*roadSurfaceZ)) return R::failure("路面高さを確認してください");
  const auto info = document.siteBoundaryGeometry(site->outline);
  if (!info) return R::failure(info.error());
  const auto footprint = heightSiteFootprint(*info);
  if (!footprint) return R::failure(footprint.error());
  const auto bounds = document.worldBounds(*target);
  if (bounds.IsVoid() || bounds.IsOpen()) return R::failure("断面を通す部材の位置を測れません");
  const auto centre = bounds.CornerMin().XYZ().Added(bounds.CornerMax().XYZ()).Divided(2);
  gp_Pnt origin(centre.X(), centre.Y(), 0);
  const auto road = roadHeightReference(document);
  gp_Dir toward(1, 0, 0);
  if (road) toward = road->inward.Crossed(gp::DZ());
  if (useActiveSection) {
    const auto& sections = document.settings().sections;
    const auto active = std::find_if(sections.rbegin(), sections.rend(), [](const auto& section) {
      return section.active && std::hypot(section.normal[0], section.normal[1]) > 1e-8 &&
             std::abs(section.normal[2]) <= 1e-8;
    });
    if (active == sections.rend()) return R::failure("有効な鉛直断面がありません");
    origin = gp_Pnt(active->origin[0], active->origin[1], 0);
    toward = gp_Dir(active->normal[0], active->normal[1], 0);
  }
  auto spec = draw::sectionView(origin, toward);
  spec.grid = false;
  spec.title = "H-H 高さ制限断面図";
  gp_Trsf toSection;
  toSection.SetTransformation(spec.frame);
  const auto fromSection = toSection.Inverted();
  const auto world = [&](double u, double z) { return gp_Pnt(u, z, 0).Transformed(fromSection); };
  const auto siteCuts = heightCutPoints(info->vertices, toSection);
  std::vector<std::pair<double, double>> siteSpans;
  for (std::size_t i = 1; i < siteCuts.size(); ++i) {
    const double a = siteCuts[i - 1].X(), b = siteCuts[i].X();
    if (b - a <= tolerance::kLinear) continue;
    auto point = world((a + b) / 2, 0);
    point.SetZ(info->vertices.front().Z());
    const auto state = BRepClass_FaceClassifier(*footprint, point, tolerance::kLinear).State();
    if (state == TopAbs_IN || state == TopAbs_ON) siteSpans.emplace_back(a, b);
  }
  if (siteSpans.empty()) return R::failure("敷地を横切る断面を選んでください");

  try {
    auto made = draw::makeView(document, spec);
    if (!made) return R::failure(made.error());
    HeightLimitEvidence evidence{std::move(made.value()), {}};
    auto& section = evidence.section;
    const auto line = [&](draw::Pen pen, double u0, double z0, double u1, double z1) {
      draw::Item item;
      item.pen = pen;
      item.points = {gp_Pnt2d(u0, z0), gp_Pnt2d(u1, z1)};
      section.items.push_back(std::move(item));
      section.minU = std::min({section.minU, u0, u1});
      section.maxU = std::max({section.maxU, u0, u1});
      section.minV = std::min({section.minV, z0, z1});
      section.maxV = std::max({section.maxV, z0, z1});
    };
    const auto label = [&](double u, double z, std::string text, int align = 1) {
      draw::Item item;
      item.kind = draw::Item::Kind::Text;
      item.pen = draw::Pen::Text;
      item.points = {gp_Pnt2d(u, z)};
      item.text = std::move(text);
      item.height = 2.6;
      item.hAlign = align;
      section.items.push_back(std::move(item));
      section.minV = std::min(section.minV, z);
      section.maxV = std::max(section.maxV, z);
    };

    std::optional<double> ground;
    if (study.neighbor || study.northRiseMm || study.district) ground = study.groundZMm;
    else for (const auto& level : document.settings().levels) if (level.name == "GL") ground = level.elevation;
    const double datum = ground.value_or(info->vertices.front().Z());
    section.levels.erase(std::remove_if(section.levels.begin(), section.levels.end(),
        [&](const auto& mark) { return mark.name == "GL" || std::abs(mark.elevation - datum) < 20; }), section.levels.end());
    const auto zText = [](double z) { return number(z / 1000, 3); };
    std::optional<double> roadU, roadFar;
    double roadWidth = 0;
    if (road) {
      const auto edge = document.siteRoadEdge(*info);
      const auto crossing = heightCutPoints({(*edge)[0], (*edge)[1]}, toSection);
      const double across = gp_Vec(road->inward).Dot(gp_Vec(spec.frame.XDirection()));
      if (crossing.size() == 1 && std::abs(across) >= 0.01) {
        roadU = crossing.front().X();
        roadWidth = study.road ? study.road->range.roadWidthMm : site->road->widthMm;
        if (roadWidth > 0) roadFar = *roadU - roadWidth / across;
      }
    }
    const std::size_t buildingItems = section.items.size();
    for (const auto& [a, b] : siteSpans) {
      line(ground ? draw::Pen::Visible : draw::Pen::Level, a, datum, b, datum);
      if (ground) {
        const double spacing = std::max(480.0, (b - a) / 85);
        for (double u = a; u + 250 <= b; u += spacing)
          line(draw::Pen::Visible, u, datum, u + 250, datum - 250);
      }
      for (const double u : {a, b}) {
        if (roadU && std::abs(u - *roadU) <= tolerance::kLinear) continue;
        line(draw::Pen::Hidden, u, datum - 450, u, datum + 1800);
        label(u, datum + 1950, "敷地境界線");
      }
    }
    // The ground symbol belongs underneath the building cut.
    std::rotate(section.items.begin(), section.items.begin() + buildingItems, section.items.end());
    label(siteSpans.back().second - 300, datum - 1000,
          (ground ? "算定用地盤面 Z " : "敷地輪郭 Z ") + zText(datum) + " m", 2);

    if (roadU) {
      const double roadZ = roadSurfaceZ.value_or(datum);
      const double dimensionZ = std::min(roadZ, datum) - 1650;
      const double boundaryLabelZ = std::max(roadZ, datum) + 2000;
      line(draw::Pen::Hidden, *roadU, dimensionZ, *roadU, boundaryLabelZ - 150);
      label(*roadU, boundaryLabelZ, "道路境界線");
      if (roadFar) {
        line(draw::Pen::Hidden, *roadFar, dimensionZ, *roadFar, boundaryLabelZ - 150);
        label(*roadFar, boundaryLabelZ, "道路境界線");
        if (roadSurfaceZ) line(draw::Pen::Visible, *roadFar, roadZ, *roadU, roadZ);
        const double middle = (*roadFar + *roadU) / 2;
        label(middle, roadZ + 470, roadSurfaceZ ? "前面道路" : "前面道路（路面高未入力）");
        const double dimension = std::abs(*roadFar - *roadU);
        const auto text = std::abs(dimension - roadWidth) <= 0.5 ? "幅員 " + zText(roadWidth) + " m" :
            "断面 " + zText(dimension) + " m（幅員 " + zText(roadWidth) + " m）";
        const gp_Pnt2d left(std::min(*roadU, *roadFar), roadZ), right(std::max(*roadU, *roadFar), roadZ);
        section.dimensions.push_back({left, right, gp_Vec2d(0, dimensionZ - roadZ), text});
        section.minV = std::min(section.minV, dimensionZ);
        if (roadSurfaceZ) label(middle, roadZ - 770, "路面 Z " + zText(roadZ) + " m");
      } else {
        label(*roadU, dimensionZ, "前面道路（幅員未入力）");
      }
    }

    std::vector<HeightCutSpan> spans;
    const auto intersect = [&](const TopoDS_Shape& shape, std::optional<HeightLimitSource> source) {
      for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
        const auto face = TopoDS::Face(faces.Current());
        std::vector<gp_Pnt> ring;
        for (BRepTools_WireExplorer edge(BRepTools::OuterWire(face), face); edge.More(); edge.Next())
          ring.push_back(BRep_Tool::Pnt(edge.CurrentVertex()));
        const auto cut = heightCutPoints(ring, toSection);
        if (cut.size() > 1 && cut.back().X() - cut.front().X() > tolerance::kLinear)
          spans.push_back({cut.front(), cut.back(), source});
      }
    };
    for (std::size_t i = 0; i < comparison.preview.surfaces.size(); ++i)
      intersect(comparison.preview.surfaces[i], static_cast<HeightLimitSource>(i));
    intersect(comparison.preview.unconfirmed, std::nullopt);

    std::vector<double> breaks;
    for (const auto& span : spans) {
      breaks.push_back(span.a.X());
      breaks.push_back(span.b.X());
    }
    for (std::size_t i = 0; i < spans.size(); ++i) {
      const auto& a = spans[i];
      if (!a.source) continue;
      for (std::size_t j = i + 1; j < spans.size(); ++j) {
        const auto& b = spans[j];
        if (!b.source) continue;
        const double left = std::max(a.a.X(), b.a.X()), right = std::min(a.b.X(), b.b.X());
        if (right - left <= tolerance::kLinear) continue;
        const double d0 = a.at(left) - b.at(left), d1 = a.at(right) - b.at(right);
        if ((d0 < -tolerance::kLinear && d1 > tolerance::kLinear) ||
            (d1 < -tolerance::kLinear && d0 > tolerance::kLinear))
          breaks.push_back(left + (right - left) * d0 / (d0 - d1));
      }
    }
    std::sort(breaks.begin(), breaks.end());
    std::vector<HeightCutSpan> outline;
    for (std::size_t i = 1; i < breaks.size(); ++i) {
      const double left = breaks[i - 1], right = breaks[i];
      if (right - left <= tolerance::kLinear) continue;
      const double mid = (left + right) / 2;
      const HeightCutSpan* lowest = nullptr;
      for (const auto& span : spans) {
        if (mid < span.a.X() || mid > span.b.X()) continue;
        if (!span.source) { lowest = &span; break; }
        if (!lowest || span.at(mid) < lowest->at(mid)) lowest = &span;
      }
      if (!lowest) continue;
      const auto z = [&](double u) { return lowest->source ? lowest->at(u) : datum - 900; };
      outline.push_back({gp_Pnt2d(left, z(left)), gp_Pnt2d(right, z(right)), lowest->source});
    }
    for (const auto& span : outline)
      line(span.source ? draw::Pen::HeightLimit : draw::Pen::HeightReview,
           span.a.X(), span.a.Y(), span.b.X(), span.b.Y());

    std::vector<PartShape> parts;
    for (const auto& row : comparison.rows)
      if (const auto* object = document.find(row.id)) {
        const auto shape = document.worldShape(*object);
        const auto box = geom::exactBounds(shape);
        if (!shape.IsNull() && !box.IsVoid() && !box.IsOpen()) parts.push_back({object, shape, box});
      }
    double rayLow = datum - 1;
    for (const auto& part : parts) rayLow = std::min(rayLow, part.box.CornerMin().Z() - 1);
    draw::Table stations;
    stations.title = "H-H 高さ制限計算表";
    stations.headers = {"断面距離 (m)", "制限・状態", "制限Z (m)", "部材Z (m)", "鉛直差 (mm)"};
    const double distanceOrigin = roadU.value_or(siteSpans.front().first);
    const double anchorU = gp_Pnt(centre).Transformed(toSection).X();
    const double spacing = std::max(600.0, (section.maxU - section.minU) / 30);
    const auto topAt = [&](double u) {
      const auto point = world(u, 0);
      return highestOver(parts, point.X(), point.Y(), rayLow);
    };
    double previous = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < outline.size() && stations.rows.size() < 16;) {
      std::size_t end = i + 1;
      while (end < outline.size() && outline[end].source == outline[i].source &&
             outline[end - 1].b.Distance(outline[end].a) <= tolerance::kLinear) ++end;
      const double l = outline[i].a.X(), r = outline[end - 1].b.X();
      double u = l < anchorU && anchorU < r ? anchorU : (l + r) / 2;
      auto top = topAt(u);
      if (!top) {
        for (const auto& part : parts) {
          const auto mid = part.box.CornerMin().XYZ().Added(part.box.CornerMax().XYZ()).Divided(2);
          const double candidate = gp_Pnt(mid).Transformed(toSection).X();
          if (candidate <= l || candidate >= r || candidate - previous < spacing) continue;
          const auto hit = topAt(candidate);
          if (!hit) continue;
          u = candidate;
          top = hit;
          break;
        }
      }
      const auto sample = std::find_if(outline.begin() + i, outline.begin() + end,
                                       [&](const auto& run) { return u >= run.a.X() && u <= run.b.X(); });
      i = end;
      if (sample == outline.end() || u - previous < spacing) continue;
      previous = u;
      const double limitZ = sample->at(u);
      stations.rows.push_back({zText(u - distanceOrigin),
          sample->source ? heightLimitSourceName(*sample->source) : "要確認",
          sample->source ? zText(limitZ) : "—", top ? zText(top->first) : "—",
          sample->source && top ? number(top->first - limitZ, 1) : "—"});
    }
    if (stations.rows.empty()) stations.rows.push_back({"—", "断面内に試算面なし", "—", "—", "—"});
    stations.notes = {roadU ? "断面距離の原点は道路側の敷地境界。右向きを正とする。"
                            : "断面距離の原点は左側の敷地境界。右向きを正とする。",
                      "部材Zは測点直上で照合対象部材の形状に当たる最高点。正の差は超過。法規適合は未評価。"};
    if (comparison.approximate) stations.notes.push_back("隣地斜線を含む近似面で算定。");
    section.calculationNotes = {"青実線：合成制限線　橙破線：未確認範囲。高さと差はH-H 高さ制限計算表。",
                                "地盤は算定に採用した高さ。法規適合は未評価。"};
    evidence.tables.push_back(std::move(stations));

    constexpr std::size_t rowsPerSheet = 22;
    for (std::size_t start = 0; start < comparison.rows.size(); start += rowsPerSheet) {
      draw::Table partsTable;
      partsTable.title = "高さ制限 部材照合表";
      if (comparison.rows.size() > rowsPerSheet) partsTable.title += " " + std::to_string(1 + start / rowsPerSheet);
      partsTable.headers = {"部材", "結果", "最大鉛直差 (mm)", "差を決めた制限", "確認範囲"};
      for (std::size_t i = start; i < std::min(start + rowsPerSheet, comparison.rows.size()); ++i) {
        const auto& row = comparison.rows[i];
        partsTable.rows.push_back({row.name, row.state, row.excessMm ? number(*row.excessMm, 2) : "—",
            row.controllingSource ? heightLimitSourceName(*row.controllingSource) : "—",
            row.needsReview ? "確認済み範囲の値" : "選択した条件"});
      }
      partsTable.notes = {"部材表の値は部材全体の照合。H-H断面の測点の差は高さ制限計算表。法規適合は未評価。"};
      if (comparison.approximate) partsTable.notes.push_back("隣地斜線を含む近似面で算定。");
      evidence.tables.push_back(std::move(partsTable));
    }
    auto basis = combinedHeightBasis(document, study, true);
    if (roadU)
      basis.push_back(roadSurfaceZ && roadFar ? "断面の路面Z　" + zText(*roadSurfaceZ) + " m（図示用）" :
                                              "断面の路面Z　未入力");
    for (std::size_t start = 0; start < basis.size(); start += rowsPerSheet) {
      draw::Table conditionTable;
      conditionTable.title = "高さ制限 採用条件";
      if (basis.size() > rowsPerSheet) conditionTable.title += " " + std::to_string(1 + start / rowsPerSheet);
      conditionTable.headers = {"項目", "採用値"};
      for (std::size_t i = start; i < std::min(start + rowsPerSheet, basis.size()); ++i) {
        const auto split = basis[i].find("　");
        const auto space = basis[i].find(' ');
        const auto cut = std::min(split, space);
        conditionTable.rows.push_back({basis[i].substr(0, cut),
            cut == std::string::npos ? "—" : basis[i].substr(cut + (cut == split ? 3 : 1))});
      }
      conditionTable.notes = {"入力条件に基づく試算。法規適合は未評価。"};
      evidence.tables.push_back(std::move(conditionTable));
    }
    const double requiredScale = std::max((section.maxU - section.minU) / 360, (section.maxV - section.minV) / 185);
    for (const double scale : {100.0, 200.0, 300.0, 500.0, 1000.0}) {
      evidence.scale = scale;
      if (scale >= requiredScale) break;
    }
    return evidence;
  } catch (const Standard_Failure&) {
    return R::failure("高さ制限の根拠図を作れません");
  }
}

std::vector<PermitCheck> permitElevationChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto& settings = document.settings();
  if (!settings.siteBoundary) {
    out.push_back({"高さ制限の条件", "敷地境界待ち", "敷地境界を登録すると、用途地域と高度地区を入力できます。", {}, true});
  } else {
    const auto conditions = siteHeightConditions(document);
    const auto label = [](const auto& choices, const std::string& code) {
      const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& choice) { return choice.first == code; });
      return it == choices.end() ? std::string("未入力") : it->second;
    };
    const std::string zone = label(heightZoneChoices(), conditions.zone);
    const std::string district = label(heightDistrictChoices(), conditions.district);
    const bool missing = zone == "未入力" || district == "未入力" || conditions.source.empty();
    out.push_back({"高さ制限の条件", missing ? "入力不足" : "入力済み",
                   "用途地域　" + zone + "\n高度地区　" + district + "\n確認資料　" +
                       (conditions.source.empty() ? "未入力" : conditions.source) + "\n高さの照合　未評価", {}, missing});
    if (const auto rule = yokohamaHeightDistrictRule(conditions.district)) {
      std::string details = district + "\n最高高さ　" + number(rule->heightCapMm / 1000, 0) + " m";
      if (rule->northRiseMm)
        details += "\n北側斜線　立上り " + number(*rule->northRiseMm / 1000, 1) + " m・勾配 0.6";
      details += "\n出典　横浜市「最高限高度地区の概要と取扱い」令和8年4月版";
      out.push_back({"横浜市の最高限高度地区", "基準値", std::move(details), {}});
    }
  }
  const FloorLevel* gl = nullptr;
  for (const FloorLevel& l : settings.levels)
    if (l.name == "GL") gl = &l;
  if (gl)
    out.push_back({"地盤（GL）", "入力済み", "レベル GL　" + number(gl->elevation, 0) + " mm（立面・断面の地盤の線と、高さの基準）", {}});
  else
    out.push_back({"地盤（GL）", "未入力", "レベルに「GL」がありません。立面・断面に地盤の線を出し、高さを測る基準にするため、GL を足します。", {}, true});
  // The highest point of the building (walls, roofs, floors and what stands on them).
  double top = -1e300;
  ObjectId highest;
  for (const Object& o : document.objects()) {
    const std::string kind = attribute(o, "kind");
    if (kind != "wall" && kind != "roof" && kind != "slab" && kind != "balcony" && kind != "canopy" && kind != "stair" &&
        kind != "column" && kind != "beam")
      continue;
    const Bnd_Box box = document.worldBounds(o);
    if (box.IsVoid()) continue;
    if (box.CornerMax().Z() > top) {
      top = box.CornerMax().Z();
      highest = o.id;
    }
  }
  if (!highest.empty()) {
    const double base = gl ? gl->elevation : 0.0;
    out.push_back({"いちばん高い所（参考）", "参考",
                   std::string(gl ? "GL" : "z = 0 ") + "から " + number(top - base, 0) + " mm（" + document.find(highest)->name +
                       "）。法の建築物の高さ・軒の高さの測り方（令 2 条 1 項 6・7 号）による値ではありません。",
                   {highest}});
  }
  const int sections = static_cast<int>(std::count_if(settings.sections.begin(), settings.sections.end(),
                                                      [](const SectionPlane& s) { return s.active && std::abs(s.normal[2]) < 0.5; }));
  out.push_back({"断面の切る位置", "入力済み",
                 sections > 0 ? "断面ツールで決めた縦の断面 " + std::to_string(sections) + " か所。図面一式で断面図になり、平面図に切る位置の線が出ます（docs/132）。"
                              : std::string("断面ツールで決めた所はありません。図面一式では建物の真ん中を通る 2 つの断面（A-A 東西・B-B 南北）を出します（docs/132）。必要な位置は断面ツールで決めます。"),
                 {}});
  if (settings.siteNorth)
    out.push_back({"立面の向き", "入力済み", "真北はモデルの +Y から時計回り " + number(settings.siteNorth->angle, 1) +
                                               "°。立面図の名前（南・東・北・西）はモデルの軸に沿った向きです。真北に対して斜めの面の呼び方は確かめます。",
                   {}});
  else
    out.push_back({"立面の向き", "要確認", "真北が未入力です。立面図の名前（南・東・北・西）はモデルの軸に沿った向きで出ます。", {}, true});
  const auto compartments = fireCompartmentReview(document);
  if (!compartments.empty()) {
    std::string details;
    std::vector<ObjectId> targets;
    for (const auto& row : compartments) {
      if (!details.empty()) details += "\n";
      details += row.boundary.mark + "　" + row.hostName + "　" + row.openingMark + "　" + row.status + "　";
      details += fireCompartmentRequirementName(row.boundary.requirement) + "　" + row.boundary.source + "　" + row.equipment.reference;
      targets.push_back(row.host);
    }
    out.push_back({"防火区画・開口部", "照合資料あり", std::move(details), std::move(targets), true});
  }
  const auto fire = fireSpreadStudy(document, false);
  if (fire.configured) {
    std::string details = fire.changed ? fire.issue : "基準線 " + std::to_string(fire.bases.size()) + " 本　" + fire.conditions.source;
    std::vector<ObjectId> targets;
    for (const auto& opening : fire.openings) {
      details += "\n" + opening.row.mark + "　" + opening.row.level + "　" + fireSpreadStatus(opening.status);
      if (opening.distance) details += "　" + number(*opening.distance / 1000, 3) + " m";
      targets.push_back(opening.row.host);
    }
    out.push_back({"延焼ライン・外壁開口部", fire.changed ? "条件変更" : "照合資料あり", std::move(details), std::move(targets), true});
  }
  out.push_back({"まだ確かめていない事", "未評価",
                 "道路斜線・隣地斜線・北側斜線と高度地区の制限、法の高さ・軒の高さ、平面・立面・断面の寸法の突合せ。申請先と申請時点の法令で確かめます。",
                 {}});
  return out;
}

namespace {
// The stages read from the model alone (the site stage is read in the app: it shows its own rows).
std::vector<std::pair<std::string, std::vector<PermitCheck>>> modelStages(const Document& document) {
  return {{"2 通り芯", permitGridChecks(document)},   {"3 平面", permitPlanChecks(document)},
          {"5 階段", permitStairChecks(document)},     {"6 屋根・庇・バルコニー", permitRoofChecks(document)},
          {"7 耐力壁", permitBraceChecks(document)},    {"8 基礎", permitFoundationChecks(document)},
          {"9 省エネ・設備", permitEnergyChecks(document)}, {"10 立面・断面", permitElevationChecks(document)}};
}
}  // namespace

std::vector<PermitCheck> permitSetChecks(const Document& document) {
  std::vector<PermitCheck> out;
  for (const auto& [stage, rows] : modelStages(document)) {
    std::map<std::string, int> counts;
    std::string items;
    int open = 0;
    for (const PermitCheck& c : rows) {
      if (!c.attention) continue;
      ++open;
      ++counts[c.state];
      if (open <= 4) items += "\n・" + c.item + "（" + c.state + "）";
    }
    std::string summary;
    for (const auto& [state, n] : counts) summary += (summary.empty() ? "" : "、") + state + " " + std::to_string(n);
    if (open > 4) items += "\n・ほか " + std::to_string(open - 4);
    out.push_back({stage, open == 0 ? "残りなし" : "残り " + std::to_string(open),
                   open == 0 ? "その工程で見る事は、どれも入力済み・参考値です（未評価の行は残ります）。" : summary + items, {}, open > 0});
  }
  // Marks given by hand to openings that are not alike (docs/109): one mark, different openings.
  std::vector<std::string> mixed;
  for (const OpeningTypeRow& t : openingSchedule(document).types)
    if (t.mixed) mixed.push_back(t.mark);
  if (!mixed.empty()) {
    std::string marks;
    for (const auto& m : mixed) marks += (marks.empty() ? "" : "、") + m;
    out.push_back({"建具の符号", "要確認", "手で付けた同じ符号なのに、種類・建具・大きさ・品番が違う開口があります：" + marks +
                                              "。建具表では最初の物の値が出ます。",
                   {}, true});
  } else {
    out.push_back({"建具の符号", "入力済み", "同じ符号の開口は、どれも同じ種類・大きさです。", {}});
  }
  // Rooms of one storey with the same name: the tables and the plans cannot be told apart.
  std::map<std::pair<std::string, std::string>, std::vector<ObjectId>> names;
  for (const RoomRow& r : roomSchedule(document).rows) names[{r.level, r.name}].push_back(r.id);
  std::vector<ObjectId> twins;
  std::string twinNames;
  for (const auto& [key, ids] : names)
    if (ids.size() > 1) {
      twins.insert(twins.end(), ids.begin(), ids.end());
      twinNames += (twinNames.empty() ? "" : "、") + (key.first.empty() ? std::string() : key.first + " ") + key.second;
    }
  if (!twins.empty())
    out.push_back({"室名", "要確認", "同じ階に同じ名前の室があります：" + twinNames + "。表と図で見分けられるよう、名前を変えます。", twins, true});
  out.push_back({"まだ確かめていない事", "未評価",
                 "図面と表と外部の添付資料の版、図番と図面リストの突合せ、法令の根拠と確認日、担当者の確認。"
                 "図面一式（ファイル → 図面一式を書き出す）で同じモデルから図と表をそろえて出し、申請先の図書一覧と照らします。",
                 {}});
  return out;
}

std::vector<PermitCheck> permitOutputChecks(const Document& document) {
  std::vector<PermitCheck> out;
  std::string left;
  int stages = 0;
  for (const auto& [stage, rows] : modelStages(document)) {
    const auto open = std::count_if(rows.begin(), rows.end(), [](const PermitCheck& c) { return c.attention; });
    if (open == 0) continue;
    ++stages;
    left += "\n・" + stage + "：残り " + std::to_string(open);
  }
  out.push_back({"申請用出力", "保留",
                 "申請先・適用法令・必要図書を確かめるまで、申請用の出力はしません。" +
                     (stages == 0 ? std::string("\nモデルから見る工程の残りはありません（敷地・居室の工程と、各工程の未評価の行は残ります）。")
                                  : "\n残りのある工程：" + left),
                 {}, true});
  out.push_back({"検討用の出力", "出せます",
                 "図面一式（ファイル → 図面一式を書き出す）、配置図のプレビュー、各図面の書き出しは、検討用としていつでも出せます。"
                 "出力に未評価・要再照合の注が付く所があります。",
                 {}});
  out.push_back({"申請先で確かめる事", "未評価", "申請先（確認検査機関・特定行政庁）、申請の区分と適用法令、必要な図書と様式、提出の方法。", {}});
  // docs/312: which 建築士 may design and supervise it, against the grades given in the outline.
  {
    const ArchitectCheck a = architectCheck(document);
    const auto outline = projectOutline(document);
    std::string details = a.basis;
    std::string state;
    bool attention = false;
    if (a.required == "unknown") {
      state = "未評価";
      attention = true;
      details += "\n" + a.problem;
    } else {
      details += "\n必要な資格：" + std::string(requiredLabel(a.required)) + "（" + a.article + "）";
      bool short_ = false, missing = false;
      for (const auto& [key, who] : {std::pair<const char*, const char*>{"form_designer_grade", "設計者"},
                                     std::pair<const char*, const char*>{"form_supervisor_grade", "工事監理者"}}) {
        const auto it = outline.find(key);
        if (it == outline.end()) {
          if (a.required != "none") missing = true;
          details += std::string("\n") + who + "の資格：未入力";
          continue;
        }
        const bool ok = gradeMeets(it->second, a.required);
        short_ |= !ok;
        details += std::string("\n") + who + "の資格：" + projectOutlineText(key, it->second) + "建築士 → " + (ok ? "足りる" : "足りない");
      }
      if (!a.problem.empty()) details += "\n" + a.problem;
      state = short_ ? "要確認" : missing ? "未入力" : "入力済み";
      attention = short_ || missing || !a.problem.empty();
    }
    details += std::string("\n第3条の2第3項により、都道府県の条例で木造以外の延べ面積が別に定められていることがあります。\n条文：") +
               kArchitectActSource + "（" + kArchitectActCheckedOn + " 確認）";
    out.push_back({"設計・工事監理ができる建築士（建築士法第3条）", state, details, {}, attention});
  }
  // docs/212: the stages a person marked as looked at (1-11; this stage's own mark is not counted).
  {
    std::vector<std::string> unchecked, again;
    for (int stage = 1; stage <= 11; ++stage) {
      const auto mark = permitConfirmation(document, stage);
      if (mark.state == "要再確認") again.push_back(std::to_string(stage));
      else if (mark.state != "確認済み") unchecked.push_back(std::to_string(stage));
    }
    const auto join = [](const std::vector<std::string>& list) {
      std::string text;
      for (const auto& s : list) text += (text.empty() ? "" : "・") + s;
      return text;
    };
    const int confirmed = 11 - static_cast<int>(unchecked.size() + again.size());
    std::string details = "工程 1〜11 のうち、人が確認した印のある工程 " + std::to_string(confirmed) + "。";
    if (!again.empty()) details += "\n要再確認（確認の後にモデルが変わった）：" + join(again);
    if (!unchecked.empty()) details += "\n未確認：" + join(unchecked);
    details += "\n印は人が見たという記録で、合格ではありません。";
    out.push_back({"人の確認", !again.empty() ? "要再確認" : confirmed == 11 ? "確認済み" : "未確認", details, {},
                   confirmed != 11});
  }
  return out;
}

const std::vector<ProjectOutlineField>& projectOutlineFields() {
  static const std::vector<ProjectOutlineField> fields = {
      // docs/231: in the order and with the choices of the application form (建築基準法施行規則 第二号様式
      // 第二面・第三面, as published 2026-04-01 by 長野県, read 2026-11-21).
      {"project_name", "物件名", "図面の表題欄の工事名にも使います（図面一式の設定で別に入れた時はそちら）", {}},
      {"client", "建築主", "氏名・名称（第二面 1）", {}},
      {"agent", "代理者", "建築士の資格・氏名・事務所（第二面 2。いない時は空のまま）", {}, true},
      {"designer", "設計者", "建築士の資格・氏名・事務所（第二面 3）", {}},
      {"supervisor", "工事監理者", "建築士の資格・氏名・事務所（第二面 5。未定なら「未定」）", {}},
      {"contractor", "工事施工者", "氏名・営業所・建設業の許可（第二面 6。未定なら空のまま）", {}, true},
      // docs/232: 第二面 7・8 (which applies is the person's call: not worked out from the model).
      {"structural_review", "構造計算適合性判定の申請", "（第二面 7）",
       {{"submitted", "申請済"}, {"not_yet", "未申請"}, {"not_required", "申請不要"}}},
      {"energy_plan", "建築物エネルギー消費性能確保計画の提出", "（第二面 8）",
       {{"submitted", "提出済"}, {"not_yet", "未提出"}, {"not_required", "提出不要"}}},
      {"address", "地名地番", "登記上の地名地番（第三面 1）", {}},
      {"residence", "住居表示", "（第三面 2）", {}, true},
      {"city_planning", "都市計画区域等", "（第三面 3）",
       {{"urbanization", "都市計画区域内（市街化区域）"}, {"control", "都市計画区域内（市街化調整区域）"},
        {"non_divided", "都市計画区域内（区域区分非設定）"}, {"quasi", "準都市計画区域内"},
        {"outside", "都市計画区域及び準都市計画区域外"}}},
      {"fire_zone", "防火地域", "（第三面 4。法 22 条区域は「その他の区域」に書く）",
       {{"fire", "防火地域"}, {"quasi_fire", "準防火地域"}, {"none", "指定なし"}, {"article22", "指定なし（法 22 条区域）"}}},
      {"other_zones", "その他の区域、地域、地区又は街区", "例：法 22 条区域、景観地区（第三面 5）", {}, true},
      {"main_use", "主要用途", "例：一戸建ての住宅（第三面 8）", {}},
      {"use_code", "主要用途の区分", "様式の別紙の区分の番号（第三面 8）", {}, true},
      {"work_kind", "工事種別", "（第三面 9）",
       {{"new", "新築"}, {"extension", "増築"}, {"rebuild", "改築"}, {"relocation", "移転"}, {"use_change", "用途変更"},
        {"major_repair", "大規模の修繕"}, {"major_remodel", "大規模の模様替"}}},
      {"structure", "構造", "例：木造、一部鉄骨造（第三面 13 ハ）", {}},
      {"permissions", "許可・認定等", "（第三面 14。無い時は空のまま）", {}, true},
      {"start_date", "工事着手予定年月日", "例：2027-04-01（第三面 15）", {}},
      {"finish_date", "工事完了予定年月日", "例：2027-10-31（第三面 16）", {}},
      {"specific_process", "特定工程", "特定工程工事終了予定年月日と工程（第三面 17。無い時は空のまま）", {}, true},
      {"other_matters", "その他必要な事項", "（第三面 18。無い時は空のまま）", {}, true},
      {"source", "確認資料", "項目を確かめた資料（登記事項証明書・都市計画図など）", {}},
      {"form_client_kana", "フリガナ", "建築主氏名のフリガナ", {}, true},
      {"form_client_postcode", "郵便番号", "例：231-0005", {}, true},
      {"form_client_address", "住所", "建築主の住所", {}, true},
      {"form_client_phone", "電話番号", "確認申請書に記載", {}, true},
      {"form_agent_name", "氏名", "代理者の氏名", {}, true},
      {"form_agent_grade", "建築士の資格", "資格証の区分", {{"first", "一級"}, {"second", "二級"}, {"wood", "木造"}}, true},
      {"form_agent_authority", "建築士の登録先", "様式の括弧に入る登録先", {}, true},
      {"form_agent_number", "建築士の登録番号", "「第」「号」を除く番号", {}, true},
      {"form_agent_office", "建築士事務所名", "登録された事務所名", {}, true},
      {"form_agent_office_grade", "事務所の区分", "事務所登録の区分", {{"first", "一級"}, {"second", "二級"}, {"wood", "木造"}}, true},
      {"form_agent_office_prefecture", "事務所登録の都道府県", "例：神奈川県", {}, true},
      {"form_agent_office_number", "事務所登録番号", "「第」「号」を除く番号", {}, true},
      {"form_agent_postcode", "郵便番号", "例：231-0005", {}, true},
      {"form_agent_address", "所在地", "事務所に属していないときは本人の住所", {}, true},
      {"form_agent_phone", "電話番号", "様式に記載する連絡先", {}, true},
      {"form_designer_name", "氏名", "代表設計者の氏名", {}, true},
      {"form_designer_grade", "建築士の資格", "資格証の区分", {{"first", "一級"}, {"second", "二級"}, {"wood", "木造"}}, true},
      {"form_designer_authority", "建築士の登録先", "様式の括弧に入る登録先", {}, true},
      {"form_designer_number", "建築士の登録番号", "「第」「号」を除く番号", {}, true},
      {"form_designer_office", "建築士事務所名", "登録された事務所名", {}, true},
      {"form_designer_office_grade", "事務所の区分", "事務所登録の区分", {{"first", "一級"}, {"second", "二級"}, {"wood", "木造"}}, true},
      {"form_designer_office_prefecture", "事務所登録の都道府県", "例：神奈川県", {}, true},
      {"form_designer_office_number", "事務所登録番号", "「第」「号」を除く番号", {}, true},
      {"form_designer_postcode", "郵便番号", "例：231-0005", {}, true},
      {"form_designer_address", "所在地", "事務所に属していないときは本人の住所", {}, true},
      {"form_designer_phone", "電話番号", "様式に記載する連絡先", {}, true},
      {"form_supervisor_name", "氏名", "代表工事監理者の氏名", {}, true},
      {"form_supervisor_grade", "建築士の資格", "資格証の区分", {{"first", "一級"}, {"second", "二級"}, {"wood", "木造"}}, true},
      {"form_supervisor_authority", "建築士の登録先", "様式の括弧に入る登録先", {}, true},
      {"form_supervisor_number", "建築士の登録番号", "「第」「号」を除く番号", {}, true},
      {"form_supervisor_office", "建築士事務所名", "登録された事務所名", {}, true},
      {"form_supervisor_office_grade", "事務所の区分", "事務所登録の区分", {{"first", "一級"}, {"second", "二級"}, {"wood", "木造"}}, true},
      {"form_supervisor_office_prefecture", "事務所登録の都道府県", "例：神奈川県", {}, true},
      {"form_supervisor_office_number", "事務所登録番号", "「第」「号」を除く番号", {}, true},
      {"form_supervisor_postcode", "郵便番号", "例：231-0005", {}, true},
      {"form_supervisor_address", "所在地", "事務所に属していないときは本人の住所", {}, true},
      {"form_supervisor_phone", "電話番号", "様式に記載する連絡先", {}, true},
      {"form_contractor_name", "氏名・名称", "代表工事施工者の氏名・名称", {}, true},
      {"form_contractor_office", "営業所名", "代表工事施工者の営業所", {}, true},
      {"form_contractor_postcode", "郵便番号", "例：231-0005", {}, true},
      {"form_contractor_address", "所在地", "営業所の所在地", {}, true},
      {"form_contractor_phone", "電話番号", "営業所の連絡先", {}, true},
      {"form_building_above", "地上階数", "確認済みの階数（整数）", {}, true},
      {"form_building_below", "地下階数", "地下なしは 0", {}, true},
      {"form_building_other", "同一敷地の他の建築物の数", "他の建築物なしは 0", {}, true},
      {"form_building_height", "建築物の最高の高さ（m）", "地盤面と算定対象を確認した値", {}, true},
      {"form_building_eaves", "最高の軒の高さ（m）", "地盤面と算定対象を確認した値", {}, true},
      {"form_building_ratio_site", "算定に使う敷地面積（㎡）", "法令上算入する敷地面積。両方の率で同じ面積を使う場合", {}, true},
      {"form_building_ratio_coverage", "建蔽率の基礎となる建築面積（㎡）", "特例軒等の取扱いを確認した値", {}, true},
      {"form_building_ratio_floor", "容積率の基礎となる延べ面積（㎡）", "不算入部分を控除した確認済みの値", {}, true},
      {"form_building_ratio_source", "建蔽率・容積率の確認資料", "面積・控除を確認した図書名など", {}, true},
  };
  return fields;
}

std::map<std::string, std::string> projectOutline(const Document& document) {
  std::map<std::string, std::string> out;
  const nlohmann::json j = nlohmann::json::parse(document.settings().projectOutline, nullptr, false);
  if (!j.is_object()) return out;
  for (const ProjectOutlineField& f : projectOutlineFields())
    if (j.contains(f.key) && j.at(f.key).is_string() && !j.at(f.key).get<std::string>().empty())
      out[f.key] = j.at(f.key).get<std::string>();
  return out;
}

std::string projectOutlineText(const std::string& key, const std::string& value) {
  for (const ProjectOutlineField& f : projectOutlineFields())
    if (f.key == key)
      for (const auto& [code, label] : f.choices)
        if (code == value) return label;
  return value;
}

namespace {
// docs/214: the project outline items as a stage 1 row.
PermitCheck projectOutlineCheck(const Document& document) {
  const auto given = projectOutline(document);
  std::string missing, done;
  for (const ProjectOutlineField& f : projectOutlineFields()) {
    // Form contacts and confirmed building dimensions are shown in the Word transfer review.
    if (f.key.compare(0, 5, "form_") == 0) continue;
    const auto explicitName = given.find("form_" + f.key + "_name");
    const auto it = explicitName == given.end() ? given.find(f.key) : explicitName;
    if (it == given.end()) {
      if (!f.optional) missing += (missing.empty() ? "" : "、") + f.label;  // docs/231: optional ones not counted
    } else {
      done += "\n" + f.label + "：" + projectOutlineText(it->first, it->second);
    }
  }
  if (given.empty())
    return {"計画概要の項目", "未入力", "建築主・地名地番・主要用途・工事種別など、モデルに無い項目を入れます。計画概要の表に出ます。", {}, true};
  return {"計画概要の項目", missing.empty() ? "入力済み" : "入力不足",
          (missing.empty() ? std::string("要る項目は全部入っています（代理者・工事施工者・住居表示・その他の区域・用途の区分・許可等は無い時は空のまま）。")
                           : "未入力：" + missing) + done, {}, !missing.empty()};
}
}  // namespace

std::vector<PermitCheck> permitSiteChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto& settings = document.settings();
  const auto& site = settings.siteBoundary;
  const auto metres = [](double mm) { return number(mm / 1000.0, 3) + " m"; };
  std::optional<SiteBoundaryGeometry> geometry;
  if (!site) {
    out.push_back({"敷地境界", "未入力", "敷地の閉じた折れ線か水平面を登録します。", {}, true});
  } else if (auto g = document.siteBoundaryGeometry(site->outline); !g) {
    out.push_back({"敷地境界", "要再照合", g.error(), {site->outline}, true});
  } else {
    geometry = *g;
    std::string points;
    for (const gp_Pnt& p : geometry->vertices) points += " (" + number(p.X(), 1) + ", " + number(p.Y(), 1) + ")";
    out.push_back({"敷地境界", "入力済み",
                   "指定輪郭 " + number(geometry->areaMm2 / 1e6, 3) + " m²\n確認資料 " + site->source + "\n頂点" + points,
                   {site->outline}});
  }
  if (const auto& north = settings.siteNorth)
    out.push_back({"真北", "入力済み",
                   "モデルの +Y から時計回り " + number(north->angle, 6) + "°\n確認資料 " + north->source + "\n確認日 " +
                       north->checkedOn,
                   {}});
  else
    out.push_back({"真北", "未入力", "真北の角度と確認資料・日付を入力します。", {}, true});
  if (!geometry) {
    for (const char* item : {"道路・接道", "接地外周・高さ", "用途地域の区分"})
      out.push_back({item, "敷地境界待ち", "敷地境界を登録すると入力できます。", {}});
    out.push_back(projectOutlineCheck(document));  // the last row, as below (the panel reads it)
    return out;
  }
  if (!site->road) {
    out.push_back({"道路・接道", "未入力", "道路側の辺と確認資料・寸法を入力します。", {}, true});
  } else if (!document.siteRoadEdge(*geometry)) {
    out.push_back({"道路・接道", "要再照合", "道路側の辺が変わりました。", {}, true});
  } else {
    const auto& road = *site->road;
    const bool partial = road.kind.empty() || road.widthMm <= 0 || road.frontageMm <= 0;
    out.push_back({"道路・接道", partial ? "入力不足" : "入力済み",
                   "種別 " + (road.kind.empty() ? std::string("未入力") : road.kind) + "\n幅員 " +
                       (road.widthMm > 0 ? metres(road.widthMm) : std::string("未入力")) + "　接道長 " +
                       (road.frontageMm > 0 ? metres(road.frontageMm) : std::string("未入力")) + "\n確認資料 " + road.source,
                   {}, partial});
  }
  if (!site->ground) {
    out.push_back({"接地外周・高さ", "未入力", "建物の接地外周を選び、基準点と測点の高さを入力します。", {}, true});
  } else {
    const auto& ground = *site->ground;
    const auto placement = document.checkSiteGroundPlacement(ground);
    const auto report = document.siteGroundReport(ground);
    if (!placement || !report) {
      out.push_back({"接地外周・高さ", "要再照合", !placement ? placement.message() : report.error(), {ground.outline}, true});
    } else {
      const bool linked = ground.placement.has_value();
      out.push_back({"接地外周・高さ", linked ? "参考値あり" : "建物未関連",
                     "参考平均 " + metres(report->averageMm()) + "（基準点 " + ground.datum + "）\n確認資料 " + ground.source,
                     {ground.outline}, !linked});
    }
  }
  const Object* outline = document.find(site->outline);
  const auto value = [&](const char* key) {
    if (!outline) return std::string();
    const auto it = outline->attributes.find(key);
    return it == outline->attributes.end() ? std::string() : it->second;
  };
  const std::string zone = value("use_zone");
  if (zone.empty()) {
    out.push_back({"用途地域の区分", "未入力", "住居系・工業系・商業系（指定なしを含む）のどれかと確認資料を入れます。", {}, true});
  } else {
    const std::string name = zone == "residential" ? "住居系" : zone == "industrial" ? "工業系" : "商業系・指定なし";
    out.push_back({"用途地域の区分", "入力済み", name + "\n確認資料 " + value("use_zone_source"), {}});
  }
  out.push_back(projectOutlineCheck(document));
  return out;
}

std::vector<PermitCheck> permitRoomChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto data = roomOpeningInputs(document);
  const auto settled = [](const std::string& state) {
    return state == "参考値内" || state == "対象外" || state == "換気設備";
  };
  for (const auto& entry : data.rooms) {
    const RoomDaylightCheck check = roomDaylightCheck(entry);
    const std::string name = entry.room.name + (entry.room.level.empty() ? std::string() : "（" + entry.room.level + "）");
    out.push_back({name, check.daylightState,
                   "採光：" + check.daylightText + "\n換気：" + check.ventilationState + "　" + check.ventilationText +
                       "\n機械換気：" + check.mechanicalState + "　" + check.mechanicalText,
                   {entry.room.id}, !settled(check.daylightState) || !settled(check.ventilationState)});
  }
  if (data.rooms.empty()) out.push_back({"居室", "未入力", "室がありません。", {}, true});
  if (!data.unmatched.empty())
    out.push_back({"室に入らない開口", "要確認", std::to_string(data.unmatched.size()) + " 個の開口が、どの室にも入りません。", {}, true});
  return out;
}

std::vector<PermitCheck> permitChecks(const Document& document, int stage) {
  switch (stage) {
    case 1: return permitSiteChecks(document);
    case 2: return permitGridChecks(document);
    case 3: return permitPlanChecks(document);
    case 4: return permitRoomChecks(document);
    case 5: return permitStairChecks(document);
    case 6: return permitRoofChecks(document);
    case 7: return permitBraceChecks(document);
    case 8: return permitFoundationChecks(document);
    case 9: return permitEnergyChecks(document);
    case 10: return permitElevationChecks(document);
    case 11: return permitSetChecks(document);
    case 12: return permitOutputChecks(document);
    default: return {};
  }
}

namespace {
std::string fnv(const std::string& text) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : text) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(h));
  return buffer;
}
std::string rowText(const PermitCheck& c) {
  std::string text = c.item + '\x1f' + c.state + '\x1f' + c.details + '\x1f';
  for (const auto& id : c.targets) text += id + ',';
  return text;
}
}  // namespace

std::string permitStageFingerprint(const std::vector<PermitCheck>& rows) {
  std::string text = "permit-v1";
  for (const PermitCheck& c : rows) text += '\x1e' + rowText(c);
  return fnv(text);
}

std::map<std::string, std::string> permitRowFingerprints(const std::vector<PermitCheck>& rows) {
  std::map<std::string, std::string> out;
  std::map<std::string, int> seen;
  for (const PermitCheck& c : rows) {
    const int n = ++seen[c.item];
    out[n == 1 ? c.item : c.item + "#" + std::to_string(n)] = fnv(rowText(c));
  }
  return out;
}

PermitConfirmation permitConfirmation(const Document& document, int stage) {
  PermitConfirmation out;
  out.stage = stage;
  out.state = "未確認";
  const nlohmann::json all = nlohmann::json::parse(document.settings().permitConfirmations, nullptr, false);
  if (!all.is_object()) return out;
  const auto it = all.find(std::to_string(stage));
  if (it == all.end() || !it->is_object()) return out;
  const nlohmann::json record = *it;
  out.on = record.value("on", std::string());
  out.by = record.value("by", std::string());
  out.note = record.value("note", std::string());
  const auto rows = permitChecks(document, stage);
  if (permitStageFingerprint(rows) == record.value("fingerprint", std::string())) {
    out.state = "確認済み";
    return out;
  }
  out.state = "要再確認";
  const auto now = permitRowFingerprints(rows);
  std::map<std::string, std::string> then;
  if (record.contains("rows") && record.at("rows").is_object())
    for (const auto& [item, print] : record.at("rows").items())
      if (print.is_string()) then[item] = print.get<std::string>();
  for (const auto& [item, print] : now) {
    const auto old = then.find(item);
    if (old == then.end()) out.changed.push_back(item + "（増えた）");
    else if (old->second != print) out.changed.push_back(item);
  }
  for (const auto& [item, print] : then)
    if (!now.count(item)) out.changed.push_back(item + "（無くなった）");
  return out;
}

std::vector<PermitCheck> permitStairChecks(const Document& document) {
  std::vector<PermitCheck> out;
  const auto label = [](const std::string& item) -> std::string {
    if (item == "riser") return "蹴上げ";
    if (item == "going") return "踏面";
    if (item == "winder_going") return "回り段の踏面";
    if (item == "width") return "幅";
    if (item == "landing") return "踊場";
    if (item == "handrail") return "手すり";
    return item;
  };
  // How a line reads: the value against the limit (mm), or what the comparison found.
  const auto line = [&](const geom::StairCheck& c) {
    const std::string state = c.state == "ok" ? "参考値内" : c.state == "ng" ? "参考値を外れる" : "未評価";
    std::string limit;
    if (c.item == "riser") limit = "230 以下";
    else if (c.item == "going" || c.item == "winder_going") limit = "150 以上";
    else if (c.item == "width") limit = "750 以上";
    else if (c.item == "landing") limit = "高さ 4000 以内ごと";
    else if (c.item == "handrail") limit = "設ける";
    std::string value;
    if (c.item == "landing" || c.item == "handrail") value = "";
    else if (c.value > 0) value = number(c.value, 1) + " mm　";
    return label(c.item) + "　" + value + "（" + limit + "）　" + state;
  };
  int stairs = 0;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "stair") continue;
    ++stairs;
    std::string name = o.name;
    if (const std::string level = levelName(document, attribute(o, "base_level")); !level.empty()) name = level + "　" + name;
    const auto spec = stairInWorld(document, o);
    if (!spec) {
      out.push_back({name, "未評価", "形を直接変えた階段は、寸法を読めません。階段ツールで作り直すと照らせます。", {o.id}, true});
      continue;
    }
    const auto shape = geom::makeStair(*spec);
    if (!shape) {
      out.push_back({name, "未評価", "寸法を読めません：" + shape.error(), {o.id}, true});
      continue;
    }
    bool off = false, open = false;
    std::string details = std::to_string(shape->risers) + " 段　上がる高さ " + number(spec->rise, 0) + " mm";
    for (const auto& c : shape->checks) {
      details += "\n" + line(c);
      off = off || c.state == "ng";
      open = open || (c.state != "ok" && c.state != "ng");
    }
    out.push_back({name, off ? "要確認" : open ? "未評価あり" : "参考値内", details, {o.id}, off || open});
  }
  if (stairs == 0) {
    out.push_back({"階段", "未入力", "2 階以上の建物は、階段ツールで階段を置くと、段・幅・踊場・手すりを照らせます。", {}, true});
    return out;
  }
  out.push_back({"照らした値", "参考",
                 "住宅の階段（共同住宅の共用の階段を除く）として照らした参考値。合否の判定ではありません。\n"
                 "蹴上げ・踏面：建築基準法施行令 第 23 条第 1 項ただし書き。幅：同項の表（四）。回り段の踏面は狭い方の端から "
                 "30 cm の位置（同条第 2 項。ここでは段の角度の弦で測る）。踊場：第 24 条第 1 項。手すり：第 25 条第 1 項（高さ "
                 "1 m 以下の部分は除く、同条第 4 項）。\n確認：e-Gov 法令検索（令和8年11月1日施行版）、2026-11-20。",
                 {}});
  out.push_back({"まだ照らしていない事", "未評価",
                 "表の（一）〜（三）に当たる階段か（直上階の居室の床面積の合計が 200 m² を超える時など）。幅は、高さ 50 cm "
                 "以下の手すり等を 10 cm まで無いものとして測る（第 23 条第 3 項）。直階段の踊場の踏幅 1.2 m 以上（第 24 条第 2 項）。"
                 "両側の側壁またはこれに代わるもの（第 25 条第 2 項）。国土交通大臣が定めた構造方法の階段（第 23 条第 4 項）。",
                 {}});
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

static AreaTable measureAreas(const Document& document, detail::AreaGeometry* evidence = nullptr) {
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
  // docs/166: the ground (地盤面) is the level named GL; the top of a storey the next level up.
  auto isGround = [](const FloorLevel& l) { return isGroundLevelName(l.name); };
  std::optional<double> ground;
  for (const FloorLevel& l : levels)
    if (isGround(l)) ground = l.elevation;
  if (ground) out.groundKnown = true, out.ground = *ground;
  // The next floor up (the ground level is not a floor).
  auto nextAbove = [&](double z) -> std::optional<double> {
    std::optional<double> best;
    for (const FloorLevel& l : levels)
      if (!isGround(l) && l.elevation > z + 1.0 && (!best || l.elevation < *best)) best = l.elevation;
    return best;
  };
  std::vector<std::string> basementsLeftOut, basementsKept;
  std::vector<TopoDS_Shape> storeyPlans;  // within the wall lines, per storey (for the building area)
  std::vector<std::string> noSlab, voids, noWalls;
  bool stairsArriving = false;
  for (const FloorLevel& level : levels) {
    if (&storeyOf(level) != &level) continue;
    if (!levelIsStorey(levels, level)) continue;  // docs/295: 基礎天端, 桁天端 ... are not storeys
    // The ground level alone at its height is not a floor (docs/166): a storey only where walls
    // stand on it or a slab's top lies on it.
    if (isGround(level)) {
      bool floor = false;
      for (const Object& o : document.objects()) {
        const std::string kind = attribute(o, "kind");
        if (kind != "wall" && kind != "slab") continue;
        if (kind == "slab" && attribute(o, "top_level") == level.id) floor = true;
        if (kind == "wall" && !o.shape.IsNull()) {
          const Bnd_Box box = geom::exactBounds(document.worldShape(o));
          if (!box.IsVoid() && std::abs(box.CornerMin().Z() - level.elevation) < 1.0) floor = true;
        }
        if (floor) break;
      }
      if (!floor) continue;
    }
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
    // docs/166: its ceiling height, and whether it is a basement.
    if (const auto above = nextAbove(level.elevation)) row.top = *above, row.hasTop = true;
    {
      double area = 0.0, weighted = 0.0;
      bool all = row.rooms > 0;
      for (const RoomRow& r : rooms.rows) {
        if (!r.counted || r.level != level.name) continue;
        if (r.ceilingHeight <= 0.0) all = false;
        area += r.centreArea;
        weighted += r.centreArea * r.ceilingHeight;
      }
      if (all && area > 0.0) row.ceiling = weighted / area, row.ceilingFromRooms = true;
      else if (row.hasTop) row.ceiling = row.top - level.elevation;
    }
    if (ground && row.ceiling > 0.0 && level.elevation < *ground - 1e-6)
      row.basement = *ground - level.elevation >= row.ceiling / 3 - 1e-6;
    // 令 2 条 1 項 2 号: a basement 1 m or less above the ground is not in the building area.
    const bool lowBasement = row.basement && row.hasTop && row.top <= *ground + 1000.0 + 1e-6;
    if (enclosedSlice) {
      if (lowBasement) basementsLeftOut.push_back(level.name);
      else {
        storeyPlans.push_back(*enclosedSlice);
        if (row.basement) basementsKept.push_back(level.name);
      }
    }
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
        if (evidence) {
          auto& item = evidence->floors[level.name];
          item.counted = *enclosedSlice;
          item.assumedFloor = true;
        }
        noSlab.push_back(level.name);
      } else if (const auto floor = joined(floorParts)) {
        const auto both = geom::booleanCombine(geom::BooleanOp::Intersect, {*enclosedSlice, *floor});
        if (both) {
          row.legalFloor = geom::volume(both.value());
          if (evidence) evidence->floors[level.name].counted = *both;
        }
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
    if (evidence && enclosedSlice) evidence->floors[level.name].enclosed = *enclosedSlice;
    out.rows.push_back(std::move(row));
  }
  AreaRow loose;  // rooms whose marker is at no level's height
  addRooms(loose, "");
  if (loose.rooms > 0) out.rows.push_back(std::move(loose));
  // docs/163: balconies. Floor area only where the person said "count": added to the storey
  // whose level is at the balcony's floor.
  int balconiesLoose = 0, balconiesInBuilding = 0, balconiesOnPosts = 0;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "balcony") continue;
    const nlohmann::json b = nlohmann::json::parse(attribute(o, "balcony"), nullptr, false);
    if (!b.is_object()) continue;
    const double area = b.value("plan_area", b.value("width", 0.0) * b.value("depth", 0.0));  // docs/168: an L's own area
    ++out.balconies;
    out.balconyArea += area;
    const std::string how = b.value("floor_area", std::string());
    if (how == "exclude") ++out.balconiesExcluded;
    double counted = how == "count" ? area : 0.0;
    if (how.empty()) {
      // docs/175: by the notice (住指発第 115 号 1 (5) → (4)). Its ceiling: the underside of what
      // lies over it (a floor, a roof, a balcony above); nothing: open to the sky.
      ++out.balconiesByNotice;
      const double top = b.value("top", 0.0);
      const Bnd_Box mine = document.worldBounds(o);
      double above = 1e300;
      for (const Object& x : document.objects()) {
        if (x.id == o.id || x.shape.IsNull()) continue;
        const std::string k = attribute(x, "kind");
        if (k != "slab" && k != "roof" && k != "balcony" && k != "canopy" && k != "ceiling") continue;
        const Bnd_Box box = document.worldBounds(x);
        if (box.IsVoid() || mine.IsVoid()) continue;
        const bool over = box.CornerMin().X() < mine.CornerMax().X() - 1 && box.CornerMax().X() > mine.CornerMin().X() + 1 &&
                          box.CornerMin().Y() < mine.CornerMax().Y() - 1 && box.CornerMax().Y() > mine.CornerMin().Y() + 1;
        if (over && box.CornerMin().Z() > top + 1.0) above = std::min(above, box.CornerMin().Z());
      }
      const std::string rail = b.value("rail", std::string("wall"));
      const double opening = above - top - (rail == "wall" ? b.value("rail_height", 1100.0) : 0.0);
      const bool open = above >= 1e299 || (opening >= 1100.0 - 1e-6 && opening >= (above - top) / 2 - 1e-6);
      double within = b.value("within_2m_area", -1.0);
      if (within < 0.0) within = b.value("width", 0.0) * std::max(0.0, std::min(b.value("depth", 0.0), 2000.0 - b.value("behind", 0.0)));
      counted = open ? std::max(0.0, area - within) : area;
    }
    if (counted <= 0.0) continue;
    AreaRow* storey = nullptr;
    for (AreaRow& r : out.rows)
      if (r.enclosedKnown && std::abs(r.elevation - b.value("top", 0.0)) < 1.0) storey = &r;
    if (!storey) {
      ++balconiesLoose;
      continue;
    }
    storey->legalFloor += counted;
    if (evidence) evidence->floors[storey->level].balconyMm2 += counted;
    out.balconyFloorArea += counted;
    ++out.balconiesCounted;
  }
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
    // docs/160: sides with an overhang of their own.
    std::map<int, double> own;
    if (spec.contains("sides") && spec.at("sides").is_array())
      for (const auto& one : spec.at("sides"))
        if (one.is_object() && one.contains("side") && one.contains("eave")) own[one.at("side").get<int>()] = one.at("eave").get<double>();
    double furthest = std::max(eave, verge);
    for (const auto& [side, out] : own) furthest = std::max(furthest, out);
    if (furthest < 1000.0) continue;  // nothing 1 m out or more
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
    for (const auto& [side, out] : own)
      if (side >= 0 && side < static_cast<int>(n)) overhang[static_cast<std::size_t>(side)] = out;
    for (const int side : spec.value("wall_sides", std::vector<int>{}))  // docs/161: against a wall: none
      if (side >= 0 && side < static_cast<int>(n)) overhang[static_cast<std::size_t>(side)] = 0.0;
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
  // Canopies (ひさし, docs/147) standing out 1 m or more from the wall's line: back 1 m from their ends.
  int canopies = 0;
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "canopy") continue;
    const nlohmann::json c = nlohmann::json::parse(attribute(o, "canopy"), nullptr, false);
    if (!c.is_object()) continue;
    const double reach = c.value("depth", 0.0) + c.value("behind", 0.0);
    if (reach < 1000.0) continue;
    const gp_Trsf world = document.worldTransform(o);
    const gp_Pnt face = gp_Pnt(c.at("face")[0].get<double>(), c.at("face")[1].get<double>(), 0).Transformed(world);
    gp_Vec out(c.at("out")[0].get<double>(), c.at("out")[1].get<double>(), 0);
    out.Transform(world);
    if (out.Magnitude() < 1e-9) continue;
    out.Normalize();
    const gp_Vec along = gp_Vec(0, 0, 1).Crossed(out);
    const double half = c.value("width", 0.0) / 2, upTo = c.value("depth", 0.0) - 1000.0;
    if (upTo <= 0.0) continue;  // within the wall: already counted
    const std::vector<gp_Pnt> part = {gp_Pnt(face.X(), face.Y(), 0).Translated(along * -half), gp_Pnt(face.X(), face.Y(), 0).Translated(along * half),
                                      gp_Pnt(face.X(), face.Y(), 0).Translated(along * half + out * upTo),
                                      gp_Pnt(face.X(), face.Y(), 0).Translated(along * -half + out * upTo)};
    if (const auto s = slice(part)) {
      plan.push_back(*s);
      ++canopies;
    }
  }
  // Balconies (はね出し縁の類, docs/163) standing out 1 m or more from the wall's line: back 1 m from their ends.
  for (const Object& o : document.objects()) {
    if (attribute(o, "kind") != "balcony") continue;
    const nlohmann::json b = nlohmann::json::parse(attribute(o, "balcony"), nullptr, false);
    if (!b.is_object() || !b.contains("face") || !b.contains("out")) continue;
    // docs/168: the part counted, as worked out when it was made (round a corner too).
    if (b.contains("counted") && b.at("counted").is_array()) {
      if (b.at("counted").size() < 3) continue;
      std::vector<gp_Pnt> part;
      for (const auto& q : b.at("counted")) part.emplace_back(q.at(0).get<double>(), q.at(1).get<double>(), 0.0);
      if (const auto s = slice(part)) {
        plan.push_back(*s);
        ++balconiesInBuilding;
        if (b.value("posts", 0) > 0) ++balconiesOnPosts;
      }
      continue;
    }
    const double depth = b.value("depth", 0.0);
    // docs/167: on posts, up to their centre line (柱の中心線); else as はね出し縁, back 1 m from its end.
    const bool onPosts = b.value("posts", 0) > 0;
    if (!onPosts && depth + b.value("behind", 0.0) < 1000.0) continue;
    const double upTo = onPosts ? depth - b.value("post_size", 105.0) / 2 : depth - 1000.0;
    if (upTo <= 0.0) continue;  // within the wall: already counted
    if (onPosts) ++balconiesOnPosts;
    const gp_Trsf world = document.worldTransform(o);
    const gp_Pnt face = gp_Pnt(b.at("face")[0].get<double>(), b.at("face")[1].get<double>(), 0);
    gp_Vec n(b.at("out")[0].get<double>(), b.at("out")[1].get<double>(), 0);
    if (n.Magnitude() < 1e-9) continue;
    n.Normalize();
    const gp_Vec along = gp_Vec(0, 0, 1).Crossed(n);
    const double half = b.value("width", 0.0) / 2;
    (void)world;  // kept where it was made: a balcony moved by hand is read where its sizes say
    const std::vector<gp_Pnt> part = {face.Translated(along * -half), face.Translated(along * half), face.Translated(along * half + n * upTo),
                                      face.Translated(along * -half + n * upTo)};
    if (const auto s = slice(part)) {
      plan.push_back(*s);
      ++balconiesInBuilding;
    }
  }
  if (const auto all = joined(plan)) {
    out.buildingArea = areaOf(all);
    out.buildingKnown = !eavesUnknown;
    if (evidence) {
      evidence->building = *all;
      if (const auto storeys = joined(storeyPlans)) evidence->storeys = *storeys;
    }
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
  out.legalNotes.push_back("未評価：壁で囲まれていない所（バルコニー・ポーチ・ピロティなど、開放性による扱い）、柱だけで囲まれた部分、特例軒等、高い開放性の指定（同 2 号ただし書）。");
  // docs/162: the total the floor area ratio is worked out from.
  std::vector<std::string> unreadRooms, notBasement;
  for (const FloorRatioPart& kind : floorRatioParts()) {
    AreaTable::RatioPart part;
    part.code = kind.code;
    part.label = kind.label;
    part.basis = kind.basis;
    for (const RoomRow& r : rooms.rows) {
      if (r.ratioPart != kind.code) continue;
      if (!r.counted) {
        unreadRooms.push_back(r.name);
        continue;
      }
      if (part.code == "basement_dwelling") {
        // In a basement whose ceiling is 1 m or less above the ground (法 52 条 3 項).
        const AreaRow* storey = nullptr;
        for (const AreaRow& row : out.rows)
          if (row.enclosedKnown && row.level == r.level) storey = &row;
        const double ceiling = r.ceilingHeight > 0.0 ? r.ceilingHeight : storey ? storey->ceiling : 0.0;
        if (!storey || !storey->basement || ceiling <= 0.0 || storey->elevation + ceiling > out.ground + 1000.0 + 1e-6) {
          notBasement.push_back(r.name);
          continue;
        }
      }
      ++part.rooms;
      part.area += r.centreArea;
    }
    if (part.rooms == 0) continue;
    if (kind.share > 0.0) part.limit = out.legalTotal * kind.share;
    part.excluded = part.limit >= 0.0 ? std::min(part.area, part.limit) : part.area;
    out.ratioParts.push_back(part);
  }
  out.ratioKnown = everyKnown && unreadRooms.empty();
  out.ratioTotal = out.legalTotal;
  for (const AreaTable::RatioPart& part : out.ratioParts) out.ratioTotal -= part.excluded;
  out.ratioTotal = std::max(0.0, out.ratioTotal);
  out.legalNotes.push_back("延べ面積は各階の床面積の合計。容積率の算定の延べ面積は、そこから「容積率の区分」を付けた室の面積（壁芯）を引いた値"
                           "（区分ごとに、延べ面積に割合を掛けた面積まで）。根拠：施行令 第 2 条 第 1 項 第 4 号ただし書・第 3 項、建築基準法 第 52 条 第 6 項 "
                           "第 1 号・第 2 号、施行令 第 135 条の 16、e-Gov 法令検索で 2026-11-17 に読んだ版（令：令和 8 年 11 月 1 日施行、法：令和 8 年 5 月 27 日施行）。");
  if (out.ratioParts.empty()) {
    out.legalNotes.push_back("容積率の区分を付けた室が無いので、容積率の算定の延べ面積は延べ面積と同じ（車庫・備蓄倉庫・昇降路・共用廊下などがあれば、室のプロパティで区分を付けてください）。");
  } else {
    for (const AreaTable::RatioPart& part : out.ratioParts) {
      std::string line = "容積率の算定で入れない部分：" + part.label + " " + std::to_string(part.rooms) + " 室 " + number(part.area / 1e6, 2) + " ㎡";
      if (part.limit >= 0.0)
        line += "、限度 " + number(part.limit / 1e6, 2) + " ㎡ → " + number(part.excluded / 1e6, 2) + " ㎡ を引いた";
      else
        line += "（限度なし）を引いた";
      out.legalNotes.push_back(line + "（" + part.basis + "）。");
    }
    out.legalNotes.push_back("区分を付けた室は、室の面積（壁芯）をそのまま引いた（室の中に床の無い所があれば、その分多く引いている）。その部分が条文の用途に当たるか"
                             "（専ら駐車のため、専ら防災のため、床に据え付ける蓄電池、など）は見ていない。");
  }
  if (!unreadRooms.empty()) {
    std::string names;
    for (const std::string& n : unreadRooms) names += (names.empty() ? "" : "・") + n;
    out.legalNotes.push_back("容積率の算定の延べ面積は未評価：区分を付けた室（" + names + "）が閉じていない、または同じ所に室が 2 つある。");
  }
  if (!notBasement.empty()) {
    std::string names;
    for (const std::string& n : notBasement) names += (names.empty() ? "" : "・") + n;
    out.legalNotes.push_back("「地階の住宅等」を付けた室のうち " + names + " は、地階で天井が地盤面から 1 m 以下の室でない（または地盤面・天井の高さが分からない）ので引いていない。");
  }
  for (const AreaTable::RatioPart& part : out.ratioParts)
    if (part.code == "basement_dwelling")
      out.legalNotes.push_back("地階の住宅等の限度は、建物全体を住宅等として延べ面積の 1/3 にした（住宅等でない部分があれば、限度はその分小さい）。天井の高さは室の天井高、無ければ次のレベルまでの高さ。");
  out.legalNotes.push_back("未評価：特定行政庁が認める機械室等（同 第 6 項 第 3 号）、同じ敷地のほかの建築物の床面積、容積率の最低限度の算定。");
  if (eaves > 0) out.legalNotes.push_back("建築面積：1 m 以上出た軒は、その先から 1 m 下がった線まで入れた（屋根を載せた線から測った）。");
  if (canopies > 0) out.legalNotes.push_back("建築面積：壁の中心線から 1 m 以上出た庇は、その先から 1 m 下がった線まで入れた。");
  // docs/166: the ground and the basements.
  if (!ground) {
    out.legalNotes.push_back("地盤面：「GL」という名前のレベルが無いので、地階かどうかは見ていない（未評価。地階の建築面積・法 52 条 3 項）。");
  } else {
    out.legalNotes.push_back("地盤面：レベル「GL」の高さ " + number(*ground, 0) + " とした（建物が周りの地面と接する位置の平均の高さ、令 2 条 2 項。高低差が 3 m を超える時の扱いは見ていない）。"
                             "地階は令 1 条 2 号（床が地盤面下で、床から地盤面までが天井の高さの 1/3 以上）、e-Gov 法令検索で 2026-11-17 に読んだ版。");
    for (const AreaRow& r : out.rows)
      if (r.basement)
        out.legalNotes.push_back(r.level + "：地階（床から地盤面まで " + number(*ground - r.elevation, 0) + "、天井の高さ " + number(r.ceiling, 0) +
                                 (r.ceilingFromRooms ? "（室の天井高の平均）" : "（次のレベルまで）") + "）。");
    if (!basementsLeftOut.empty()) {
      std::string names;
      for (const std::string& n : basementsLeftOut) names += (names.empty() ? "" : "・") + n;
      out.legalNotes.push_back("建築面積：地階で地盤面上 1 m 以下の " + names + " は入れていない（上の階で覆われている所は、上の階として入る）。");
    }
    if (!basementsKept.empty()) {
      std::string names;
      for (const std::string& n : basementsKept) names += (names.empty() ? "" : "・") + n;
      out.legalNotes.push_back("建築面積：地階の " + names + " は、上の端（次のレベル）が地盤面から 1 m を超えるので入れた。");
    }
  }
  if (balconiesInBuilding > balconiesOnPosts) out.legalNotes.push_back("建築面積：壁の中心線から 1 m 以上出たバルコニーは、その先から 1 m 下がった線まで入れた（はね出し縁の類として）。");
  if (balconiesOnPosts > 0)
    out.legalNotes.push_back("建築面積：柱で支えたバルコニー " + std::to_string(balconiesOnPosts) + " 個は、柱の中心線まで全部入れた（令 2 条 1 項 2 号「外壁又はこれに代わる柱の中心線」。特定行政庁の取扱いを確かめてください）。");
  if (out.balconies > 0) {
    std::string line = "バルコニー " + std::to_string(out.balconies) + " 個（床の合計 " + number(out.balconyArea / 1e6, 2) + " ㎡）：";
    line += "床面積に入れた " + std::to_string(out.balconiesCounted) + " 個（" + number(out.balconyFloorArea / 1e6, 2) + " ㎡）。通達どおりに数えた " +
            std::to_string(out.balconiesByNotice) + " 個、手で「入れない」とした " + std::to_string(out.balconiesExcluded) + " 個。";
    out.legalNotes.push_back(line);
    out.legalNotes.push_back("バルコニーの床面積：昭和 61 年 4 月 30 日 建設省住指発第 115 号「床面積の算定方法について」1 (5)（吹きさらしの廊下に準じる）・(4)"
                             "（外気に有効に開放されている部分の高さが 1.1 m 以上かつ天井の高さの 1/2 以上なら、幅 2 m までは算入しない）。国土交通省の通達のページで "
                             "2026-11-18 に読んだ。2 m は壁の中心線から、天井の高さは真上の床・屋根・バルコニーの下の面まで（上に何も無ければ開放とした）、"
                             "開放の高さは腰壁の上から（手すり・手すりなしは床から）。");
    out.legalNotes.push_back("未評価（バルコニー）：隣地・ほかの建物からの離れ（外気に有効に開放か）、2 面以上開放の角、手すりの開放の割合。特定行政庁の取扱いを確かめてください。");
    if (balconiesLoose > 0)
      out.legalNotes.push_back("床面積に入れるとしたバルコニーのうち " + std::to_string(balconiesLoose) + " 個は、床の高さにレベル（階）が無いので入れていない。");
  }
  if (eavesUnknown) out.legalNotes.push_back("建築面積は未評価：1 m 以上出た軒のどこが妻かを読めない屋根がある（L 字などの切妻）。");
  if (!everyKnown) out.legalNotes.push_back("床面積を出せない階がある（形を直接変えて壁として読めない壁がある）：その階と延べ面積は未評価。");
  (void)stairsArriving;
  return out;
}

AreaTable areaTable(const Document& document) { return measureAreas(document); }

Result<std::vector<draw::Sheet>> areaEvidenceSheets(const Document& document, double scale) {
  using R = Result<std::vector<draw::Sheet>>;
  if (!(scale > 0) || !std::isfinite(scale)) return R::failure("縮尺を指定してください");
  detail::AreaGeometry geometry;
  const auto areas = measureAreas(document, &geometry);
  std::vector<draw::Sheet> sheets;
  draw::SheetSpec spec;
  spec.scale = scale;
  const auto project = projectOutline(document);
  if (const auto name = project.find("project_name"); name != project.end()) spec.title.project = name->second;
  const auto advance = [&](const std::string& title) {
    spec.title.drawing = title;
    const auto number = sheets.size() + 1;
    spec.title.number = std::string(number < 10 ? "A-0" : "A-") + std::to_string(number);
  };
  for (const auto& page : detail::areaEvidencePages(document, areas, geometry, scale)) {
    advance(page.view.spec.title);
    auto laid = draw::layoutSheetWithTables(page.view, page.tables, spec);
    if (!laid) return R::failure(laid.error());
    sheets.push_back(std::move(*laid));
    for (const auto& table : page.continuation) {
      advance(table.title);
      auto continued = draw::tableSheet(table, spec);
      if (!continued) return R::failure(continued.error());
      sheets.push_back(std::move(*continued));
    }
  }
  if (sheets.empty()) return R::failure("求積図の範囲がありません。階レベルと壁の囲いを確認してください。");
  return sheets;
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

LayerQuantities layerQuantities(const Document& document) {
  LayerQuantities out;
  std::map<std::tuple<int, std::string, long long>, std::size_t> at;  // (part order, name, thickness in 0.01 mm) -> row
  for (const Object& o : document.objects()) {
    const std::string kind = attribute(o, "kind");
    if (kind != "wall" && kind != "slab" && kind != "roof") continue;
    if (o.shape.IsNull()) continue;
    const auto usage = Modeler::layerUsage(document, o);
    if (!usage) {
      ++out.withoutLayers;
      continue;
    }
    double thickness = 0.0;
    for (const auto& l : usage->layers) thickness += l.thickness;
    if (thickness <= 0.0) continue;
    const double area = geom::volume(document.worldShape(o)) / thickness;
    const int order = kind == "wall" ? 0 : kind == "slab" ? 1 : 2;
    const std::string part = kind == "wall" ? "壁" : kind == "slab" ? "床" : "屋根";
    std::set<std::size_t> counted;
    for (const auto& l : usage->layers) {
      const auto key = std::make_tuple(order, l.name, std::llround(l.thickness * 100));
      auto found = at.find(key);
      if (found == at.end()) {
        found = at.emplace(key, out.rows.size()).first;
        out.rows.push_back({part, l.name, l.thickness, 0, 0.0, 0.0});
      }
      LayerQuantityRow& row = out.rows[found->second];
      if (counted.insert(found->second).second) ++row.elements;  // a layer twice in one element (board both sides): one element
      row.area += area;
      row.volume += area * l.thickness;
    }
  }
  std::stable_sort(out.rows.begin(), out.rows.end(), [](const LayerQuantityRow& a, const LayerQuantityRow& b) {
    auto rank = [](const std::string& p) { return p == "壁" ? 0 : p == "床" ? 1 : 2; };
    return rank(a.part) < rank(b.part);
  });
  out.notes.push_back("面積は、部材の体積を厚さで割った値（開口の分は引いてある。屋根は勾配なりの面積）。体積は、その面積に層の厚さを掛けた値。");
  out.notes.push_back("壁の角で、外側の層が内側の層より長い分（入隅・出隅の差）は見ていない。概算として使ってください（ロス・割付は入っていない）。");
  out.notes.push_back("同じ部材に同じ層が 2 枚ある時（石膏ボード両面など）は、面積を 2 回数え、部材の数は 1 つ。");
  if (out.withoutLayers > 0)
    out.notes.push_back("層の無い壁・床・屋根（仕様・種類の層が無い、または厚さを変えて層の合計と合わない物）" + std::to_string(out.withoutLayers) + " 個は入っていない。");
  return out;
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
  } else if (which == 11) {
    // docs/210: the energy specification as given: each part's insulation and each opening's performance.
    t.title = "省エネ仕様の表";
    t.headers = {"部位", "名前", "数", "断熱・性能", "資料"};
    std::map<std::pair<std::string, std::string>, std::pair<int, std::pair<std::string, std::string>>> parts;
    for (const Object& o : document.objects()) {
      const std::string kind = attribute(o, "kind");
      if (kind != "wall" && kind != "slab" && kind != "roof" && kind != "ceiling") continue;  // docs/236
      const std::string text = insulationText(o);
      if (text.empty()) continue;
      const std::string part = kind == "wall" ? "外皮：壁" : kind == "slab" ? "外皮：床" : kind == "ceiling" ? "外皮：天井" : "外皮：屋根";
      std::string name = kind == "wall" ? attribute(o, "wall_type_name") : attribute(o, "type_name");
      if (name.empty()) name = kind == "wall" ? "（仕様なし）" : o.name;
      const nlohmann::json j = nlohmann::json::parse(attribute(o, "insulation"), nullptr, false);
      auto& entry = parts[{part, name + "｜" + text}];
      ++entry.first;
      entry.second = {text, j.is_object() ? j.value("source", std::string()) : std::string()};
    }
    for (const auto& [key, value] : parts)
      t.rows.push_back({key.first, key.second.substr(0, key.second.find("｜")), std::to_string(value.first), value.second.first, value.second.second});
    std::map<std::string, std::pair<int, std::pair<std::string, std::string>>> rated;
    for (const OpeningRow& o : openingSchedule(document).rows) {
      if (o.uValue < 0) continue;
      auto& entry = rated[o.mark + "｜" + number(o.uValue, 2) + "｜" + number(o.solarGain, 2) + "｜" + o.glass];
      ++entry.first;
      entry.second = {"熱貫流率 " + number(o.uValue, 2) + " W/(m2K)" +
                          (o.solarGain >= 0 ? "　日射熱取得率 " + number(o.solarGain, 2) : std::string()) +  // docs/227
                          (o.glass.empty() ? std::string() : "　" + o.glass),
                      o.performanceSource};
    }
    for (const auto& [key, value] : rated)
      t.rows.push_back({"開口部", key.substr(0, key.find("｜")), std::to_string(value.first), value.second.first, value.second.second});
    t.notes = {"入れた仕様と資料の一覧です。熱抵抗 R は厚さ÷熱伝導率（入れた時はその値）。",
               "仕様基準の値（地域ごとの断熱材の熱抵抗・開口部の熱貫流率など）との比較、外皮・一次エネルギーの計算はしていません。"
               "基準は申請先と申請時点の告示で確かめます。"};
  } else if (which == 10) {
    // docs/209: the outline of the project for the cover, read from the model's data.
    t.title = "計画概要";
    t.headers = {"項目", "内容", "根拠・注"};
    const auto& settings = document.settings();
    const auto& site = settings.siteBoundary;
    const auto geometry = site ? document.siteBoundaryGeometry(site->outline) : Result<SiteBoundaryGeometry>::failure("");
    const AreaTable areas = areaTable(document);
    const std::string missing = "未入力";
    // docs/214: the items given for the project (not in the model).
    const auto given = projectOutline(document);
    const auto item = [&](const char* key, const std::string& label, const std::string& note) {
      const auto it = given.find(key);
      t.rows.push_back({label, it == given.end() ? missing : projectOutlineText(key, it->second), note});
    };
    const std::string source = given.count("source") ? "入力した項目。確認資料 " + given.at("source") : "入力した項目";
    item("project_name", "物件名", given.count("project_name") ? source : "計画概要の項目（図面一式の物件名）");
    item("client", "建築主", source);
    for (const auto& [key, label] : {std::pair{"agent", "代理者"}, {"designer", "設計者"},
                                     {"supervisor", "工事監理者"}, {"contractor", "工事施工者"}}) {
      const std::string explicitName = std::string("form_") + key + "_name";
      if (given.count(explicitName)) item(explicitName.c_str(), label, source);
      else if (std::string(key) != "agent" || given.count(key)) item(key, label, source);
    }
    item("structural_review", "構造計算適合性判定の申請", source);                  // docs/232
    item("energy_plan", "建築物エネルギー消費性能確保計画の提出", source);       // docs/232
    item("address", "地名地番", source);
    item("residence", "住居表示", source);
    item("city_planning", "都市計画区域等", source + "。法の区域の確認は申請先で");
    item("fire_zone", "防火地域", source + "。防火の規定との照合はしていません");
    item("other_zones", "その他の区域、地域、地区又は街区", source);
    item("main_use", "主要用途", source + (given.count("use_code") ? "。区分 " + given.at("use_code") : std::string()));
    item("work_kind", "工事種別", source);
    if (given.count("permissions")) item("permissions", "許可・認定等", source);
    item("start_date", "工事着手予定年月日", source);
    item("finish_date", "工事完了予定年月日", source);
    if (given.count("specific_process")) item("specific_process", "特定工程", source);  // docs/232
    if (given.count("other_matters")) item("other_matters", "その他必要な事項", source);
    if (geometry) {
      t.rows.push_back({"敷地面積", m2(geometry->areaMm2), "指定した敷地境界の輪郭の面積。確認資料 " + site->source});
    } else {
      t.rows.push_back({"敷地面積", missing, "敷地境界を登録します"});
    }
    std::string zone, zoneSource;
    if (site)
      if (const Object* outline = document.find(site->outline)) {
        zone = attribute(*outline, "use_zone");
        zoneSource = attribute(*outline, "use_zone_source");
      }
    t.rows.push_back({"用途地域の区分", zone.empty() ? missing : zone == "residential" ? "住居系" : zone == "industrial" ? "工業系" : "商業系・指定なし",
                      zone.empty() ? "どの地域かは申請先で確認" : "確認資料 " + zoneSource});
    if (site && site->road) {
      const auto& road = *site->road;
      t.rows.push_back({"道路", (road.kind.empty() ? missing : road.kind) + "　幅員 " + (road.widthMm > 0 ? number(road.widthMm / 1000, 2) + " m" : missing) +
                                    "　接道長 " + (road.frontageMm > 0 ? number(road.frontageMm / 1000, 2) + " m" : missing),
                        "確認資料 " + road.source + "。法上の道路・接道は未評価"});
    } else {
      t.rows.push_back({"道路", missing, "配置図の道路・接道情報"});
    }
    t.rows.push_back({"真北", settings.siteNorth ? "モデルの +Y から時計回り " + number(settings.siteNorth->angle, 1) + "°" : missing,
                      settings.siteNorth ? "確認資料 " + settings.siteNorth->source : ""});
    if (areas.buildingKnown) {
      t.rows.push_back({"建築面積", m2(areas.buildingArea), "面積の表（令 2 条 1 項 2 号、docs/141）"});
      if (geometry && geometry->areaMm2 > 0)
        t.rows.push_back({"建蔽率（参考）", number(areas.buildingArea / geometry->areaMm2 * 100, 2) + " %", "建築面積÷敷地面積。上限との比較はしていません"});
    } else {
      t.rows.push_back({"建築面積", "未評価", "面積の表で出せません"});
    }
    t.rows.push_back({"延べ面積", m2(areas.legalTotal), "面積の表（令 2 条 1 項 4 号）"});
    if (areas.ratioKnown) {
      t.rows.push_back({"容積率の算定の延べ面積", m2(areas.ratioTotal), "面積の表（docs/162）"});
      if (geometry && geometry->areaMm2 > 0)
        t.rows.push_back({"容積率（参考）", number(areas.ratioTotal / geometry->areaMm2 * 100, 2) + " %", "÷敷地面積。上限との比較はしていません"});
    }
    int storeys = 0, basements = 0;
    for (const AreaRow& row : areas.rows) {
      if (!row.enclosedKnown || row.regions == 0) continue;
      ++storeys;
      if (row.basement) ++basements;
    }
    t.rows.push_back({"階数", storeys > 0 ? "地上 " + std::to_string(storeys - basements) + (basements > 0 ? "　地下 " + std::to_string(basements) : std::string()) : missing,
                      "壁で閉じた範囲のある階を数えた（法の階数の数え方ではありません）"});
    for (const PermitCheck& c : permitElevationChecks(document))
      if (c.item == "いちばん高い所（参考）") t.rows.push_back({"最も高い所（参考）", c.details.substr(0, c.details.find("（")), "法の建築物の高さ・軒の高さではありません"});
    if (given.count("structure"))  // docs/231: as given (第三面 13 ハ)
      t.rows.push_back({"構造", given.at("structure"), source});
    else
      t.rows.push_back({"構造", settings.wallCalculation != "{}" ? "木造（在来軸組、壁量の計算の入力から）" : missing, "構造の種別"});
    t.notes = {"モデルから読んだ今の値です。法の制限（建蔽率・容積率の上限、高さの制限など）との比較はしていません。",
               "モデルに無い項目は「計画概要の項目」で入れた値です（申請書 第二面・第三面の項目から。様式の原文・申請先の様式と照らすのは申請時）。"};
  } else if (which == 7 || which == 8 || which == 9) {
    // docs/208: the wall quantity forms, from the same calculation as check_wall_quantity.
    const auto made = construction::checkWallQuantity(document);
    const nlohmann::json report = made ? nlohmann::json::parse(*made, nullptr, false) : nlohmann::json();
    const nlohmann::json inputs = report.is_object() ? report.value("inputs", nlohmann::json::object()) : nlohmann::json::object();
    std::map<std::string, nlohmann::json> floors;
    for (const auto& f : inputs.value("floors", nlohmann::json::array()))
      if (f.is_object() && f.contains("id")) floors[f.at("id").is_string() ? f.at("id").get<std::string>() : f.at("id").dump()] = f;
    const auto num = [](const nlohmann::json& j, const char* key, int decimals) {
      return j.is_object() && j.contains(key) && j.at(key).is_number() ? number(j.at(key).get<double>(), decimals) : std::string();
    };
    const auto state = [](const std::string& s) {
      return s == "pass" ? std::string("参考値内") : s == "fail" ? std::string("要確認") : std::string("未評価");
    };
    const auto reasons = [](const nlohmann::json& row) {
      std::string text;
      for (const auto& r : row.value("reasons", nlohmann::json::array()))
        if (r.is_string()) text += (text.empty() ? "" : "／") + r.get<std::string>();
      return text;
    };
    const nlohmann::json entries = report.is_object() ? report.value("table_snapshot", nlohmann::json::object()).value("entries", nlohmann::json::object())
                                                      : nlohmann::json::object();
    if (which == 7) {
      t.title = "壁量計算表";
      t.headers = {"階", "方向", "床面積 (m2)", "地震 係数 (cm/m2)", "地震 必要 (m)", "見付面積 (m2)", "風 係数 (cm/m2)", "風 必要 (m)",
                   "必要壁量 (m)", "存在壁量 (m)", "照合", "理由"};
      const std::string windCoefficient = num(inputs, "wind_coefficient_cm_m2", 0);
      for (const auto& row : report.value("rows", nlohmann::json::array())) {
        const std::string id = row.value("floor", std::string()), direction = row.value("direction", std::string());
        if (id.empty()) continue;
        const nlohmann::json floor = floors.count(id) ? floors.at(id) : nlohmann::json::object();
        t.rows.push_back({row.value("name", id), direction, num(floor, "area_m2", 2), num(row, "earthquake_cm_m2", 1), num(row, "earthquake_m", 2),
                          num(floor, direction == "X" ? "wind_x_m2" : "wind_y_m2", 2), windCoefficient, num(row, "wind_m", 2),
                          num(row, "required_m", 2), num(row, "existing_m", 2), state(row.value("status", std::string())), reasons(row)});
      }
    } else if (which == 8) {
      t.title = "耐力壁の一覧";
      t.headers = {"階", "方向", "符号", "仕様", "長さ (m)", "倍率", "高さの係数", "有効長さ (m)", "状態", "条件・理由"};
      for (const auto& w : report.value("walls", nlohmann::json::array())) {
        const std::string id = w.value("floor", std::string());
        const nlohmann::json floor = floors.count(id) ? floors.at(id) : nlohmann::json::object();
        const std::string spec = w.value("spec", std::string());
        const std::string specName = entries.contains(spec) ? entries.at(spec).value("name", spec) : spec;
        const std::string why = reasons(w);
        t.rows.push_back({floor.value("name", id), w.value("direction", std::string()), w.contains("id") && w.at("id").is_string() ? w.at("id").get<std::string>() : std::string(),
                          specName, w.contains("length_mm") && w.at("length_mm").is_number() ? number(w.at("length_mm").get<double>() / 1000, 2) : std::string(),
                          num(w, "ratio", 1), num(w, "height_factor", 2), num(w, "effective_m", 2),
                          w.value("status", std::string()) == "evaluated" ? "計算に入れた" : "未評価",
                          why.empty() ? w.value("conditions", std::string()) : why});
      }
    } else {
      t.title = "四分割（側端部分）";
      t.headers = {"階", "方向", "側端", "面積 (m2)", "必要 (m)", "存在 (m)", "充足率", "壁率比", "照合", "見た決まり・理由"};
      for (const auto& row : report.value("quarter_balance", nlohmann::json::array())) {
        bool first = true;
        for (const auto& s : row.value("strips", nlohmann::json::array())) {
          const std::string side = s.value("side", std::string());
          t.rows.push_back({first ? row.value("name", std::string()) : "", first ? row.value("direction", std::string()) : "",
                            side == "low" ? "下側（小さい座標）" : side == "high" ? "上側（大きい座標）" : side, num(s, "area_m2", 2), num(s, "required_m", 2),
                            num(s, "existing_m", 2), num(s, "ratio", 2), first ? num(row, "wall_ratio", 2) : "",
                            first ? state(row.value("status", std::string())) : "",
                            first ? (row.value("basis", std::string()) + (reasons(row).empty() ? "" : "／" + reasons(row))) : ""});
          first = false;
        }
        if (first)
          t.rows.push_back({row.value("name", std::string()), row.value("direction", std::string()), "", "", "", "", "", "",
                            state(row.value("status", std::string())), reasons(row)});
      }
    }
    if (report.is_object()) t.notes.push_back(report.value("notice", std::string()));
    t.notes.push_back("建物全体の安全・法適合の判定ではありません。入力は壁量の計算の入力（set_wall_calculation）のとおりです。");
  } else if (which == 6) {
    // docs/207: daylight and ventilation of the rooms (法 28 条, 令 19・20 条; docs/203, 205).
    const RoomOpeningInputs inputs = roomOpeningInputs(document);
    t.title = "採光・換気計算表";
    t.headers = {"室名", "階", "区分", "床面積 (m2)", "開口", "開口の面積 (m2)", "採光補正係数", "採光に有効な面積 (m2)",
                 "換気に有効な面積 (m2)", "照合", "係数の根拠"};
    const auto daylightKindName = [](const std::string& k) {
      return k == "dwelling" ? std::string("住宅の居室") : k == "other" ? std::string("その他の居室") : k == "none" ? std::string("居室でない") : std::string("未入力");
    };
    for (const RoomOpeningInput& entry : inputs.rooms) {
      const RoomRow& r = entry.room;
      if (r.daylightKind.empty() || r.daylightKind == "none") continue;
      const RoomDaylightCheck check = roomDaylightCheck(entry);
      const std::string floor = r.counted ? number(r.centreArea / 1e6, 2) : "未評価";
      bool first = true;
      for (std::size_t i : entry.openingIndices) {
        const OpeningRow& o = inputs.openings[i];
        const double area = o.width * o.height / 1e6;
        t.rows.push_back({first ? r.name : "", first ? r.level : "", first ? daylightKindName(r.daylightKind) : "", first ? floor : "",
                          o.mark, number(area, 2), o.daylightFactor >= 0 ? number(o.daylightFactor, 2) : "未入力",
                          o.daylightFactor >= 0 ? number(area * o.daylightFactor, 2) : "", o.ventilationM2 >= 0 ? number(o.ventilationM2, 2) : "未入力",
                          "", o.daylightBasis});
        first = false;
      }
      if (first)
        t.rows.push_back({r.name, r.level, daylightKindName(r.daylightKind), floor, "（開口候補なし）", "", "", "", "", "", ""});
      const bool dwelling = r.daylightKind == "dwelling";
      t.rows.push_back({"", "", "", "", "計", "", "",
                        dwelling && entry.factorsMissing == 0 && r.counted
                            ? number(entry.daylightArea / 1e6, 2) + "／必要 " + number(r.centreArea / 7.0 / 1e6, 2)
                            : "",
                        r.ventilation != "equipment" && entry.ventilationMissing == 0 && r.counted
                            ? number(entry.ventilationArea / 1e6, 2) + "／必要 " + number(r.centreArea / 20.0 / 1e6, 2)
                            : "",
                        "採光 " + check.daylightState + "・換気 " + check.ventilationState + "・機械換気 " + check.mechanicalState, ""});
    }
    t.notes = {"床面積は室の壁芯の面積。開口は、室の壁の区間に収まる矩形の開口（候補）。開口の面積は建具表の幅×高さ。",
               "採光：住宅の居住のための居室は、採光に有効な面積の合計を床面積の 1/7 と比べる（建築基準法 第 28 条 1 項、施行令 第 19 条 3 項 表（三））。"
               "有効な面積＝開口の面積×採光補正係数（施行令 第 20 条）。",
               "換気：換気に有効な面積の合計を床面積の 1/20 と比べる（建築基準法 第 28 条 2 項）。換気設備の室は、設備の基準を見ていない。",
               "法令は e-Gov 法令検索で 2026-11-20 に確認（建築基準法 令和8年5月27日施行版、施行令 令和8年11月1日施行版）。",
               "機械換気：必要有効換気量＝回数（住宅等の居室 0.5、その他 0.3）×床面積×天井高と、入れた有効換気量を比べる（施行令 第 20 条の 8、2026-11-21 確認）。",
               "ふすま・障子などで一続きにした二室は、合わせて一室として照らす（法 28 条 4 項）。",
               "照合は参考で、合否の判定ではない。天窓・大臣の定める緩和・1台で複数の室をまかなう換気設備の合計は見ていない。"};
  } else if (which == 5) {
    // docs/172: the quantities of the layers.
    const LayerQuantities q = layerQuantities(document);
    t.title = "層の数量";
    t.headers = {"部位", "層", "厚さ (mm)", "部材の数", "面積 (m2)", "体積 (m3)"};
    for (const LayerQuantityRow& r : q.rows)
      t.rows.push_back({r.part, r.name, number(r.thickness, r.thickness == std::floor(r.thickness) ? 0 : 1), std::to_string(r.elements),
                        number(r.area / 1e6, 2), number(r.volume / 1e9, 3)});
    t.notes = q.notes;
  } else if (which == 4) {
    // Types of floors and roofs (docs/134, docs/136): read from the objects as they are now.
    t.title = "種類の表";
    t.headers = {"種類名", "部位", "数", "厚さ", "勾配", "軒の出", "けらばの出", "注"};
    struct Values {
      int count = 0;
      bool mixed = false;
      std::vector<double> v;  // thickness[, pitch, eave, gable eave]
      bool registered = false, differs = false;
      std::vector<std::optional<double>> want;  // the registered values (docs/149)
    };
    std::map<std::pair<int, std::string>, Values> types;  // (0 floor / 1 roof, name)
    const nlohmann::json reg = nlohmann::json::parse(document.settings().elementTypes, nullptr, false);
    if (reg.is_object())
      for (const auto& [name, entry] : reg.items()) {
        if (!entry.is_object()) continue;
        const bool roof = entry.value("part", "") == "roof";
        Values& at = types[{roof ? 1 : 0, name}];
        at.registered = true;
        for (const char* key : {"thickness", "pitch", "eave", "gable_eave"}) {
          if (!roof && at.want.size() == 1) break;
          at.want.push_back(entry.contains(key) ? std::optional<double>(entry.at(key).get<double>()) : std::nullopt);
        }
      }
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
      for (std::size_t i = 0; i < v.size() && i < at.want.size(); ++i)
        at.differs |= at.want[i] && std::abs(v[i] - *at.want[i]) > 0.5;
      ++at.count;
    }
    bool anyRegistered = false;
    for (const auto& [key, at] : types) {
      const bool roof = key.first == 1;
      anyRegistered |= at.registered;
      // A registered type shows its registered values; otherwise those of its first object.
      auto value = [&](std::size_t i, int digits) -> std::string {
        if (i < at.want.size() && at.want[i]) return number(*at.want[i], digits);
        return i < at.v.size() ? number(at.v[i], digits) : std::string();
      };
      std::string note;
      if (at.registered) note = at.count == 0 ? "登録だけ（まだ使っていない）" : at.differs ? "登録済み・登録した値と違う物があります" : "登録済み";
      else if (at.mixed) note = "値の違う物が混ざっています（1 つ目の値）";
      const std::string pitch = roof ? value(1, 1) : "";
      t.rows.push_back({key.second, roof ? "屋根" : "床", std::to_string(at.count), value(0, 0),
                        pitch.empty() ? "" : pitch + " 寸", roof ? value(2, 0) : "", roof ? value(3, 0) : "", note});
    }
    t.notes = {"床（スラブ）・屋根に付けた種類名ごとです。寸法は mm。同じ種類名の物は「同じ種類の…にする」でそろえられます。"};
    if (anyRegistered) t.notes.push_back("登録した種類は、その種類名を付けた床・屋根がその値になります（docs/149）。");
    // The build-ups of floor types with layers (docs/151), top to bottom.
    if (reg.is_object())
      for (const auto& [name, entry] : reg.items()) {
        if (!entry.is_object() || !entry.contains("layers") || !entry.at("layers").is_array() || entry.at("layers").empty()) continue;
        std::string text = name + " の構成（上から）：";
        double sum = 0.0;
        bool first = true;
        for (const auto& l : entry.at("layers")) {
          text += (first ? "" : "・") + l.value("name", std::string()) + " " + number(l.value("thickness", 0.0), 0);
          sum += l.value("thickness", 0.0);
          first = false;
        }
        text += "（計 " + number(sum, 0) + "）";
        if (entry.contains("thickness") && std::abs(entry.at("thickness").get<double>() - sum) > 0.5)
          text += "。厚さと合いません（図面に層の線は出ません）";
        t.notes.push_back(text + "。");
      }
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
    t.rows.push_back({"容積率の算定の延べ面積", "", "", "", "", "", "", a.ratioKnown ? number(a.ratioTotal / 1e6, 2) : "未評価"});
    t.rows.push_back({"建築面積", "", "", "", "", "", "", a.buildingKnown ? number(a.buildingArea / 1e6, 2) : "未評価"});
    t.notes = {"モデルから測った面積です。法の床面積・延べ面積・建築面積は、下の決まりで出しました（確認申請に使う時は、特定行政庁・確認検査機関の取扱いを確かめてください）。"};
    // On a sheet a note is one line: long ones are broken after a 。 or a 、 (about 100 characters a line).
    for (const std::string& n : a.legalNotes) {
      std::string line;
      std::size_t count = 0, breakAt = 0;  // characters in `line`; bytes of `line` up to the last 。/、
      for (std::size_t i = 0; i < n.size();) {
        const unsigned char c = static_cast<unsigned char>(n[i]);
        const std::size_t bytes = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const std::string ch = n.substr(i, bytes);
        i += bytes;
        line += ch;
        ++count;
        if (ch == "。" || ch == "、") breakAt = line.size();
        if (count >= 100 && breakAt > 0 && i < n.size()) {
          t.notes.push_back(line.substr(0, breakAt));
          line = "　" + line.substr(breakAt);
          count = 0;
          for (std::size_t k = 0; k < line.size(); ++k) count += (static_cast<unsigned char>(line[k]) & 0xC0) != 0x80;
          breakAt = 0;
        }
      }
      if (!line.empty()) t.notes.push_back(line);
    }
  }
  return t;
}

// docs/233: the vicinity map as a drawing: the picture of the tiles with the site in the middle
// (view mm = ground mm, north up), the site marked, at the smallest usual scale at which the picture
// is at most 200 x 140 mm on paper; the 出典 lines as its notes. Without a picture: an empty frame of
// that size saying where the map goes.
draw::View vicinityView(const Document& document, const VicinityMap& map, const std::optional<VicinityPicture>& picture,
                                double& scale) {
  draw::View view;
  view.spec.kind = draw::ViewKind::Plan;  // north mark up (the map's north is true north)
  view.spec.title = "付近見取図";
  view.spec.grid = false;
  double mpp = 0.0, width = 0.0, height = 0.0, siteX = 0.0, siteY = 0.0;
  if (picture && picture->width > 0 && picture->height > 0 && picture->rgba) {
    mpp = picture->metresPerPixel;
    width = picture->width;
    height = picture->height;
    siteX = picture->siteX;
    siteY = picture->siteY;
  } else {
    const VicinityTiles tiles = vicinityTiles(map);
    mpp = tiles.metresPerPixel;
    width = tiles.cols * 256.0;
    height = tiles.rows * 256.0;
    siteX = tiles.siteX;
    siteY = tiles.siteY;
  }
  const double unit = mpp * 1000.0;  // view mm per pixel
  const double x0 = -siteX * unit, x1 = (width - siteX) * unit, y1 = siteY * unit, y0 = -(height - siteY) * unit;
  scale = 25000.0;
  for (const double s : {1000.0, 1500.0, 2000.0, 2500.0, 3000.0, 5000.0, 7500.0, 10000.0, 15000.0, 20000.0, 25000.0})
    if ((x1 - x0) / s <= 200.0 && (y1 - y0) / s <= 140.0) {
      scale = s;
      break;
    }
  draw::Item frame;
  frame.kind = draw::Item::Kind::Polyline;
  frame.pen = draw::Pen::Visible;
  frame.closed = true;
  frame.points = {gp_Pnt2d(x0, y0), gp_Pnt2d(x1, y0), gp_Pnt2d(x1, y1), gp_Pnt2d(x0, y1)};
  if (picture && picture->rgba) {
    draw::Item image;
    image.kind = draw::Item::Kind::Image;
    image.points = {gp_Pnt2d(x0, y0), gp_Pnt2d(x1, y1)};
    image.imageWidth = picture->width;
    image.imageHeight = picture->height;
    image.rgba = picture->rgba;
    view.items.push_back(std::move(image));
  } else {
    draw::Item note;
    note.kind = draw::Item::Kind::Text;
    note.pen = draw::Pen::Text;
    note.points = {gp_Pnt2d((x0 + x1) / 2, (y0 + y1) / 2 - 8 * scale)};
    note.text = "地図は、画面の「図面一式を書き出す」で国土地理院の地図タイルから入ります";
    note.height = 3.0;
    note.hAlign = 1;
    note.vAlign = 1;
    view.items.push_back(std::move(note));
  }
  view.items.push_back(frame);
  // docs/234: the site's boundary turned to true north, its centre on the map's (the position given),
  // when there are a boundary and a true north and it is 5 mm or more across on paper.
  std::string siteNote;
  bool outlined = false;
  const auto& settings = document.settings();
  if (settings.siteBoundary && settings.siteNorth) {
    if (const auto g = document.siteBoundaryGeometry(settings.siteBoundary->outline); g && g->vertices.size() >= 3) {
      const double a = std::remainder(settings.siteNorth->angle, 360.0) * 3.14159265358979323846 / 180.0;
      const gp_Vec2d north(std::sin(a), std::cos(a)), east(std::cos(a), -std::sin(a));
      double cx = 0.0, cy = 0.0;
      for (const gp_Pnt& v : g->vertices) {
        cx += v.X();
        cy += v.Y();
      }
      cx /= g->vertices.size();
      cy /= g->vertices.size();
      draw::Item outline;
      outline.kind = draw::Item::Kind::Polyline;
      outline.pen = draw::Pen::Cut;
      outline.closed = true;
      double e0 = 1e300, e1 = -1e300, n0 = 1e300, n1 = -1e300;
      for (const gp_Pnt& v : g->vertices) {
        const gp_Vec2d d(v.X() - cx, v.Y() - cy);
        const double e = d.Dot(east), n = d.Dot(north);
        outline.points.push_back(gp_Pnt2d(e, n));
        e0 = std::min(e0, e);
        e1 = std::max(e1, e);
        n0 = std::min(n0, n);
        n1 = std::max(n1, n);
      }
      if (std::max(e1 - e0, n1 - n0) / scale >= 5.0) {
        view.items.push_back(std::move(outline));
        outlined = true;
        siteNote = "太線：敷地境界（真北に合わせて回し、重心を入れた緯度・経度に置いた。位置は入れた値のとおりで、地図とは照合していない）";
      } else {
        siteNote = "敷地は紙の上で 5 mm に満たないので、丸で示した（ズームを上げると敷地境界の形が出る）";
      }
    }
  } else if (settings.siteBoundary) {
    siteNote = "真北が入っていないので、敷地境界の形は描いていない（丸で示した）";
  }
  // The site: a ring 4 mm on paper and its name beside it (docs/234: or its outline, above).
  draw::Item ring;
  ring.kind = draw::Item::Kind::Arc;
  ring.pen = draw::Pen::Cut;
  ring.centre = gp_Pnt2d(0, 0);
  ring.radius = 4.0 * scale;
  ring.sweep = 2 * 3.14159265358979323846;
  if (!outlined) view.items.push_back(ring);
  draw::Item label;
  label.kind = draw::Item::Kind::Text;
  label.pen = draw::Pen::Text;
  label.points = {gp_Pnt2d(5.5 * scale, 0)};
  label.text = "敷地";
  label.height = 3.5;
  label.vAlign = 1;
  view.items.push_back(label);
  // docs/235: the landmarks given, on the map: a small ring and the name beside it; those off the map
  // are counted in the notes.
  int offMap = 0;
  for (const VicinityMark& m : map.marks) {
    const auto [e, n] = vicinityOffset(map, m.lat, m.lon);
    if (e < x0 || e > x1 || n < y0 || n > y1) {
      ++offMap;
      continue;
    }
    draw::Item dot;
    dot.kind = draw::Item::Kind::Arc;
    dot.pen = draw::Pen::Visible;
    dot.centre = gp_Pnt2d(e, n);
    dot.radius = 1.2 * scale;
    dot.sweep = 2 * 3.14159265358979323846;
    view.items.push_back(dot);
    draw::Item name;
    name.kind = draw::Item::Kind::Text;
    name.pen = draw::Pen::Text;
    name.points = {gp_Pnt2d(e + 2.5 * scale, n)};
    name.text = m.name;
    name.height = 2.5;
    name.vAlign = 1;
    view.items.push_back(name);
  }
  view.minU = x0;
  view.maxU = x1;
  view.minV = y0;
  view.maxV = y1;
  view.objects = 1;
  view.trueNorth = gp_Dir2d(0, 1);  // the map's up is true north (no 「仮」 on the north mark)
  view.calculationNotes = vicinityAttribution(map.layer);
  char where[96];
  std::snprintf(where, sizeof where, "中心：北緯 %.6f 度・東経 %.6f 度（ズーム %d）", map.lat, map.lon, map.zoom);
  view.calculationNotes.push_back(where);
  if (picture && picture->missingTiles > 0)
    view.calculationNotes.push_back("取れなかった地図タイル " + std::to_string(picture->missingTiles) + " 枚は灰色です");
  if (!siteNote.empty()) view.calculationNotes.push_back(siteNote);
  if (!map.marks.empty())  // docs/235
    view.calculationNotes.push_back("○：目標物（入れた緯度・経度に描いた）" +
                                    (offMap > 0 ? "。地図の外の " + std::to_string(offMap) + " 個は描いていない" : std::string()));
  return view;
}

Result<DrawingSet> drawingSet(const Document& document, const DrawingSetSpec& given) {
  // docs/214: the project's name from its outline when the title block has none.
  DrawingSetSpec spec = given;
  if (spec.title.project.empty()) {
    const auto outline = projectOutline(document);
    if (const auto it = outline.find("project_name"); it != outline.end()) spec.title.project = it->second;
  }
  using R = Result<DrawingSet>;
  struct Planned {
    std::string name;
    std::optional<draw::ViewSpec> view;
    std::optional<draw::Table> table;
    std::vector<draw::Table> beside;  // tables on the same sheet (docs/131)
    std::optional<draw::View> made;   // docs/233: a drawing made here (the vicinity map), at its own scale
    double scale = 0.0;
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
    if (shadowed || !levelIsStorey(levels, levels[i])) continue;  // docs/295
    draw::ViewSpec view = draw::planView(levels[i].elevation + 1100.0);
    view.depth = 1100.0 + 400.0;  // down to 400 under its floor: the storeys below are left out
    // docs/295: the timber frame's floor and roof framing are the framing plans' (伏図); the columns stay.
    view.excludeTags = {"基礎・土台", "1階床組", "2階床組", "小屋組"};
    view.wallsBelow = levels[i].elevation;  // docs/298: the storey below's walls
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
      // docs/255: through the middle of the building (walls, floors, roofs), not of everything shown:
      // the site's outline and a grid moved the cuts onto the outer walls' lines. A cut that would run
      // along a wall's line (within 300) goes to 3/8, 5/8, 1/4 or 3/4 of the building instead.
      Bnd_Box building, all;
      struct Line { double at; bool alongX; };
      std::vector<Line> wallLines;
      for (const Object& o : document.objects()) {
        if (o.shape.IsNull() || !document.isVisible(o.id)) continue;
        const Bnd_Box b = document.worldBounds(o);
        all.Add(b);
        const auto kind = o.attributes.find("kind");
        if (kind == o.attributes.end()) continue;
        if (kind->second == "wall" || kind->second == "slab" || kind->second == "roof") building.Add(b);
        if (kind->second == "wall" && !b.IsVoid()) {
          double a0, b0, c0, a1, b1, c1;
          b.Get(a0, b0, c0, a1, b1, c1);
          if (b1 - b0 < 0.5 * (a1 - a0)) wallLines.push_back({(b0 + b1) / 2, true});   // runs east-west
          if (a1 - a0 < 0.5 * (b1 - b0)) wallLines.push_back({(a0 + a1) / 2, false});  // runs north-south
        }
      }
      const Bnd_Box& box = building.IsVoid() ? all : building;
      if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const auto clearOfWalls = [&](double lo, double hi, bool alongX) {
          for (const double f : {0.5, 0.375, 0.625, 0.25, 0.75}) {
            const double at = lo + (hi - lo) * f;
            if (std::none_of(wallLines.begin(), wallLines.end(),
                             [&](const Line& l) { return l.alongX == alongX && std::abs(l.at - at) < 300.0; }))
              return at;
          }
          return (lo + hi) / 2;
        };
        const double cy = clearOfWalls(y0, y1, true), cx = clearOfWalls(x0, x1, false);
        cuts.push_back({"A", "A-A 断面図", gp_Pnt(cx, cy, 0), gp_Dir(0, -1, 0)});
        cuts.push_back({"B", "B-B 断面図", gp_Pnt(cx, cy, 0), gp_Dir(1, 0, 0)});
      }
    }
  }
  for (const Cut& c : cuts) {
    draw::ViewSpec view = draw::sectionView(c.point, c.toward);
    view.title = c.name;
    view.heightDims = true;  // docs/296
    planned.push_back({view.title, view, std::nullopt, {}});
  }
  const char* sides[4] = {"南立面図", "東立面図", "北立面図", "西立面図"};
  for (int side = 0; side < 4; ++side) {
    draw::ViewSpec view = draw::elevationView(document, side);
    view.title = sides[side];
    view.heightDims = true;  // docs/296
    planned.push_back({view.title, view, std::nullopt, {}});
  }
  // The first planned is the lowest storey's plan.
  for (int which : {0, 1, 2, 3, 4, 6, 7, 8, 9, 11}) {  // docs/210: and the energy specification when given  // docs/208: and the wall quantity forms when there is a calculation  // docs/207: and daylight / ventilation when rooms have a kind
    draw::Table table = scheduleTable(document, which);
    if (!tableHasContent(table, which)) continue;
    if (spec.tablesWithPlan) {
      planned.front().name += "・" + table.title;
      planned.front().beside.push_back(std::move(table));
    } else {
      planned.push_back({table.title, std::nullopt, table, {}});
    }
  }
  const auto compartments = fireCompartmentReview(document);
  if (!compartments.empty()) {
    auto table = fireCompartmentTable(compartments);
    planned.push_back({table.title, std::nullopt, std::move(table), {}});
  }
  const auto fire = fireSpreadStudy(document, false, true);
  if (fire.configured) {
    auto table = fireSpreadTable(fire);
    planned.push_back({table.title, std::nullopt, std::move(table), {}});
  }
  // docs/208: a plan of the bracing walls for each storey of the wall calculation (its n-th storey
  // on the model's n-th floor level, or the ground when there are fewer levels).
  if (document.settings().wallCalculation != "{}") {
    std::vector<double> floors;
    for (const FloorLevel& l : levels)
      if (isFloor(l) && (floors.empty() || std::abs(floors.back() - l.elevation) > 1e-6)) floors.push_back(l.elevation);
    const auto made = construction::checkWallQuantity(document);
    const nlohmann::json report = made ? nlohmann::json::parse(*made, nullptr, false) : nlohmann::json();
    const nlohmann::json inputs = report.is_object() ? report.value("inputs", nlohmann::json::object()) : nlohmann::json::object();
    std::size_t n = 0;
    for (const auto& f : inputs.value("floors", nlohmann::json::array())) {
      if (!f.is_object() || !f.contains("id")) continue;
      const std::string id = f.at("id").is_string() ? f.at("id").get<std::string>() : f.at("id").dump();
      draw::ViewSpec view = draw::planView((n < floors.size() ? floors[n] : 0.0) + 1100.0);
      view.depth = 1100.0 + 400.0;
      view.bracingFloor = id;
      view.title = "耐力壁配置図（" + f.value("name", id) + "）";
      planned.push_back({view.title, view, std::nullopt, {}});
      ++n;
    }
  }
  // docs/216: a plan of the insulation for each storey whose walls have some given; docs/217: a
  // section of it for each cut.
  {
    std::vector<Planned> insulated;
    for (const Planned& p : planned) {
      if (!p.view || p.view->kind != draw::ViewKind::Plan || !p.view->bracingFloor.empty() ||
          p.name.find("平面図") == std::string::npos)
        continue;
      if (Modeler::insulationRuns(document, p.view->frame.Location().Z()).empty() &&
          Modeler::insulationAreas(document, p.view->frame.Location().Z()).empty())  // docs/221
        continue;
      draw::ViewSpec view = *p.view;
      view.insulation = true;
      view.fill = false;  // the walls' outlines, the marks inside
      const std::string level = p.name.substr(0, p.name.find(" 平面図"));
      view.title = "断熱範囲図" + (level.empty() || level == "平面図" ? std::string() : "（" + level + "）");
      insulated.push_back({view.title, view, std::nullopt, {}});
    }
    for (auto& p : insulated) planned.push_back(std::move(p));
    // docs/217: and of each section that cuts a wall, floor or roof with insulation given.
    for (const Cut& c : cuts) {
      bool crosses = false;
      for (const Object& o : document.objects()) {
        if (crosses || o.shape.IsNull() || !document.isVisible(o.id)) continue;
        if (!Modeler::insulationBand(document, o)) continue;
        const Bnd_Box b = document.worldBounds(o);
        if (b.IsVoid()) continue;
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        bool below = false, above = false;
        for (const double x : {x0, x1})
          for (const double y : {y0, y1}) {
            const double d = gp_Vec(c.point, gp_Pnt(x, y, c.point.Z())).Dot(gp_Vec(c.toward));
            below = below || d < -1e-3;
            above = above || d > 1e-3;
          }
        crosses = below && above;
      }
      if (!crosses) continue;
      draw::ViewSpec view = draw::sectionView(c.point, c.toward);
      view.insulation = true;
      view.fill = false;
      view.title = "断熱範囲図（" + c.label + "-" + c.label + " 断面）";
      planned.push_back({view.title, view, std::nullopt, {}});
    }
    // docs/223: the insulation specification table on each insulation sheet.
    const draw::Table specTable = insulationSpecTable(document);
    const draw::Table equipment = equipmentTable(document);  // docs/224
    const draw::Table openingSpec = openingSpecTable(document);  // docs/225
    std::vector<draw::Table> tables;
    if (!specTable.rows.empty()) tables.push_back(specTable);
    if (!openingSpec.rows.empty()) tables.push_back(openingSpec);
    if (!equipment.rows.empty()) tables.push_back(equipment);
    bool onSheet = false;
    for (Planned& p : planned)
      if (p.view && p.view->insulation) {
        p.beside = tables;
        onSheet = true;
      }
    if (!onSheet && !equipment.rows.empty()) planned.push_back({equipment.title, std::nullopt, equipment, {}});  // docs/224
  }
  if (document.settings().siteBoundary) {
    const auto site = draw::sitePlanView();
    planned.insert(planned.begin(), {site.title, site, std::nullopt, {}});
    // docs/288: the ground level (令 2 条 2 項) right after the site plan, on the contact perimeter's drawing.
    const GroundLevelStudy ground = groundLevelStudy(document);
    if (ground.report) {
      auto spec2 = site;
      spec2.title = "地盤面算出表";
      if (const auto view = draw::makeView(document, spec2)) {
        draw::View made = *view;
        made.siteGroundTable = groundLevelTable(ground);
        Planned p{spec2.title, std::nullopt, std::nullopt, {}, std::move(made), spec.scale};
        planned.insert(planned.begin() + 1, std::move(p));
      }
    }
  }
  {
    // docs/209: the outline of the project first, after the sheet list (its name from the title block).
    draw::Table summary = scheduleTable(document, 10);
    if (!spec.title.project.empty())
      for (auto& row : summary.rows)
        if (row[0] == "物件名") {
          row[1] = spec.title.project;
          row[2] = "図面一式の物件名（表題欄）";
        }
    planned.insert(planned.begin(), {summary.title, std::nullopt, summary, {}});
  }
  // docs/233: the vicinity map after the outline, when its position is given.
  if (const auto map = vicinityMapOf(document)) {
    double scale = 0.0;
    draw::View view = vicinityView(document, *map, spec.vicinity, scale);
    Planned p{view.spec.title, std::nullopt, std::nullopt, {}, std::move(view), scale};
    planned.insert(planned.begin() + 1, std::move(p));
  }
  // docs/289: the timber framing plans (伏図) when the frame has members under their tags.
  for (const char* preset : {"foundation", "first_floor", "second_floor", "roof"}) {
    const auto framing = draw::framingView(document, preset, "", false, {});
    if (!framing) continue;
    // docs/295: at 1:50 (the member marks crowd at 1:100); not a plan the sections are marked on.
    if (const auto made = draw::makeView(document, *framing)) {
      Planned p{framing->title, std::nullopt, std::nullopt, {}, *made, std::min(spec.scale, 50.0)};
      planned.push_back(std::move(p));
    }
  }
  // docs/290: frame elevations (軸組図) along the outer grid lines of the frame's grid, each seeing only
  // the members within 300 mm of its line (1:50), when the frame has wall-frame members.
  if (draw::framingView(document, "foundation", "", false, {"1階軸組"})) {
    const doc::Object* gridObject = nullptr;
    for (const doc::Object& o : document.objects()) {
      const auto* node = std::get_if<GroupNode>(&o.recipe.base);
      if (node && node->grid && document.isVisible(o.id) && (node->grid->x.size() >= 2 || node->grid->y.size() >= 2)) {
        gridObject = &o;
        break;
      }
    }
    if (gridObject) {
      const auto& grid = *std::get<GroupNode>(gridObject->recipe.base).grid;
      const gp_Trsf w = document.worldTransform(*gridObject);
      constexpr double kHalf = 150.0;
      const auto add = [&](const GridAxis& axis, int family, int outward) {
        // family 0: an X line (along Y), seen from -X (first) or +X (last); family 1: a Y line.
        const gp_Pnt on = (family == 0 ? gp_Pnt(axis.offset, 0, 0) : gp_Pnt(0, axis.offset, 0)).Transformed(w);
        const gp_Dir viewer = (family == 0 ? gp_Dir(outward, 0, 0) : gp_Dir(0, outward, 0)).Transformed(w);
        draw::ViewSpec s = draw::sectionView(on.Translated(gp_Vec(viewer) * kHalf), viewer);
        s.sectionDepth = 2 * kHalf;
        s.tags = {"基礎・土台", "1階床組", "1階軸組", "2階床組", "2階軸組", "小屋組"};
        s.memberLabels = true;
        s.fill = false;
        s.title = axis.name + "通り軸組図";
        // Made here at 1:50 (the member marks crowd at 1:100), outside the sections' cut marks.
        if (const auto made = draw::makeView(document, s)) {
          Planned p{s.title, std::nullopt, std::nullopt, {}, *made, std::min(spec.scale, 50.0)};
          planned.push_back(std::move(p));
        }
      };
      if (grid.x.size() >= 2) {
        add(grid.x.front(), 0, -1);
        add(grid.x.back(), 0, 1);
      }
      if (grid.y.size() >= 2) {
        add(grid.y.front(), 1, -1);
        add(grid.y.back(), 1, 1);
      }
    }
  }
  // docs/292: the interior materials (使用建築材料表) when rooms have finishes.
  if (const auto interior = interiorMaterialTable(document); !interior.rows.empty())
    planned.push_back({interior.title, std::nullopt, interior, {}});
  // docs/314: the 第二種・第三種 materials' areas against the rooms' floor areas.
  if (const auto areas = interiorAreaTable(document); !areas.rows.empty())
    planned.push_back({areas.title, std::nullopt, areas, {}});
  // docs/291: the structural materials of the timber frame.
  if (const auto materials = structuralMaterialTable(document); !materials.rows.empty())
    planned.push_back({materials.title, std::nullopt, materials, {}});
  // docs/308: the 基礎・地盤説明書 when the model has a foundation plan or the survey's results are given.
  if (const auto study = foundationGroundStudy(document); study.hasFoundation || !study.ground.empty()) {
    const auto table = foundationGroundTable(study);
    planned.push_back({table.title, std::nullopt, table, {}});
  }
  // docs/309: the foundation's section detail at 1:20, from the plan's figures.
  if (auto detail = foundationDetailView(document)) {
    const std::string name = detail->spec.title;
    planned.push_back({name, std::nullopt, std::nullopt, {}, std::move(*detail), 20.0});
  }
  // docs/310: the columns' sizes and slenderness (構造詳細図) when the model has timber columns.
  // docs/311: the 構造詳細図's specifications when the model has timber columns or any are given.
  if (!columnRows(document).empty() || !structuralSpecsOf(document).empty()) {
    const auto table = structuralSpecTable(document);
    planned.push_back({table.title, std::nullopt, table, {}});
  }
  if (const auto columns = columnRows(document); !columns.empty()) {
    const auto table = columnTable(columns);
    planned.push_back({table.title, std::nullopt, table, {}});
  }
  detail::AreaGeometry areaGeometry;
  const auto areas = measureAreas(document, &areaGeometry);
  for (auto& page : detail::areaEvidencePages(document, areas, areaGeometry, spec.scale)) {
    const auto name = page.view.spec.title;
    planned.push_back({name, std::nullopt, std::nullopt, std::move(page.tables), std::move(page.view), spec.scale});
    for (auto& table : page.continuation) planned.push_back({table.title, std::nullopt, std::move(table), {}});
  }
  if (spec.heightEvidence) {
    const auto& evidence = *spec.heightEvidence;
    planned.push_back({evidence.section.spec.title, std::nullopt, std::nullopt, {},
                       evidence.section, std::max(spec.scale, evidence.scale)});
    for (const auto& table : evidence.tables)
      planned.push_back({table.title, std::nullopt, table, {}});
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
    if (p.made) {  // docs/233
      sheetSpec.scale = p.scale;
      sheetSpec.paper = "auto";
      auto laid = draw::layoutSheetWithTables(*p.made, p.beside, sheetSpec);
      if (!laid) {
        out.notes.push_back(p.name + ": " + laid.error());
        continue;
      }
      sheets.push_back(std::move(*laid));
    } else if (p.view) {
      draw::ViewSpec hatched = *p.view;
      hatched.hatchScale = hatched.insulation ? 0.0 : spec.scale;  // docs/173 (docs/216: not under the insulation marks)
      hatched.paperScale = spec.scale;  // docs/219
      const auto view = draw::makeView(document, hatched);
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
    tableOnly.push_back(!p.view && !p.made);
  }
  if (sheets.empty()) return R::failure("Nothing to draw: the model is empty");
  // Numbers: the list is 01, the drawings 02, 03, ...
  auto number = [&](std::size_t k) {
    char text[16];
    std::snprintf(text, sizeof text, "%02zu", k);
    return spec.prefix + text;
  };
  // docs/306: with combineViews, the storeys' plans on one sheet and the elevations and sections on
  // another (when they are next to each other in the set and have no tables beside them): which sheet
  // each drawing is on, numbered in order.
  const auto plannedOf = [&](std::size_t i) -> const Planned* {
    for (const Planned& q : planned)
      if (q.name == names[i]) return &q;
    return nullptr;
  };
  const auto groupOf = [&](std::size_t i) {
    const Planned* p = plannedOf(i);
    if (!spec.combineViews || !p || !p->view || p->made || !p->beside.empty()) return 0;
    if (p->view->kind == draw::ViewKind::Plan && p->view->framingPreset.empty()) return 1;
    if (p->view->kind == draw::ViewKind::Elevation || p->view->kind == draw::ViewKind::Section) return 2;
    return 0;
  };
  std::vector<std::size_t> sheetOf(names.size());
  for (std::size_t i = 0, n = 0; i < names.size(); ++i) {
    if (i > 0 && groupOf(i) != 0 && groupOf(i) == groupOf(i - 1)) {
      sheetOf[i] = sheetOf[i - 1];
    } else {
      sheetOf[i] = n++;
    }
  }
  std::vector<std::optional<draw::Sheet>> bare(names.size());
  std::vector<std::optional<gp_Dir2d>> northOf(names.size());
  // The drawings again with their numbers in the title block and with the cut marks.
  for (std::size_t i = 0; i < sheets.size(); ++i) {
    // Lay out again with the number (the title block is drawn in the layout).
    const Planned* p = plannedOf(i);
    draw::SheetSpec sheetSpec;
    sheetSpec.scale = spec.scale;
    sheetSpec.paper = p && p->table ? "auto" : spec.paper;
    sheetSpec.title = spec.title;
    sheetSpec.title.drawing = names[i];
    sheetSpec.title.number = number(sheetOf[i] + 2);
    if (groupOf(i) != 0) {  // laid out without its frame here, put on the shared sheet below
      sheetSpec.frame = false;
      sheetSpec.north = false;
      sheetSpec.paper = "auto";
    }
    Result<draw::Sheet> laid = Result<draw::Sheet>::failure("");
    if (p && p->made) {  // docs/233
      sheetSpec.scale = p->scale;
      sheetSpec.paper = "auto";
      laid = draw::layoutSheetWithTables(*p->made, p->beside, sheetSpec);
    } else if (p && p->view) {
      draw::ViewSpec viewSpec = *p->view;
      viewSpec.hatchScale = viewSpec.insulation ? 0.0 : spec.scale;  // docs/173 (docs/216: not under the insulation marks)
      viewSpec.paperScale = spec.scale;  // docs/219
      // Plans: where the sections are cut, with their sheet numbers.
      if (viewSpec.kind == draw::ViewKind::Plan) {
        for (const Cut& c : cuts)
          for (std::size_t k = 0; k < names.size(); ++k)
            if (names[k] == c.name) viewSpec.cutMarks.push_back({c.point, c.toward, c.label, number(sheetOf[k] + 2)});
        if (spec.heightEvidence) {
          const auto& section = spec.heightEvidence->section.spec;
          for (std::size_t k = 0; k < names.size(); ++k)
            if (names[k] == section.title)
              viewSpec.cutMarks.push_back({section.frame.Location(), section.frame.Direction(), "H", number(sheetOf[k] + 2)});
        }
      }
      const auto view = draw::makeView(document, viewSpec);
      if (view) {
        draw::View named = *view;
        named.spec.title = p->view->title;
        if (viewSpec.kind == draw::ViewKind::Plan) northOf[i] = view->trueNorth;
        // docs/145: a section names the plan sheets it is marked on (where it is cut).
        if (viewSpec.kind == draw::ViewKind::Section) {
          std::string plans;
          for (std::size_t k = 0; k < names.size(); ++k)
            for (const Planned& q : planned)
              if (q.name == names[k] && q.view && q.view->kind == draw::ViewKind::Plan) {
                const std::string n = number(sheetOf[k] + 2);
                if (plans.find(n) == std::string::npos) plans += (plans.empty() ? "" : "・") + n;
              }
          if (!plans.empty()) named.spec.title += "（切る位置：" + plans + "）";
        }
        laid = draw::layoutSheetWithTables(named, p->beside, sheetSpec);
      } else {
        laid = Result<draw::Sheet>::failure(view.error());
      }
    } else if (p) {
      laid = draw::tableSheet(*p->table, sheetSpec);
    }
    if (groupOf(i) != 0) {
      if (laid) bare[i] = std::move(*laid);
      else out.notes.push_back(names[i] + ": " + laid.error());
      continue;
    }
    out.sheets.push_back(laid ? *laid : sheets[i]);
    out.numbers.push_back(number(sheetOf[i] + 2));
    out.names.push_back(names[i]);
  }
  // docs/306: the shared sheets, in their place in the set.
  if (spec.combineViews) {
    DrawingSet merged;
    merged.notes = out.notes;
    std::size_t single = 0;
    std::vector<bool> singleTable;
    for (std::size_t i = 0; i < names.size();) {
      if (groupOf(i) == 0) {
        merged.sheets.push_back(out.sheets[single]);
        merged.numbers.push_back(out.numbers[single]);
        merged.names.push_back(out.names[single]);
        singleTable.push_back(tableOnly[i]);
        ++single;
        ++i;
        continue;
      }
      std::size_t j = i;
      std::vector<draw::Sheet> drawings;
      std::string name;
      for (; j < names.size() && sheetOf[j] == sheetOf[i]; ++j) {
        if (bare[j]) drawings.push_back(*bare[j]);
        name += (name.empty() ? "" : "・") + names[j];
      }
      // The elevations and sections under one short name (each drawing keeps its own caption).
      if (groupOf(i) == 2) name = "立面図・断面図";
      draw::SheetSpec sheetSpec;
      sheetSpec.scale = spec.scale;
      sheetSpec.paper = spec.paper;
      sheetSpec.title = spec.title;
      sheetSpec.title.drawing = name;
      sheetSpec.title.number = number(sheetOf[i] + 2);
      sheetSpec.north = groupOf(i) == 1;
      auto laid = drawings.empty() ? Result<draw::Sheet>::failure("No drawing")
                                   : draw::combineSheets(drawings, sheetSpec, northOf[i]);
      if (laid) {
        for (const std::string& n : laid->notes) merged.notes.push_back(name + ": " + n);
        merged.sheets.push_back(std::move(*laid));
        merged.numbers.push_back(number(sheetOf[i] + 2));
        merged.names.push_back(name);
        singleTable.push_back(false);
      } else {
        merged.notes.push_back(name + ": " + laid.error());
      }
      i = j;
    }
    out = std::move(merged);
    tableOnly = std::move(singleTable);
  }
  // Cut marks and their sheet numbers can change which paper a plan needs.
  // Build the index from the final layouts.
  draw::Table list;
  list.title = "図面リスト";
  list.headers = {"図番", "図面名", "縮尺", "用紙"};
  list.rows.push_back({number(1), list.title, "", ""});
  for (std::size_t i = 0; i < out.sheets.size(); ++i) {
    char scale[32];
    std::snprintf(scale, sizeof scale, "1:%g", out.sheets[i].scale);
    list.rows.push_back({out.numbers[i], out.names[i], tableOnly[i] ? "" : scale, out.sheets[i].paper});
  }
  list.notes = {"モデルから作った図面です。図面を変えた時は、もう一度書き出してください。"};
  draw::SheetSpec listSpec;
  listSpec.title = spec.title;
  listSpec.title.drawing = list.title;
  listSpec.title.number = number(1);
  auto listSheet = draw::tableSheet(list, listSpec);
  if (!listSheet) return R::failure(listSheet.error());
  out.sheets.insert(out.sheets.begin(), std::move(*listSheet));
  out.numbers.insert(out.numbers.begin(), number(1));
  out.names.insert(out.names.begin(), list.title);
  return out;
}

draw::Table structuralMaterialTable(const Document& document) { return materialTableOf(document); }

bool isFloorLevelName(const std::string& name) { return name.size() >= 2 && name.compare(name.size() - 2, 2, "FL") == 0; }

bool isGroundLevelName(const std::string& name) {
  std::string n = name;
  n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (n == "GL") return true;
  // a frame's own ground level ("木造架構 1 GL")
  return name.size() > 3 && name.compare(name.size() - 3, 3, " GL") == 0;
}

bool levelIsStorey(const std::vector<FloorLevel>& levels, const FloorLevel& level) {
  const bool anyFloor = std::any_of(levels.begin(), levels.end(), [](const FloorLevel& l) { return isFloorLevelName(l.name); });
  return !anyFloor || isFloorLevelName(level.name);
}

draw::Table interiorMaterialTable(const Document& document) {
  draw::Table t;
  t.title = "使用建築材料表";
  t.headers = {"室名", "階", "部位", "仕上げ", "ホルムアルデヒドの区分", "表示", "令20条の7", "資料"};
  const auto given = Modeler::interiorMaterialsOf(document);
  int missing = 0, forbidden = 0, limited = 0;
  for (const RoomRow& r : roomSchedule(document).rows) {
    if (r.daylightKind == "none") continue;
    const std::pair<const char*, const std::string*> parts[] = {{"床", &r.floor}, {"壁", &r.wall}, {"天井", &r.ceiling}};
    for (const auto& [part, finish] : parts) {
      if (finish->empty()) continue;
      const Modeler::InteriorMaterial* m = nullptr;
      for (const auto& g : given)
        if (g.name == *finish) m = &g;
      std::string cls = "未入力", rule = "区分を入れて照合";
      if (!m) {
        ++missing;
      } else if (m->cls == "first") {
        cls = "第一種";
        rule = "使用できない（第1項第1号）";
        ++forbidden;
      } else if (m->cls == "second" || m->cls == "third") {
        cls = m->cls == "second" ? "第二種" : "第三種";
        rule = "使用面積の制限（第1項第2号）。使用面積の表で照合";
        ++limited;
      } else {
        cls = "規制対象外";
        rule = "制限なし";
      }
      t.rows.push_back({r.name, r.level, part, *finish, cls, m ? m->mark : "", rule, m ? m->source : ""});
    }
  }
  if (t.rows.empty()) return t;
  t.notes = {"居室（居室でないと入れた室を除く）の床・壁・天井の仕上げと、材料ごとに入れたホルムアルデヒドの区分です。",
             "根拠：建築基準法施行令 第20条の7（e-Gov で 2026-11-26 に確認）。建具の室内側も内装に含まれるが、建具の材料は未入力。"};
  if (missing > 0) t.notes.push_back("区分が未入力の行が " + std::to_string(missing) + " 行あります（set_interior_materials）。");
  if (forbidden > 0) t.notes.push_back("第一種の材料を使う行が " + std::to_string(forbidden) + " 行あります。居室の内装には使えません。");
  if (limited > 0)
    t.notes.push_back("第二種・第三種の材料の面積は「ホルムアルデヒド発散建築材料の使用面積」の表で照合します。");
  return t;
}

// docs/314: 令第20条の7第1項第2号 per living room: the areas finished with 第二種 / 第三種 materials times the
// table's figures (by 住宅等の居室 or not and the air changes n = V / (A h)) against the room's floor area.
// Floor and ceiling: the inner floor area; walls: the inner perimeter times the ceiling height, openings not
// taken off (on the safe side). Read on e-Gov 2026-11-27.
draw::Table interiorAreaTable(const Document& document) {
  draw::Table t;
  t.title = "ホルムアルデヒド発散建築材料の使用面積";
  t.headers = {"室名", "階", "居室の区分", "床面積 (m²)", "換気回数 (回/h)", "第二種の面積 (m²)", "第三種の面積 (m²)",
               "N2·S2＋N3·S3 (m²)", "令20条の7第1項第2号"};
  const auto given = Modeler::interiorMaterialsOf(document);
  const auto classOf = [&](const std::string& finish) {
    for (const auto& g : given)
      if (g.name == finish) return g.cls;
    return std::string();
  };
  const auto rooms = roomSchedule(document).rows;
  // A shared ventilation unit: its airflow over the rooms it serves (Σ A h).
  std::map<std::string, double> unitVolume;
  for (const RoomRow& r : rooms)
    if (!r.fanUnit.empty() && r.innerKnown && r.ceilingHeight > 0)
      unitVolume[r.fanUnit] += r.innerArea / 1e6 * r.ceilingHeight / 1000.0;
  int over = 0, open = 0;
  for (const RoomRow& r : rooms) {
    if (r.daylightKind == "none") continue;
    const std::string f = classOf(r.floor), w = classOf(r.wall), c = classOf(r.ceiling);
    const auto limited = [](const std::string& k) { return k == "second" || k == "third"; };
    if (!limited(f) && !limited(w) && !limited(c)) continue;
    const bool dwelling = r.daylightKind == "dwelling";
    char a[32], nText[32], s2Text[32], s3Text[32], sum[32];
    if (!r.innerKnown || !(r.ceilingHeight > 0)) {
      ++open;
      t.rows.push_back({r.name, r.level, dwelling ? "住宅等の居室" : "住宅等の居室以外", "", "", "", "", "",
                        "未評価（内法の面積・天井高が要る）"});
      continue;
    }
    const double area = r.innerArea / 1e6, h = r.ceilingHeight / 1000.0;
    const double wallArea = r.innerPerimeter / 1000.0 * h;
    double s2 = 0, s3 = 0;
    for (const auto& [k, s] : {std::pair<std::string, double>{f, area}, {w, wallArea}, {c, area}}) {
      if (k == "second") s2 += s;
      if (k == "third") s3 += s;
    }
    std::optional<double> n;
    if (r.fanM3h > 0) {
      const double volume = !r.fanUnit.empty() && unitVolume.count(r.fanUnit) ? unitVolume.at(r.fanUnit) : area * h;
      if (volume > 0) n = r.fanM3h / volume;
    }
    double n2, n3;
    if (dwelling) {
      const bool high = n && *n >= 0.7;
      n2 = high ? 1.2 : 2.8;
      n3 = high ? 0.20 : 0.50;
    } else {
      const bool high = n && *n >= 0.7, mid = n && *n >= 0.5 && *n < 0.7;
      n2 = high ? 0.88 : mid ? 1.4 : 3.0;
      n3 = high ? 0.15 : mid ? 0.25 : 0.50;
    }
    const double total = n2 * s2 + n3 * s3;
    std::snprintf(a, sizeof a, "%.2f", area);
    if (n) std::snprintf(nText, sizeof nText, "%.2f", *n);
    else std::snprintf(nText, sizeof nText, "未入力（その他）");
    std::snprintf(s2Text, sizeof s2Text, "%.2f", s2);
    std::snprintf(s3Text, sizeof s3Text, "%.2f", s3);
    std::snprintf(sum, sizeof sum, "%.2f×%.2f＋%.2f×%.2f＝%.2f", n2, s2, n3, s3, total);
    const bool ok = total <= area + 1e-9;
    over += !ok;
    t.rows.push_back({r.name, r.level, dwelling ? "住宅等の居室" : "住宅等の居室以外", a, nText, s2Text, s3Text, sum,
                      ok ? "規定内" : "規定外"});
  }
  if (t.rows.empty()) return t;
  t.notes = {
      "第二種・第三種の材料を使う居室ごとに、その面積に令第20条の7第1項第2号の表の数値を掛けた合計と床面積を比べました。",
      "床・天井は内法の床面積、壁は内法の周長×天井高（開口部は差し引いていないので、大きめの値）。建具の室内側は未入力のため含みません。",
      "換気回数は 機械換気の有効換気量 ÷（床面積×天井高）。同じ換気設備の室はまとめて計算。換気量が無い室は「その他の居室」の数値を使います。",
      "根拠：建築基準法施行令 第20条の7（e-Gov で 2026-11-27 に確認）。",
  };
  if (over > 0) t.notes.push_back("床面積を超える居室が " + std::to_string(over) + " 室あります。");
  if (open > 0) t.notes.push_back("内法の面積・天井高が無いため照合できない居室が " + std::to_string(open) + " 室あります。");
  return t;
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
  csv.line({"容積率の算定の延べ面積", "", "", "", "", "", "", "", "", "", "", table.ratioKnown ? number(table.ratioTotal / 1e6, 2) : "未評価", ""});
  for (const AreaTable::RatioPart& part : table.ratioParts)
    csv.line({"  入れない部分：" + part.label, "", "", "", std::to_string(part.rooms), "", number(part.area / 1e6, 2), "", "", "", "",
              "-" + number(part.excluded / 1e6, 2), ""});
  csv.line({});
  csv.line({"注", "モデルから測った面積です。法の床面積・延べ面積・建築面積は、下の決まりで出しました（確認申請に使う時は、特定行政庁・確認検査機関の取扱いを確かめてください）。"});
  for (const std::string& n : table.legalNotes) csv.line({"", n});
  return csv.out.str();
}

}  // namespace sbr::doc
