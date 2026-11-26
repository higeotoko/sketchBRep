#include "PermitForms.h"
#include "PermitApplicationArea.h"

#include "DrawingExport.h"
#include "VicinityMapFetcher.h"
#include "sbr/doc/Schedules.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QMap>
#include <QCoreApplication>
#include <QDataStream>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTableWidget>
#include <QVBoxLayout>

namespace sbr::app {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("PermitForms", text); }

struct Entry {
  QByteArray name;
  QByteArray data;
};

QList<Entry> readTemplate(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QDataStream in(&file);
  in.setByteOrder(QDataStream::LittleEndian);
  quint32 count = 0;
  in >> count;
  if (count == 0 || count > 100) return {};
  QList<Entry> entries;
  for (quint32 i = 0; i < count; ++i) {
    quint16 nameSize = 0;
    quint32 dataSize = 0;
    in >> nameSize >> dataSize;
    if (nameSize == 0 || static_cast<qint64>(nameSize) + dataSize > file.bytesAvailable()) return {};
    Entry entry{file.read(nameSize), file.read(dataSize)};
    if (entry.name.size() != nameSize || entry.data.size() != dataSize) return {};
    entries.push_back(std::move(entry));
  }
  return file.atEnd() ? entries : QList<Entry>{};
}

quint32 crc32(const QByteArray& data) {
  quint32 crc = 0xffffffff;
  for (const unsigned char byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
  }
  return ~crc;
}

QByteArray zip(const QList<Entry>& entries) {
  // The small, embedded OOXML package and its optional PNGs fit classic ZIP.
  QByteArray archive, directory;
  QDataStream out(&archive, QIODevice::WriteOnly), central(&directory, QIODevice::WriteOnly);
  out.setByteOrder(QDataStream::LittleEndian);
  central.setByteOrder(QDataStream::LittleEndian);
  for (const Entry& entry : entries) {
    if (entry.data.size() > std::numeric_limits<quint32>::max() - archive.size()) return {};
    const auto offset = static_cast<quint32>(archive.size());
    const auto size = static_cast<quint32>(entry.data.size());
    const auto nameSize = static_cast<quint16>(entry.name.size());
    const auto crc = crc32(entry.data);
    const quint16 date = (46 << 9) | (11 << 5) | 24;  // 2026-11-24 in MS-DOS date format
    out << quint32(0x04034b50) << quint16(20) << quint16(0) << quint16(0) << quint16(0) << date
        << crc << size << size << nameSize << quint16(0);
    out.writeRawData(entry.name.constData(), entry.name.size());
    out.writeRawData(entry.data.constData(), entry.data.size());
    central << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0) << quint16(0)
            << quint16(0) << date << crc << size << size << nameSize << quint16(0) << quint16(0)
            << quint16(0) << quint16(0) << quint32(0) << offset;
    central.writeRawData(entry.name.constData(), entry.name.size());
  }
  const auto offset = static_cast<quint32>(archive.size());
  out.writeRawData(directory.constData(), directory.size());
  const auto count = static_cast<quint16>(entries.size());
  out << quint32(0x06054b50) << quint16(0) << quint16(0) << count << count
      << static_cast<quint32>(directory.size()) << offset << quint16(0);
  return out.status() == QDataStream::Ok && central.status() == QDataStream::Ok ? archive : QByteArray{};
}

int slotColumns(const QString& value) {
  int columns = 0;
  for (const QChar c : value) columns += c.unicode() < 128 ? 1 : 2;
  return columns;
}

QString mergeFields(const QString& original, const QMap<QString, QString>& values) {
  static const QRegularExpression marker(QStringLiteral("@@SBR_([a-z0-9_]+)@@"));
  QString filled;
  qsizetype offset = 0;
  for (const auto& match : marker.globalMatch(original)) {
    filled += original.sliced(offset, match.capturedStart() - offset);
    const auto key = match.captured(1);
    const auto value = values.value(key);
    if (key.startsWith("slot_")) {
      const auto widthStart = key.lastIndexOf("__");
      const auto slotKey = key.sliced(5, widthStart - 5);
      const auto slotValue = values.value(slotKey);
      const int width = key.sliced(widthStart + 2).toInt();
      if (slotKey == "zone_reserve") {
        // Give a long official zone name part of the unused width in this single-zone case.
        const int extra = std::max(0, 1 + slotColumns(values.value("zone")) - width);
        filled += QString(std::max(1, width - (extra + 1) / 2), QLatin1Char(' '));
      } else if (slotValue.isEmpty()) {
        filled += QString(width, QLatin1Char(' '));
      } else {
        filled += QLatin1Char(' ') + slotValue.toHtmlEscaped();
        filled += QString(std::max(0, width - slotColumns(slotValue) - 1), QLatin1Char(' '));
      }
    } else if (key.startsWith("check_")) {
      filled += value.isEmpty() ? QStringLiteral("□") : QStringLiteral("☑");
    } else if (key.startsWith("date_")) {
      filled += value.isEmpty() ? QStringLiteral("　　　　年    月    日") : value;
    } else if (key == "structure" || key == "detail_structure") {
      filled += value.isEmpty() ? QStringLiteral("            ") : QStringLiteral("　") + value.toHtmlEscaped();
    } else if (!value.isEmpty()) {
      filled += QStringLiteral("　") + value.toHtmlEscaped();
    }
    offset = match.capturedEnd();
  }
  filled += original.sliced(offset);
  return filled;
}

QByteArray renderImage(const draw::Sheet& sheet) {
  QImage image(2400, qRound(2400 * sheet.paperHeight / sheet.paperWidth), QImage::Format_RGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  renderSheet(painter, sheet, QRectF(image.rect()));
  painter.end();
  int left = image.width(), right = -1, top = image.height(), bottom = -1;
  for (int y = 0; y < image.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      if ((row[x] & 0x00ffffffu) == 0x00ffffffu) continue;
      left = std::min(left, x);
      right = std::max(right, x);
      top = std::min(top, y);
      bottom = std::max(bottom, y);
    }
  }
  if (right < left) return {};
  image = image.copy(QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-12, -12, 12, 12).intersected(image.rect()));
  image = image.scaled(1600, 880, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  return image.save(&buffer, "PNG") ? png : QByteArray{};
}

QByteArray anchoredImage(const QByteArray& png, const QByteArray& id, int number) {
  const auto image = QImage::fromData(png, "PNG");
  if (image.isNull()) return {};
  // Word uses EMUs (36,000 per mm). Centre within the existing 155.8 mm form boxes.
  const QSize size = image.size().scaled(151 * 36000, 67 * 36000, Qt::KeepAspectRatio);
  const auto left = QByteArray::number((5610225 - size.width()) / 2);
  const auto width = QByteArray::number(size.width()), height = QByteArray::number(size.height());
  return "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" "
         "simplePos=\"0\" relativeHeight=\"251659264\" behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\">"
         "<wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\"><wp:posOffset>" + left +
         "</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>"
         "<wp:extent cx=\"" + width + "\" cy=\"" + height +
         "\"/><wp:wrapNone/><wp:docPr id=\"" + QByteArray::number(number) +
         "\" name=\"" + id + "\"/><wp:cNvGraphicFramePr/>"
         "<a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
         "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
         "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
         "<pic:nvPicPr><pic:cNvPr id=\"0\" name=\"" + id + ".png\"/><pic:cNvPicPr/></pic:nvPicPr>"
         "<pic:blipFill><a:blip r:embed=\"" + id + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
         "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + width + "\" cy=\"" + height +
         "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
         "</pic:pic></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>";
}

struct Images {
  QByteArray map, site;
};

QByteArray documentBytes(bool summary, const QMap<QString, QString>& values, const Images& images) {
  auto entries = readTemplate(summary ? QStringLiteral(":/permit_forms/third.sbrform")
                                      : QStringLiteral(":/permit_forms/second.sbrform"));
  if (entries.isEmpty()) return {};
  QByteArray relationships;
  QList<Entry> media;
  auto addImage = [&](const QByteArray& png, const QByteArray& id, int number) {
    if (png.isEmpty()) return QByteArray{};
    const auto anchor = anchoredImage(png, id, number);
    if (anchor.isEmpty()) return QByteArray{};
    const QByteArray path = "media/" + id + ".png";
    relationships += "<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                     "relationships/image\" Target=\"" + path + "\"/>";
    media.push_back({"word/" + path, png});
    return anchor;
  };
  for (Entry& entry : entries) {
    if (entry.name == "word/document.xml") {
      // Patch fixed XML fragments before inserting user text, so a literal marker in a field stays text.
      if (summary) {
        entry.data.replace("<!--SBR_IMAGE_MAP-->", addImage(images.map, "rIdSbrMap", 9001));
        entry.data.replace("<!--SBR_IMAGE_SITE-->", addImage(images.site, "rIdSbrSite", 9002));
      }
      entry.data = mergeFields(QString::fromUtf8(entry.data), values).toUtf8();
    }
  }
  for (Entry& entry : entries) {
    if (entry.name == "word/_rels/document.xml.rels" && summary)
      entry.data.replace("<!--SBR_RELS-->", relationships);
  }
  entries.append(media);
  return zip(entries);
}

struct TransferRow {
  QString label, value, source, state;
  bool applicationOnly = false;
};
struct Transfer {
  QMap<QString, QString> values;
  QList<TransferRow> rows;
  bool newBuilding = false;
};

QString metres(double mm) { return QString::number(mm / 1000.0, 'f', 3).append(QStringLiteral(" m")); }
Transfer collect(const doc::Document& document, bool applyArea) {
  Transfer result;
  const auto outline = doc::projectOutline(document);
  const auto outlineSource = tr("計画概要");
  const auto get = [&](const std::string& key) {
    const auto it = outline.find(key);
    if (it == outline.end()) return QString{};
    auto value = QString::fromStdString(it->second).simplified();
    value.remove(QRegularExpression(QStringLiteral("[\\x{0}-\\x{1f}\\x{fffe}\\x{ffff}]")));
    return value;
  };
  const auto row = [&](const QString& label, const QString& value, const QString& source,
                       const QString& state, bool only = false) {
    result.rows.push_back({label, value, source, state, only});
  };
  const auto field = [&](const QString& key, const QString& label, const QString& value, const QString& source,
                         bool optional = false) {
    if (!value.isEmpty()) result.values.insert(key, value);
    row(label, value, source, value.isEmpty() ? (optional ? tr("該当時に記入") : tr("要記入")) : tr("転記"));
  };
  const auto choice = [&](const char* key, const QString& prefix, const QString& label, bool only = false) {
    const auto value = get(key);
    if (!value.isEmpty()) result.values.insert("check_" + prefix + value, QStringLiteral("1"));
    row(label, value.isEmpty() ? QString{} : QString::fromStdString(doc::projectOutlineText(key, value.toStdString())),
        outlineSource, value.isEmpty() ? tr("要選択") : tr("転記"), only);
    return value;
  };

  field("client", tr("建築主氏名"), get("client"), outlineSource);
  field("client_kana", tr("建築主のフリガナ"), get("form_client_kana"), outlineSource);
  field("client_postcode", tr("建築主の郵便番号"), get("form_client_postcode"), outlineSource);
  field("client_address", tr("建築主の住所"), get("form_client_address"), outlineSource);
  const auto clientPhone = get("form_client_phone");
  if (!clientPhone.isEmpty()) result.values.insert("client_phone", clientPhone);
  row(tr("建築主の電話番号"), clientPhone, outlineSource, clientPhone.isEmpty() ? tr("要記入") : tr("転記"), true);
  for (const auto& [role, label] : {std::pair{"agent", tr("代理者")}, {"designer", tr("代表設計者")},
                                   {"supervisor", tr("代表工事監理者")}, {"contractor", tr("代表工事施工者")}}) {
    const std::string prefix = std::string("form_") + role + "_";
    const auto name = get(prefix + "name");
    const auto memo = get(role);
    if (!name.isEmpty()) result.values.insert(QString::fromLatin1(role) + "_name", name);
    row(label, name.isEmpty() ? memo : name, outlineSource,
        !name.isEmpty() ? tr("転記") : (!memo.isEmpty() ? tr("従来のメモ・氏名欄を入力") : tr("該当時に記入")));
    if (name.isEmpty()) continue;
    for (const auto& contact : doc::projectOutlineFields()) {
      if (contact.key.compare(0, prefix.size(), prefix) != 0 || contact.key == prefix + "name") continue;
      const auto raw = get(contact.key);
      const auto key = QString::fromStdString(contact.key.substr(5));
      field(key, label + tr("・") + QString::fromStdString(contact.label),
          QString::fromStdString(doc::projectOutlineText(contact.key, raw.toStdString())), outlineSource, true);
    }
  }
  const auto projectName = get("project_name");
  if (!projectName.isEmpty()) result.values.insert("project_name", projectName);
  row(tr("建築物の名称・工事名"), projectName, outlineSource, projectName.isEmpty() ? tr("該当時に記入") : tr("転記"), true);
  choice("structural_review", "struct_", tr("構造計算適合性判定"), true);
  choice("energy_plan", "energy_", tr("省エネ適判の計画提出"), true);
  field("address", tr("地名地番"), get("address"), outlineSource);
  field("residence", tr("住居表示"), get("residence"), outlineSource, true);
  const auto city = choice("city_planning", "city_", tr("都市計画区域"));
  if (city == "urbanization" || city == "control" || city == "non_divided")
    result.values.insert("check_city_inside", "1");
  const auto fire = choice("fire_zone", "fire_", tr("防火地域"));
  auto zones = get("other_zones");
  if (fire == "article22") {
    result.values.insert("check_fire_none", "1");
    static const QRegularExpression article(QStringLiteral("第?[ 　]*[2２][2２][ 　]*条"));
    if (!zones.contains(article))
      zones = zones.isEmpty() ? QStringLiteral("法第22条区域") : zones + QStringLiteral("、法第22条区域");
  }
  field("other_zones", tr("その他の区域"), zones, outlineSource, true);
  const auto& boundary = document.settings().siteBoundary;
  QString width, frontage, siteArea, zone;
  double measuredSiteMm2 = 0.0;
  if (boundary) {
    if (const auto geometry = document.siteBoundaryGeometry(boundary->outline)) {
      measuredSiteMm2 = geometry->areaMm2;
      siteArea = permitSquareMetres(measuredSiteMm2);
      if (boundary->road && document.siteRoadEdge(*geometry)) {
        if (boundary->road->widthMm > 0) width = metres(boundary->road->widthMm);
        if (boundary->road->frontageMm > 0) frontage = metres(boundary->road->frontageMm);
      }
    }
  }
  field("road_width", tr("道路幅員"), width, tr("敷地・道路の登録値"));
  field("frontage", tr("接道長さ"), frontage, tr("敷地・道路の登録値"));
  const auto zoneKey = doc::siteHeightConditions(document).zone;
  for (const auto& [key, label] : doc::heightZoneChoices())
    if (key == zoneKey && !key.empty()) zone = QString::fromStdString(label);
  // The registered zone is a candidate. A zoning split requires area allocation by hand.
  field("main_use", tr("主要用途"), get("main_use"), outlineSource);
  field("use_code", tr("主要用途の区分"), get("use_code"), outlineSource);
  result.newBuilding = choice("work_kind", "work_", tr("工事種別")) == "new";
  const auto area = doc::areaTable(document);
  const bool knownFloor = hasApplicationFloorArea(area);
  const auto candidate = [&](const QString& key, const QString& label, const QString& value, const QString& source) {
    const bool use = applyArea && result.newBuilding && !value.isEmpty();
    if (use) result.values.insert(key, value);
    row(label, value, source, use ? tr("転記") : value.isEmpty() ? tr("要記入") : tr("候補"));
  };
  candidate("site_area", tr("敷地面積"), siteArea, tr("敷地境界"));
  candidate("zone", tr("用途地域"), zone, tr("高さの制限の設定"));
  candidate("building_area", tr("建築面積"), area.buildingKnown && area.buildingArea > 0 ? permitSquareMetres(area.buildingArea) : QString{}, tr("面積の根拠図"));
  candidate("floor_area", tr("延べ面積"), knownFloor ? permitSquareMetres(area.legalTotal) : QString{}, tr("面積の根拠図"));
  const auto structure = get("structure");
  // The first slot ends in a printed 造. Mixed structures must be allocated to both slots.
  static const QRegularExpression single(QStringLiteral("^(木|鉄骨|軽量鉄骨|鉄筋コンクリート|鉄骨鉄筋コンクリート|組積|補強コンクリートブロック)造$"));
  const auto structureMatch = single.match(structure);
  if (structureMatch.hasMatch()) result.values.insert("structure", structureMatch.captured(1));
  row(tr("構造"), structure, outlineSource, structureMatch.hasMatch() ? tr("転記") : tr("様式の構造欄に記入"));
  field("permissions", tr("許可・認定等"), get("permissions"), outlineSource, true);
  for (const auto& [key, label] : {std::pair{"start_date", tr("着手予定")}, {"finish_date", tr("完了予定")}}) {
    const auto raw = get(key);
    const auto date = QDate::fromString(raw, Qt::ISODate);
    if (date.isValid()) {
      field("date_" + QString::fromLatin1(key), label, date.toString(QStringLiteral("yyyy年M月d日")), outlineSource);
    } else {
      row(label, raw, outlineSource, raw.isEmpty() ? tr("要記入") : tr("様式の日付欄に記入"));
    }
  }
  row(tr("特定工程"), get("specific_process"), outlineSource, tr("該当する場合は様式に記入"));
  field("other_matters", tr("その他必要な事項"), get("other_matters"), outlineSource, true);
  const auto integer = [&](const char* key, int minimum) {
    static const QRegularExpression format(QStringLiteral("^[0-9]{1,4}$"));
    const auto value = get(key);
    return format.match(value).hasMatch() && value.toInt() >= minimum ? QString::number(value.toInt()) : QString{};
  };
  const auto height = [&](const char* key) {
    static const QRegularExpression format(QStringLiteral("^[0-9]{1,4}(\\.[0-9]{1,3})?$"));
    const auto value = get(key);
    return format.match(value).hasMatch() && value.toDouble() > 0.0 ? value : QString{};
  };
  const auto checked = tr("確認済みの建築物情報");
  const auto verified = [&](const char* formKey, const QString& label, const QString& value) {
    const auto entered = get(std::string("form_") + formKey);
    if (value.isEmpty() && !entered.isEmpty()) row(label, entered, checked, tr("入力値を確認"));
    else candidate(QString::fromLatin1(formKey), label, value, checked);
  };
  const auto buildingHeight = height("form_building_height");
  auto eaves = height("form_building_eaves");
  if (!buildingHeight.isEmpty() && eaves.toDouble() > buildingHeight.toDouble()) eaves.clear();
  const auto otherBuildings = integer("form_building_other", 0);
  const auto above = integer("form_building_above", 1);
  const auto below = integer("form_building_below", 0);
  const bool buildingDetail = result.newBuilding && knownFloor && area.legalTotal > 10'000'000.0;
  candidate("building_count", tr("申請に係る建築物の数"), buildingDetail ? QStringLiteral("1") : QString{},
            tr("単一棟の新築・延べ面積が10㎡超"));
  verified("building_other", tr("同一敷地の他の建築物の数（10㎡超）"), otherBuildings);
  verified("building_above", tr("地上階数"), above);
  verified("building_below", tr("地下階数"), below);
  verified("building_height", tr("最高の高さ"), buildingHeight.isEmpty() ? QString{} : buildingHeight + " m");

  if (applyArea && buildingDetail) {
    if (!eaves.isEmpty()) result.values.insert("building_eaves", eaves + " m");
    result.values.insert("detail_main_use", get("main_use"));
    result.values.insert("detail_use_code", get("use_code"));
    result.values.insert("check_detail_work_new", QStringLiteral("1"));
    if (structureMatch.hasMatch()) result.values.insert("detail_structure", structureMatch.captured(1));
  }
  const auto ratioSource = get("form_building_ratio_source");
  const auto ratioArea = [&](const char* key) {
    const auto entered = get(key);
    static const QRegularExpression decimal(QStringLiteral("^[0-9]{1,9}(\\.[0-9]{1,4})?$"));
    return decimal.match(entered).hasMatch() ? entered.toDouble() * 1e6 : -1.0;
  };
  const double confirmedSite = ratioArea("form_building_ratio_site");
  const bool sameSite = confirmedSite > 0 && measuredSiteMm2 > 0 &&
                        permitSquareMetres(confirmedSite) == permitSquareMetres(measuredSiteMm2);
  const auto addRatio = [&](const char* key, const QString& label, const QString& areaKey,
                            const QString& percentKey, double gross) {
    const double numerator = ratioArea(key);
    QString state;
    if (ratioSource.isEmpty()) state = tr("確認資料を入力");
    else if (!sameSite) state = tr("算定用敷地面積とモデルを照合");
    else if (otherBuildings != "0") state = tr("同一敷地の既存棟・別棟を確認");
    else if (numerator < 0.0 || gross <= 0.0 || numerator > gross + 5'000.0) state = tr("算定面積とモデルを照合");
    if (!state.isEmpty()) {
      row(label, get(key), ratioSource, state);
      return;
    }
    const auto percentage = QString::number(100.0 * numerator / confirmedSite, 'f', 2) + " %";
    const bool transfer = applyArea && result.newBuilding;
    if (transfer) {
      result.values.insert(areaKey, permitSquareMetres(numerator));
      result.values.insert(percentKey, percentage);
    }
    row(label, QStringLiteral("%1 ÷ %2 ＝ %3").arg(permitSquareMetres(numerator), permitSquareMetres(confirmedSite), percentage),
        ratioSource, transfer ? tr("転記") : tr("候補"));
  };
  addRatio("form_building_ratio_coverage", tr("建蔽率"), "coverage_area", "coverage_percent",
           area.buildingKnown ? area.buildingArea : 0.0);
  addRatio("form_building_ratio_floor", tr("容積率"), "ratio_floor_area", "floor_percent",
           knownFloor ? area.legalTotal : 0.0);

  // The fourth face has six rows. Require a complete correspondence to the entered legal storey
  // count; levels such as GL/RF or split levels cannot silently take a numbered storey's place.
  QMap<int, double> floors;
  const int aboveCount = above.toInt(), belowCount = below.toInt();
  bool validFloors = buildingDetail && !above.isEmpty() && !below.isEmpty() && aboveCount + belowCount <= 6;
  static const QRegularExpression levelName(QStringLiteral("^(B)?([1-6])(?:FL|F|階)?$"),
                                             QRegularExpression::CaseInsensitiveOption);
  double floorSum = 0.0;
  for (const auto& floor : area.rows) {
    if (!floor.legalKnown || floor.legalFloor <= 0.0) continue;
    const auto match = levelName.match(QString::fromStdString(floor.level).trimmed());
    const int number = match.captured(2).toInt();
    const bool basement = !match.captured(1).isEmpty();
    const int index = basement ? -number : number;
    if (!match.hasMatch() || floor.unreadableWalls || number > (basement ? belowCount : aboveCount) ||
        floors.contains(index)) {
      validFloors = false;
      continue;
    }
    floors.insert(index, floor.legalFloor);
    floorSum += floor.legalFloor;
  }
  validFloors = validFloors && floors.size() == aboveCount + belowCount &&
                std::abs(floorSum - area.legalTotal) <= 1.0;
  if (!validFloors) {
    row(tr("第四面の階別床面積"), {}, tr("面積の根拠図"), tr("階名・階数・面積を照合"), true);
  } else {
    auto it = floors.constEnd();
    for (int i = 0; it != floors.constBegin(); ++i) {
      --it;
      const auto number = it.key() < 0 ? QStringLiteral("B%1").arg(-it.key()) : QString::number(it.key());
      const auto key = QStringLiteral("detail_floor_%1").arg(i);
      const auto value = permitSquareMetres(it.value());
      if (applyArea) {
        result.values.insert(key, value);
        result.values.insert(key + "_number", number);
      }
      row(tr("第四面・%1階の床面積").arg(number), value, tr("面積の根拠図"), applyArea ? tr("転記") : tr("候補"), true);
    }
    if (applyArea) result.values.insert("detail_floor_total", permitSquareMetres(area.legalTotal));
  }
  const auto eavesInput = get("form_building_eaves");
  row(tr("最高の軒の高さ"), eaves.isEmpty() ? eavesInput : eaves + " m", checked,
      eaves.isEmpty() ? (eavesInput.isEmpty() ? tr("要記入") : tr("入力値を確認"))
                      : applyArea && buildingDetail ? tr("転記") : tr("候補"), true);
  row(tr("申請日・宛先"), {}, {}, tr("確認申請書の第一面に記入"), true);
  row(tr("主要構造部・第五面の階別概要"), {}, {}, tr("確認申請書の第四面以降に記入"), true);
  return result;
}

Images prepareImages(const doc::Document& document, bool siteChecked, bool mapChecked) {
  Images images;
  draw::SheetSpec spec;
  spec.frame = false;
  spec.compactDiagram = true;
  if (siteChecked) {
    if (auto view = draw::makeView(document, draw::sitePlanView())) {
      view.value().siteGroundTable.reset();
      for (const double scale : {100.0, 200.0, 500.0, 1000.0}) {
        spec.scale = scale;
        const auto sheet = draw::layoutSheet(*view, spec);
        if (!sheet || sheet->paper == "A1") continue;
        images.site = renderImage(*sheet);
        break;
      }
    }
  }
  if (mapChecked) {
    if (const auto map = doc::vicinityMapOf(document)) {
      const auto picture = fetchVicinityPicture(*map);
      if (picture && picture->missingTiles == 0) {
        auto view = doc::vicinityView(document, *map, picture, spec.scale);
        view.calculationNotes = doc::vicinityAttribution(map->layer);
        if (const auto sheet = draw::layoutSheet(view, spec)) images.map = renderImage(*sheet);
      }
    }
  }
  return images;
}

}  // namespace

PermitFormDraft buildPermitForm(const doc::Document& document, const PermitFormOptions& options) {
  const auto values = collect(document, options.transferAreas).values;
  const auto images = options.summary ? prepareImages(document, options.includeSitePlan, options.includeVicinityMap) : Images{};
  return {documentBytes(options.summary, values, images), !images.site.isEmpty(), !images.map.isEmpty()};
}

QByteArray buildAgreementForm(const doc::Document& document, QStringList* unfilled) {
  // docs/287: 横浜市 建築協定等手続状況届出書 第１号様式, first face (site and building). The same
  // values as the application forms (areas only for a single new building whose areas are read).
  auto values = collect(document, true).values;
  QStringList missing;
  const auto take = [&](const QString& from, const QString& to, const QString& unit, const QString& label) {
    auto v = values.value(from);
    if (v.endsWith(unit)) v.chop(unit.size());
    v = v.trimmed();
    if (v.isEmpty()) missing.push_back(label);
    else values.insert(to, v + QLatin1Char(' '));
  };
  take(QStringLiteral("site_area"), QStringLiteral("agreement_site_area"), QStringLiteral("㎡"), tr("敷地面積"));
  take(QStringLiteral("floor_area"), QStringLiteral("agreement_floor_area"), QStringLiteral("㎡"), tr("延べ面積"));
  take(QStringLiteral("building_area"), QStringLiteral("agreement_building_area"), QStringLiteral("㎡"), tr("建築面積"));
  take(QStringLiteral("building_height"), QStringLiteral("agreement_height"), QStringLiteral("m"), tr("最高の高さ"));
  const auto use = values.value(QStringLiteral("main_use"));
  if (use.isEmpty()) missing.push_back(tr("主要用途"));
  else values.insert(QStringLiteral("agreement_main_use"), use);
  // 地名地番: "横浜市" and "区" are printed; the ward goes between, the rest after 区.
  auto address = values.value(QStringLiteral("address"));
  if (address.isEmpty()) missing.push_back(tr("地名地番"));
  static const QRegularExpression yokohama(QStringLiteral("^横浜市[ 　]*([^ 　区]{1,6})区[ 　]*(.*)$"));
  if (const auto m = yokohama.match(address); m.hasMatch()) {
    values.insert(QStringLiteral("agreement_ward"), m.captured(1));
    if (!m.captured(2).isEmpty()) values.insert(QStringLiteral("agreement_address_rest"), m.captured(2));
  } else if (!address.isEmpty()) {
    values.insert(QStringLiteral("agreement_address_rest"), address);
    missing.push_back(tr("地名地番の区"));
  }
  // 用途地域: the zone registered for the height limits.
  const auto zone = doc::siteHeightConditions(document).zone;
  const std::map<std::string, std::pair<const char*, const char*>> zones = {
      {"low_1", {"low", "一"}}, {"low_2", {"low", "二"}}, {"residential_1", {"residential", "一"}},
      {"residential_2", {"residential", "二"}}, {"mid_high_1", {"mid", "一"}}, {"mid_high_2", {"mid", "二"}},
      {"neighborhood_commercial", {"neighborhood_commercial", ""}}, {"quasi_industrial", {"quasi_industrial", ""}},
      {"exclusive_industrial", {"exclusive_industrial", ""}}, {"quasi_residential", {"quasi_residential", ""}},
      {"commercial", {"commercial", ""}}, {"industrial", {"industrial", ""}}};
  if (const auto it = zones.find(zone); it != zones.end()) {
    const auto key = QStringLiteral("agreement_zone_") + QString::fromLatin1(it->second.first);
    values.insert(QStringLiteral("check_") + key, QStringLiteral("1"));
    if (*it->second.second) values.insert(key + QStringLiteral("_number"), QString::fromUtf8(it->second.second));
  } else {
    missing.push_back(tr("用途地域"));
  }
  if (values.value(QStringLiteral("building_above")).isEmpty()) missing.push_back(tr("階数"));
  values.insert(QStringLiteral("check_agreement_yokohama"), QStringLiteral("1"));
  if (unfilled) *unfilled = missing;
  auto entries = readTemplate(QStringLiteral(":/permit_forms/agreement.sbrform"));
  if (entries.isEmpty()) return {};
  for (Entry& entry : entries)
    if (entry.name == "word/document.xml") entry.data = mergeFields(QString::fromUtf8(entry.data), values).toUtf8();
  return zip(entries);
}

void showPermitForms(const doc::Document& document, QWidget* parent, const std::function<void()>& editOutline) {
  QDialog dialog(parent);
  dialog.setWindowTitle(tr("確認申請のWord様式"));
  dialog.resize(890, 620);
  auto* layout = new QVBoxLayout(&dialog);
  auto* intro = new QLabel(tr("横浜市の様式に、計画概要とモデルの値を転記します。残る欄はWordで記入できます。"));
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* form = new QComboBox;
  form->setObjectName(QStringLiteral("permitWordFormType"));
  form->addItems({tr("確認申請書（第二号様式）"), tr("建築計画概要書（第三号様式）")});
  const auto summary = [form] { return form->currentIndex() == 1; };
  auto* formLabel = new QLabel(tr("様式"));
  formLabel->setBuddy(form);
  auto* selection = new QHBoxLayout;
  selection->addWidget(formLabel);
  selection->addWidget(form, 1);
  layout->addLayout(selection);
  auto* quantities = new QCheckBox(tr("単一棟の新築・用途地域の区分なしとして、面積と棟別の項目も転記する"));
  Transfer transfer = collect(document, false);
  quantities->setEnabled(transfer.newBuilding);
  layout->addWidget(quantities);
  auto* site = new QCheckBox(tr("配置図（検討用）を第三面に入れる"));
  const auto& boundary = document.settings().siteBoundary;
  site->setEnabled(boundary && document.siteBoundaryGeometry(boundary->outline));
  site->setChecked(site->isEnabled());
  site->setHidden(true);
  layout->addWidget(site);
  auto* map = new QCheckBox(tr("国土地理院の地図を読み込み、付近見取図を第三面に入れる"));
  map->setEnabled(doc::vicinityMapOf(document).has_value());
  map->setHidden(true);
  layout->addWidget(map);
  auto* remaining = new QCheckBox(tr("記入が残る欄だけ表示"));
  layout->addWidget(remaining);
  auto* table = new QTableWidget(0, 4);
  table->setObjectName(QStringLiteral("permitWordTransfer"));
  table->setHorizontalHeaderLabels({tr("様式の項目"), tr("値"), tr("転記元"), tr("状態")});
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  layout->addWidget(table);
  const auto refresh = [&] {
    table->setRowCount(0);
    const auto add = [&](const TransferRow& row) {
      const int index = table->rowCount();
      table->insertRow(index);
      const QString values[] = {row.label, row.value, row.source, row.state};
      for (int col = 0; col < 4; ++col) {
        auto* item = new QTableWidgetItem(values[col]);
        item->setToolTip(values[col]);
        table->setItem(index, col, item);
      }
    };
    for (const auto& row : transfer.rows) {
      if (summary() && row.applicationOnly) continue;
      if (remaining->isChecked() && row.state == tr("転記")) continue;
      add(row);
    }
    if (summary()) {
      add({tr("配置図"), {}, tr("敷地・建物モデル"), site->isChecked() ? tr("書き出し時に配置") : tr("様式に追加")});
      add({tr("付近見取図"), {}, tr("国土地理院"), map->isChecked() ? tr("地図の取得後に配置") : tr("様式に追加")});
      add({tr("定期報告・防火設備"), {}, {}, tr("様式で要否を選択")});
    }
  };
  refresh();
  QObject::connect(form, &QComboBox::currentIndexChanged, &dialog, [&](int) {
    site->setVisible(summary());
    map->setVisible(summary());
    refresh();
  });
  QObject::connect(remaining, &QCheckBox::toggled, &dialog, refresh);
  QObject::connect(site, &QCheckBox::toggled, &dialog, refresh);
  QObject::connect(map, &QCheckBox::toggled, &dialog, refresh);
  QObject::connect(quantities, &QCheckBox::toggled, &dialog, [&](bool on) {
    transfer = collect(document, on);
    refresh();
  });
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
  auto* edit = buttons->addButton(tr("転記元の項目を編集…"), QDialogButtonBox::ActionRole);
  edit->setObjectName(QStringLiteral("permitWordEditOutline"));
  QObject::connect(edit, &QPushButton::clicked, &dialog, [&] {
    editOutline();
    transfer = collect(document, quantities->isChecked());
    quantities->setEnabled(transfer.newBuilding);
    if (!transfer.newBuilding) quantities->setChecked(false);
    refresh();
  });
  buttons->button(QDialogButtonBox::Save)->setText(tr("Wordで書き出す…"));
  buttons->button(QDialogButtonBox::Close)->setText(tr("閉じる"));
  layout->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    const bool useSummary = summary();
    QFileDialog chooser(&dialog, tr("Word様式の保存先"),
                        useSummary ? tr("建築計画概要書.docx") : tr("確認申請書.docx"), tr("Word文書 (*.docx)"));
    chooser.setAcceptMode(QFileDialog::AcceptSave);
    chooser.setDefaultSuffix(QStringLiteral("docx"));
    if (chooser.exec() != QDialog::Accepted) return;
    const auto path = chooser.selectedFiles().value(0);
    if (path.isEmpty()) return;
    const bool wantSite = useSummary && site->isChecked(), wantMap = useSummary && map->isChecked();
    const auto sequence = document.sequence();
    dialog.setEnabled(false);
    const auto draft = buildPermitForm(document, {useSummary, quantities->isChecked(), wantSite, wantMap});
    dialog.setEnabled(true);
    if (sequence != document.sequence()) {
      transfer = collect(document, quantities->isChecked());
      quantities->setEnabled(transfer.newBuilding);
      refresh();
      QMessageBox::information(&dialog, tr("モデルが変更されました"), tr("転記内容を更新しました。もう一度書き出してください。"));
      return;
    }
    const auto& data = draft.docx;
    QSaveFile file(path);
    if (data.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
      QMessageBox::warning(&dialog, tr("Word様式の保存"), tr("保存できませんでした。保存先とファイルの使用状況を確認してください。"));
      return;
    }
    QString status = tr("Word様式を保存しました。");
    if (wantMap && !draft.hasVicinityMap) status += tr("\n付近見取図の地図を読み込めませんでした。Wordで追加してください。");
    if (wantSite && !draft.hasSitePlan) status += tr("\n配置図を作成できませんでした。Wordで追加してください。");
    QMessageBox::information(&dialog, tr("Word様式の保存"), status);
  });
  dialog.exec();
}

}  // namespace sbr::app
