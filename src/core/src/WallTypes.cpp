#include "sbr/WallTypes.h"

#include <cmath>

#include <nlohmann/json.hpp>

namespace sbr::construction {
namespace {
const WallLayer kCore{"柱・間柱", 105.0, "core"};
const WallLayer kBoard{"石膏ボード", 12.5, "finish"};
const WallLayer kPly{"構造用合板", 9.0, "other"};
const WallLayer kBatten{"通気胴縁（通気層）", 18.0, "other"};
}  // namespace

double WallType::core() const {
  for (const WallLayer& l : layers)
    if (l.role == "core") return l.thickness;
  return 0.0;
}

double WallType::outside() const {
  double d = 0.0;
  for (const WallLayer& l : layers) {
    if (l.role == "core") return d + l.thickness / 2;
    d += l.thickness;
  }
  return d;
}

double WallType::inside() const {
  double d = 0.0;
  for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
    if (it->role == "core") return d + it->thickness / 2;
    d += it->thickness;
  }
  return d;
}

std::string WallType::layersJson() const {
  nlohmann::json out = nlohmann::json::array();
  for (const WallLayer& l : layers) out.push_back({{"name", l.name}, {"thickness", l.thickness}, {"role", l.role}});
  return out.dump();
}

const std::vector<WallType>& wallTypes() {
  static const std::vector<WallType> types = {
      {"exterior_siding", "外壁 窯業系サイディング（通気）", true,
       {{"窯業系サイディング", 16.0, "finish"}, kBatten, kPly, kCore, kBoard}},
      {"exterior_metal", "外壁 金属サイディング（通気）", true,
       {{"金属サイディング", 15.0, "finish"}, kBatten, kPly, kCore, kBoard}},
      {"exterior_lath_mortar", "外壁 ラスモルタル（通気）", true,
       {{"モルタル（ラス・防水紙）", 20.0, "finish"}, {"ラス下地板", 12.0, "other"}, kBatten, kPly, kCore, kBoard}},
      {"exterior_lath_mortar_direct", "外壁 ラスモルタル（直張り）", true,
       {{"モルタル（ラス・防水紙）", 20.0, "finish"}, kPly, kCore, kBoard}},
      {"interior_board", "内壁 石膏ボード両面", false, {kBoard, kCore, kBoard}},
  };
  return types;
}

Result<WallType> wallType(const std::string& id, double core) {
  if (!std::isfinite(core) || core < 30 || core > 300) return Result<WallType>::failure("Core width must be 30 to 300 mm");
  for (WallType t : wallTypes()) {
    if (t.id != id) continue;
    for (WallLayer& l : t.layers)
      if (l.role == "core") l.thickness = core;
    return t;
  }
  return Result<WallType>::failure("Unknown wall type: " + id);
}
}  // namespace sbr::construction
