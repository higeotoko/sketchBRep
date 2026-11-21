#pragma once

// docs/233: the vicinity map (付近見取図) from 国土地理院's map tiles (地理院タイル): where it is
// centred, which tiles cover it, and the sheet made from the picture the app puts together. The core
// does not fetch anything: the app fetches the tiles (or finds them in its cache), joins them into
// one picture in the order vicinityTiles() gives and hands it over in DrawingSetSpec.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sbr/Types.h"
#include "sbr/draw/Drawing.h"

namespace sbr::doc {

class Document;

// docs/235: a landmark written on the map (a station, a road ...): its name and where it is.
struct VicinityMark {
  std::string name;
  double lat = 0.0, lon = 0.0;
};
struct VicinityMap {
  double lat = 0.0, lon = 0.0;  // degrees (WGS84), the site
  int zoom = 16;                // tile zoom, 14..18
  std::string layer = "std";    // std 標準地図 / pale 淡色地図 / seamlessphoto 写真
  std::vector<VicinityMark> marks;  // docs/235: up to 20
};
// The layers offered: key, name, file extension.
struct VicinityLayer {
  std::string key, name, extension;
};
const std::vector<VicinityLayer>& vicinityLayers();
// As given (Modeler::setVicinityMap), or nothing.
std::optional<VicinityMap> vicinityMapOf(const Document& document);
// Checks the values (Japan and around it: latitude 20..46, longitude 122..154; zoom 14..18; a layer
// of vicinityLayers(); docs/235: at most 20 marks, each named in one line of up to 40 characters, in
// the same latitude / longitude range).
Status checkVicinityMap(const VicinityMap& map);
// docs/235: where (lat, lon) is from the site on the ground, east and north in mm (Web Mercator at the
// map's zoom, as the tiles are drawn: right within the few km of a vicinity map).
std::pair<double, double> vicinityOffset(const VicinityMap& map, double lat, double lon);

struct VicinityTile {
  int z = 0, x = 0, y = 0;
  std::string url;  // https://cyberjapandata.gsi.go.jp/xyz/<layer>/<z>/<x>/<y>.<ext>
};
// The cols x rows tiles around the site, row by row from the north-west, 256 px each; where the site
// falls in the picture they make (pixels from its top left) and the ground size of a pixel there.
struct VicinityTiles {
  std::vector<VicinityTile> tiles;
  int cols = 0, rows = 0;
  double siteX = 0.0, siteY = 0.0;
  double metresPerPixel = 0.0;
};
VicinityTiles vicinityTiles(const VicinityMap& map, int cols = 3, int rows = 3);

// The picture the app made from vicinityTiles() (RGBA, rows from the top).
struct VicinityPicture {
  int width = 0, height = 0;
  std::shared_ptr<const std::vector<unsigned char>> rgba;
  double siteX = 0.0, siteY = 0.0;  // as VicinityTiles
  double metresPerPixel = 0.0;
  int missingTiles = 0;             // tiles that could not be had (left grey)
};

// The 出典 lines for the layer: what the map is (and, for 標準地図・淡色地図, that it is a 基本測量成果:
// whether an application is needed for the way it is used is checked with 国土地理院's guide), and
// that the site's mark and the north mark were added.
std::vector<std::string> vicinityAttribution(const std::string& layer);

}  // namespace sbr::doc
