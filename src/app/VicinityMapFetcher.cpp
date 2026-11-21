#include "VicinityMapFetcher.h"

#include <cstring>
#include <memory>
#include <vector>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace sbr::app {

QString vicinityTileCacheDir() {
  const QByteArray given = qgetenv("SBR_TILE_CACHE");
  if (!given.isEmpty()) return QString::fromLocal8Bit(given);
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/gsi-tiles");
}

namespace {

// One tile: from the cache, else (when allowed) from the server, kept in the cache.
QImage tileImage(const doc::VicinityTile& tile, const QString& layer, bool offline, int* fetched, int* cached) {
  const QString suffix = QFileInfo(QUrl(QString::fromStdString(tile.url)).path()).suffix();
  const QString path = QStringLiteral("%1/%2/%3/%4/%5.%6")
                           .arg(vicinityTileCacheDir(), layer)
                           .arg(tile.z)
                           .arg(tile.x)
                           .arg(tile.y)
                           .arg(suffix);
  QImage image;
  if (QFileInfo::exists(path) && image.load(path)) {
    ++*cached;
    return image;
  }
  if (offline) return {};
  QNetworkAccessManager network;
  QNetworkRequest request{QUrl(QString::fromStdString(tile.url))};
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("SketchBRep"));
  QNetworkReply* reply = network.get(request);
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  timeout.start(15000);
  loop.exec();
  const QByteArray data = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
  reply->deleteLater();
  if (data.isEmpty() || !image.loadFromData(data)) return {};
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (file.open(QIODevice::WriteOnly)) file.write(data);
  ++*fetched;
  return image;
}

}  // namespace

std::optional<doc::VicinityPicture> fetchVicinityPicture(const doc::VicinityMap& map, QString* message) {
  const doc::VicinityTiles tiles = doc::vicinityTiles(map);
  const bool offline = !qgetenv("SBR_TILES_OFFLINE").isEmpty();
  QImage joined(tiles.cols * 256, tiles.rows * 256, QImage::Format_RGBA8888);
  joined.fill(QColor(220, 220, 220));
  int fetched = 0, cached = 0, missing = 0;
  {
    QPainter painter(&joined);
    for (std::size_t k = 0; k < tiles.tiles.size(); ++k) {
      const QImage tile = tileImage(tiles.tiles[k], QString::fromStdString(map.layer), offline, &fetched, &cached);
      if (tile.isNull()) {
        ++missing;
        continue;
      }
      const int c = static_cast<int>(k) % tiles.cols, r = static_cast<int>(k) / tiles.cols;
      painter.drawImage(QRect(c * 256, r * 256, 256, 256), tile);
    }
  }
  if (message)
    *message = QObject::tr("地図タイル：取得 %1・キャッシュ %2・取れなかった物 %3").arg(fetched).arg(cached).arg(missing);
  if (missing == static_cast<int>(tiles.tiles.size())) return std::nullopt;
  auto bytes = std::make_shared<std::vector<unsigned char>>(static_cast<std::size_t>(joined.width()) * joined.height() * 4);
  for (int y = 0; y < joined.height(); ++y)
    std::memcpy(bytes->data() + static_cast<std::size_t>(y) * joined.width() * 4, joined.constScanLine(y),
                static_cast<std::size_t>(joined.width()) * 4);
  doc::VicinityPicture picture;
  picture.width = joined.width();
  picture.height = joined.height();
  picture.rgba = bytes;
  picture.siteX = tiles.siteX;
  picture.siteY = tiles.siteY;
  picture.metresPerPixel = tiles.metresPerPixel;
  picture.missingTiles = missing;
  return picture;
}

}  // namespace sbr::app
