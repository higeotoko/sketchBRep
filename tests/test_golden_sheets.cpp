// docs/249: the drawing set of a fixed house compared with the one kept in tests/data
// (golden_sheets.json): every sheet's paper, how many lines / arcs / fills / texts per pen, the
// extent, and every word. A change to the drawings shows here as what changed, sheet by sheet; when
// it is meant, the kept one is written again with SBR_UPDATE_GOLDEN=1 and the change is committed
// (the diff of the JSON is the review of what the drawings now say).
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/doc/Modeler.h"
#include "sbr/doc/Schedules.h"

using nlohmann::json;

namespace {

const char* kKindNames[] = {"line", "arc", "polyline", "text", "fill", "image"};
const char* kPenNames[] = {"cut", "visible", "hidden", "fill", "grid", "grid_label", "level", "dimension", "text", "frame",
                           "title_block", "height_limit", "height_review", "fire_limit_3", "fire_limit_5",
                           "fire_compartment", "boundary", "ground"};
// Every pen has a name here (docs/297: a pen added without one read past the end).
static_assert(sizeof(kPenNames) / sizeof(kPenNames[0]) == sbr::draw::kPenCount);

// What a sheet says, in a form that does not change from one computer to another: counts, the
// extent to 10 mm on paper, and the words (the title block's date, the day's, left out).
json describeSheet(const sbr::draw::Sheet& sheet) {
  std::map<std::string, int> counts;
  std::multiset<std::string> words;
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  const double scale = sheet.scale > 0 ? sheet.scale : 1.0;
  for (const auto& item : sheet.items) {
    ++counts[std::string(kKindNames[static_cast<int>(item.kind)]) + "/" + kPenNames[static_cast<int>(item.pen)]];
    if (item.kind == sbr::draw::Item::Kind::Text) {
      const std::string& t = item.text;
      const bool date = t.size() == 10 && t[4] == '-' && t[7] == '-';
      if (!date) words.insert(t);
    }
    for (const auto& p : item.points) {
      x0 = std::min(x0, p.X() / scale);
      y0 = std::min(y0, p.Y() / scale);
      x1 = std::max(x1, p.X() / scale);
      y1 = std::max(y1, p.Y() / scale);
    }
  }
  const auto r10 = [](double v) { return std::round(v / 10.0) * 10.0; };
  return {{"paper", sheet.paper},
          {"scale", sheet.scale},
          {"counts", counts},
          {"extent", {r10(x0), r10(y0), r10(x1), r10(y1)}},
          {"words", std::vector<std::string>(words.begin(), words.end())},
          {"notes", sheet.notes}};
}

json describeSet() {
  sbr::doc::Modeler modeler;
  sbr::api::CommandApi api{modeler};
  const auto ok = [&](const std::string& command, json args) {
    const json r = json::parse(api.execute(json{{"command", command}, {"args", args}}.dump()));
    EXPECT_TRUE(r.value("ok", false)) << command << ": " << r.dump();
    return r.value("result", json());
  };
  // The house of the hands-on scenarios (tools/handson/scenarios.json "house"), with the energy inputs.
  ok("set_levels", {{"levels", {{{"name", "1FL"}, {"elevation", 0}}, {{"name", "2FL"}, {"elevation", 3000}}}}});
  const json walls = ok("create_wall", {{"points", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}, {0, 0}}}, {"thickness", 200}, {"height", 3000}})["ids"];
  ok("create_wall", {{"points", {{4000, 0}, {4000, 6000}}}, {"thickness", 100}, {"height", 3000}});
  ok("place_openings", {{"host", walls[0]}, {"face_normal", {0, -1, 0}}, {"count", 2}, {"width", 1600}, {"height", 1200}, {"sill", 900}});
  ok("set_opening_fittings", {{"host", walls[0]}, {"fitting", {{"type", "sliding_double"}}}});
  ok("place_openings", {{"host", walls[1]}, {"face_normal", {1, 0, 0}}, {"count", 1}, {"width", 900}, {"height", 2100}, {"sill", 0}});
  ok("set_opening_fittings", {{"host", walls[1]}, {"fitting", {{"type", "swing_single"}, {"hinge", "left"}, {"swing", "in"}}}});
  const std::string slab = ok("create_slab", {{"outline", {{0, 0}, {8000, 0}, {8000, 6000}, {0, 6000}}}, {"top", 0}, {"thickness", 200}})["id"];
  const std::string living = ok("create_room", {{"at", {2000, 3000}}, {"name", "居間"}})["id"];
  ok("create_room", {{"at", {6000, 3000}}, {"name", "洋室"}});
  const std::string site = ok("create_polygon", {{"points", {{-2000, -2000, 0}, {14000, -2000, 0}, {14000, 10000, 0}, {-2000, 10000, 0}}}})["id"];
  ok("set_site_boundary", {{"outline", site}, {"source", "確認用"}});
  ok("set_energy_route", {{"route", "spec"}, {"region", 6}, {"source", "確認用"}});
  ok("set_insulation", {{"ids", walls}, {"material", "グラスウール 16K"}, {"thickness_mm", 105}, {"conductivity", 0.038}, {"placement", "fill"}, {"source", "確認用"}});
  ok("set_insulation", {{"ids", {slab}}, {"material", "押出法ポリスチレンフォーム 3種"}, {"thickness_mm", 90}, {"conductivity", 0.028}, {"placement", "fill"}, {"source", "確認用"}});
  ok("set_opening_performance", {{"host", walls[0]}, {"u_value", 2.33}, {"glass", "Low-E 複層"}, {"solar_gain", 0.51}, {"source", "確認用"}});
  ok("set_room_finish", {{"id", living}, {"ceiling_height", 2400}});
  ok("make_ceilings", {{"rooms", {living}}});
  ok("set_equipment", {{"items", {{{"kind", "heating_cooling"}, {"name", "ルームエアコン"}, {"count", 2}}, {{"kind", "hot_water"}, {"name", "電気ヒートポンプ給湯機"}}}}});
  ok("set_project_outline", {{"items", {{"project_name", "確認用の家"}, {"client", "確認 太郎"}}}});
  const auto set = sbr::doc::drawingSet(modeler.document(), sbr::doc::DrawingSetSpec{});
  EXPECT_TRUE(set) << (set ? "" : set.error());
  json out = json::array();
  if (!set) return out;
  for (std::size_t i = 0; i < set->sheets.size(); ++i) {
    json sheet = describeSheet(set->sheets[i]);
    sheet["number"] = set->numbers[i];
    sheet["name"] = set->names[i];
    out.push_back(sheet);
  }
  return out;
}

std::string goldenPath() { return std::string(SBR_TEST_DATA_DIR) + "/golden_sheets.json"; }

}  // namespace

TEST(GoldenSheets, TheDrawingSetSaysWhatItSaidBefore) {
  const json now = describeSet();
  ASSERT_FALSE(now.empty());
  const char* update = std::getenv("SBR_UPDATE_GOLDEN");
  std::ifstream in(goldenPath(), std::ios::binary);
  if ((update && *update) || !in) {
    std::ofstream(goldenPath(), std::ios::binary) << now.dump(1) << "\n";
    if (!(update && *update)) FAIL() << "no " << goldenPath() << ": written now; commit it";
    return;
  }
  const json before = json::parse(in, nullptr, false);
  ASSERT_TRUE(before.is_array()) << "golden_sheets.json is not readable";
  std::ostringstream changes;
  const std::size_t n = std::max(before.size(), now.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (i >= before.size()) { changes << "sheet added: " << now[i]["number"] << " " << now[i]["name"] << "\n"; continue; }
    if (i >= now.size()) { changes << "sheet gone: " << before[i]["number"] << " " << before[i]["name"] << "\n"; continue; }
    const json& a = before[i];
    const json& b = now[i];
    const std::string where = b["number"].get<std::string>() + " " + b["name"].get<std::string>();
    for (const char* key : {"name", "paper", "scale", "extent", "notes"})
      if (a[key] != b[key]) changes << where << ": " << key << " " << a[key].dump() << " -> " << b[key].dump() << "\n";
    if (a["counts"] != b["counts"]) changes << where << ": counts " << a["counts"].dump() << " -> " << b["counts"].dump() << "\n";
    if (a["words"] != b["words"]) {
      std::multiset<std::string> wa(a["words"].begin(), a["words"].end()), wb(b["words"].begin(), b["words"].end());
      for (const auto& w : wa) if (!wb.count(w)) changes << where << ": words gone  「" << w << "」\n";
      for (const auto& w : wb) if (!wa.count(w)) changes << where << ": words added 「" << w << "」\n";
    }
  }
  EXPECT_TRUE(changes.str().empty())
      << "The drawing set changed (docs/249). If it is meant, run with SBR_UPDATE_GOLDEN=1 and commit tests/data/golden_sheets.json:\n"
      << changes.str();
}
