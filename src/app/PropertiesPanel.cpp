#include "PropertiesPanel.h"

#include <algorithm>
#include <cmath>

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Validation.h"

namespace sbr::app {
namespace {

QString kindLabel(doc::ObjectKind kind) {
  switch (kind) {
    case doc::ObjectKind::Solid: return QObject::tr("ソリッド");
    case doc::ObjectKind::Face: return QObject::tr("面");
    case doc::ObjectKind::Curve: return QObject::tr("線");
    case doc::ObjectKind::Empty: return QObject::tr("空");
    case doc::ObjectKind::Other: break;
  }
  return QObject::tr("その他");
}

QString parameterLabel(const doc::Parameter& p) {
  if (p.key == "width") return QObject::tr("幅");
  if (p.key == "height") return QObject::tr("奥行");
  if (p.label == "Extrude distance") return QObject::tr("押出距離");
  if (p.key.rfind("op", 0) == 0) {
    return QObject::tr("プッシュ/プル %1").arg(QString::fromStdString(p.key.substr(2, p.key.find('.') - 2)));
  }
  return QString::fromStdString(p.label);
}

}  // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);

  layout->addWidget(new QLabel(tr("<b>オブジェクト</b>")));
  tree_ = new QTreeWidget;
  tree_->setColumnCount(2);
  tree_->setHeaderLabels({tr("名前"), tr("種類")});
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setRootIsDecorated(false);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  layout->addWidget(tree_, 1);

  layout->addWidget(new QLabel(tr("<b>属性・パラメータ</b>")));
  formHost_ = new QWidget;
  form_ = new QFormLayout(formHost_);
  form_->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(formHost_, 1);

  connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (updating_) return;
    std::vector<doc::ObjectId> ids;
    for (QTreeWidgetItem* item : tree_->selectedItems()) {
      ids.push_back(item->data(0, Qt::UserRole).toString().toStdString());
    }
    emit selectionChanged(ids);
  });
  connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
    if (updating_ || column != 0) return;
    emit visibilityToggled(item->data(0, Qt::UserRole).toString().toStdString(),
                           item->checkState(0) == Qt::Checked);
  });
}

void PropertiesPanel::refresh(const doc::Modeler& modeler,
                              const std::vector<doc::ObjectId>& selection) {
  updating_ = true;
  tree_->clear();
  for (const doc::Object& o : modeler.document().objects()) {
    auto* item = new QTreeWidgetItem(tree_);
    item->setText(0, QString::fromStdString(o.name));
    item->setText(1, kindLabel(o.kind()));
    item->setData(0, Qt::UserRole, QString::fromStdString(o.id));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, o.visible ? Qt::Checked : Qt::Unchecked);
    item->setToolTip(0, tr("チェックで表示/非表示"));
    if (std::find(selection.begin(), selection.end(), o.id) != selection.end()) {
      item->setSelected(true);
    }
  }
  updating_ = false;
  rebuildForm(modeler, selection);
}

void PropertiesPanel::rebuildForm(const doc::Modeler& modeler,
                                  const std::vector<doc::ObjectId>& selection) {
  while (form_->rowCount() > 0) form_->removeRow(0);
  if (selection.size() != 1) {
    form_->addRow(new QLabel(selection.empty() ? tr("未選択") : tr("%1 個選択").arg(selection.size())));
    return;
  }
  const doc::Object* o = modeler.document().find(selection.front());
  if (!o) return;
  const doc::ObjectId id = o->id;

  auto* name = new QLineEdit(QString::fromStdString(o->name));
  connect(name, &QLineEdit::editingFinished, this, [this, id, name] {
    emit renameRequested(id, name->text());
  });
  form_->addRow(tr("名前"), name);
  auto* idLabel = new QLabel(QString::fromStdString(o->id));
  idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  idLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 9pt"));
  form_->addRow(tr("ID"), idLabel);
  form_->addRow(tr("レシピ"), new QLabel(QString::fromStdString(doc::describe(o->recipe))));

  if (o->kind() == doc::ObjectKind::Solid) {
    const geom::ValidationReport r = geom::validateShape(o->shape, true);
    form_->addRow(tr("体積"), new QLabel(QString::number(r.volume / 1e9, 'f', 6) + tr(" m³")));
    form_->addRow(tr("B-rep"), new QLabel(r.valid ? tr("有効 (面 %1 / 稜線 %2)").arg(r.faces).arg(r.edges)
                                                  : tr("不正")));
  } else if (o->kind() == doc::ObjectKind::Face) {
    form_->addRow(tr("面積"), new QLabel(QString::number(geom::area(o->shape) / 1e6, 'f', 6) + tr(" m²")));
  }

  for (const doc::Parameter& p : modeler.parameters(id)) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(-1e9, 1e9);
    spin->setDecimals(3);
    spin->setSuffix(p.isLength ? tr(" mm") : QString());
    spin->setKeyboardTracking(false);
    spin->setValue(p.value);
    const std::string key = p.key;
    const double initial = p.value;
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, key, spin, initial] {
      if (std::abs(spin->value() - initial) > 1e-9) emit parameterEdited(id, key, spin->value());
    });
    form_->addRow(parameterLabel(p), spin);
  }
}

}  // namespace sbr::app
