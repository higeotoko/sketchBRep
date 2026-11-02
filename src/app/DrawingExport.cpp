#include "DrawingExport.h"

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
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <Bnd_Box.hxx>

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

DrawingDialog::DrawingDialog(const doc::Document& document, QWidget* parent) : QDialog(parent), document_(document) {
  setWindowTitle(tr("図面を書き出す（平面・断面・立面）"));
  setObjectName(QStringLiteral("drawingDialog"));
  QSettings settings;
  auto* root = new QHBoxLayout(this);
  auto* left = new QVBoxLayout;
  root->addLayout(left);

  auto* what = new QGroupBox(tr("図面"));
  auto* form = new QFormLayout(what);
  kind_ = new QComboBox;
  kind_->setObjectName(QStringLiteral("drawingKind"));
  kind_->addItems({tr("平面図"), tr("断面図"), tr("立面図")});
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
  left->addWidget(contents);

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
  QPushButton* exportButton = buttons->addButton(tr("書き出す（PDF・DXF・JWW）..."), QDialogButtonBox::AcceptRole);
  exportButton->setObjectName(QStringLiteral("drawingExportButton"));
  buttons->addButton(QDialogButtonBox::Close);
  right->addWidget(buttons);
  connect(exportButton, &QPushButton::clicked, this, &DrawingDialog::chooseFileAndExport);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  timer_ = new QTimer(this);
  timer_->setSingleShot(true);
  timer_->setInterval(250);
  connect(timer_, &QTimer::timeout, this, &DrawingDialog::refreshNow);
  auto view = [this] { viewChanged(); };
  auto layout = [this] { layoutChanged(); };
  connect(kind_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, view] {
    updateVisibleOptions();
    view();
  });
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
  } else {
    const int side = static_cast<QComboBox*>(elevationRow_)->currentIndex();
    spec = draw::elevationView(document_, side);
    static const char* const names[] = {"南立面図", "東立面図", "北立面図", "西立面図"};
    spec.title = names[std::clamp(side, 0, 3)];
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

void DrawingDialog::refreshNow() {
  timer_->stop();
  error_.clear();
  QString why;
  const auto spec = viewSpec(&why);
  if (!spec) {
    error_ = why;
    sheet_.reset();
    showPreview();
    return;
  }
  if (viewDirty_ || !view_) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto v = draw::makeView(document_, *spec);
    QApplication::restoreOverrideCursor();
    viewDirty_ = false;
    if (!v) {
      view_.reset();
      sheet_.reset();
      error_ = QString::fromStdString(v.error());
      showPreview();
      return;
    }
    view_ = std::move(v.value());
  }
  view_->spec.title = spec->title;
  auto sheet = draw::layoutSheet(*view_, sheetSpec());
  if (!sheet) {
    sheet_.reset();
    error_ = QString::fromStdString(sheet.error());
  } else {
    sheet_ = std::move(sheet.value());
  }
  showPreview();
}

void DrawingDialog::showPreview() {
  QStringList notes;
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

Status DrawingDialog::exportTo(const QString& path) {
  if (timer_->isActive() || !sheet_) refreshNow();
  if (!sheet_) return Status::failure(error_.isEmpty() ? "Nothing to write" : error_.toStdString());
  const QString ext = QFileInfo(path).suffix().toLower();
  Status s = Status::failure("Unknown format: choose .pdf, .dxf or .jww");
  if (ext == QLatin1String("pdf")) s = writePdf(*sheet_, path);
  else if (ext == QLatin1String("dxf")) s = draw::writeDxf(*sheet_, path.toStdString());
  else if (ext == QLatin1String("jww")) s = draw::writeJww(*sheet_, path.toStdString());
  if (s) {
    QSettings settings;
    settings.setValue("drawing/cutHeight", cutHeight_->value());
    settings.setValue("drawing/scale", scale_->currentData());
    settings.setValue("drawing/project", project_->text());
    settings.setValue("drawing/author", author_->text());
  }
  return s;
}

void DrawingDialog::chooseFileAndExport() {
  QSettings settings;
  const QString last = settings.value("drawing/format", QStringLiteral("pdf")).toString();
  const QStringList filters{tr("PDF（印刷用） (*.pdf)"), tr("DXF（AutoCAD・Jw_cad など） (*.dxf)"), tr("Jw_cad (*.jww)")};
  QString selected = last == QLatin1String("dxf") ? filters[1] : last == QLatin1String("jww") ? filters[2] : filters[0];
  QString name = drawingName_->text().trimmed();
  if (name.isEmpty()) name = tr("図面");
  QString path = QFileDialog::getSaveFileName(this, tr("図面を書き出す"),
                                              settings.value("drawing/dir").toString() + QLatin1Char('/') + name,
                                              filters.join(QStringLiteral(";;")), &selected);
  if (path.isEmpty()) return;
  const QString ext = selected == filters[1] ? QStringLiteral("dxf") : selected == filters[2] ? QStringLiteral("jww") : QStringLiteral("pdf");
  if (QFileInfo(path).suffix().isEmpty()) path += QLatin1Char('.') + ext;
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
