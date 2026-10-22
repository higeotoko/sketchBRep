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
#include <QToolButton>
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
    case doc::ObjectKind::Shell: return QObject::tr("開いた形状");
    case doc::ObjectKind::Curve: return QObject::tr("線");
    case doc::ObjectKind::Group: return QObject::tr("グループ");
    case doc::ObjectKind::Instance: return QObject::tr("コンポーネント");
    case doc::ObjectKind::Annotation: return QObject::tr("注記");
    case doc::ObjectKind::Guide: return QObject::tr("ガイド");
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
  } else if (tail == "Radius") {
    label = QObject::tr("半径");
  } else if (tail == "Sides") {
    label = QObject::tr("辺の数");
  } else if (tail == "Bulge") {
    label = QObject::tr("膨らみ");
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

// Whole-number parameters (polygon sides, also inside boolean operands).
void adjustForKey(QDoubleSpinBox* spin, const std::string& key) {
  if (key.size() >= 5 && key.compare(key.size() - 5, 5, "sides") == 0) {
    spin->setDecimals(0);
    spin->setRange(3, 256);
  }
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
                              const std::vector<doc::ObjectId>& selection,
                              const QString& elementSummary) {
  updating_ = true;
  const bool rebuild = treeDocument_ != &modeler.document() || treeSequence_ != modeler.document().sequence() ||
                       modeler.document().sequence() == 0;
  if (rebuild) {
    treeDocument_ = &modeler.document();
    treeSequence_ = modeler.document().sequence();
    tree_->clear();
    treeItems_.clear();
    std::map<doc::ObjectId, QTreeWidgetItem*>& items = treeItems_;
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
    tree_->expandAll();
  }
  tree_->clearSelection();
  for (const doc::ObjectId& id : selection) {
    const auto it = treeItems_.find(id);
    if (it != treeItems_.end()) it->second->setSelected(true);
  }
  updating_ = false;
  rebuildForm(modeler, selection, elementSummary);
}

void PropertiesPanel::rebuildForm(const doc::Modeler& modeler,
                                  const std::vector<doc::ObjectId>& selection,
                                  const QString& elementSummary) {
  while (form_->rowCount() > 0) form_->removeRow(0);
  if (selection.size() != 1) {
    QString text = selection.empty() ? tr("未選択") : tr("%1 個のオブジェクト").arg(selection.size());
    if (!elementSummary.isEmpty()) text = tr("選択: %1").arg(elementSummary) + QStringLiteral("\n") + text;
    form_->addRow(new QLabel(text));
    return;
  }
  const doc::Document& document = modeler.document();
  const doc::Object* o = document.find(selection.front());
  if (!o) return;
  const doc::ObjectId id = o->id;

  // ===== Basic: what you need while modelling =====================================
  auto* name = new QLineEdit(QString::fromStdString(o->name));
  connect(name, &QLineEdit::editingFinished, this, [this, id, name] {
    emit renameRequested(id, name->text());
  });
  form_->addRow(tr("名前"), name);
  if (!elementSummary.isEmpty()) form_->addRow(tr("選択"), new QLabel(elementSummary));
  if (o->kind() == doc::ObjectKind::Shell) {
    auto* hint = new QLabel(tr("開いた形状です（面を消した状態）。描く・塗る・面を消すはそのまま続けられます。"
                               "押し出し・ブール演算は「編集 > 穴をふさぐ」でソリッドに戻してから"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("QLabel { color: #8a5a00; }"));
    form_->addRow(hint);
  }
  // Material: own, else where it comes from; faces painted on their own.
  {
    auto nameOf = [&](const std::string& mid) {
      const doc::Material* mat = document.findMaterial(mid);
      return mat ? QString::fromStdString(mat->name) : QString();
    };
    const std::string shown = document.inheritedMaterial(*o);
    QString text = !o->material.empty() ? nameOf(o->material)
                   : !shown.empty()     ? tr("%1（グループ・配置から）").arg(nameOf(shown))
                                        : tr("なし");
    if (!o->faceMaterials.empty()) {
      const doc::FaceMaterialMap faces = doc::resolveFaceMaterials(*o);
      text += tr(" ／ 面ごと %1").arg(faces.faces.size());
      if (faces.unresolved) text += tr("（%1 件は形の変更で面を特定できず未適用）").arg(faces.unresolved);
    }
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("materialLabel"));
    label->setToolTip(tr("ペイントツール（B）と「素材」パネルで変更。面の素材が部材全体より優先されます"));
    form_->addRow(tr("素材"), label);
  }
  if (o->kind() == doc::ObjectKind::Annotation) {
    // Dimensions: empty = measured value.
    const bool dimension = std::holds_alternative<doc::LinearDimension>(o->recipe.base);
    const auto* d = std::get_if<doc::LinearDimension>(&o->recipe.base);
    const auto* t = std::get_if<doc::TextNote>(&o->recipe.base);
    auto* text = new QLineEdit(QString::fromStdString(d ? d->text : t ? t->text : std::string()));
    if (dimension) {
      const auto label = doc::annotationLabel(o->recipe);
      text->setPlaceholderText(label ? tr("自動: %1").arg(QString::fromStdString(label->text)) : QString());
    }
    connect(text, &QLineEdit::editingFinished, this, [this, id, text] { emit annotationTextEdited(id, text->text()); });
    form_->addRow(tr("テキスト"), text);
  }

  auto addParameter = [&](QFormLayout* target, const doc::Parameter& p) {
    auto* spin = lengthSpin(p.value);
    if (!p.isLength) spin->setSuffix(QString());
    adjustForKey(spin, p.key);
    const std::string key = p.key;
    const double initial = p.value;
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, key, spin, initial] {
      if (std::abs(spin->value() - initial) > 1e-9) emit parameterEdited(id, key, spin->value());
    });
    target->addRow(parameterLabel(p), spin);
  };

  // Own dimensions are basic; dimensions of nested inputs (Boolean operands,
  // loft sections, ...) live in the details.
  std::vector<doc::Parameter> nested;
  for (const doc::Parameter& p : modeler.parameters(id)) {
    if (p.key.rfind("in", 0) == 0) {
      nested.push_back(p);
    } else {
      addParameter(form_, p);
    }
  }

  const doc::ComponentDefinition* def = nullptr;
  if (const auto* ref = std::get_if<doc::ComponentInstance>(&o->recipe.base)) {
    def = document.findDefinition(ref->definitionId);
  }
  if (def) {
    const std::string defId = def->id;
    form_->addRow(new QLabel(tr("<b>共通寸法</b>（%1 個のコンポーネントに反映）")
                                 .arg(document.instanceCount(defId))));
    for (const doc::Object& child : def->objects) {
      for (const doc::Parameter& p : modeler.definitionParameters(defId, child.id)) {
        auto* spin = lengthSpin(p.value);
        if (!p.isLength) spin->setSuffix(QString());
        adjustForKey(spin, p.key);
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

  // Openings cut into this object (walls): width x height, editable.
  if (const auto openings = modeler.openings(id); openings && !openings->empty()) {
    form_->addRow(new QLabel(tr("<b>開口</b>（幅 × 高さ）")));
    for (const doc::Modeler::OpeningInfo& op : *openings) {
      auto* row = new QWidget;
      auto* h = new QHBoxLayout(row);
      h->setContentsMargins(0, 0, 0, 0);
      auto* w = lengthSpin(op.width);
      auto* ht = lengthSpin(op.height);
      w->setRange(1, 1e9);
      ht->setRange(1, 1e9);
      h->addWidget(w);
      h->addWidget(ht);
      const int index = static_cast<int>(op.index);
      const double w0 = op.width, h0 = op.height;
      auto commit = [this, id, index, w, ht, w0, h0] {
        if (std::abs(w->value() - w0) > 1e-9 || std::abs(ht->value() - h0) > 1e-9) {
          emit openingSizeEdited(id, index, w->value(), ht->value());
        }
      };
      connect(w, &QDoubleSpinBox::editingFinished, this, commit);
      connect(ht, &QDoubleSpinBox::editingFinished, this, commit);
      row->setObjectName(QStringLiteral("opening%1").arg(index));
      form_->addRow(tr("開口 %1").arg(index), row);
    }
  }

  // ===== Details: folded, for reference ===========================================
  auto* toggle = new QToolButton;
  toggle->setText(tr("詳細"));
  toggle->setCheckable(true);
  toggle->setChecked(detailsExpanded_);
  toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggle->setArrowType(detailsExpanded_ ? Qt::DownArrow : Qt::RightArrow);
  toggle->setAutoRaise(true);
  form_->addRow(toggle);

  auto* details = new QWidget;
  auto* detailsForm = new QFormLayout(details);
  detailsForm->setContentsMargins(12, 0, 0, 0);
  details->setVisible(detailsExpanded_);
  form_->addRow(details);
  connect(toggle, &QToolButton::toggled, this, [this, toggle, details](bool on) {
    detailsExpanded_ = on;
    toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    details->setVisible(on);
  });

  detailsForm->addRow(tr("種類"), new QLabel(kindLabel(o->kind())));
  if (!o->attributes.empty()) {
    // Meaning attributes (set by people or agents, e.g. level = 2F).
    QStringList pairs;
    for (const auto& [key, value] : o->attributes) {
      pairs << QStringLiteral("%1 = %2").arg(QString::fromStdString(key), QString::fromStdString(value));
    }
    auto* label = new QLabel(pairs.join(QStringLiteral("\n")));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsForm->addRow(tr("属性"), label);
  }
  auto* tag = new QLineEdit(QString::fromStdString(o->layer));
  connect(tag, &QLineEdit::editingFinished, this, [this, id, tag] { emit tagEdited(id, tag->text()); });
  detailsForm->addRow(tr("タグ"), tag);

  if (o->kind() != doc::ObjectKind::Group) {
    auto* color = new QPushButton;
    const QColor qc = QColor::fromRgbF(o->color.r, o->color.g, o->color.b);
    color->setStyleSheet(QStringLiteral("background-color: %1").arg(qc.name()));
    color->setText(qc.name());
    connect(color, &QPushButton::clicked, this, [this, id, qc] {
      const QColor chosen = QColorDialog::getColor(qc, this, tr("マテリアル色"));
      if (chosen.isValid()) emit colorEdited(id, doc::Color{chosen.redF(), chosen.greenF(), chosen.blueF()});
    });
    detailsForm->addRow(tr("色"), color);
  }

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
    detailsForm->addRow(tr("位置 (mm)"), row);
  }

  switch (o->kind()) {
    case doc::ObjectKind::Solid: {
      const geom::ValidationReport r = geom::validateShape(o->shape, false);
      detailsForm->addRow(tr("体積"), new QLabel(QString::number(r.volume / 1e9, 'f', 6) + tr(" m³")));
      detailsForm->addRow(tr("B-rep"), new QLabel(r.solidValid ? tr("ソリッドとして有効 (ソリッド %1 / 面 %2 / 稜線 %3)")
                                                                     .arg(r.solids).arg(r.faces).arg(r.edges)
                                                               : r.drawingValid ? tr("作図として有効（ソリッドとしては不完全）")
                                                                                : tr("不正")));
      break;
    }
    case doc::ObjectKind::Shell: {
      const geom::ValidationReport r = geom::validateShape(o->shape, false);
      detailsForm->addRow(tr("B-rep"), new QLabel(r.drawingValid ? tr("作図として有効・開いた形状（面 %1 / 開いた縁 %2 本）")
                                                                       .arg(r.faces).arg(r.freeEdges)
                                                                 : tr("不正")));
      break;
    }
    case doc::ObjectKind::Face:
      detailsForm->addRow(tr("面積"), new QLabel(QString::number(geom::area(o->shape) / 1e6, 'f', 6) + tr(" m²")));
      break;
    case doc::ObjectKind::Group:
      detailsForm->addRow(tr("子要素"), new QLabel(QString::number(document.children(id).size())));
      break;
    default:
      break;
  }

  if (const auto* curve = std::get_if<doc::InterpolatedCurve>(&o->recipe.base)) {
    detailsForm->addRow(new QLabel(tr("<b>通過点 (mm)</b>")));
    for (int i = 0; i < static_cast<int>(curve->points.size()); ++i) {
      const gp_Pnt pt = curve->points[static_cast<std::size_t>(i)];
      auto* row = new QWidget;
      auto* h = new QHBoxLayout(row);
      h->setContentsMargins(0, 0, 0, 0);
      QDoubleSpinBox* spins[3] = {lengthSpin(pt.X()), lengthSpin(pt.Y()), lengthSpin(pt.Z())};
      for (QDoubleSpinBox* spin : spins) {
        spin->setSuffix(QString());
        spin->setDecimals(1);
        h->addWidget(spin);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, i, spins, pt] {
          const double x = spins[0]->value(), y = spins[1]->value(), z = spins[2]->value();
          if (gp_Pnt(x, y, z).Distance(pt) > 1e-9) emit curvePointEdited(id, i, x, y, z);
        });
      }
      detailsForm->addRow(tr("点 %1").arg(i + 1), row);
    }
  }

  if (!nested.empty()) {
    detailsForm->addRow(new QLabel(tr("<b>入力の寸法</b>")));
    for (const doc::Parameter& p : nested) addParameter(detailsForm, p);
  }

  if (def) {
    const std::string defId = def->id;
    auto* defName = new QLineEdit(QString::fromStdString(def->name));
    connect(defName, &QLineEdit::editingFinished, this, [this, defId, defName] {
      emit definitionRenamed(defId, defName->text());
    });
    detailsForm->addRow(tr("定義名"), defName);
  }

  auto* recipe = new QLabel(QString::fromStdString(doc::describe(o->recipe)));
  recipe->setWordWrap(true);
  detailsForm->addRow(tr("履歴"), recipe);
  auto* idLabel = new QLabel(QString::fromStdString(o->id));
  idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  idLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 9pt"));
  detailsForm->addRow(tr("ID"), idLabel);
}

}  // namespace sbr::app
