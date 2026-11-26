#include "sbr/doc/ArchitectCheck.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <set>

#include <Bnd_Box.hxx>

#include "sbr/doc/Document.h"
#include "sbr/doc/GroundLevel.h"
#include "sbr/doc/Schedules.h"

namespace sbr::doc {

namespace {
bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }
int rank(const std::string& level) {
  if (level == "first") return 3;
  if (level == "second") return 2;
  if (level == "wood") return 1;
  return 0;
}
}  // namespace

const char* requiredLabel(const std::string& required) {
  if (required == "first") return "一級建築士";
  if (required == "second") return "一級建築士又は二級建築士";
  if (required == "wood") return "一級建築士、二級建築士又は木造建築士";
  if (required == "none") return "建築士法第3条〜第3条の3の対象外";
  return "未評価";
}

bool gradeMeets(const std::string& grade, const std::string& required) {
  if (required == "none") return true;
  if (required == "unknown") return false;
  return rank(grade) >= rank(required);
}

ArchitectCheck architectCheck(const Document& document) {
  ArchitectCheck out;
  const auto outline = projectOutline(document);
  const auto get = [&](const char* key) {
    const auto it = outline.find(key);
    return it == outline.end() ? std::string() : it->second;
  };
  const std::string structure = get("structure"), use = get("main_use"), work = get("work_kind");
  std::vector<std::string> missing;
  if (structure.empty()) missing.push_back("構造");
  if (use.empty()) missing.push_back("主要用途");
  // The structure: timber, the 第3条第1項第3号 kinds, or both (a building with parts of each is left to the person).
  const bool masonry = has(structure, "鉄筋コンクリート") || has(structure, "鉄骨") || has(structure, "石造") ||
                       has(structure, "れんが") || has(structure, "コンクリートブロック") || has(structure, "無筋コンクリート") ||
                       has(structure, "RC") || has(structure, "S造");
  const bool timber = has(structure, "木造");
  const bool special = has(use, "学校") || has(use, "病院") || has(use, "劇場") || has(use, "映画館") || has(use, "観覧場") ||
                       has(use, "公会堂") || has(use, "集会場") || has(use, "百貨店");
  // Floor area (延べ面積), storeys and height from the model.
  const AreaTable areas = areaTable(document);
  const double area = areas.legalTotal / 1e6;  // m²
  if (!(area > 0)) missing.push_back("延べ面積（モデルの床面積）");
  std::optional<double> ground;
  std::string groundName;
  const auto study = groundLevelStudy(document);
  if (study.state == "算出" && study.levelZ) ground = *study.levelZ, groundName = "平均地盤面";
  else
    for (const FloorLevel& l : document.settings().levels)
      if (isGroundLevelName(l.name)) ground = l.elevation, groundName = l.name;
  std::set<long long> storeys, above;
  for (const FloorLevel& l : document.settings().levels)
    if (levelIsStorey(document.settings().levels, l)) {
      storeys.insert(std::llround(l.elevation));
      if (!ground || l.elevation >= *ground - 1.0) above.insert(std::llround(l.elevation));
    }
  double top = -1e300;
  for (const Object& o : document.objects()) {
    if (o.shape.IsNull() || !document.isVisible(o.id)) continue;
    const auto kind = o.attributes.find("kind");
    if (kind == o.attributes.end()) continue;
    const std::string& k = kind->second;
    if (k != "wall" && k != "roof" && k != "slab" && k != "balcony" && k != "canopy") continue;
    const Bnd_Box b = document.worldBounds(o);
    if (b.IsVoid()) continue;
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    top = std::max(top, z1);
  }
  std::optional<double> height;
  if (ground && top > *ground) height = top - *ground;
  if (storeys.empty()) missing.push_back("階（…FL のレベル）");
  const int all = static_cast<int>(storeys.size()), aboveGround = static_cast<int>(above.size());
  char basis[256];
  std::snprintf(basis, sizeof basis, "延べ面積 %.2f m²、階数 %d（地階を除く %d）、高さ %s、構造 %s、主要用途 %s", area, all,
                aboveGround, height ? (std::to_string(static_cast<long long>(std::llround(*height))) + " mm（" + groundName + "から）").c_str() : "未評価",
                structure.empty() ? "未入力" : structure.c_str(), use.empty() ? "未入力" : use.c_str());
  out.basis = basis;
  if (!missing.empty()) {
    for (const auto& m : missing) out.problem += (out.problem.empty() ? "" : "・") + m;
    out.problem += " が未入力です";
    return out;
  }
  if (timber && masonry) {
    out.problem = "木造と鉄筋コンクリート造等が混ざる構造です。部分ごとの延べ面積・高さで人が確かめてください";
    return out;
  }
  const bool tall = height && *height > 16000.0;
  if (!height && (timber || masonry) && aboveGround >= 3) {
    out.problem = "地盤面（GL）が無いため高さを測れません";
    return out;
  }
  if (special && area > 500.0) out.required = "first", out.article = "建築士法第3条第1項第1号";
  else if (timber && (tall || aboveGround >= 4)) out.required = "first", out.article = "建築士法第3条第1項第2号";
  else if (masonry && (area > 300.0 || tall || aboveGround >= 4)) out.required = "first", out.article = "建築士法第3条第1項第3号";
  else if (area > 1000.0 && all >= 2) out.required = "first", out.article = "建築士法第3条第1項第4号";
  else if (masonry && area > 30.0) out.required = "second", out.article = "建築士法第3条の2第1項第1号";
  else if (area > (timber ? 300.0 : 100.0) || all >= 3) out.required = "second", out.article = "建築士法第3条の2第1項第2号";
  else if (timber && area > 100.0) out.required = "wood", out.article = "建築士法第3条の3第1項";
  else out.required = "none", out.article = "建築士法第3条〜第3条の3";
  if (!work.empty() && work != "new")
    out.problem = "新築以外は、増築等に係る部分を新築とみなして当てはめます（第3条第2項）。その部分の面積で確かめてください";
  return out;
}

}  // namespace sbr::doc
