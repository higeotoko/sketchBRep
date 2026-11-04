#include "sbr/ConstructionGuide.h"
#include "sbr/ConstructionQuantity.h"
#include <nlohmann/json.hpp>
#include <QListWidget>
#include <QScrollArea>
#include <QSaveFile>
#include <QSignalBlocker>
#include <set>
#include "DrawingExport.h"

#include <chrono>
#include <cmath>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

#include <QPdfWriter>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <Bnd_Box.hxx>

#include "DwgConverter.h"
#include "sbr/doc/Document.h"

namespace sbr::app {

namespace {

QColor penColor(draw::Pen pen) {
  switch (pen) {
    case draw::Pen::Fill: return QColor(200, 200, 200);
    case draw::Pen::Hidden: return QColor(90, 90, 90);
    case draw::Pen::Grid:
    case draw::Pen::GridLabel: return QColor(160, 30, 30);
    case draw::Pen::Level: return QColor(30, 60, 160);
    default: return Qt::black;
  }
}

QString gridDirectionName(const gp_Dir& d) {
  if (std::abs(d.X()) >= std::abs(d.Y())) return d.X() < 0 ? QObject::tr("西") : QObject::tr("東");
  return d.Y() < 0 ? QObject::tr("南") : QObject::tr("北");
}

}  // namespace

void renderSheet(QPainter& p, const draw::Sheet& sheet, const QRectF& target) {
  const double s = sheet.scale;
  // Paper mm -> device: fit the paper into the target.
  const double k = std::min(target.width() / sheet.paperWidth, target.height() / sheet.paperHeight);
  const double ox = target.left() + (target.width() - sheet.paperWidth * k) / 2;
  const double oy = target.top() + (target.height() - sheet.paperHeight * k) / 2;
  auto map = [&](const gp_Pnt2d& q) { return QPointF(ox + q.X() / s * k, oy + (sheet.paperHeight - q.Y() / s) * k); };
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(QRectF(ox, oy, sheet.paperWidth * k, sheet.paperHeight * k), Qt::white);
  // Fills first (lines over them).
  for (int pass = 0; pass < 2; ++pass) {
    for (const draw::Item& i : sheet.items) {
      const bool fill = i.kind == draw::Item::Kind::Fill;
      if ((pass == 0) != fill) continue;
      if (fill) {
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);
        auto loop = [&](const std::vector<gp_Pnt2d>& pts) {
          if (pts.empty()) return;
          path.moveTo(map(pts[0]));
          for (std::size_t n = 1; n < pts.size(); ++n) path.lineTo(map(pts[n]));
          path.closeSubpath();
        };
        loop(i.points);
        for (const auto& h : i.holes) loop(h);
        p.fillPath(path, penColor(i.pen));
        continue;
      }
      QPen pen(penColor(i.pen));
      const double width = std::max(draw::penWidth(i.pen), 0.05) * k;
      pen.setWidthF(width);
      pen.setCapStyle(Qt::FlatCap);
      const std::vector<double> dashes = draw::penDashes(i.pen);
      if (!dashes.empty()) {
        QList<qreal> pattern;
        for (double d : dashes) pattern << std::max(d * k / width, 0.5);
        pen.setDashPattern(pattern);
      }
      p.setPen(pen);
      p.setBrush(Qt::NoBrush);
      switch (i.kind) {
        case draw::Item::Kind::Line: p.drawLine(map(i.points[0]), map(i.points[1])); break;
        case draw::Item::Kind::Polyline: {
          QPolygonF poly;
          for (const gp_Pnt2d& q : i.points) poly << map(q);
          if (i.closed) p.drawPolygon(poly);
          else p.drawPolyline(poly);
          break;
        }
        case draw::Item::Kind::Arc: {
          const QPointF c = map(i.centre);
          const double r = i.radius / s * k;
          const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
          if (i.sweep >= 2 * M_PI - 1e-9) p.drawEllipse(box);
          else p.drawArc(box, static_cast<int>(std::lround(i.start * 180 / M_PI * 16)),
                         static_cast<int>(std::lround(i.sweep * 180 / M_PI * 16)));
          break;
        }
        case draw::Item::Kind::Text: {
          QFont font(QStringLiteral("MS Gothic"));
          font.setStyleHint(QFont::SansSerif);
          font.setPixelSize(std::max(1, static_cast<int>(std::lround(i.height * k))));
          p.setFont(font);
          const QString text = QString::fromStdString(i.text);
          const QFontMetricsF fm(font);
          const double w = fm.horizontalAdvance(text), h = i.height * k;
          const double dx = -w * i.hAlign / 2.0;
          const double dy = i.vAlign == 1 ? h / 2 : i.vAlign == 2 ? h : 0.0;  // down on the device
          p.save();
          p.translate(map(i.points[0]));
          p.rotate(-i.angle * 180 / M_PI);
          p.drawText(QPointF(dx, dy), text);
          p.restore();
          break;
        }
        case draw::Item::Kind::Fill: break;
      }
    }
  }
  p.restore();
}

Status writePdf(const draw::Sheet& sheet, const QString& path) {
  QPdfWriter pdf(path);
  pdf.setPageSize(QPageSize(QSizeF(sheet.paperWidth, sheet.paperHeight), QPageSize::Millimeter,
                            QString::fromStdString(sheet.paper)));
  pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
  pdf.setResolution(600);
  pdf.setTitle(QString::fromStdString(sheet.title));
  pdf.setCreator(QStringLiteral("SketchBRep"));
  QPainter painter;
  if (!painter.begin(&pdf)) return Status::failure("Cannot write " + path.toStdString());
  renderSheet(painter, sheet, QRectF(0, 0, pdf.width(), pdf.height()));
  painter.end();
  return QFileInfo(path).size() > 0 ? Status::success() : Status::failure("Writing " + path.toStdString() + " failed");
}

void setUpPrinter(const draw::Sheet& sheet, QPrinter& printer) {
  // The page size is given upright; the orientation turns it.
  const double shortSide = std::min(sheet.paperWidth, sheet.paperHeight);
  const double longSide = std::max(sheet.paperWidth, sheet.paperHeight);
  printer.setPageSize(QPageSize(QSizeF(shortSide, longSide), QPageSize::Millimeter, QString::fromStdString(sheet.paper)));
  printer.setPageOrientation(sheet.paperWidth >= sheet.paperHeight ? QPageLayout::Landscape : QPageLayout::Portrait);
  printer.setFullPage(true);
  printer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
  printer.setDocName(QString::fromStdString(sheet.title));
  printer.setCreator(QStringLiteral("SketchBRep"));
}

Status printSheet(const draw::Sheet& sheet, QPrinter& printer, bool* scaled) {
  // The sheet at real size from the paper's corner (the frame keeps 10 mm
  // and more from the edges, more than printers leave unprinted). On a
  // smaller paper than the sheet's, it is shrunk to fit (scale no longer
  // true): *scaled says so.
  QPainter painter;
  if (!painter.begin(&printer)) return Status::failure("The printer could not be started");
  const QRectF paper = printer.paperRect(QPrinter::DevicePixel);
  const double dpmmX = printer.logicalDpiX() / 25.4, dpmmY = printer.logicalDpiY() / 25.4;
  QRectF target(0, 0, sheet.paperWidth * dpmmX, sheet.paperHeight * dpmmY);
  // Printers round paper sizes to whole points: allow half a millimetre.
  const double slack = 0.5 * std::max(dpmmX, dpmmY);
  const bool fits = target.width() <= paper.width() + slack && target.height() <= paper.height() + slack;
  if (!fits) target = QRectF(0, 0, paper.width(), paper.height());
  if (scaled) *scaled = !fits;
  renderSheet(painter, sheet, target);
  painter.end();
  return Status::success();
}

DrawingDialog::DrawingDialog(const doc::Document& document, QWidget* parent)
    : QDialog(parent), document_(document), snapshot_(std::make_shared<const doc::Document>(document)) {
  setWindowTitle(tr("図面を書き出す（平面・断面・立面・伏図）"));
  setObjectName(QStringLiteral("drawingDialog"));
  QSettings settings;
  auto* root = new QHBoxLayout(this);
  auto* controls = new QWidget(this);
  auto* left = new QVBoxLayout(controls);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setWidget(controls);
  scroll->setMinimumWidth(330);
  scroll->setMaximumWidth(440);
  root->addWidget(scroll);

  auto* what = new QGroupBox(tr("図面"));
  auto* form = new QFormLayout(what);
  kind_ = new QComboBox;
  kind_->setObjectName(QStringLiteral("drawingKind"));
  kind_->addItems({tr("平面図"), tr("断面図"), tr("立面図")});
  const auto presets =
      nlohmann::json::parse(construction::guideData("timber")).at("outputs").at("framing_presets");
  for (const auto* key : {"foundation", "first_floor", "second_floor", "roof"})
    kind_->addItem(QString::fromStdString(presets.at(key).at("name").get<std::string>()));
  form->addRow(tr("種類"), kind_);
  // Plan: level + cut height.
  planRow_ = new QWidget;
  auto* planLayout = new QHBoxLayout(planRow_);
  planLayout->setContentsMargins(0, 0, 0, 0);
  level_ = new QComboBox;
  level_->setObjectName(QStringLiteral("drawingLevel"));
  for (const doc::FloorLevel& l : document_.settings().levels) {
    level_->addItem(QString::fromStdString(l.name), l.elevation);
  }
  if (level_->count() == 0) level_->addItem(tr("GL（0）"), 0.0);
  cutHeight_ = new QDoubleSpinBox;
  cutHeight_->setObjectName(QStringLiteral("drawingCutHeight"));
  cutHeight_->setRange(-100000, 100000);
  cutHeight_->setDecimals(0);
  cutHeight_->setSuffix(QStringLiteral(" mm"));
  cutHeight_->setValue(settings.value("drawing/cutHeight", 1200.0).toDouble());
  planLayout->addWidget(level_);
  planLayout->addWidget(new QLabel(tr("から")));
  planLayout->addWidget(cutHeight_);
  planLayout->addWidget(new QLabel(tr("の高さで切る")));
  form->addRow(tr("切る位置"), planRow_);
  // Section: where and from which side.
  sectionRow_ = new QWidget;
  auto* sectionLayout = new QHBoxLayout(sectionRow_);
  sectionLayout->setContentsMargins(0, 0, 0, 0);
  position_ = new QComboBox;
  position_->setObjectName(QStringLiteral("drawingPosition"));
  side_ = new QComboBox;
  side_->setObjectName(QStringLiteral("drawingSide"));
  sectionLayout->addWidget(position_);
  sectionLayout->addWidget(side_);
  form->addRow(tr("断面の位置"), sectionRow_);
  // Elevation: the side.
  elevationRow_ = new QComboBox;
  auto* elevationSide = static_cast<QComboBox*>(elevationRow_);
  elevationSide->setObjectName(QStringLiteral("drawingElevationSide"));
  elevationSide->addItems({tr("南（正面）"), tr("東"), tr("北"), tr("西")});
  form->addRow(tr("見る方向"), elevationRow_);
  left->addWidget(what);

  // Section positions: the active section plane, the grid axes, the middle.
  for (const doc::SectionPlane& sp : document_.settings().sections) {
    const gp_Dir n(sp.normal[0], sp.normal[1], sp.normal[2]);
    if (!sp.active || std::abs(n.Z()) > 0.5) continue;
    const gp_Dir h(n.X(), n.Y(), 0);
    positions_.push_back({tr("今の断面（%1）").arg(QString::fromStdString(sp.name)), gp_Pnt(sp.origin[0], sp.origin[1], 0),
                          {h, h.Reversed()},
                          {tr("断面の向きどおり"), tr("反対側から")}});
  }
  for (const doc::Object& o : document_.objects()) {
    const auto* node = std::get_if<doc::GroupNode>(&o.recipe.base);
    if (!node || !node->grid || !document_.isVisible(o.id)) continue;
    const gp_Trsf w = document_.worldTransform(o);
    for (int family = 0; family < 2; ++family) {
      for (const doc::GridAxis& a : family == 0 ? node->grid->x : node->grid->y) {
        const gp_Pnt at = (family == 0 ? gp_Pnt(a.offset, 0, 0) : gp_Pnt(0, a.offset, 0)).Transformed(w);
        const gp_Dir along = (family == 0 ? gp::DY() : gp::DX()).Transformed(w);
        const gp_Dir across = along.Crossed(gp::DZ());
        positions_.push_back({tr("%1通り").arg(QString::fromStdString(a.name)), at,
                              {across.Reversed(), across},
                              {tr("%1から見る").arg(gridDirectionName(across.Reversed())),
                               tr("%1から見る").arg(gridDirectionName(across))}});
      }
    }
  }
  {
    Bnd_Box all;
    for (const doc::Object& o : document_.objects()) {
      if (!o.shape.IsNull() && document_.isVisible(o.id)) all.Add(document_.worldBounds(o));
    }
    gp_Pnt c(0, 0, 0);
    if (!all.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      all.Get(x0, y0, z0, x1, y1, z1);
      c = gp_Pnt((x0 + x1) / 2, (y0 + y1) / 2, 0);
    }
    positions_.push_back({tr("中央（東西に切る）"), c, {gp_Dir(0, -1, 0), gp_Dir(0, 1, 0)}, {tr("南から見る"), tr("北から見る")}});
    positions_.push_back({tr("中央（南北に切る）"), c, {gp_Dir(1, 0, 0), gp_Dir(-1, 0, 0)}, {tr("東から見る"), tr("西から見る")}});
  }
  for (const SectionPosition& sp : positions_) position_->addItem(sp.label);
  auto fillSides = [this] {
    const int i = position_->currentIndex();
    side_->blockSignals(true);
    side_->clear();
    if (i >= 0 && i < static_cast<int>(positions_.size())) side_->addItems({positions_[i].sides[0], positions_[i].sides[1]});
    side_->blockSignals(false);
  };
  fillSides();

  auto* sheetBox = new QGroupBox(tr("用紙"));
  auto* sheetForm = new QFormLayout(sheetBox);
  scale_ = new QComboBox;
  scale_->setObjectName(QStringLiteral("drawingScale"));
  for (int v : {20, 30, 50, 100, 200, 300, 500}) scale_->addItem(QStringLiteral("1:%1").arg(v), v);
  scale_->setCurrentIndex(scale_->findData(settings.value("drawing/scale", 100).toInt()));
  if (scale_->currentIndex() < 0) scale_->setCurrentIndex(3);
  paper_ = new QComboBox;
  paper_->setObjectName(QStringLiteral("drawingPaper"));
  paper_->addItem(tr("自動（収まる大きさ）"), QStringLiteral("auto"));
  for (const char* p : {"A4", "A3", "A2", "A1"}) paper_->addItem(tr("%1 横").arg(p), QString::fromLatin1(p));
  sheetForm->addRow(tr("縮尺"), scale_);
  sheetForm->addRow(tr("用紙"), paper_);
  left->addWidget(sheetBox);

  auto* contents = new QGroupBox(tr("書き出すもの"));
  auto* c = new QVBoxLayout(contents);
  fill_ = new QCheckBox(tr("切った面の塗り"));
  fill_->setChecked(true);
  hidden_ = new QCheckBox(tr("隠れている線（破線）"));
  grid_ = new QCheckBox(tr("通り芯と符号・スパン"));
  grid_->setChecked(true);
  levels_ = new QCheckBox(tr("レベル（断面・立面）"));
  levels_->setChecked(true);
  annotations_ = new QCheckBox(tr("寸法・文字"));
  annotations_->setChecked(true);
  frame_ = new QCheckBox(tr("図面枠と表題欄"));
  frame_->setObjectName(QStringLiteral("drawingFrame"));
  frame_->setChecked(true);
  for (QCheckBox* b : {fill_, hidden_, grid_, levels_, annotations_, frame_}) c->addWidget(b);
  memberLabels_ = new QCheckBox(tr("部材符号・断面"));
  memberLabels_->setObjectName(QStringLiteral("drawingMemberLabels"));
  c->addWidget(memberLabels_);
  includeHidden_ = new QCheckBox(tr("非表示の物も含める"));
  includeHidden_->setObjectName(QStringLiteral("drawingIncludeHidden"));
  c->addWidget(includeHidden_);
  left->addWidget(contents);
  auto* filterBox = new QGroupBox(tr("対象"));
  auto* filters = new QFormLayout(filterBox);
  within_ = new QComboBox;
  within_->setObjectName(QStringLiteral("drawingWithin"));
  within_->addItem(tr("すべての架構"), QString());
  std::set<std::string> tagNames;
  for (const auto& object : document.objects()) {
    const auto* group = std::get_if<doc::GroupNode>(&object.recipe.base);
    if (group && !group->constructionFrame.empty())
      within_->addItem(QString::fromStdString(object.name), QString::fromStdString(object.id));
    if (!object.isContainer() && !object.shape.IsNull() && !object.attributes.count("grid_part"))
      tagNames.insert(object.layer);
  }
  filters->addRow(tr("架構"), within_);
  tags_ = new QListWidget;
  tags_->setObjectName(QStringLiteral("drawingTags"));
  tags_->setMaximumHeight(100);
  for (const auto& tag : tagNames) {
    auto* item = new QListWidgetItem(QString::fromStdString(tag), tags_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  filters->addRow(tr("タグ（未選択は全体／伏図の既定）"), tags_);
  left->addWidget(filterBox);

  auto* titleBox = new QGroupBox(tr("表題欄"));
  auto* titleForm = new QFormLayout(titleBox);
  project_ = new QLineEdit(settings.value("drawing/project").toString());
  drawingName_ = new QLineEdit;
  drawingName_->setObjectName(QStringLiteral("drawingName"));
  number_ = new QLineEdit;
  author_ = new QLineEdit(settings.value("drawing/author").toString());
  date_ = new QLineEdit(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
  titleForm->addRow(tr("工事名"), project_);
  titleForm->addRow(tr("図面名"), drawingName_);
  titleForm->addRow(tr("図番"), number_);
  titleForm->addRow(tr("作成者"), author_);
  titleForm->addRow(tr("日付"), date_);
  left->addWidget(titleBox);
  left->addStretch();

  auto* right = new QVBoxLayout;
  root->addLayout(right, 1);
  preview_ = new QLabel;
  preview_->setObjectName(QStringLiteral("drawingPreview"));
  preview_->setMinimumSize(560, 400);
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setStyleSheet(QStringLiteral("QLabel { background: #8a8f96; }"));
  right->addWidget(preview_, 1);
  notes_ = new QLabel;
  notes_->setObjectName(QStringLiteral("drawingNotes"));
  notes_->setWordWrap(true);
  right->addWidget(notes_);
  auto* buttons = new QDialogButtonBox;
  QPushButton* exportButton = buttons->addButton(tr("書き出す（PDF・DXF・JWW・DWG）..."), QDialogButtonBox::AcceptRole);
  exportButton->setObjectName(QStringLiteral("drawingExportButton"));
  QPushButton* printButton = buttons->addButton(tr("印刷..."), QDialogButtonBox::ActionRole);
  printButton->setObjectName(QStringLiteral("drawingPrintButton"));
  connect(printButton, &QPushButton::clicked, this, &DrawingDialog::choosePrinterAndPrint);
  auto* quantitiesButton = buttons->addButton(tr("この図の拾い出しCSV..."), QDialogButtonBox::ActionRole);
  quantitiesButton->setObjectName(QStringLiteral("drawingQuantitiesButton"));
  connect(quantitiesButton, &QPushButton::clicked, this, [this] {
    const auto path =
        QFileDialog::getSaveFileName(this, tr("この図の拾い出し"), QString(), tr("CSV (*.csv)"));
    if (!path.isEmpty()) {
      const auto result = exportQuantityCsv(path);
      if (!result) notes_->setText(QString::fromStdString(result.message()));
    }
  });
  buttons->addButton(QDialogButtonBox::Close);
  right->addWidget(buttons);
  connect(exportButton, &QPushButton::clicked, this, &DrawingDialog::chooseFileAndExport);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  timer_ = new QTimer(this);
  timer_->setSingleShot(true);
  timer_->setInterval(250);
  connect(timer_, &QTimer::timeout, this, &DrawingDialog::refreshInBackground);
  poll_ = new QTimer(this);
  poll_->setInterval(40);
  connect(poll_, &QTimer::timeout, this, &DrawingDialog::collectJob);
  auto view = [this] { viewChanged(); };
  auto layout = [this] { layoutChanged(); };
  connect(kind_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, view] {
    if (kind_->currentIndex() >= 3) {
      static const char* keys[] = {"foundation", "first_floor", "second_floor", "roof"};
      const auto preset = nlohmann::json::parse(construction::guideData("timber"))
                              .at("outputs")
                              .at("framing_presets")
                              .at(keys[kind_->currentIndex() - 3]);
      hidden_->setChecked(preset.value("hidden", true));
      const auto stageTag = QString::fromStdString(preset.at("tag").get<std::string>());
      const QSignalBlocker blocker(tags_);
      for (int i = 0; i < tags_->count(); ++i)
        tags_->item(i)->setCheckState(tags_->item(i)->text() == stageTag ? Qt::Checked : Qt::Unchecked);
    } else {
      const QSignalBlocker blocker(tags_);
      for (int i = 0; i < tags_->count(); ++i) tags_->item(i)->setCheckState(Qt::Unchecked);
    }
    memberLabels_->setChecked(kind_->currentIndex() >= 3);
    updateVisibleOptions();
    view();
  });
  connect(within_, qOverload<int>(&QComboBox::currentIndexChanged), this, view);
  connect(tags_, &QListWidget::itemChanged, this, [view](QListWidgetItem*) { view(); });
  connect(memberLabels_, &QCheckBox::toggled, this, view);
  connect(includeHidden_, &QCheckBox::toggled, this, view);
  connect(level_, qOverload<int>(&QComboBox::currentIndexChanged), this, view);
  connect(cutHeight_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, view);
  connect(position_, qOverload<int>(&QComboBox::currentIndexChanged), this, [fillSides, view] {
    fillSides();
    view();
  });
  connect(side_, qOverload<int>(&QComboBox::currentIndexChanged), this, view);
  connect(elevationSide, qOverload<int>(&QComboBox::currentIndexChanged), this, view);
  for (QCheckBox* b : {fill_, hidden_, grid_, levels_, annotations_}) connect(b, &QCheckBox::toggled, this, view);
  connect(frame_, &QCheckBox::toggled, this, layout);
  connect(scale_, qOverload<int>(&QComboBox::currentIndexChanged), this, layout);
  connect(paper_, qOverload<int>(&QComboBox::currentIndexChanged), this, layout);
  for (QLineEdit* e : {project_, number_, author_, date_}) connect(e, &QLineEdit::textEdited, this, layout);
  connect(drawingName_, &QLineEdit::textEdited, this, [this, layout] {
    nameEdited_ = true;
    layout();
  });
  updateVisibleOptions();
  viewChanged();
}

void DrawingDialog::updateVisibleOptions() {
  const int k = kind_->currentIndex();
  planRow_->setEnabled(k == 0);
  sectionRow_->setEnabled(k == 1);
  elevationRow_->setEnabled(k == 2);
}

std::optional<draw::ViewSpec> DrawingDialog::viewSpec(QString* why) const {
  draw::ViewSpec spec;
  const int k = kind_->currentIndex();
  if (k == 0) {
    const double z = level_->currentData().toDouble() + cutHeight_->value();
    spec = draw::planView(z);
    spec.title = (level_->currentText() + tr(" 平面図")).toStdString();
  } else if (k == 1) {
    const int i = position_->currentIndex();
    if (i < 0 || i >= static_cast<int>(positions_.size())) {
      *why = tr("断面の位置を選んでください");
      return std::nullopt;
    }
    const SectionPosition& p = positions_[i];
    spec = draw::sectionView(p.point, p.viewers[std::clamp(side_->currentIndex(), 0, 1)]);
    spec.title = (p.label + tr(" 断面図")).toStdString();
  } else if (k == 2) {
    const int side = static_cast<QComboBox*>(elevationRow_)->currentIndex();
    spec = draw::elevationView(*snapshot_, side);
    static const char* const names[] = {"南立面図", "東立面図", "北立面図", "西立面図"};
    spec.title = names[std::clamp(side, 0, 3)];
  }
  spec.within = within_->currentData().toString().toStdString();
  for (int i = 0; i < tags_->count(); ++i)
    if (tags_->item(i)->checkState() == Qt::Checked)
      spec.tags.push_back(tags_->item(i)->text().toStdString());
  spec.includeHidden = includeHidden_->isChecked();
  spec.memberLabels = memberLabels_->isChecked();
  if (k >= 3) {
    static const char* presets[] = {"foundation", "first_floor", "second_floor", "roof"};
    const auto framing =
        draw::framingView(*snapshot_, presets[k - 3], spec.within, spec.includeHidden, spec.tags);
    if (!framing) {
      *why = QString::fromStdString(framing.error());
      return std::nullopt;
    }
    const bool labels = spec.memberLabels;
    spec = *framing;
    spec.memberLabels = labels;
  }
  spec.hidden = hidden_->isChecked();
  spec.fill = fill_->isChecked();
  spec.grid = grid_->isChecked();
  spec.levels = levels_->isChecked();
  spec.annotations = annotations_->isChecked();
  if (!nameEdited_) {
    // The drawing's name follows the choices until it is typed in.
    drawingName_->setText(QString::fromStdString(spec.title));
  }
  spec.title = drawingName_->text().toStdString();
  return spec;
}

draw::SheetSpec DrawingDialog::sheetSpec() const {
  draw::SheetSpec s;
  s.scale = scale_->currentData().toDouble();
  s.paper = paper_->currentData().toString().toStdString();
  s.frame = frame_->isChecked();
  s.title.project = project_->text().toStdString();
  s.title.drawing = drawingName_->text().toStdString();
  s.title.number = number_->text().toStdString();
  s.title.author = author_->text().toStdString();
  s.title.date = date_->text().toStdString();
  return s;
}

void DrawingDialog::viewChanged() {
  viewDirty_ = true;
  timer_->start();
}

void DrawingDialog::layoutChanged() { timer_->start(); }

DrawingDialog::~DrawingDialog() {
  // A view still being made works on the dialog's own copy of the model:
  // wait for it (hidden line removal cannot be stopped half way).
  if (job_.valid()) job_.wait();
}

bool DrawingDialog::computing() const { return job_.valid(); }

void DrawingDialog::startJob(const draw::ViewSpec& spec) {
  // The model as it was when the dialog opened (its own copy): the window
  // may change the model meanwhile (AI), never the shapes drawn here.
  viewDirty_ = false;
  jobSpec_ = spec;
  ++jobsStarted_;
  job_ = std::async(std::launch::async, [model = snapshot_, spec] { return draw::makeView(*model, spec); });
  poll_->start();
}

void DrawingDialog::finishJob() {
  poll_->stop();
  auto v = job_.get();
  if (!v) {
    view_.reset();
    sheet_.reset();
    error_ = QString::fromStdString(v.error());
    return;
  }
  view_ = std::move(v.value());
}

void DrawingDialog::layOut() {
  error_.clear();
  QString why;
  const auto spec = viewSpec(&why);
  if (!spec) {
    error_ = why;
    sheet_.reset();
    return;
  }
  if (!view_) return;
  view_->spec.title = spec->title;
  auto sheet = draw::layoutSheet(*view_, sheetSpec());
  if (!sheet) {
    sheet_.reset();
    error_ = QString::fromStdString(sheet.error());
  } else {
    sheet_ = std::move(sheet.value());
  }
}

void DrawingDialog::refreshInBackground() {
  timer_->stop();
  if (computing()) return;  // collectJob starts the next one
  QString why;
  const auto spec = viewSpec(&why);
  if (!spec) {
    error_ = why;
    sheet_.reset();
    showPreview();
    return;
  }
  if (viewDirty_ || !view_) {
    startJob(*spec);
    showPreview();  // "計算しています" while the previous drawing stays shown
    return;
  }
  layOut();
  showPreview();
}

void DrawingDialog::collectJob() {
  if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  finishJob();
  if (viewDirty_) {  // changed while it was being made: make it again
    refreshInBackground();
    return;
  }
  if (view_) layOut();
  showPreview();
}

void DrawingDialog::refreshNow() {
  timer_->stop();
  // processEvents below must not let the poller consume this same future.
  const QSignalBlocker blockPolling(poll_);
  // A view being made for older settings is waited for, then made again
  // if the settings changed; the window keeps painting meanwhile.
  for (;;) {
    if (job_.valid()) {
      while (job_.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      }
      finishJob();
    }
    QString why;
    const auto spec = viewSpec(&why);
    if (!spec) {
      error_ = why;
      sheet_.reset();
      showPreview();
      return;
    }
    if (!viewDirty_ && view_) break;
    if (!viewDirty_ && !view_ && !error_.isEmpty()) {  // made, and it failed
      showPreview();
      return;
    }
    startJob(*spec);
  }
  layOut();
  showPreview();
}

void DrawingDialog::showPreview() {
  QStringList notes;
  if (computing()) notes << tr("図を計算しています…（その間も設定を変えられます）");
  if (!error_.isEmpty()) notes << error_;
  if (view_) {
    for (const std::string& n : view_->notes) notes << QString::fromStdString(n);
  }
  if (sheet_) {
    for (const std::string& n : sheet_->notes) notes << QString::fromStdString(n);
    notes.prepend(tr("%1 横・1:%2").arg(QString::fromStdString(sheet_->paper)).arg(sheet_->scale));
  }
  notes_->setText(notes.join(QStringLiteral("\n")));
  QImage image(preview_->size() * devicePixelRatioF(), QImage::Format_RGB32);
  image.setDevicePixelRatio(devicePixelRatioF());
  image.fill(QColor(0x8a, 0x8f, 0x96));
  if (sheet_) {
    QPainter p(&image);
    renderSheet(p, *sheet_, QRectF(QPointF(8, 8), QSizeF(preview_->size()) - QSizeF(16, 16)));
  }
  preview_->setPixmap(QPixmap::fromImage(image));
}

Status DrawingDialog::exportQuantityCsv(const QString& path) {
  QString why;
  const auto spec = viewSpec(&why);
  if (!spec) return Status::failure(why.toStdString());
  const auto report = construction::takeOff(
      *snapshot_,
      nlohmann::json{{"within", spec->within}, {"tags", spec->tags}, {"visible_only", !spec->includeHidden}}
          .dump());
  if (!report) return Status::failure(report.error());
  const auto csv = construction::takeOffCsv(*report);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(csv.data(), static_cast<qint64>(csv.size())) != static_cast<qint64>(csv.size()) ||
      !file.commit())
    return Status::failure("CSVを保存できませんでした");
  return Status::success();
}

Status DrawingDialog::exportTo(const QString& path) {
  if (timer_->isActive() || !sheet_) refreshNow();
  if (!sheet_) return Status::failure(error_.isEmpty() ? "Nothing to write" : error_.toStdString());
  const QString ext = QFileInfo(path).suffix().toLower();
  Status s = Status::failure("Unknown format: choose .pdf, .dxf, .jww or .dwg");
  if (ext == QLatin1String("pdf")) s = writePdf(*sheet_, path);
  else if (ext == QLatin1String("dxf")) s = draw::writeDxf(*sheet_, path.toStdString());
  else if (ext == QLatin1String("jww")) s = draw::writeJww(*sheet_, path.toStdString());
  else if (ext == QLatin1String("dwg")) {
    // DWG: our DXF converted by ODA File Converter (installed separately).
    QTemporaryDir temp;
    const QString dxf = temp.filePath(QStringLiteral("drawing.dxf"));
    s = temp.isValid() ? draw::writeDxf(*sheet_, dxf.toStdString()) : Status::failure("No temporary folder");
    if (s) s = convertDxfToDwg(dxf, QSettings().value("import/odaConverter").toString(), path, this);
  }
  if (s) {
    QSettings settings;
    settings.setValue("drawing/cutHeight", cutHeight_->value());
    settings.setValue("drawing/scale", scale_->currentData());
    settings.setValue("drawing/project", project_->text());
    settings.setValue("drawing/author", author_->text());
  }
  return s;
}

Status DrawingDialog::printTo(QPrinter& printer, bool* scaled) {
  if (timer_->isActive() || !sheet_) refreshNow();
  if (!sheet_) return Status::failure(error_.isEmpty() ? "Nothing to print" : error_.toStdString());
  return printSheet(*sheet_, printer, scaled);
}

void DrawingDialog::choosePrinterAndPrint() {
  if (timer_->isActive() || !sheet_) refreshNow();
  if (!sheet_) return;
  QPrinter printer(QPrinter::HighResolution);
  setUpPrinter(*sheet_, printer);
  QPrintDialog dialog(&printer, this);
  dialog.setWindowTitle(tr("図面を印刷"));
  if (dialog.exec() != QDialog::Accepted) return;
  bool scaled = false;
  const Status s = printTo(printer, &scaled);
  if (!s) {
    error_ = tr("印刷できませんでした: %1").arg(QString::fromStdString(s.message()));
    showPreview();
    return;
  }
  if (scaled) {
    QMessageBox::information(this, tr("図面を印刷"),
                             tr("選んだ用紙が図面（%1）より小さいため、縮小して印刷しました。縮尺どおりではありません。")
                                 .arg(QString::fromStdString(sheet_->paper)));
  }
}

void DrawingDialog::chooseFileAndExport() {
  QSettings settings;
  const QString last = settings.value("drawing/format", QStringLiteral("pdf")).toString();
  const QStringList filters{tr("PDF（印刷用） (*.pdf)"), tr("DXF（AutoCAD・Jw_cad など） (*.dxf)"), tr("Jw_cad (*.jww)"),
                            tr("DWG（ODA File Converter で変換） (*.dwg)")};
  QString selected = last == QLatin1String("dxf")   ? filters[1]
                     : last == QLatin1String("jww") ? filters[2]
                     : last == QLatin1String("dwg") ? filters[3]
                                                    : filters[0];
  QString name = drawingName_->text().trimmed();
  if (name.isEmpty()) name = tr("図面");
  QString path = QFileDialog::getSaveFileName(this, tr("図面を書き出す"),
                                              settings.value("drawing/dir").toString() + QLatin1Char('/') + name,
                                              filters.join(QStringLiteral(";;")), &selected);
  if (path.isEmpty()) return;
  const QString ext = selected == filters[1]   ? QStringLiteral("dxf")
                      : selected == filters[2] ? QStringLiteral("jww")
                      : selected == filters[3] ? QStringLiteral("dwg")
                                               : QStringLiteral("pdf");
  if (QFileInfo(path).suffix().isEmpty()) path += QLatin1Char('.') + ext;
  if (QFileInfo(path).suffix().toLower() == QLatin1String("dwg") &&
      QSettings().value("import/odaConverter").toString().isEmpty()) {
    // Asked for once, as importing DWG does.
    QMessageBox::information(this, tr("DWG で書き出す"),
                             tr("DWG で書き出すには ODA File Converter の別途インストールが必要です。\n"
                                "インストール済みの場合は、次の画面で実行ファイルを選択してください。\n"
                                "公式配布元: https://www.opendesign.com/guestfiles/oda_file_converter"));
    const QString exe = QFileDialog::getOpenFileName(this, tr("ODA File Converter の実行ファイルを選択"));
    if (exe.isEmpty() || !QFileInfo(exe).isExecutable()) return;
    QSettings().setValue("import/odaConverter", exe);
  }
  settings.setValue("drawing/format", QFileInfo(path).suffix().toLower());
  settings.setValue("drawing/dir", QFileInfo(path).absolutePath());
  const Status s = exportTo(path);
  if (!s) {
    error_ = tr("書き出せませんでした: %1").arg(QString::fromStdString(s.message()));
    showPreview();
    return;
  }
  notes_->setText(tr("書き出しました: %1").arg(QDir::toNativeSeparators(path)));
}

}  // namespace sbr::app
