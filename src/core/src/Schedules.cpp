#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <tuple>

#include <nlohmann/json.hpp>

#include "sbr/OpeningSymbols.h"
#include "sbr/doc/Modeler.h"
#include "sbr/geom/Measure.h"

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
  for (const draw::FittingChoice& c : draw::fittingChoices()) {
    const nlohmann::json choice = nlohmann::json::parse(c.fitting, nullptr, false);
    if (choice.is_discarded()) continue;
    bool same = true;
    for (const auto& [key, value] : choice.items()) same &= fitting.contains(key) && fitting.at(key) == value;
    if (same) return c.label;
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
    out.rows.push_back(std::move(row));
  }
  AreaRow loose;  // rooms whose marker is at no level's height
  addRooms(loose, "");
  if (loose.rooms > 0) out.rows.push_back(std::move(loose));
  for (const AreaRow& r : out.rows) {
    out.enclosedTotal += r.enclosedArea;
    out.roomCentreTotal += r.roomCentre;
    out.slabTotal += r.slabArea;
  }
  return out;
}

std::string roomTagText(const RoomRow& room) {
  if (!room.closed) return room.name + "（閉じていない）";
  if (!room.counted) return room.name;
  return room.name + "  " + number(room.centreArea / 1e6, 2) + " m²";
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
  csv.line({"室名", "階", "壁芯(m2)", "内法(m2)", "内法の周長(m)", "状態"});
  for (const RoomRow& r : schedule.rows) {
    csv.line({r.name, r.level, r.closed ? number(r.centreArea / 1e6, 2) : "",
              r.closed ? (r.innerKnown ? number(r.innerArea / 1e6, 2) : "未評価") : "",
              r.closed && r.innerKnown ? number(r.innerPerimeter / 1e3, 2) : "",
              r.counted ? "閉じている" : !r.closed ? "閉じていない" : "ほかの室と同じ場所"});
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
              std::to_string(r.unreadableWalls), "未評価", "未評価"});
  }
  csv.line({"合計", "", number(table.enclosedTotal / 1e6, 2), "", "", "", number(table.roomCentreTotal / 1e6, 2), "",
            number(table.slabTotal / 1e6, 2), "", "", "未評価", "未評価"});
  csv.line({});
  csv.line({"注", "モデルから測った面積です。法の床面積・建築面積（算入・不算入の決まり、吹抜け・階段・庇などの扱い）は見ていないので未評価です。"});
  return csv.out.str();
}

}  // namespace sbr::doc
