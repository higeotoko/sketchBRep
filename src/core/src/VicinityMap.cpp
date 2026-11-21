#include "sbr/doc/VicinityMap.h"

#include <cmath>

#include <nlohmann/json.hpp>

#include "sbr/doc/Document.h"

namespace sbr::doc {

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

const std::vector<VicinityLayer>& vicinityLayers() {
  // docs/233: as listed on 国土地理院「地理院タイル一覧」 (read 2026-11-21).
  static const std::vector<VicinityLayer> layers = {
      {"std", "標準地図", "png"}, {"pale", "淡色地図", "png"}, {"seamlessphoto", "写真", "jpg"}};
  return layers;
}

std::optional<VicinityMap> vicinityMapOf(const Document& document) {
  const nlohmann::json j = nlohmann::json::parse(document.settings().vicinityMap, nullptr, false);
  if (!j.is_object() || !j.contains("lat") || !j.contains("lon")) return std::nullopt;
  VicinityMap map;
  map.lat = j.value("lat", 0.0);
  map.lon = j.value("lon", 0.0);
  map.zoom = j.value("zoom", 16);
  map.layer = j.value("layer", std::string("std"));
  if (!checkVicinityMap(map)) return std::nullopt;
  return map;
}

Status checkVicinityMap(const VicinityMap& map) {
  if (!std::isfinite(map.lat) || !std::isfinite(map.lon) || map.lat < 20 || map.lat > 46 || map.lon < 122 || map.lon > 154)
    return Status::failure("付近見取図の位置は、日本の緯度 20〜46 度・経度 122〜154 度の中で入れます");
  if (map.zoom < 14 || map.zoom > 18) return Status::failure("地図の拡大率（ズーム）は 14〜18 です");
  for (const VicinityLayer& l : vicinityLayers())
    if (l.key == map.layer) return Status::success();
  return Status::failure("地図の種類は std（標準地図）・pale（淡色地図）・seamlessphoto（写真）のどれかです");
}

VicinityTiles vicinityTiles(const VicinityMap& map, int cols, int rows) {
  VicinityTiles out;
  out.cols = cols;
  out.rows = rows;
  const double n = std::pow(2.0, map.zoom);
  const double lat = map.lat * kPi / 180.0;
  // Web Mercator, as the tiles are cut: tile coordinates of the site (fractions within a tile).
  const double tx = (map.lon + 180.0) / 360.0 * n;
  const double ty = (1.0 - std::asinh(std::tan(lat)) / kPi) / 2.0 * n;
  const int cx = static_cast<int>(std::floor(tx)), cy = static_cast<int>(std::floor(ty));
  const int x0 = cx - cols / 2, y0 = cy - rows / 2;
  std::string extension = "png";
  for (const VicinityLayer& l : vicinityLayers())
    if (l.key == map.layer) extension = l.extension;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c) {
      VicinityTile t{map.zoom, x0 + c, y0 + r, {}};
      t.url = "https://cyberjapandata.gsi.go.jp/xyz/" + map.layer + "/" + std::to_string(t.z) + "/" + std::to_string(t.x) +
              "/" + std::to_string(t.y) + "." + extension;
      out.tiles.push_back(std::move(t));
    }
  out.siteX = (tx - x0) * 256.0;
  out.siteY = (ty - y0) * 256.0;
  out.metresPerPixel = 2 * kPi * 6378137.0 * std::cos(lat) / (256.0 * n);
  return out;
}

std::vector<std::string> vicinityAttribution(const std::string& layer) {
  std::string name = layer;
  for (const VicinityLayer& l : vicinityLayers())
    if (l.key == layer) name = l.name;
  std::vector<std::string> out = {"出典：国土地理院「地理院タイル」（" + name + "）https://maps.gsi.go.jp/development/ichiran.html",
                                  "地理院タイルに敷地の位置・方位を追記して掲載（国土地理院が作成したものではありません）"};
  if (layer == "std" || layer == "pale")
    out.push_back("この地理院タイルは基本測量成果（名称：電子地形図（タイル））です。図面として使う時の申請の要否は、"
                  "国土地理院「地図の利用手続」で確かめてください");
  return out;
}

}  // namespace sbr::doc
