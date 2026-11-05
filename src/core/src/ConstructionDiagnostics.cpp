#include "sbr/ConstructionDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "sbr/ConstructionCheck.h"
#include "sbr/ConstructionFoundation.h"
#include "sbr/ConstructionWall.h"

namespace sbr::construction {
namespace {
using json = nlohmann::json;
constexpr const char* kVersion = "structural-diagnostics-1";

// The storey of a wall designation ("1", "2", ...) drawn at the lowest level whose name
// ends with the word "<n>FL" (the timber recipe names them "<frame> 1FL"). Without one the wall is drawn at Z=0
// and the result says so; it is a display position, not a structural input.
std::pair<double, bool> storeyElevation(const doc::Document& d, const std::string& floor) {
  const std::string token = floor + "FL";
  bool found = false;
  double z = 0;
  for (const auto& level : d.settings().levels) {
    const auto& n = level.name;
    const auto at = n.rfind(token);
    const bool starts = at == 0 || (at != std::string::npos && n[at - 1] == ' ');
    if (!starts || at + token.size() != n.size()) continue;
    if (!found || level.elevation < z) z = level.elevation;
    found = true;
  }
  return {z, found};
}
json point(double x, double y, double z) { return json::array({x, y, z}); }
std::string supportMessage(const std::string& status, const std::string& reason) {
  if (!reason.empty()) return reason;
  if (status == "missing") return "下に支持する面が見つかりません";
  if (status == "gap") return "支持面との間に隙間があります";
  if (status == "interference") return "支持面に食い込んでいます";
  if (status == "partial") return "下面の一部だけが支持面に載っています";
  if (status == "ambiguous") return "支持の候補が複数あります";
  return "支持経路を評価していません";
}
}  // namespace

Result<std::string> structuralDiagnostics(const doc::Document& d, const std::string& options) {
  try {
    const json opt = json::parse(options);
    if (!opt.is_object()) throw std::runtime_error("options must be an object");
    for (const auto& [key, unused] : opt.items())
      if (key != "frame_rules") throw std::runtime_error("unknown option: " + key);
    json out = {{"version", kVersion},
                {"sequence", d.sequence()},
                {"verified", false},
                {"notice", "診断の表示です。モデルは変えません。「合格」は計算した項目だけの結果で、"
                           "建物全体の安全・法令への適合・施工できることの判定ではありません。"},
                {"wall_quantity", json::object()},
                {"walls", json::array()},
                {"markers", json::array()},
                {"counts", json::object()}};

    // 1. Wall quantity: rows and the designated walls with a drawing position.
    bool entered = false;
    if (!d.settings().wallCalculation.empty()) {
      const json stored = json::parse(d.settings().wallCalculation, nullptr, false);
      entered = stored.is_object() && (!stored.value("floors", json::array()).empty() || !stored.value("walls", json::array()).empty());
    }
    json wall = {{"entered", entered}, {"rows", json::array()}};
    if (entered) {
      const auto report = checkWallQuantity(d);
      if (!report) {
        wall["error"] = report.error();
      } else {
        const auto r = json::parse(*report);
        wall["rows"] = r.at("rows");
        wall["notice"] = r.at("notice");
        wall["as_of"] = r.at("as_of");
        wall["stale_table"] = r.at("stale_table");
        wall["unassessed"] = r.at("unassessed");
        std::map<std::pair<std::string, std::string>, std::string> rowStatus;
        for (const auto& row : r.at("rows"))
          rowStatus[{row.value("floor", std::string()), row.value("direction", std::string())}] =
              row.value("status", std::string("not_evaluated"));
        std::set<std::string> unplaced;
        for (const auto& w : r.at("walls")) {
          if (!w.contains("start") || !w.contains("end") || !w["start"].is_array() || !w["end"].is_array()) continue;
          const std::string floor = w.value("floor", std::string()), direction = w.value("direction", std::string());
          const auto [z, placed] = storeyElevation(d, floor);
          if (!placed) unplaced.insert(floor);
          const auto it = rowStatus.find({floor, direction});
          out["walls"].push_back({{"id", w.value("id", std::string())},
                                  {"floor", floor},
                                  {"direction", direction},
                                  {"wall_status", w.value("status", std::string("not_evaluated"))},
                                  {"row_status", it == rowStatus.end() ? std::string("not_evaluated") : it->second},
                                  {"start", point(w["start"][0].get<double>(), w["start"][1].get<double>(), z)},
                                  {"end", point(w["end"][0].get<double>(), w["end"][1].get<double>(), z)},
                                  {"elevation_from_level", placed}});
        }
        if (!unplaced.empty())
          wall["display_note"] = "階のレベル（nFL）が見つからない壁は Z=0 に表示しています";
      }
    }
    out["wall_quantity"] = wall;

    // 2. Frame check: fail / not evaluated rows become markers on their targets.
    json frameOptions = json::object();
    if (opt.contains("frame_rules")) frameOptions["rules"] = opt.at("frame_rules");
    int frameFail = 0, frameOpen = 0;
    if (const auto frame = checkFrame(d, frameOptions.dump())) {
      // Keep the parsed report alive for the loop (a temporary's member would dangle).
      const json report = json::parse(*frame);
      for (const auto& row : report.at("results")) {
        const std::string status = row.value("status", std::string());
        if (status != "fail" && status != "not_evaluated") continue;
        (status == "fail" ? frameFail : frameOpen)++;
        out["markers"].push_back({{"source", "frame_check"},
                                  {"status", status},
                                  {"rule", row.value("rule", std::string())},
                                  {"targets", row.value("targets", json::array())},
                                  {"message", row.value("message", std::string())}});
      }
    } else {
      out["frame_check_error"] = frame.error();
    }

    // 3. Support paths: members without a detected support below.
    int supportOpen = 0;
    if (const auto supports = foundationSupports(d)) {
      const json report = json::parse(*supports);
      for (const auto& row : report.at("rows")) {
        const std::string status = row.value("status", std::string());
        if (status == "detected") continue;
        ++supportOpen;
        out["markers"].push_back({{"source", "foundation_supports"},
                                  {"status", status},
                                  {"rule", row.value("role", std::string())},
                                  {"targets", json::array({row.value("id", std::string())})},
                                  {"message", supportMessage(status, row.value("reason", std::string()))}});
      }
    } else {
      out["foundation_supports_error"] = supports.error();
    }
    out["counts"] = {{"frame_fail", frameFail}, {"frame_not_evaluated", frameOpen}, {"support_issues", supportOpen},
                     {"walls", out["walls"].size()}};
    // Checks this app does not compute yet: shown as unassessed, never as passing.
    out["unassessed"] = {"四分割法（壁の配置のつり合い）", "N値（柱頭・柱脚の接合）", "柱の小径・直下率",
                         "横架材の断面", "地盤・基礎の構造計算"};
    return out.dump();
  } catch (const std::exception& e) {
    return Result<std::string>::failure(e.what());
  }
}
}  // namespace sbr::construction
