#include "PropertiesPanel.h"
#include "Messages.h"

#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <map>

#include <QColorDialog>
#include <QComboBox>
#include <QTimer>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QIcon>
#include <QPixmap>
#include <QMenu>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QCompleter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>
#include "sbr/Units.h"
#include "sbr/WallTypes.h"
#include "sbr/OpeningSymbols.h"
#include "sbr/doc/Schedules.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Validation.h"

namespace sbr::app {
namespace {

QString kindLabel(doc::ObjectKind kind) {
  switch (kind) {
    case doc::ObjectKind::Solid: return QObject::tr("ソリッド");
    case doc::ObjectKind::Face: return QObject::tr("面");
    case doc::ObjectKind::Shell: return QObject::tr("開いた形状");
    case doc::ObjectKind::Image: return QObject::tr("画像（下敷き）");
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

// docs/134: a slab's / roof's type name, and the others of that name made like it.
void addTypeRow(PropertiesPanel* panel, QFormLayout* form, const doc::Modeler& modeler, const doc::Object& o,
                const QString& sizes) {
  const auto found = o.attributes.find("type_name");
  const QString name = found == o.attributes.end() ? QString() : QString::fromStdString(found->second);
  auto* edit = new QLineEdit(name);
  edit->setObjectName(QStringLiteral("typeName"));
  edit->setPlaceholderText(QObject::tr("種類名（例 F1、R1）"));
  edit->setToolTip(QObject::tr("同じ種類名の物は 1 つの種類です。1 つを直して「揃える」と、同じ種類の全部が%1になります").arg(sizes));
  edit->setMaxLength(40);
  edit->setEnabled(!o.locked);
  const doc::ObjectId id = o.id;
  // Registered types of this part (docs/149) are offered as the name is typed.
  const std::string kind = o.attributes.count("kind") ? o.attributes.at("kind") : std::string();
  const std::string part = kind == "slab" ? "floor" : "roof";
  QStringList registered;
  std::optional<doc::Modeler::ElementTypeDefinition> mine;
  for (const auto& t : modeler.elementTypes()) {
    if (t.part != part) continue;
    registered << QString::fromStdString(t.name);
    if (QString::fromStdString(t.name) == name) mine = t;
  }
  if (!registered.isEmpty()) {
    auto* completer = new QCompleter(registered, edit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    edit->setCompleter(completer);
    edit->setToolTip(edit->toolTip() + QObject::tr("。登録した種類（%1）の名前を付けると、その値になります").arg(registered.join(QStringLiteral("・"))));
  }
  QObject::connect(edit, &QLineEdit::editingFinished, panel, [panel, id, edit, name] {
    if (edit->text().trimmed() != name) emit panel->typeNameEdited(id, edit->text().trimmed());
  });
  form->addRow(QObject::tr("種類名"), edit);
  if (name.isEmpty()) return;
  auto* reg = new QToolButton;
  reg->setObjectName(QStringLiteral("typeRegister"));
  reg->setText(mine ? QObject::tr("種類 %1 の登録を、これの値にする").arg(name) : QObject::tr("種類 %1 を、これの値で登録する").arg(name));
  reg->setToolTip(QObject::tr("登録した種類は図面といっしょに保存されます。あとでこの種類名を付けた物は、登録した%1になります。"
                              "同じ種類名の物も、これと同じになります。Ctrl+Z 1 回で戻せます").arg(sizes));
  reg->setEnabled(!o.locked);
  QObject::connect(reg, &QToolButton::clicked, panel, [panel, id] { emit panel->typeRegistered(id); });
  form->addRow(reg);
  int others = 0;
  for (const doc::Object& other : modeler.document().objects()) {
    const auto it = other.attributes.find("type_name");
    others += other.id != id && it != other.attributes.end() && QString::fromStdString(it->second) == name;
  }
  if (others == 0) return;
  auto* match = new QToolButton;
  match->setObjectName(QStringLiteral("typeMatch"));
  match->setText(QObject::tr("同じ種類（%1）のほか %2 個を、これと同じにする").arg(name).arg(others));
  match->setToolTip(QObject::tr("同じ種類名の物が、これと同じ%1になります。Ctrl+Z 1 回で戻せます").arg(sizes));
  match->setEnabled(!o.locked);
  QObject::connect(match, &QToolButton::clicked, panel, [panel, id] { emit panel->typeMatched(id); });
  form->addRow(match);
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
                              const QString& elementSummary, std::size_t focusOpening) {
  updating_ = true;
  const doc::Document& document = modeler.document();
  bool rebuild = treeDocument_ != &document || document.sequence() == 0;
  if (!rebuild && treeSequence_ != document.sequence()) {
    // Objects only changed (moved, reshaped, renamed...): their rows are
    // updated in place; added, removed or regrouped ones rebuild the tree.
    const auto changed = document.changedSince(treeSequence_);
    rebuild = !changed;
    for (std::size_t i = 0; !rebuild && i < changed->size(); ++i) {
      const doc::Object* o = document.find((*changed)[i]);
      const auto it = o ? treeItems_.find(o->id) : treeItems_.end();
      const QTreeWidgetItem* parentItem = it == treeItems_.end() ? nullptr : it->second->parent();
      const QString shownParent = parentItem ? parentItem->data(0, Qt::UserRole).toString() : QString();
      rebuild = it == treeItems_.end() || shownParent != QString::fromStdString(o->parent);
    }
    if (!rebuild) {
      for (const doc::ObjectId& id : *changed) {
        const doc::Object* o = document.find(id);
        QTreeWidgetItem* item = treeItems_.at(id);
        item->setText(0, QString::fromStdString(o->name));
        item->setText(1, kindLabel(o->kind()));
        item->setText(2, QString::fromStdString(o->layer));
        item->setCheckState(0, o->visible ? Qt::Checked : Qt::Unchecked);
      }
      treeSequence_ = document.sequence();
    }
  }
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
  {
    // Thousands of rows selected at once: the view repaints once, not per row.
    const QSignalBlocker quiet(tree_->selectionModel());
    tree_->clearSelection();
    for (const doc::ObjectId& id : selection) {
      const auto it = treeItems_.find(id);
      if (it != treeItems_.end()) it->second->setSelected(true);
    }
  }
  tree_->viewport()->update();
  updating_ = false;
  focusOpening_ = focusOpening;
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

  // ===== A roof (docs/90 G-08): kind, storey, reference, sizes - the same order as other parts.
  if (const auto kind = o->attributes.find("kind"); kind != o->attributes.end() && kind->second == "roof") {
    auto* title = new QLabel(tr("<b>屋根</b>"));
    title->setObjectName(QStringLiteral("partTitle"));
    form_->addRow(title);
    const auto roof = modeler.roofInfo(id);
    if (!roof) {
      auto* why = new QLabel(userMessage(QString::fromStdString(roof.error())));
      why->setObjectName(QStringLiteral("roofProblem"));
      why->setWordWrap(true);
      form_->addRow(why);
    } else {
      auto* kinds = new QComboBox;
      kinds->setObjectName(QStringLiteral("roofKind"));
      kinds->addItem(tr("切妻"), QStringLiteral("gable"));
      kinds->addItem(tr("寄棟"), QStringLiteral("hip"));
      kinds->addItem(tr("片流れ"), QStringLiteral("shed"));
      kinds->setCurrentIndex(std::max(0, kinds->findData(QString::fromStdString(roof->spec.kind))));
      kinds->setEnabled(!o->locked);
      connect(kinds, &QComboBox::activated, this, [this, id, kinds] {
        emit roofEdited(id, QStringLiteral("kind"), 0.0, kinds->currentData().toString());
      });
      form_->addRow(tr("種類"), kinds);
      QString level = tr("なし（レベルの高さにありません）");
      for (const doc::FloorLevel& l : modeler.levels())
        if (l.id == roof->level) level = tr("%1 に付いています").arg(QString::fromStdString(l.name));
      auto* storey = new QLabel(level);
      storey->setObjectName(QStringLiteral("partStorey"));
      storey->setToolTip(tr("レベルの高さで作った屋根は、そのレベルの高さを変えると付いてきます"));
      form_->addRow(tr("階"), storey);
      auto* reference = new QLabel(tr("下面が、外周の線の上を「軒の高さ」で通る"));
      reference->setObjectName(QStringLiteral("partReference"));
      reference->setWordWrap(true);
      form_->addRow(tr("基準"), reference);
      auto size = [&](const QString& label, const char* key, double value, bool length, const QString& tip) {
        auto* spin = length ? lengthSpin(value) : new QDoubleSpinBox;
        if (!length) {
          spin->setDecimals(2);
          spin->setSingleStep(0.5);
          spin->setSuffix(tr(" 寸"));
          spin->setKeyboardTracking(false);
          spin->setRange(0.1, 30.0);
          spin->setValue(value);
        } else if (QLatin1String(key) != QLatin1String("height")) {
          spin->setRange(QLatin1String(key) == QLatin1String("thickness") ? 1 : 0, 1e9);
        }
        spin->setObjectName(QStringLiteral("roofSize_") + QLatin1String(key));
        spin->setToolTip(tip);
        spin->setEnabled(!o->locked);
        const QString k = QLatin1String(key);
        const double initial = spin->value();
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, spin, k, initial] {
          if (std::abs(spin->value() - initial) > 1e-9) emit roofEdited(id, k, spin->value(), QString());
        });
        form_->addRow(label, spin);
      };
      size(tr("勾配"), "pitch", roof->spec.pitch, false, tr("10 に対する立ち上がり（4 寸 = 4/10）"));
      size(tr("軒の出"), "eave", roof->spec.eave, true, tr("外周の線から軒先まで（水平）"));
      size(tr("けらばの出"), "gable_eave", roof->spec.gableEave, true, tr("妻側の出（水平）。片流れでは、流れに沿う 2 辺の出。寄棟では使いません"));
      size(tr("厚さ"), "thickness", roof->spec.thickness, true, tr("勾配に直角に測った厚さ"));
      size(tr("軒の高さ"), "height", roof->spec.height, true, tr("屋根の下面が、外周の線の上を通る高さ（壁・桁の上端）"));
      if (roof->spec.kind == "gable") {
        // docs/138: which sides end in gables, for outlines with slanted sides.
        QStringList sides;
        for (const int g : roof->spec.gableSides) sides << QString::number(g + 1);
        auto* gables = new QLineEdit(sides.join(QStringLiteral(",")));
        gables->setObjectName(QStringLiteral("roofGableSides"));
        gables->setPlaceholderText(tr("自動（向かい合う平行な辺の短い方）"));
        gables->setToolTip(tr("斜めの辺がある外周で、妻（切妻の三角の面）にする辺の番号。1 番目の角から 2 番目の角までが 1。"
                              "「1,3」のように。空：向かい合う平行な 2 辺のうち短い方の組（棟が長い方になる）。直角だけの外周では使いません"));
        gables->setEnabled(!o->locked);
        const QString initial = gables->text();
        connect(gables, &QLineEdit::editingFinished, this, [this, id, gables, initial] {
          if (gables->text().trimmed() != initial) emit roofEdited(id, QStringLiteral("gable_sides"), 0.0, gables->text().trimmed());
        });
        form_->addRow(tr("妻の辺"), gables);
      }
      addTypeRow(this, form_, modeler, *o, tr("勾配・厚さ・軒の出・けらばの出"));
      auto row = [&](const QString& label, const QString& text, const char* name) {
        auto* value = new QLabel(text);
        value->setObjectName(QLatin1String(name));
        form_->addRow(label, value);
      };
      {
        // The walls under it: up to its underside, or flat again.
        const std::size_t fitted = modeler.wallsFittedToRoof(id).size();
        const std::size_t under = modeler.wallsUnderRoof(id).size();
        auto* line = new QWidget;
        auto* box = new QHBoxLayout(line);
        box->setContentsMargins(0, 0, 0, 0);
        auto* state = new QLabel(fitted > 0 ? tr("%1 枚が屋根まで").arg(fitted) : tr("合わせていない（下に %1 枚）").arg(under));
        state->setObjectName(QStringLiteral("roofWallsState"));
        auto* fit = new QPushButton(tr("合わせる"));
        fit->setObjectName(QStringLiteral("roofFitWalls"));
        fit->setToolTip(tr("この屋根の下にある壁（壁ツールで描いた壁）の上端を、屋根の下面まで伸ばして合わせます。"
                           "妻側の壁は三角になります。壁は壁のまま（長さ・仕様・開口を直せます）"));
        fit->setEnabled(under > 0);
        auto* release = new QPushButton(tr("外す"));
        release->setObjectName(QStringLiteral("roofReleaseWalls"));
        release->setToolTip(tr("合わせた壁を、元の高さの平らな上端に戻します"));
        release->setEnabled(fitted > 0);
        connect(fit, &QPushButton::clicked, this, [this, id] { emit roofWallsRequested(id, true); });
        connect(release, &QPushButton::clicked, this, [this, id] { emit roofWallsRequested(id, false); });
        box->addWidget(state, 1);
        box->addWidget(fit);
        box->addWidget(release);
        form_->addRow(tr("下の壁"), line);
      }
      row(tr("屋根面積"), tr("%1 ㎡（勾配なりの上面）").arg(roof->slopeArea / 1e6, 0, 'f', 2), "roofSlopeArea");
      row(tr("水平投影"), tr("%1 ㎡（軒の出を含む）").arg(roof->planArea / 1e6, 0, 'f', 2), "roofPlanArea");
      row(tr("いちばん高い所"), tr("%1 mm").arg(roof->ridgeHeight, 0, 'f', 1), "roofRidgeHeight");
    }
  }

  // ===== A canopy (docs/147, docs/154): its sizes, made again over its opening when changed.
  if (const auto kind = o->attributes.find("kind"); kind != o->attributes.end() && kind->second == "canopy") {
    auto* title = new QLabel(tr("<b>庇</b>"));
    title->setObjectName(QStringLiteral("partTitle"));
    form_->addRow(title);
    const auto spec = modeler.canopySpec(id);
    if (!spec) {
      auto* why = new QLabel(userMessage(QString::fromStdString(spec.error())));
      why->setWordWrap(true);
      form_->addRow(why);
    } else {
      auto size = [&](const QString& label, const char* key, double value, double low, double high, const QString& tip) {
        auto* spin = lengthSpin(value);
        spin->setRange(low, high);
        spin->setValue(value);
        spin->setObjectName(QStringLiteral("canopySize_") + QLatin1String(key));
        spin->setToolTip(tip);
        spin->setEnabled(!o->locked);
        const QString k = QLatin1String(key);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, spin, k, value] {
          if (std::abs(spin->value() - value) > 1e-9) emit canopyEdited(id, k, spin->value());
        });
        form_->addRow(label, spin);
      };
      size(tr("出"), "depth", spec->depth, 100, 5000, tr("開口をあけた面から先まで（水平）"));
      size(tr("厚さ"), "thickness", spec->thickness, 10, 500, tr("板の厚さ"));
      size(tr("両側の長さ"), "extra", spec->extra, 0, 2000, tr("開口の幅より、片側でどれだけ長いか"));
      size(tr("開口の上から"), "above", spec->above, 0, 2000, tr("開口の上端から、庇の下の面まで"));
    }
  }

  // ===== A stair (docs/90 G-09, docs/103): storeys, sizes, what it measures, the floor over it.
  if (const auto kind = o->attributes.find("kind"); kind != o->attributes.end() && kind->second == "stair") {
    auto* title = new QLabel(tr("<b>階段</b>"));
    title->setObjectName(QStringLiteral("partTitle"));
    form_->addRow(title);
    const auto stair = modeler.stairInfo(id);
    if (!stair) {
      auto* why = new QLabel(userMessage(QString::fromStdString(stair.error())));
      why->setObjectName(QStringLiteral("stairProblem"));
      why->setWordWrap(true);
      form_->addRow(why);
    } else {
      auto levelName = [&](const std::string& levelId) {
        for (const doc::FloorLevel& l : modeler.levels())
          if (l.id == levelId) return QString::fromStdString(l.name);
        return tr("レベルなし");
      };
      auto* storey = new QLabel(tr("%1 から %2 へ").arg(levelName(stair->baseLevel), levelName(stair->topLevel)));
      storey->setObjectName(QStringLiteral("partStorey"));
      storey->setToolTip(tr("レベルの高さで作った階段は、そのレベルの高さを変えると付いてきます（段数が自動なら、段数も変わります）"));
      form_->addRow(tr("階"), storey);
      auto* reference = new QLabel(tr("描いた線が幅の真ん中。上がりきった所が、上の階の床の端"));
      reference->setObjectName(QStringLiteral("partReference"));
      reference->setWordWrap(true);
      form_->addRow(tr("基準"), reference);
      auto size = [&](const QString& label, const char* key, double value, double least, const QString& tip) {
        auto* spin = lengthSpin(value);
        spin->setRange(least, 1e9);
        spin->setValue(value);
        spin->setObjectName(QStringLiteral("stairSize_") + QLatin1String(key));
        spin->setToolTip(tip);
        spin->setEnabled(!o->locked);
        const QString k = QLatin1String(key);
        const double initial = spin->value();
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, spin, k, initial] {
          if (std::abs(spin->value() - initial) > 1e-9) emit stairEdited(id, k, spin->value(), QString());
        });
        form_->addRow(label, spin);
      };
      size(tr("幅"), "width", stair->spec.width, 300, tr("階段の幅（描いた線の両側に半分ずつ）"));
      size(tr("上がる高さ"), "rise", stair->spec.rise, 100, tr("下の階の床から、上の階の床まで"));
      auto* risers = new QSpinBox;
      risers->setObjectName(QStringLiteral("stairRisers"));
      risers->setRange(0, 200);
      risers->setSpecialValueText(tr("自動（今は %1 段）").arg(stair->risers));
      risers->setValue(stair->spec.risers);
      risers->setKeyboardTracking(false);
      risers->setToolTip(tr("蹴上の数。0（自動）は、蹴上が上限以下になるいちばん少ない段数。階高を変えると数え直します"));
      risers->setEnabled(!o->locked);
      const int risersBefore = stair->spec.risers;
      connect(risers, &QSpinBox::editingFinished, this, [this, id, risers, risersBefore] {
        if (risers->value() != risersBefore) emit stairEdited(id, QStringLiteral("risers"), risers->value(), QString());
      });
      form_->addRow(tr("段数"), risers);
      size(tr("蹴上の上限"), "riser_limit", stair->spec.riserLimit, 50, tr("段数が自動の時に、蹴上がこれ以下になるようにします"));
      // One choice per turn, in order up the stair (docs/119); a straight stair has none to make.
      const int corners = std::max(1, static_cast<int>(stair->spec.path.size()) - 2);
      for (int c = 0; c < corners; ++c) {
        auto* turn = new QComboBox;
        turn->setObjectName(c == 0 ? QStringLiteral("stairTurn") : QStringLiteral("stairTurn%1").arg(c + 1));
        turn->addItem(tr("踊り場"), 1);
        for (int t = 2; t <= 6; ++t) turn->addItem(tr("%1 段回り（%2°）").arg(t).arg(90.0 / t, 0, 'g', 3), t);
        const std::vector<int>& turns = stair->spec.turns;
        const int now = turns.empty() ? 3 : turns[std::min<std::size_t>(static_cast<std::size_t>(c), turns.size() - 1)];
        turn->setCurrentIndex(std::max(0, turn->findData(now)));
        turn->setToolTip(tr("曲がり角の四角（幅 × 幅）の作り方。回り段は、内側の角から三角の段に分けます。上りの順に、角ごとに選べます"));
        turn->setEnabled(!o->locked && stair->spec.path.size() > 2);
        connect(turn, &QComboBox::activated, this, [this, id, turn, c] {
          emit stairEdited(id, QStringLiteral("turn%1").arg(c + 1), turn->currentData().toInt(), QString());
        });
        form_->addRow(corners > 1 ? tr("曲がり角 %1").arg(c + 1) : tr("曲がり角"), turn);
      }
      auto* style = new QComboBox;
      style->setObjectName(QStringLiteral("stairStyle"));
      style->addItem(tr("段々の塊"), QStringLiteral("block"));
      style->addItem(tr("踏板だけ"), QStringLiteral("boards"));
      style->setCurrentIndex(std::max(0, style->findData(QString::fromStdString(stair->spec.style))));
      style->setEnabled(!o->locked);
      connect(style, &QComboBox::activated, this, [this, id, style] {
        emit stairEdited(id, QStringLiteral("style"), 0.0, style->currentData().toString());
      });
      form_->addRow(tr("形"), style);
      size(tr("踏板の厚さ"), "thickness", stair->spec.thickness, 5, tr("踏板の厚さ。段々の塊では、段の裏の厚さ（蹴上より小さく）"));
      size(tr("段鼻の出"), "nosing", stair->spec.nosing, 0, tr("踏板が前の縁から前へ出る長さ（0〜50）。踏面の寸法（法の照合）には入れません"));
      if (stair->spec.shape == "spiral") {
        // docs/139: how far it turns (degrees, + counter-clockwise going up).
        auto* sweep = new QDoubleSpinBox;
        sweep->setObjectName(QStringLiteral("stairSweep"));
        sweep->setRange(-720, 720);
        sweep->setDecimals(1);
        sweep->setSuffix(tr(" °"));
        sweep->setKeyboardTracking(false);
        sweep->setValue(stair->spec.sweep);
        sweep->setToolTip(tr("らせん階段が上りながら回る角度（90〜720、正：反時計回り、負：時計回り）"));
        sweep->setEnabled(!o->locked);
        const double initial = stair->spec.sweep;
        connect(sweep, &QDoubleSpinBox::editingFinished, this, [this, id, sweep, initial] {
          if (std::abs(sweep->value() - initial) > 1e-9) emit stairEdited(id, QStringLiteral("sweep"), sweep->value(), QString());
        });
        form_->addRow(tr("回る角度"), sweep);
      }
      {
        // docs/116: handrails, looking up the stair.
        auto* rail = new QComboBox;
        rail->setObjectName(QStringLiteral("stairHandrail"));
        rail->addItem(tr("なし"), QStringLiteral("none"));
        rail->addItem(tr("左"), QStringLiteral("left"));
        rail->addItem(tr("右"), QStringLiteral("right"));
        rail->addItem(tr("両側"), QStringLiteral("both"));
        rail->setCurrentIndex(std::max(0, rail->findData(QString::fromStdString(stair->spec.handrail))));
        rail->setToolTip(tr("上りの向きに見て、左・右。段の先から上へ測った高さに、丸い手すり（径 35）を付けます"));
        rail->setEnabled(!o->locked);
        connect(rail, &QComboBox::activated, this, [this, id, rail] {
          emit stairEdited(id, QStringLiteral("handrail"), 0.0, rail->currentData().toString());
        });
        form_->addRow(tr("手すり"), rail);
        if (stair->spec.handrail != "none")
          size(tr("手すりの高さ"), "handrail_height", stair->spec.handrailHeight, 600, tr("段の先から手すりの上まで（600〜1200）"));
      }
      {
        // docs/128: side boards (ささら桁・側板), looking up the stair.
        auto* sides = new QComboBox;
        sides->setObjectName(QStringLiteral("stairStringer"));
        sides->addItem(tr("なし"), QStringLiteral("none"));
        sides->addItem(tr("左"), QStringLiteral("left"));
        sides->addItem(tr("右"), QStringLiteral("right"));
        sides->addItem(tr("両側"), QStringLiteral("both"));
        sides->setCurrentIndex(std::max(0, sides->findData(QString::fromStdString(stair->spec.stringer))));
        sides->setToolTip(tr("上りの向きに見て、左・右。幅のすぐ外に側板を付けます（上は段の先の線より 50 上、段板をくわえる深さ）"));
        sides->setEnabled(!o->locked);
        connect(sides, &QComboBox::activated, this, [this, id, sides] {
          emit stairEdited(id, QStringLiteral("stringer"), 0.0, sides->currentData().toString());
        });
        form_->addRow(tr("側板（ささら桁）"), sides);
        if (stair->spec.stringer != "none") {
          size(tr("側板の厚さ"), "stringer_thickness", stair->spec.stringerThickness, 15, tr("側板の厚さ（15〜100）"));
          // docs/135: side boards or a cut stringer; newel posts.
          auto* shape = new QComboBox;
          shape->setObjectName(QStringLiteral("stairStringerStyle"));
          shape->addItem(tr("側板（段をはさむ）"), QStringLiteral("side"));
          shape->addItem(tr("ささら桁（段を上に乗せる）"), QStringLiteral("cut"));
          shape->setCurrentIndex(std::max(0, shape->findData(QString::fromStdString(stair->spec.stringerStyle))));
          shape->setEnabled(!o->locked);
          connect(shape, &QComboBox::activated, this, [this, id, shape] {
            emit stairEdited(id, QStringLiteral("stringer_style"), 0.0, shape->currentData().toString());
          });
          form_->addRow(tr("板の形"), shape);
          auto* newel = new QCheckBox(tr("親柱（足元と、回りの内側の角）"));
          newel->setObjectName(QStringLiteral("stairNewels"));
          newel->setChecked(stair->spec.newels);
          newel->setEnabled(!o->locked);
          connect(newel, &QCheckBox::toggled, this, [this, id](bool on) {
            emit stairEdited(id, QStringLiteral("newels"), 0.0, on ? QStringLiteral("true") : QStringLiteral("false"));
          });
          form_->addRow(QString(), newel);
        }
        if (stair->spec.style == "boards") {
          auto* risers = new QCheckBox(tr("蹴込み板（段の前の板）"));
          risers->setObjectName(QStringLiteral("stairRiserBoards"));
          risers->setChecked(stair->spec.riserBoards);
          risers->setEnabled(!o->locked);
          connect(risers, &QCheckBox::toggled, this, [this, id](bool on) {
            emit stairEdited(id, QStringLiteral("riser_boards"), 0.0, on ? QStringLiteral("true") : QStringLiteral("false"));
          });
          form_->addRow(QString(), risers);
        }
      }
      auto row = [&](const QString& label, const QString& text, const char* name) {
        auto* value = new QLabel(text);
        value->setObjectName(QLatin1String(name));
        value->setWordWrap(true);
        form_->addRow(label, value);
      };
      row(tr("蹴上"), tr("%1 mm × %2 段").arg(stair->riser, 0, 'f', 1).arg(stair->risers), "stairRiser");
      if (stair->goingMin > 0)
        row(tr("踏面"), std::abs(stair->goingMax - stair->goingMin) < 0.05
                            ? tr("%1 mm").arg(stair->goingMin, 0, 'f', 1)
                            : tr("%1〜%2 mm（直の部分ごとに割り付け）").arg(stair->goingMin, 0, 'f', 1).arg(stair->goingMax, 0, 'f', 1),
            "stairGoing");
      if (stair->winders > 0)
        row(tr("回り段の踏面"), stair->winderGoing > 0 ? tr("%1 mm（狭い端から 300 の位置）").arg(stair->winderGoing, 0, 'f', 1)
                                                       : tr("未評価（幅が 300 以下で測れません）"),
            "stairWinderGoing");
      {
        // Against the sizes for a house's stair: what is not measured is said, never passed.
        QStringList lines;
        for (const geom::StairCheck& c : stair->checks) {
          const QString item = c.item == "riser" ? tr("蹴上") : c.item == "going" ? tr("踏面") : c.item == "winder_going" ? tr("回り段の踏面")
                               : c.item == "width" ? tr("幅") : c.item == "landing" ? tr("踊場（高さ 4 m ごと）") : tr("手すり");
          if (c.state == "not_evaluated") {
            lines << tr("%1：未評価").arg(item);
            continue;
          }
          const bool atMost = c.item == "riser" || c.item == "landing";
          lines << tr("%1 %2（%3 %4）：%5").arg(item).arg(c.value, 0, 'f', 1).arg(c.limit, 0, 'f', 0)
                       .arg(atMost ? tr("以下") : tr("以上"), c.state == "ok" ? tr("範囲内") : tr("範囲外"));
        }
        auto* checks = new QLabel(lines.join(QStringLiteral("\n")));
        checks->setObjectName(QStringLiteral("stairChecks"));
        checks->setWordWrap(true);
        checks->setToolTip(tr("住宅の階段（共同住宅の共用階段を除く）の寸法との照合です（建築基準法施行令 第 23〜25 条。"
                              "根拠と確認日は docs/103）。幅は立体の幅で、手すり・仕上げを引いた有効幅ではありません。"
                              "未評価は、ここでは測っていない項目です。合否の判定ではありません"));
        form_->addRow(tr("寸法の照合（住宅）"), checks);
      }
      {
        const std::size_t opened = stair->openedSlabs.size();
        auto* line = new QWidget;
        auto* box = new QHBoxLayout(line);
        box->setContentsMargins(0, 0, 0, 0);
        auto* state = new QLabel(opened > 0 ? tr("%1 枚に開口あり").arg(opened) : tr("開口なし"));
        state->setObjectName(QStringLiteral("stairFloorState"));
        auto* open = new QPushButton(tr("あける"));
        open->setObjectName(QStringLiteral("stairOpenFloor"));
        open->setToolTip(tr("上がりきった高さにある床（スラブツールで作った床）に、頭の上が 2000 より低くなる段の分だけ開口をあけます。"
                            "床は床のまま（外周・厚さを直せます）。階段を変える・動かすと開口も付いてきます"));
        auto* close = new QPushButton(tr("閉じる"));
        close->setObjectName(QStringLiteral("stairCloseFloor"));
        close->setEnabled(opened > 0);
        connect(open, &QPushButton::clicked, this, [this, id] { emit stairFloorRequested(id, true); });
        connect(close, &QPushButton::clicked, this, [this, id] { emit stairFloorRequested(id, false); });
        box->addWidget(state, 1);
        box->addWidget(open);
        box->addWidget(close);
        form_->addRow(tr("上の床"), line);
      }
    }
  }

  // ===== A wall standing up to a roof (docs/97 N-05): said, and released here.
  if (const auto roofOf = o->attributes.find("top_roof"); roofOf != o->attributes.end()) {
    const doc::Object* roofObject = document.find(roofOf->second);
    auto* line = new QWidget;
    auto* box = new QHBoxLayout(line);
    box->setContentsMargins(0, 0, 0, 0);
    auto* state = new QLabel(roofObject ? tr("屋根「%1」まで").arg(QString::fromStdString(roofObject->name))
                                        : tr("屋根まで（その屋根は、もうありません）"));
    state->setObjectName(QStringLiteral("wallTopRoof"));
    auto* release = new QPushButton(tr("外す"));
    release->setObjectName(QStringLiteral("wallReleaseRoof"));
    release->setToolTip(tr("この壁を、元の高さの平らな上端に戻します"));
    release->setEnabled(!o->locked);
    connect(release, &QPushButton::clicked, this, [this, id] { emit roofWallsRequested(id, false); });
    box->addWidget(state, 1);
    box->addWidget(release);
    form_->addRow(tr("上端"), line);
  }

  // ===== A room (docs/90 G-07): its areas as the walls are now.
  if (const auto kind = o->attributes.find("kind"); kind != o->attributes.end() && kind->second == "room") {
    for (const doc::RoomInfo& room : modeler.rooms()) {
      if (room.id != id) continue;
      auto* title = new QLabel(tr("<b>室</b>"));
      title->setObjectName(QStringLiteral("partTitle"));
      form_->addRow(title);
      QString level = tr("なし（レベルの高さにありません）");
      for (const doc::FloorLevel& l : modeler.levels())
        if (l.id == room.level) level = QString::fromStdString(l.name);
      auto* storey = new QLabel(level);
      storey->setObjectName(QStringLiteral("partStorey"));
      form_->addRow(tr("階"), storey);
      auto row = [&](const QString& label, const QString& text, const char* name, const QString& tip) {
        auto* value = new QLabel(text);
        value->setObjectName(QLatin1String(name));
        value->setToolTip(tip);
        value->setWordWrap(true);
        form_->addRow(label, value);
      };
      if (room.closed) {
        row(tr("壁芯の面積"), tr("%1 ㎡").arg(room.centreArea / 1e6, 0, 'f', 2), "roomAreaCentre",
            tr("壁を描いた線（木造の壁は柱心）の内側。壁を動かすと変わります"));
        row(tr("内法の面積"), room.innerKnown ? tr("%1 ㎡").arg(room.innerArea / 1e6, 0, 'f', 2) : tr("未評価"), "roomAreaInner",
            room.innerKnown ? tr("壁の面の内側") : tr("室の中に壁の端があるなど、壁の面から形を決められないため、数字を出していません"));
        row(tr("囲む壁"), tr("%1 枚").arg(room.walls.size()), "roomWalls", QString());
        if (!room.problem.empty())
          row(tr("注意"), userMessage(QString::fromStdString(room.problem)), "roomProblem", QString());
      } else {
        QString why = userMessage(QString::fromStdString(room.problem));
        if (!room.looseEnds.empty())
          why += tr("。近くの壁の端: X %1, Y %2").arg(room.looseEnds.front().X(), 0, 'f', 0).arg(room.looseEnds.front().Y(), 0, 'f', 0);
        if (room.unreadableWalls > 0) why += tr("。形を直接変えた壁 %1 枚は壁として数えていません").arg(room.unreadableWalls);
        row(tr("面積"), tr("なし（%1）").arg(why), "roomProblem", QString());
      }
      {
        // docs/115: its use and finishes, for the finish schedule.
        const doc::RoomFinish finish = doc::roomFinishOf(*o);
        auto word = [&](const QString& label, const char* key, const std::string& value, const QString& hint) {
          auto* edit = new QLineEdit(QString::fromStdString(value));
          edit->setObjectName(QStringLiteral("roomFinish_") + QLatin1String(key));
          edit->setPlaceholderText(hint);
          edit->setMaxLength(60);
          edit->setEnabled(!o->locked);
          const QString k = QLatin1String(key), before = QString::fromStdString(value);
          connect(edit, &QLineEdit::editingFinished, this, [this, id, edit, k, before] {
            if (edit->text().trimmed() != before) emit roomFinishEdited(id, k, edit->text(), 0.0);
          });
          form_->addRow(label, edit);
        };
        word(tr("用途"), "use", finish.use, tr("例：居間"));
        word(tr("床"), "floor", finish.floor, tr("例：フローリング t15"));
        word(tr("壁"), "wall", finish.wall, tr("例：PB t12.5 クロス貼り"));
        word(tr("天井"), "ceiling", finish.ceiling, tr("例：PB t9.5 クロス貼り"));
        auto* height = lengthSpin(finish.ceilingHeight);
        height->setObjectName(QStringLiteral("roomFinish_ceiling_height"));
        height->setRange(0, 10000);
        height->setSpecialValueText(tr("未入力"));
        height->setValue(finish.ceilingHeight);
        height->setToolTip(tr("床から天井まで（mm）。0 で消します"));
        height->setEnabled(!o->locked);
        const double h0 = finish.ceilingHeight;
        connect(height, &QDoubleSpinBox::editingFinished, this, [this, id, height, h0] {
          if (std::abs(height->value() - h0) > 1e-9) emit roomFinishEdited(id, QStringLiteral("ceiling_height"), QString(), height->value());
        });
        form_->addRow(tr("天井高"), height);
        auto* ceiling = new QPushButton(tr("天井を作る（作り直す）"));
        ceiling->setObjectName(QStringLiteral("roomMakeCeiling"));
        ceiling->setToolTip(tr("天井高の所に、壁の面の内側の形で天井板（厚さ 10）を作ります。壁を動かした後・天井高を変えた後は、もう一度押すと作り直します"));
        ceiling->setEnabled(!o->locked && finish.ceilingHeight > 0);
        connect(ceiling, &QPushButton::clicked, this, [this, id] { emit roomCeilingRequested(id); });
        form_->addRow(QString(), ceiling);
      }
      break;
    }
  }

  // ===== The part (docs/90 G-04): what it is, in the same order for every building part:
  // kind (種類), storey (階), reference (基準), sizes (寸法). The same words as its tool's settings.
  const std::string partKind = [&] {
    const auto it = o->attributes.find("kind");
    const std::string kind = it == o->attributes.end() ? std::string() : it->second;
    const auto info = modeler.memberInfo(id);
    if (!info) return std::string();
    if (kind == "wall" || kind == "slab") return kind;
    return info->element == "member" ? std::string("member") : std::string();
  }();
  // Columns, beams and other members (step 3): placed ones of a generated frame are shown,
  // and edited through the frame; a member of its own is edited here.
  const bool ownMember = partKind == "member" && std::holds_alternative<doc::LinearMember>(o->recipe.base);
  if (!partKind.empty()) {
    QString what = partKind == "wall" ? tr("壁") : partKind == "slab" ? tr("床（スラブ）") : tr("部材");
    if (partKind == "member") {
      const auto it = o->attributes.find("kind");
      const std::string role = it == o->attributes.end() ? std::string() : it->second;
      static const std::map<std::string, QString> names = {
          {"column", tr("柱")}, {"through_column", tr("通し柱")}, {"beam", tr("梁")}, {"girder", tr("大梁")},
          {"floor_beam", tr("床梁")}, {"rim_beam", tr("胴差")}, {"eave_beam", tr("軒桁")}, {"roof_beam", tr("小屋梁")},
          {"sill", tr("土台")}, {"joist", tr("根太")}, {"rafter", tr("垂木")}, {"stud", tr("間柱")},
          {"brace", tr("筋かい")}, {"purlin", tr("母屋")}, {"ridge", tr("棟木")}, {"roof_post", tr("小屋束")},
          {"floor_post", tr("床束")}, {"floor_girder", tr("大引")}, {"lintel", tr("まぐさ")}};
      if (const auto found = names.find(role); found != names.end()) what = found->second;
    }
    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(what));
    title->setObjectName(QStringLiteral("partTitle"));
    form_->addRow(title);
  }
  if (partKind == "member") {
    const auto info = modeler.memberInfo(id);
    // 種類: its section.
    auto* section = new QLabel(QString::fromStdString(info->section));
    section->setObjectName(QStringLiteral("partSection"));
    section->setToolTip(tr("断面。別の断面にする時は「部材」パネルで選びます"));
    form_->addRow(tr("種類"), section);
    // 階: the levels its ends stand on (set when placed on a grid with levels, docs/45).
    auto levelName = [&](const std::string& levelId) {
      for (const doc::FloorLevel& l : modeler.levels())
        if (l.id == levelId) return QString::fromStdString(l.name);
      return levelId.empty() ? tr("なし") : tr("（削除されたレベル）");
    };
    auto* storey = new QLabel(o->gridPin ? tr("下端 %1 ／ 上端 %2").arg(levelName(o->gridPin->startLevel), levelName(o->gridPin->endLevel))
                                         : tr("なし（レベルに付けていない）"));
    storey->setObjectName(QStringLiteral("partStorey"));
    storey->setToolTip(tr("「通り芯に柱・梁を配置」でレベルを選んで置いた部材は、階高を変えると付いてきます"));
    form_->addRow(tr("階"), storey);
  }
  // A wall's build-up on its drawn line (timber walls).
  if (const doc::Object* o = modeler.document().find(id);
      o && o->attributes.count("kind") && o->attributes.at("kind") == "wall") {
    auto* type = new QComboBox;
    type->setObjectName(QStringLiteral("wallType"));
    type->addItem(tr("厚さ指定（仕様なし）"), QString());
    for (const auto& t : construction::wallTypes())
      type->addItem(QString::fromStdString(t.name), QString::fromStdString(t.id));
    const auto current = o->attributes.find("wall_type");
    type->setCurrentIndex(current == o->attributes.end()
                              ? 0
                              : std::max(0, type->findData(QString::fromStdString(current->second))));
    type->setToolTip(tr("描いた線（柱心）はそのままで、外側・内側の仕上げの厚みが付きます。何枚もまとめて変える時は、壁を選んで建築ツールバーの仕様から。"
                        "角・T字の取り合いと開口も合わせて直します"));
    connect(type, &QComboBox::activated, this, [this, id, type] {
      const QString value = type->currentData().toString();
      if (!value.isEmpty()) emit wallTypeEdited(id, value);
    });
    form_->addRow(tr("種類"), type);
  }
  if (partKind == "slab") addTypeRow(this, form_, modeler, *o, tr("厚さ（上の面はそのまま）"));

  // The levels a wall or a slab is on (docs/90 G-02): it follows them when a storey height
  // changes. Choosing a level puts that end on it now.
  if (const doc::Object* o = modeler.document().find(id);
      o && o->attributes.count("kind") && (o->attributes.at("kind") == "wall" || o->attributes.at("kind") == "slab") &&
      !modeler.levels().empty()) {
    const bool wall = o->attributes.at("kind") == "wall";
    auto row = [&](bool top, const QString& label, const QString& tip) {
      auto* level = new QComboBox;
      level->setObjectName(top ? QStringLiteral("levelTop") : QStringLiteral("levelBase"));
      level->addItem(tr("なし（レベルに付けない）"), QString());
      for (const doc::FloorLevel& l : modeler.levels())
        level->addItem(tr("%1（%2）").arg(QString::fromStdString(l.name),
                                        QString::fromStdString(units::formatLength(l.elevation))),
                       QString::fromStdString(l.id));
      const auto current = o->attributes.find(top ? "top_level" : "base_level");
      const int at = current == o->attributes.end() ? 0 : level->findData(QString::fromStdString(current->second));
      if (at < 0) {
        // Its level was taken away: it stays where it is, and says so.
        level->insertItem(1, tr("（削除されたレベル）"), QString::fromStdString(current->second));
        level->setCurrentIndex(1);
      } else {
        level->setCurrentIndex(at);
      }
      level->setToolTip(tip);
      connect(level, &QComboBox::activated, this, [this, id, top, level] {
        emit levelAttachmentEdited(id, top, level->currentData().toString());
      });
      form_->addRow(label, level);
    };
    if (wall) {
      row(false, tr("下端のレベル"),
          tr("壁の足元を、このレベルに付けます。選ぶと足元がその高さに動き、あとでレベルの高さを変えると付いてきます。"
             "上端がレベルに付いている壁は、上端をそのままにして伸び縮みします"));
      row(true, tr("上端のレベル"),
          tr("壁の上端を、このレベルに付けます。選ぶと高さがそのレベルまでになり、階高を変えると伸び縮みします。"
             "「なし」の壁は、高さを保ったまま足元と一緒に動きます。開口の腰高は変わりません"));
    } else {
      row(true, tr("上面のレベル"),
          tr("床の上面を、このレベルに付けます。選ぶと上面がその高さに動き、あとでレベルの高さを変えると付いてきます"));
    }
  }

  if (!partKind.empty()) {
    // 基準: what stays put when the sizes change.
    auto attribute = [&](const char* key) {
      const auto it = o->attributes.find(key);
      return it == o->attributes.end() ? std::string() : it->second;
    };
    QString reference = tr("上面（厚さを変えても上面はそのまま）");
    if (partKind == "member") {
      reference = tr("なし（通り芯に付けていない）");
      if (o->gridPin) {
        auto axisName = [&](const std::string& axisId) {
          if (const doc::GridSpec* grid = modeler.gridSpec(o->gridPin->grid)) {
            for (const auto* list : {&grid->x, &grid->y})
              for (const doc::GridAxis& a : *list)
                if (a.id == axisId) return QString::fromStdString(a.name);
          }
          return tr("？");
        };
        const QString start = axisName(o->gridPin->startX) + QLatin1Char('/') + axisName(o->gridPin->startY);
        const QString end = axisName(o->gridPin->endX) + QLatin1Char('/') + axisName(o->gridPin->endY);
        reference = start == end ? tr("通り芯 %1").arg(start) : tr("通り芯 %1 〜 %2").arg(start, end);
      }
    }
    if (partKind == "wall") {
      const std::string line = attribute("wall_line");
      reference = line == "left" ? tr("基準線：左の面") : line == "right" ? tr("基準線：右の面")
                  : line == "offset" ? tr("基準線：柱心") : tr("基準線：中心");
      if (const std::string outside = attribute("wall_outside"); !outside.empty())
        reference += outside == "right" ? tr("／外側：右") : tr("／外側：左");
    }
    auto* label = new QLabel(reference);
    label->setObjectName(QStringLiteral("partReference"));
    label->setToolTip(partKind == "member"
                          ? tr("通り芯に付けた部材は、通り芯を動かすと付いてきます")
                          : tr("描いた時の基準。左右は、描いた向きに進んで見た時。外側の入れ替えは建築ツールバーの「外⇄内」"));
    form_->addRow(tr("基準"), label);
    // 寸法: the part's own sizes, in its own words (a wall: 長さ・厚さ・高さ).
    if (const auto info = modeler.memberInfo(id)) {
      for (const auto& field : info->fields) {
        if (field.unit != "mm") continue;  // a member's roll is turned with the rotate tool
        const QString name = field.key == "length" ? tr("長さ") : field.key == "thickness" ? tr("厚さ")
                             : field.key == "height" ? tr("高さ") : field.key == "top" ? tr("上面の高さ")
                             : field.key == "width" ? tr("幅") : field.key == "depth" ? tr("せい")
                             : field.key == "diameter" ? tr("直径") : field.key == "web" ? tr("ウェブ厚")
                             : field.key == "flange" ? tr("フランジ厚") : field.key == "radius" ? tr("隅の丸み")
                             : QString::fromStdString(field.key);
        auto* spin = lengthSpin(field.value);
        spin->setObjectName(QStringLiteral("partSize_") + QString::fromStdString(field.key));
        if (field.key != "top") spin->setRange(field.key == "radius" ? 0 : 1, 1e9);
        else spin->setRange(-1e9, 1e9);
        spin->setEnabled(field.editable && (partKind != "member" || ownMember));
        if (partKind == "member" && !ownMember)
          spin->setToolTip(tr("木造架構などの配置された部材です。寸法は、架構の作り直しか「部材」パネルで変えます"));
        else if (partKind == "member")
          spin->setToolTip(QString::fromStdString(field.note));
        else if (!field.editable)
          spin->setToolTip(field.key == "thickness" ? tr("開口のある壁の厚さは、上の「種類」（壁の仕様）で変えます。開口も合わせて直ります")
                                                    : tr("上面の高さは「階」のレベル、または移動で変えます"));
        else if (field.key == "length" || field.key == "height")
          spin->setToolTip(tr("開口はそのままの位置に残ります。開口が壁からはみ出す寸法にはできません"));
        const std::string key = field.key;
        const double initial = field.value;
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, id, key, spin, initial] {
          if (std::abs(spin->value() - initial) > 1e-9) emit partSizeEdited(id, QString::fromStdString(key), spin->value());
        });
        form_->addRow(name, spin);
      }
    }
  }

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
  // Lock: shown first for images (calibrate, then lock to trace over).
  {
    auto* lock = new QCheckBox(tr("ロック（選択・移動・削除しない。吸着はする）"));
    lock->setObjectName(QStringLiteral("lockCheck"));
    lock->setChecked(o->locked);
    connect(lock, &QCheckBox::toggled, this, [this, id](bool on) { emit lockToggled(id, on); });
    form_->addRow(tr("ロック"), lock);
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
    // Pressing it offers the library: assign to this object, edit, or new.
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("materialButton"));
    button->setText(text + QStringLiteral(" ▾"));
    button->setPopupMode(QToolButton::InstantPopup);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setToolTip(tr("部材全体のマテリアルを選ぶ・編集する。面ごとに塗るときはペイントツール（B）。面の素材が部材全体より優先されます"));
    if (const doc::Material* mat = document.findMaterial(o->material)) {
      QPixmap swatch(16, 12);
      swatch.fill(QColor::fromRgbF(mat->color.r, mat->color.g, mat->color.b));
      button->setIcon(QIcon(swatch));
    }
    auto* menu = new QMenu(button);
    for (const doc::Material& mat : document.materials()) {
      QPixmap swatch(16, 12);
      swatch.fill(QColor::fromRgbF(mat.color.r, mat.color.g, mat.color.b));
      const std::string mid = mat.id;
      QAction* a = menu->addAction(QIcon(swatch), QString::fromStdString(mat.name),
                                   this, [this, id, mid] { emit materialAssigned(id, mid); });
      a->setCheckable(true);
      a->setChecked(mid == o->material);
    }
    menu->addAction(tr("（なし：グループ・配置から継承）"), this, [this, id] { emit materialAssigned(id, {}); });
    menu->addSeparator();
    if (!o->material.empty()) {
      const std::string mid = o->material;
      menu->addAction(tr("このマテリアルの名前・色を変更..."), this, [this, mid] { emit materialEditRequested(mid); });
    }
    menu->addAction(tr("新しいマテリアル..."), this, [this] { emit materialEditRequested({}); });
    button->setMenu(menu);
    form_->addRow(tr("マテリアル"), button);
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
    if (p.key.rfind("in", 0) == 0 || !partKind.empty()) {
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
    const doc::ObjectId instanceId = o->id;
    const auto& ref = std::get<doc::ComponentInstance>(o->recipe.base);
    const int count = static_cast<int>(document.instanceCount(defId));
    form_->addRow(new QLabel(tr("<b>コンポーネント「%1」</b>（配置 %2 個）")
                                 .arg(QString::fromStdString(def->name).toHtmlEscaped())
                                 .arg(count)));
    // What a dimension edit reaches: every placement, or only this one.
    auto* scope = new QComboBox;
    scope->addItem(tr("すべての配置（%1 個）に反映").arg(count));
    scope->addItem(tr("この配置だけ変更"));
    scope->setCurrentIndex(placementOnly_ ? 1 : 0);
    const doc::Modeler* source = &modeler;
    const std::vector<doc::ObjectId> selected = selection;
    connect(scope, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, source, selected](int index) {
      placementOnly_ = index == 1;
      QTimer::singleShot(0, this, [this, source, selected] { refresh(*source, selected); });
    });
    form_->addRow(tr("寸法の変更"), scope);
    int own = 0;
    for (const doc::Modeler::PlacementParameter& p : modeler.placementParameters(instanceId)) {
      own += p.own;
      const double shown = placementOnly_ ? p.parameter.value : p.shared;
      auto* spin = lengthSpin(shown);
      if (!p.parameter.isLength) spin->setSuffix(QString());
      adjustForKey(spin, p.parameter.key);
      const std::string key = p.parameter.key;
      const doc::ObjectId childId = p.object;
      const bool only = placementOnly_;
      connect(spin, &QDoubleSpinBox::editingFinished, this,
              [this, defId, instanceId, childId, key, spin, shown, only] {
                if (std::abs(spin->value() - shown) <= 1e-9) return;
                if (only) {
                  emit placementParameterEdited(instanceId, childId, key, spin->value());
                } else {
                  emit definitionParameterEdited(defId, childId, key, spin->value());
                }
              });
      QString label = QString::fromStdString(p.objectName) + QStringLiteral(": ") + parameterLabel(p.parameter);
      if (p.own) {
        // Marked in both modes: a shared edit does not reach this placement.
        label += tr(" ＊");
        spin->setToolTip(tr("この配置だけ %1（共通は %2）")
                             .arg(QString::number(p.parameter.value), QString::number(p.shared)));
      }
      form_->addRow(label, spin);
    }
    if (own > 0) {
      auto* note = new QLabel(tr("＊ この配置だけの寸法 %1 件（共通寸法を変えてもこの配置は変わりません）").arg(own));
      note->setWordWrap(true);
      note->setStyleSheet(QStringLiteral("color: #8a5a00; font-size: 9pt"));
      form_->addRow(note);
    }
    if (ref.mirrored) form_->addRow(new QLabel(tr("この配置は鏡像です")));

    auto* actions = new QWidget;
    auto* h = new QHBoxLayout(actions);
    h->setContentsMargins(0, 0, 0, 0);
    auto addAction = [&](const QString& text, const QString& tip, const QString& action, bool enabled = true) {
      auto* b = new QToolButton;
      b->setText(text);
      b->setToolTip(tip);
      b->setEnabled(enabled);
      connect(b, &QToolButton::clicked, this, [this, instanceId, action] { emit placementAction(instanceId, action); });
      h->addWidget(b);
    };
    addAction(tr("独立"), tr("この配置だけ別のコンポーネントにする（ほかの %1 個はそのまま）").arg(count - 1),
              QStringLiteral("unique"), count > 1);
    addAction(tr("寸法を戻す"), tr("この配置だけの寸法をやめて共通寸法に戻す"), QStringLiteral("reset"), own > 0);
    addAction(tr("鏡像"), tr("赤軸方向に反転（その場で）"), QStringLiteral("mirror0"));
    addAction(tr("軸…"), tr("原点と赤軸の向きを決め直す（どの配置も動きません）"), QStringLiteral("axes"));
    h->addStretch();
    form_->addRow(actions);
  }

  // Openings cut into this object (walls): width x height, editable.
  if (const auto openings = modeler.openings(id); openings && !openings->empty()) {
    if (const std::size_t focusOpening = focusOpening_; focusOpening > 0) {
      // One opening picked by a face of its hole: it alone (docs/110).
      auto* heading = new QLabel(tr("<b>開口 %1 を選んでいます</b>").arg(focusOpening));
      heading->setObjectName(QStringLiteral("openingFocus"));
      form_->addRow(heading);
      auto* hint = new QLabel(tr("Delete でふさぐ。移動ツールで穴をつかむと動かせます（Ctrl でコピー）。壁全体はトリプルクリックで選べます"));
      hint->setWordWrap(true);
      form_->addRow(hint);
    } else {
      form_->addRow(new QLabel(tr("<b>開口</b>")));
    }
    nlohmann::json fittings = nlohmann::json::object();
    if (const doc::Object* host = modeler.document().find(id)) {
      if (const auto it = host->attributes.find("opening_fittings"); it != host->attributes.end()) {
        fittings = nlohmann::json::parse(it->second, nullptr, false);
        if (!fittings.is_object()) fittings = nlohmann::json::object();
      }
    }
    // Door / window kinds, shown in the plan (cut) and on plan drawings. 外 = the side the
    // opening was made from, 内 = the other side; left / right as seen from the side it opens to.
    std::vector<std::pair<QString, std::string>> kinds;
    for (const auto& c : draw::fittingChoices()) kinds.emplace_back(QString::fromStdString(c.label), c.fitting);
    // Their marks as the schedule has them (docs/104, docs/109): one's own, or worked out.
    std::map<std::string, const doc::OpeningRow*> marks;
    const doc::OpeningSchedule schedule = doc::openingSchedule(modeler.document());
    for (const doc::OpeningRow& r : schedule.rows)
      if (r.host == id) marks[r.openingId] = &r;
    for (const doc::Modeler::OpeningInfo& op : *openings) {
      if (focusOpening_ > 0 && op.index != focusOpening_) continue;
      auto* row = new QWidget;
      auto* h = new QHBoxLayout(row);
      h->setContentsMargins(0, 0, 0, 0);
      auto* w = lengthSpin(op.width);
      auto* ht = lengthSpin(op.height);
      w->setRange(1, 1e9);
      ht->setRange(1, 1e9);
      w->setPrefix(tr("幅 "));
      ht->setPrefix(tr("高さ "));
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
      auto* kind = new QComboBox;
      kind->setObjectName(QStringLiteral("openingFitting%1").arg(index));
      kind->setToolTip(tr("平面（切断）と平面図に建具の記号を描きます。外＝開口をあけた面の側、内＝反対側。"
                          "左右は開く側から見て（引戸は開口をあけた面の側から見て）"));
      const nlohmann::json current = fittings.contains(op.id) ? fittings.at(op.id) : nlohmann::json{{"type", "none"}};
      for (const auto& [label, value] : kinds) {
        kind->addItem(label, QString::fromStdString(value));
        if (nlohmann::json::parse(value) == current) kind->setCurrentIndex(kind->count() - 1);
      }
      connect(kind, &QComboBox::activated, this, [this, id, index, kind] {
        emit openingFittingEdited(id, index, kind->currentData().toString());
      });
      h->addWidget(kind, 1);
      row->setObjectName(QStringLiteral("opening%1").arg(index));
      // Where it sits in the wall, and removing it. The opening keeps its fitting when it moves,
      // and the other openings keep theirs when it is removed (they are kept by id, not number).
      auto* place = new QWidget;
      auto* ph = new QHBoxLayout(place);
      ph->setContentsMargins(0, 0, 0, 0);
      auto* offset = lengthSpin(op.offset);
      auto* sill = lengthSpin(op.sill);
      offset->setRange(0, 1e9);
      sill->setRange(0, 1e9);
      offset->setObjectName(QStringLiteral("openingOffset%1").arg(index));
      sill->setObjectName(QStringLiteral("openingSill%1").arg(index));
      offset->setToolTip(tr("壁の左端から開口の左の縁まで（開口をあけた面の側から見て）"));
      sill->setToolTip(tr("壁の下端から開口の下の縁まで（腰高）"));
      sill->setPrefix(tr("腰高 "));
      offset->setPrefix(tr("左端から "));
      ph->addWidget(sill);
      ph->addWidget(offset);
      const double o0 = op.offset, s0 = op.sill;
      auto moved = [this, id, index, offset, sill, o0, s0] {
        if (std::abs(offset->value() - o0) > 1e-9 || std::abs(sill->value() - s0) > 1e-9)
          emit openingPlaced(id, index, offset->value(), sill->value());
      };
      connect(offset, &QDoubleSpinBox::editingFinished, this, moved);
      connect(sill, &QDoubleSpinBox::editingFinished, this, moved);
      auto* remove = new QToolButton;
      remove->setObjectName(QStringLiteral("openingRemove%1").arg(index));
      remove->setText(tr("削除"));
      remove->setToolTip(tr("この開口をふさぎます。ほかの開口の建具・寸法は変わりません"));
      connect(remove, &QToolButton::clicked, this, [this, id, index] { emit openingRemoved(id, index); });
      ph->addWidget(remove);
      place->setObjectName(QStringLiteral("openingPlace%1").arg(index));
      // Its mark and product number (docs/109): typed in, they stay whatever else changes;
      // left empty, the mark is worked out from kind and size (shown greyed).
      auto* tag = new QWidget;
      auto* th = new QHBoxLayout(tag);
      th->setContentsMargins(0, 0, 0, 0);
      const auto found = marks.find(op.id);
      const doc::OpeningRow* scheduled = found != marks.end() ? found->second : nullptr;
      const QString own = scheduled && scheduled->ownMark ? QString::fromStdString(scheduled->mark) : QString();
      const QString product = scheduled ? QString::fromStdString(scheduled->product) : QString();
      auto* markEdit = new QLineEdit(own);
      markEdit->setObjectName(QStringLiteral("openingMark%1").arg(index));
      markEdit->setPlaceholderText(scheduled && !scheduled->ownMark ? tr("符号 %1（自動）").arg(QString::fromStdString(scheduled->mark))
                                                                     : tr("符号"));
      markEdit->setToolTip(tr("建具表と平面の札に出る符号。入れると、開口を足したり大きさを変えたりしても変わりません。"
                              "空にすると、種類・建具・大きさから自動で付けます"));
      markEdit->setMaxLength(40);
      auto* productEdit = new QLineEdit(product);
      productEdit->setObjectName(QStringLiteral("openingProduct%1").arg(index));
      productEdit->setPlaceholderText(tr("品番"));
      productEdit->setToolTip(tr("建具表と CSV の「品番」の列に出ます"));
      productEdit->setMaxLength(120);
      markEdit->setEnabled(!modeler.document().find(id) || !modeler.document().find(id)->locked);
      productEdit->setEnabled(markEdit->isEnabled());
      th->addWidget(markEdit, 1);
      th->addWidget(productEdit, 2);
      auto named = [this, id, index, markEdit, productEdit, own, product] {
        const QString m = markEdit->text().trimmed(), p = productEdit->text().trimmed();
        if (m != own || p != product) emit openingMarkEdited(id, index, m, p);
      };
      connect(markEdit, &QLineEdit::editingFinished, this, named);
      connect(productEdit, &QLineEdit::editingFinished, this, named);
      tag->setObjectName(QStringLiteral("openingTag%1").arg(index));
      // One row per opening: its size and fitting, its mark, then where it sits.
      auto* cell = new QWidget;
      auto* lines = new QVBoxLayout(cell);
      lines->setContentsMargins(0, 0, 0, 0);
      lines->setSpacing(2);
      lines->addWidget(row);
      lines->addWidget(tag);
      lines->addWidget(place);
      // docs/147: a canopy over it.
      if (focusOpening_ > 0) {
        auto* canopy = new QToolButton;
        canopy->setObjectName(QStringLiteral("openingCanopy"));
        canopy->setText(tr("この開口に庇を付ける（出 600）"));
        canopy->setToolTip(tr("開口をあけた面の側に、出 600・厚さ 60・両側に 150 長い庇を、開口の上端より 100 上に付けます。"
                              "大きさは庇を選んで変えられます（AI からは make_canopy）"));
        canopy->setEnabled(markEdit->isEnabled());
        connect(canopy, &QToolButton::clicked, this, [this, id, index] { emit canopyRequested(id, index); });
        lines->addWidget(canopy);
      }
      // docs/133: one picked, others of its mark elsewhere: make them all like this one.
      if (focusOpening_ > 0 && scheduled) {
        int same = 0;
        for (const doc::OpeningRow& r : schedule.rows) same += r.mark == scheduled->mark;
        if (same > 1) {
          auto* match = new QToolButton;
          match->setObjectName(QStringLiteral("openingMatchType"));
          match->setText(tr("同じ符号（%1）のほか %2 個を、これと同じ大きさ・建具・品番にする").arg(QString::fromStdString(scheduled->mark)).arg(same - 1));
          match->setToolTip(tr("建具表で同じ符号の開口（どの壁でも）が、この開口の幅・高さ・建具・品番になります。位置と腰高はそのまま。Ctrl+Z 1 回で戻せます"));
          match->setEnabled(markEdit->isEnabled());
          connect(match, &QToolButton::clicked, this, [this, id, index] { emit openingTypeMatched(id, index); });
          lines->addWidget(match);
        }
      }
      form_->addRow(tr("開口 %1").arg(index), cell);
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
  if (o->follow) {
    const doc::Object* host = modeler.document().find(o->follow->host);
    auto* label = new QLabel(host ? tr("「%1」に追従中").arg(QString::fromStdString(host->name))
                                  : tr("追従先がありません（その場に保持。戻れば再び追従）"));
    label->setObjectName(QStringLiteral("followLabel"));
    detailsForm->addRow(tr("追従"), label);
  }
  if (o->kind() == doc::ObjectKind::Solid || o->kind() == doc::ObjectKind::Shell || o->kind() == doc::ObjectKind::Face) {
    // Role and material class (docs/41): added to any shape afterwards;
    // they change nothing about it (no resizing, no rules).
    auto choose = [&](const QString& label, const char* key, const QString& objectName,
                      std::initializer_list<std::pair<QString, QString>> options) {
      auto* box = new QComboBox;
      box->setObjectName(objectName);
      for (const auto& [text, value] : options) box->addItem(text, value);
      const auto it = o->attributes.find(key);
      const QString now = it == o->attributes.end() ? QString() : QString::fromStdString(it->second);
      int index = box->findData(now);
      if (index < 0) {  // a value set elsewhere: shown as it is
        box->addItem(now, now);
        index = box->count() - 1;
      }
      box->setCurrentIndex(index);
      const std::string attribute = key;
      connect(box, &QComboBox::activated, this, [this, id, box, attribute] {
        emit attributeEdited(id, attribute, box->currentData().toString().toStdString());
      });
      detailsForm->addRow(label, box);
    };
    choose(tr("用途"), "kind", QStringLiteral("roleBox"),
           {{tr("なし"), QString()}, {tr("柱"), QStringLiteral("column")}, {tr("梁"), QStringLiteral("beam")},
            {tr("筋かい"), QStringLiteral("brace")}, {tr("壁"), QStringLiteral("wall")}, {tr("スラブ"), QStringLiteral("slab")}});
    choose(tr("材料種別"), "material_class", QStringLiteral("materialClassBox"),
           {{tr("未設定"), QString()}, {tr("木"), QStringLiteral("timber")}, {tr("鋼"), QStringLiteral("steel")},
            {tr("RC"), QStringLiteral("rc")}});
  }
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
