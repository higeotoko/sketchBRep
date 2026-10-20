#include "PropertiesPanel.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
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
    case doc::ObjectKind::Group: return QObject::tr("グループ");
    case doc::ObjectKind::Instance: return QObject::tr("コンポーネント");
    case doc::ObjectKind::Empty: return QObject::tr("空");
    case doc::ObjectKind::Other: break;
  }
  return QObject::tr("その他");
}

QString parameterLabel(const doc::Parameter& p) {
  // Boolean operands: "<operand name>: <label>"; translate the tail only.
  const auto colon = p.label.rfind(": ");
  const std::string tail = colon == std::string::npos ? p.label : p.label.substr(colon + 2);
  const QString head = colon == std::string::npos
                           ? QString()
                           : QString::fromStdString(p.label.substr(0, colon)) + QStringLiteral(": ");
  QString label;
  if (tail == "Width") {
    label = QObject::tr("幅");
  } else if (tail == "Height") {
    label = QObject::tr("奥行");
  } else if (tail == "Extrude distance") {
    label = QObject::tr("押出距離");
  } else if (tail == "Offset distance") {
    label = QObject::tr("オフセット距離");
  } else if (tail == "Revolve angle") {
    label = QObject::tr("回転角 (°)");
  } else if (tail.rfind("Push/Pull", 0) == 0) {
    label = QObject::tr("プッシュ/プル %1").arg(QString::fromStdString(tail.substr(10, tail.find(' ', 10) - 10)));
  } else {
    label = QString::fromStdString(tail);
  }
  return head + label;
}

QDoubleSpinBox* lengthSpin(double value) {
  auto* spin = new QDoubleSpinBox;
  spin->setRange(-1e9, 1e9);
  spin->setDecimals(3);
  spin->setSuffix(QObject::tr(" mm"));
  spin->setKeyboardTracking(false);
  spin->setValue(value);
  return spin;
}

}  // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);

  layout->addWidget(new QLabel(tr("<b>オブジェクト</b>")));
  tree_ = new QTreeWidget;
  tree_->setColumnCount(3);
  tree_->setHeaderLabels({tr("名前"), tr("種類"), tr("タグ")});
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  layout->addWidget(tree_, 1);

  layout->addWidget(new QLabel(tr("<b>属性・パラメータ</b>")));
  formHost_ = new QWidget;
  form_ = new QFormLayout(formHost_);
  form_->setContentsMargins(0, 0, 0, 0);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(formHost_);
  layout->addWidget(scroll, 1);

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
  std::map<doc::ObjectId, QTreeWidgetItem*> items;
  const auto& objects = modeler.document().objects();
  // Parents may appear after their children in the list: create in passes.
  std::vector<const doc::Object*> pending;
  for (const doc::Object& o : objects) pending.push_back(&o);
  while (!pending.empty()) {
    std::vector<const doc::Object*> next;
    for (const doc::Object* o : pending) {
      QTreeWidgetItem* parentItem = nullptr;
      if (!o->parent.empty()) {
        const auto it = items.find(o->parent);
        if (it == items.end()) {
          if (modeler.document().find(o->parent)) {
            next.push_back(o);
            continue;
          }
        } else {
          parentItem = it->second;
        }
      }
      auto* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree_);
      item->setText(0, QString::fromStdString(o->name));
      item->setText(1, kindLabel(o->kind()));
      item->setText(2, QString::fromStdString(o->layer));
      item->setData(0, Qt::UserRole, QString::fromStdString(o->id));
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      item->setCheckState(0, o->visible ? Qt::Checked : Qt::Unchecked);
      item->setToolTip(0, tr("チェックで表示/非表示"));
      items[o->id] = item;
    }
    if (next.size() == pending.size()) break;  // broken hierarchy: stop
    pending = std::move(next);
  }
  for (const doc::ObjectId& id : selection) {
    const auto it = items.find(id);
    if (it != items.end()) it->second->setSelected(true);
  }
  tree_->expandAll();
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
  const doc::Document& document = modeler.document();
  const doc::Object* o = document.find(selection.front());
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

  auto* tag = new QLineEdit(QString::fromStdString(o->layer));
  connect(tag, &QLineEdit::editingFinished, this, [this, id, tag] { emit tagEdited(id, tag->text()); });
  form_->addRow(tr("タグ"), tag);

  if (o->kind() != doc::ObjectKind::Group) {
    auto* color = new QPushButton;
    const QColor qc = QColor::fromRgbF(o->color.r, o->color.g, o->color.b);
    color->setStyleSheet(QStringLiteral("background-color: %1").arg(qc.name()));
    color->setText(qc.name());
    connect(color, &QPushButton::clicked, this, [this, id, qc] {
      const QColor chosen = QColorDialog::getColor(qc, this, tr("マテリアル色"));
      if (chosen.isValid()) emit colorEdited(id, doc::Color{chosen.redF(), chosen.greenF(), chosen.blueF()});
    });
    form_->addRow(tr("色"), color);
  }

  // Local position (translation part of the placement).
  {
    const gp_XYZ t = o->transform.TranslationPart();
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    QDoubleSpinBox* spins[3] = {lengthSpin(t.X()), lengthSpin(t.Y()), lengthSpin(t.Z())};
    for (QDoubleSpinBox* spin : spins) {
      spin->setSuffix(QString());
      spin->setDecimals(1);
      h->addWidget(spin);
      connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, spins, t] {
        const double x = spins[0]->value(), y = spins[1]->value(), z = spins[2]->value();
        if (std::abs(x - t.X()) + std::abs(y - t.Y()) + std::abs(z - t.Z()) > 1e-9) {
          emit positionEdited(id, x, y, z);
        }
      });
    }
    form_->addRow(tr("位置 (mm)"), row);
  }

  form_->addRow(tr("レシピ"), new QLabel(QString::fromStdString(doc::describe(o->recipe))));

  switch (o->kind()) {
    case doc::ObjectKind::Solid: {
      const geom::ValidationReport r = geom::validateShape(o->shape, false);
      form_->addRow(tr("体積"), new QLabel(QString::number(r.volume / 1e9, 'f', 6) + tr(" m³")));
      form_->addRow(tr("B-rep"), new QLabel(r.valid ? tr("有効 (ソリッド %1 / 面 %2 / 稜線 %3)")
                                                          .arg(r.solids).arg(r.faces).arg(r.edges)
                                                    : tr("不正")));
      break;
    }
    case doc::ObjectKind::Face:
      form_->addRow(tr("面積"), new QLabel(QString::number(geom::area(o->shape) / 1e6, 'f', 6) + tr(" m²")));
      break;
    case doc::ObjectKind::Group:
      form_->addRow(tr("子要素"), new QLabel(QString::number(document.children(id).size())));
      break;
    default:
      break;
  }

  for (const doc::Parameter& p : modeler.parameters(id)) {
    auto* spin = lengthSpin(p.value);
    if (!p.isLength) spin->setSuffix(QString());
    const std::string key = p.key;
    const double initial = p.value;
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, key, spin, initial] {
      if (std::abs(spin->value() - initial) > 1e-9) emit parameterEdited(id, key, spin->value());
    });
    form_->addRow(parameterLabel(p), spin);
  }

  // Component instance: edit the shared definition.
  if (const auto* ref = std::get_if<doc::ComponentInstance>(&o->recipe.base)) {
    const doc::ComponentDefinition* def = document.findDefinition(ref->definitionId);
    if (!def) return;
    const std::string defId = def->id;
    auto* header = new QLabel(tr("<b>コンポーネント定義</b>（%1 インスタンス共通）")
                                  .arg(document.instanceCount(defId)));
    form_->addRow(header);
    auto* defName = new QLineEdit(QString::fromStdString(def->name));
    connect(defName, &QLineEdit::editingFinished, this, [this, defId, defName] {
      emit definitionRenamed(defId, defName->text());
    });
    form_->addRow(tr("定義名"), defName);
    for (const doc::Object& child : def->objects) {
      for (const doc::Parameter& p : modeler.definitionParameters(defId, child.id)) {
        auto* spin = lengthSpin(p.value);
        const std::string key = p.key;
        const doc::ObjectId childId = child.id;
        const double initial = p.value;
        connect(spin, &QDoubleSpinBox::editingFinished, this,
                [this, defId, childId, key, spin, initial] {
                  if (std::abs(spin->value() - initial) > 1e-9) {
                    emit definitionParameterEdited(defId, childId, key, spin->value());
                  }
                });
        form_->addRow(QString::fromStdString(child.name) + QStringLiteral(": ") + parameterLabel(p), spin);
      }
    }
  }
}

}  // namespace sbr::app
