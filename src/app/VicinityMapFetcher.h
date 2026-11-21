#pragma once

// docs/233: 国土地理院's map tiles for the vicinity map: found in the cache or fetched (the site's
// position goes to 国土地理院's server), joined into one picture as doc::vicinityTiles() orders them.
//
// SBR_TILE_CACHE: the cache folder (default: the user's cache folder / gsi-tiles).
// SBR_TILES_OFFLINE: when set, nothing is fetched (tests, no network): only the cache is used.

#include <optional>

#include <QString>

#include "sbr/doc/VicinityMap.h"

namespace sbr::app {

QString vicinityTileCacheDir();
// The picture, or nothing when no tile could be had at all. `message`: what happened, for the status
// line (how many tiles were fetched, came from the cache or are missing).
std::optional<doc::VicinityPicture> fetchVicinityPicture(const doc::VicinityMap& map, QString* message = nullptr);

}  // namespace sbr::app
