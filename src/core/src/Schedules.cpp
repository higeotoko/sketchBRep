#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
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

#include "sbr/ConstructionWall.h"
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
      // docs/176: inner walls (an inner build-up) give WD / WW; outer ones and walls without one AD / AW.
      row.inner = attribute(o, "wall_type").rfind("interior", 0) == 0;
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
    }
    out.rooms.push_back(std::move(row));
  }
  for (std::size_t i = 0; i < matched.size(); ++i)
    if (!matched[i]) out.unmatched.push_back(i);
  return out;
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
  // docs/166: the ground (地盤面) is the level named GL; the top of a storey the next level up.
  auto isGround = [](const FloorLevel& l) {
    std::string name = l.name;
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return name == "GL";
  };
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
  if (document.settings().siteBoundary) {
    const auto site = draw::sitePlanView();
    planned.insert(planned.begin(), {site.title, site, std::nullopt, {}});
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
      draw::ViewSpec hatched = *p.view;
      hatched.hatchScale = spec.scale;  // docs/173
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
      viewSpec.hatchScale = spec.scale;  // docs/173
      // Plans: where the sections are cut, with their sheet numbers.
      if (viewSpec.kind == draw::ViewKind::Plan)
        for (const Cut& c : cuts)
          for (std::size_t k = 0; k < names.size(); ++k)
            if (names[k] == c.name) viewSpec.cutMarks.push_back({c.point, c.toward, c.label, number(k + 2)});
      const auto view = draw::makeView(document, viewSpec);
      if (view) {
        draw::View named = *view;
        named.spec.title = p->view->title;
        // docs/145: a section names the plan sheets it is marked on (where it is cut).
        if (viewSpec.kind == draw::ViewKind::Section) {
          std::string plans;
          for (std::size_t k = 0; k < names.size(); ++k)
            for (const Planned& q : planned)
              if (q.name == names[k] && q.view && q.view->kind == draw::ViewKind::Plan)
                plans += (plans.empty() ? "" : "・") + number(k + 2);
          if (!plans.empty()) named.spec.title += "（切る位置：" + plans + "）";
        }
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
