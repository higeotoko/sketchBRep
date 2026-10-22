#include "MaterialsPanel.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace sbr::app {
namespace {

QIcon swatch(const doc::Color& c, bool inherit = false) {
  QPixmap pixmap(28, 20);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(90, 90, 90), 1));
  if (inherit) {
    p.setBrush(Qt::white);
    p.drawRoundedRect(QRectF(1.5, 1.5, 25, 17), 3, 3);
    p.drawLine(QPointF(4, 17), QPointF(24, 3));  // "none"
  } else {
    p.setBrush(QColor::fromRgbF(c.r, c.g, c.b));
    p.drawRoundedRect(QRectF(1.5, 1.5, 25, 17), 3, 3);
  }
  return QIcon(pixmap);
}

}  // namespace

const std::vector<MaterialsPanel::Preset>& MaterialsPanel::presets() {
  static const std::vector<Preset> all = {
      {"木（明るい）", {0.80, 0.64, 0.44}}, {"木（濃い）", {0.48, 0.32, 0.20}}, {"漆喰（白）", {0.95, 0.94, 0.90}},
      {"コンクリート", {0.66, 0.66, 0.64}}, {"モルタル", {0.74, 0.72, 0.68}},  {"レンガ", {0.63, 0.32, 0.22}},
      {"タイル（グレー）", {0.52, 0.54, 0.56}}, {"畳", {0.73, 0.73, 0.48}},  {"ガラス", {0.62, 0.80, 0.88}},
      {"金属（ステンレス）", {0.76, 0.77, 0.78}}, {"黒（鉄）", {0.18, 0.18, 0.19}}, {"芝", {0.40, 0.60, 0.28}},
  };
  return all;
}

MaterialsPanel::MaterialsPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  list_ = new QListWidget;
  list_->setObjectName(QStringLiteral("materialList"));
  list_->setIconSize(QSize(28, 20));
  list_->setToolTip(tr("クリックで塗る素材に（ペイントツールに切り替え）。ダブルクリックで名前・色を変更"));
  layout->addWidget(list_);
  auto* row = new QHBoxLayout;
  auto* add = new QPushButton(tr("＋ 新規"));
  auto* presets = new QPushButton(tr("定番 ▾"));
  edit_ = new QPushButton(tr("変更"));
  remove_ = new QPushButton(tr("削除"));
  row->addWidget(add);
  row->addWidget(presets);
  row->addWidget(edit_);
  row->addWidget(remove_);
  layout->addLayout(row);
  apply_ = new QPushButton(tr("選択に適用"));
  apply_->setToolTip(tr("選んでいる部材（面を選んでいればその面）にこの素材を割り当てます"));
  layout->addWidget(apply_);

  auto* menu = new QMenu(presets);
  for (int i = 0; i < static_cast<int>(MaterialsPanel::presets().size()); ++i) {
    const Preset& p = MaterialsPanel::presets()[static_cast<std::size_t>(i)];
    menu->addAction(swatch(p.color), QString::fromUtf8(p.name), this, [this, i] { emit presetRequested(i); });
  }
  presets->setMenu(menu);

  connect(add, &QPushButton::clicked, this, [this] { emit createRequested(); });
  connect(edit_, &QPushButton::clicked, this, [this] {
    if (!currentId().empty()) emit editRequested(currentId());
  });
  connect(remove_, &QPushButton::clicked, this, [this] {
    if (!currentId().empty()) emit deleteRequested(currentId());
  });
  connect(apply_, &QPushButton::clicked, this, [this] { emit applyRequested(currentId()); });
  connect(list_, &QListWidget::currentRowChanged, this, [this](int) {
    const bool real = !currentId().empty();
    edit_->setEnabled(real);
    remove_->setEnabled(real);
    if (!updating_) emit materialChosen(currentId());
  });
  connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
    if (!currentId().empty()) emit editRequested(currentId());
  });
}

std::string MaterialsPanel::currentId() const {
  const QListWidgetItem* item = list_->currentItem();
  return item ? item->data(Qt::UserRole).toString().toStdString() : std::string();
}

void MaterialsPanel::refresh(const doc::Document& document, const std::string& current) {
  updating_ = true;
  if (document.materials() != shown_ || list_->count() == 0) {
    shown_ = document.materials();
    list_->clear();
    auto* inherit = new QListWidgetItem(swatch({}, true), tr("（素材なし：継承に戻す）"));
    inherit->setData(Qt::UserRole, QString());
    inherit->setToolTip(tr("塗ると面・部材の個別の素材を外し、部材全体・グループの素材に戻します"));
    list_->addItem(inherit);
    for (const doc::Material& m : shown_) {
      auto* item = new QListWidgetItem(swatch(m.color), QString::fromStdString(m.name));
      item->setData(Qt::UserRole, QString::fromStdString(m.id));
      list_->addItem(item);
    }
  }
  for (int i = 0; i < list_->count(); ++i) {
    if (list_->item(i)->data(Qt::UserRole).toString().toStdString() == current) list_->setCurrentRow(i);
  }
  const bool real = !currentId().empty();
  edit_->setEnabled(real);
  remove_->setEnabled(real);
  updating_ = false;
}

}  // namespace sbr::app
