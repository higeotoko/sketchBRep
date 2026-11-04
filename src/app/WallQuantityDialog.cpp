#include "WallQuantityDialog.h"
#include "MainWindow.h"
#include "sbr/ConstructionWall.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <cmath>

namespace sbr::app {
namespace {
using json = nlohmann::json;
QString display(const json& v) {
  return v.is_null() ? QString() : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
QTableWidgetItem* item(const json& v) { return new QTableWidgetItem(display(v)); }
QTableWidgetItem* checked(bool value) {
  auto* result = new QTableWidgetItem;
  result->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
  result->setCheckState(value ? Qt::Checked : Qt::Unchecked);
  return result;
}
QString cell(QTableWidget* t, int r, int c) {
  return t->item(r, c) ? t->item(r, c)->text().trimmed() : QString();
}
QString reasons(const json& values) {
  QStringList result;
  for (const auto& value : values) result << display(value);
  return result.join(QStringLiteral("／"));
}
json numeric(const QString& text) {
  if (text.trimmed().isEmpty()) return nullptr;
  bool ok = false;
  const double v = text.toDouble(&ok);
  if (!ok || !std::isfinite(v)) throw std::runtime_error("数値の欄に数値以外が入力されています");
  return v;
}
void setup(QTableWidget* t, const QStringList& labels) {
  t->setColumnCount(labels.size());
  t->setHorizontalHeaderLabels(labels);
  t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
}
}  // namespace
WallQuantityDialog::WallQuantityDialog(MainWindow* window) : QDialog(window), window_(window) {
  setObjectName(QStringLiteral("wallQuantityDialog"));
  setWindowTitle(tr("木造の壁量（入力と比較）"));
  resize(1160, 820);
  auto* layout = new QVBoxLayout(this);
  auto* explanation = new QLabel(
      tr("荷重は別途算定して入力します。空欄は未評価です。耐力壁の指定は形状を変更しません。\n"
         "床面積は小屋裏等を考慮した算定用面積、荷重はその階から上の累積kN。階は下から上の順です。"),
      this);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto* form = new QFormLayout;
  height_ = new QLineEdit(this);
  height_->setObjectName(QStringLiteral("wallHeight"));
  co_ = new QComboBox(this);
  co_->setObjectName(QStringLiteral("wallCo"));
  co_->addItems({tr("未確認"), tr("C0通常区域（同梱表0.2）"), tr("C0指定区域（同梱表0.3）")});
  wind_ = new QLineEdit(this);
  wind_->setObjectName(QStringLiteral("wallWindCoefficient"));
  wind_->setPlaceholderText(tr("通常区域50／強風の指定区域は指定された値（50超75以下）"));
  basis_ = new QLineEdit(this);
  basis_->setObjectName(QStringLiteral("wallInputBasis"));
  form->addRow(tr("建物の高さ m"), height_);
  form->addRow(tr("区域の確認"), co_);
  form->addRow(tr("風係数 cm/m²"), wind_);
  form->addRow(tr("荷重・面積・区域の入力根拠"), basis_);
  layout->addLayout(form);
  auto* tabs = new QTabWidget(this);
  auto* floorsPage = new QWidget(tabs);
  auto* fl = new QVBoxLayout(floorsPage);
  floors_ = new QTableWidget(this);
  floors_->setObjectName(QStringLiteral("wallFloors"));
  setup(floors_, {tr("階ID"), tr("階名"), tr("算定用床面積 m²"), tr("累積荷重 kN"), tr("X風見付 m²"),
                  tr("Y風見付 m²"), tr("壁入力完了")});
  fl->addWidget(new QLabel(tr("風見付面積：当該階から上の投影面積から、床上1."
                              "35m以下を控除した値。入力した階の壁が揃ってから「壁入力完了」。"),
                           this));
  fl->addWidget(floors_);
  auto* floorButtons = new QHBoxLayout;
  auto* addFloorButton = new QPushButton(tr("階を追加"), this);
  auto* removeFloorButton = new QPushButton(tr("選択階を削除"), this);
  floorButtons->addWidget(addFloorButton);
  floorButtons->addWidget(removeFloorButton);
  fl->addLayout(floorButtons);
  tabs->addTab(floorsPage, tr("階と荷重"));
  auto* wallPage = new QWidget(tabs);
  auto* wl = new QVBoxLayout(wallPage);
  auto* wallHelp =
      new QLabel(tr("区間は世界XY座標mm。X/"
                    "Yは抵抗する水平力の方向。仕様の原典・施工・接合条件を確認してから「仕様確認」。\n"
                    "通り芯参照の区間は芯の編集に追従します。筋かい高さは横架材上端間。準耐力壁・組合せ壁は今"
                    "回の対象外。"),
                 this);
  wallHelp->setWordWrap(true);
  wl->addWidget(wallHelp);
  walls_ = new QTableWidget(this);
  walls_->setObjectName(QStringLiteral("wallDesignations"));
  setup(walls_, {tr("壁ID"), tr("階ID"), tr("方向"), tr("始X"), tr("始Y"), tr("終X"), tr("終Y"),
                 tr("高さ mm"), tr("仕様"), tr("仕様確認")});
  wl->addWidget(walls_);
  auto* wallButtons = new QHBoxLayout;
  auto* addWallButton = new QPushButton(tr("手動区間を追加"), this);
  addWallButton->setObjectName(QStringLiteral("wallAdd"));
  auto* gridButton = new QPushButton(tr("通り芯から区間を追加..."), this);
  auto* removeWallButton = new QPushButton(tr("選択壁を削除"), this);
  wallButtons->addWidget(addWallButton);
  wallButtons->addWidget(gridButton);
  wallButtons->addWidget(removeWallButton);
  wl->addLayout(wallButtons);
  tabs->addTab(wallPage, tr("耐力壁の指定"));
  layout->addWidget(tabs, 1);
  auto* tableButtons = new QHBoxLayout;
  auto* builtinButton = new QPushButton(tr("係数表を現在の同梱版へ切替"), this);
  auto* tableLoad = new QPushButton(tr("出典付きの係数表JSONを読み込む..."), this);
  auto* tableRemove = new QPushButton(tr("係数表を外す"), this);
  tableButtons->addWidget(builtinButton);
  tableButtons->addWidget(tableLoad);
  tableButtons->addWidget(tableRemove);
  layout->addLayout(tableButtons);
  notice_ = new QTextBrowser(this);
  notice_->setObjectName(QStringLiteral("wallNotice"));
  notice_->setMaximumHeight(110);
  layout->addWidget(notice_);
  results_ = new QTableWidget(this);
  results_->setObjectName(QStringLiteral("wallQuantityResults"));
  setup(results_, {tr("階"), tr("方向"), tr("存在 m"), tr("地震必要 m"), tr("風必要 m"), tr("必要 m"),
                   tr("評価・理由")});
  results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  results_->setMaximumHeight(165);
  layout->addWidget(results_);
  status_ = new QLabel(this);
  status_->setWordWrap(true);
  status_->setObjectName(QStringLiteral("wallQuantityStatus"));
  layout->addWidget(status_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto* apply = buttons->addButton(tr("入力をモデルに保存（Undo可）"), QDialogButtonBox::ActionRole);
  auto* reloadButton = buttons->addButton(tr("モデルから再読込"), QDialogButtonBox::ActionRole);
  auto* exportButton = buttons->addButton(tr("比較表CSV..."), QDialogButtonBox::ActionRole);
  layout->addWidget(buttons);
  debounce_ = new QTimer(this);
  debounce_->setInterval(150);
  debounce_->setSingleShot(true);
  connect(debounce_, &QTimer::timeout, this, &WallQuantityDialog::refreshNow);
  poll_ = new QTimer(this);
  poll_->setInterval(150);
  connect(poll_, &QTimer::timeout, this, [this] {
    if (window_->modeler().history().revision() != revision_) {
      report_.clear();
      results_->setRowCount(0);
      status_->setText(tr(
          "モデルが変更されました。再読込で最新のモデルを取得してください（入力中の変更は破棄されます）。"));
    }
  });
  poll_->start();
  for (auto* field : {height_, wind_, basis_})
    connect(field, &QLineEdit::textChanged, this, [this] { changed(); });
  connect(co_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { changed(); });
  for (auto* table : {floors_, walls_})
    connect(table, &QTableWidget::itemChanged, this, [this] { changed(); });
  connect(addFloorButton, &QPushButton::clicked, this, [this] {
    if (floors_->rowCount() < 2) {
      int id = 1;
      while (floors_->rowCount() && cell(floors_, 0, 0) == QString::number(id)) ++id;
      addFloor(json{{"id", std::to_string(id)}, {"name", std::to_string(id) + "階"}}.dump());
    }
    changed();
  });
  connect(removeFloorButton, &QPushButton::clicked, this, [this] {
    floors_->removeRow(floors_->currentRow());
    changed();
  });
  connect(addWallButton, &QPushButton::clicked, this, [this] {
    int next = 1;
    while (true) {
      bool found = false;
      for (int r = 0; r < walls_->rowCount(); ++r)
        if (cell(walls_, r, 0) == "W" + QString::number(next)) found = true;
      if (!found) break;
      ++next;
    }
    addWall(json{{"id", "W" + std::to_string(next)},
                 {"floor", cell(floors_, 0, 0).toStdString()},
                 {"direction", "X"},
                 {"spec", "brace_45x90"}}
                .dump());
    changed();
  });
  connect(removeWallButton, &QPushButton::clicked, this, [this] {
    walls_->removeRow(walls_->currentRow());
    changed();
  });
  connect(gridButton, &QPushButton::clicked, this, [this] {
    QDialog picker(this);
    picker.setWindowTitle(tr("通り芯の区間"));
    auto* f = new QFormLayout(&picker);
    auto* grid = new QComboBox(&picker);
    auto* floor = new QComboBox(&picker);
    auto* direction = new QComboBox(&picker);
    direction->addItems({"X", "Y"});
    for (int r = 0; r < floors_->rowCount(); ++r) floor->addItem(cell(floors_, r, 1), cell(floors_, r, 0));
    for (const auto& o : snapshot_->objects()) {
      const auto* g = std::get_if<doc::GroupNode>(&o.recipe.base);
      if (g && g->grid) grid->addItem(QString::fromStdString(o.name), QString::fromStdString(o.id));
    }
    QComboBox *sx = new QComboBox(&picker), *sy = new QComboBox(&picker), *ex = new QComboBox(&picker),
              *ey = new QComboBox(&picker);
    auto fill = [this, grid, sx, sy, ex, ey] {
      for (auto* combo : {sx, sy, ex, ey}) combo->clear();
      const auto* o = snapshot_->find(grid->currentData().toString().toStdString());
      const auto* g = o ? std::get_if<doc::GroupNode>(&o->recipe.base) : nullptr;
      if (!g || !g->grid) return;
      for (const auto& a : g->grid->x)
        for (auto* c : {sx, ex}) c->addItem(QString::fromStdString(a.name), QString::fromStdString(a.id));
      for (const auto& a : g->grid->y)
        for (auto* c : {sy, ey}) c->addItem(QString::fromStdString(a.name), QString::fromStdString(a.id));
      ex->setCurrentIndex(std::min(1, ex->count() - 1));
    };
    connect(grid, qOverload<int>(&QComboBox::currentIndexChanged), &picker, fill);
    fill();
    f->addRow(tr("階"), floor);
    f->addRow(tr("方向"), direction);
    f->addRow(tr("通り芯"), grid);
    f->addRow(tr("始点X通り"), sx);
    f->addRow(tr("始点Y通り"), sy);
    f->addRow(tr("終点X通り"), ex);
    f->addRow(tr("終点Y通り"), ey);
    auto* b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
    f->addRow(b);
    connect(b, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(b, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    if (picker.exec() == QDialog::Accepted && grid->count() && floor->count() && sx->count() && sy->count()) {
      addWall(json{{"id", doc::generateObjectId()},
                   {"floor", floor->currentData().toString().toStdString()},
                   {"direction", direction->currentText().toStdString()},
                   {"spec", "brace_45x90"},
                   {"grid", grid->currentData().toString().toStdString()},
                   {"start_axes",
                    {sx->currentData().toString().toStdString(), sy->currentData().toString().toStdString()}},
                   {"end_axes",
                    {ex->currentData().toString().toStdString(), ey->currentData().toString().toStdString()}}}
                  .dump());
      changed();
    }
  });
  connect(builtinButton, &QPushButton::clicked, this, [this] { useTable(construction::wallTables()); });
  connect(tableRemove, &QPushButton::clicked, this, [this] { useTable("null"); });
  connect(tableLoad, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getOpenFileName(this, tr("係数表"), {}, tr("JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    try {
      if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
      const auto t = json::parse(file.readAll().toStdString());
      construction::normalizeWallInputs(json{{"table_snapshot", t}}.dump());
      useTable(t.dump());
    } catch (const std::exception& e) {
      status_->setText(QString::fromUtf8(e.what()));
    }
  });
  connect(apply, &QPushButton::clicked, this, [this] {
    const auto s = applyInputs();
    if (!s) status_->setText(QString::fromStdString(s.message()));
  });
  connect(reloadButton, &QPushButton::clicked, this, &WallQuantityDialog::reload);
  connect(exportButton, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getSaveFileName(this, tr("壁量比較表"), {}, tr("CSV (*.csv)"));
    if (!path.isEmpty()) {
      const auto s = exportCsv(path);
      if (!s) status_->setText(QString::fromStdString(s.message()));
    }
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  reload();
}
void WallQuantityDialog::addFloor(const std::string& value) {
  const auto f = json::parse(value);
  const int r = floors_->rowCount();
  floors_->insertRow(r);
  int c = 0;
  for (const auto* key : {"id", "name", "area_m2", "weight_kn", "wind_x_m2", "wind_y_m2"})
    floors_->setItem(r, c++, item(f.value(key, json())));
  floors_->setItem(r, 6, checked(f.value("walls_complete", false)));
}
void WallQuantityDialog::useTable(const std::string& table) {
  try {
    auto in = json::parse(inputs());
    in["table_snapshot"] = json::parse(table);
    construction::normalizeWallInputs(in.dump());
    filling_ = true;
    table_ = table;
    walls_->setRowCount(0);
    for (auto w : in.at("walls")) {
      w["verified"] = false;
      addWall(w.dump());
    }
    filling_ = false;
    changed();
    status_->setText(
        tr("係数表を切り替えました。各壁の施工・接合条件を確認し直してください（入力は未保存）。"));
  } catch (const std::exception& e) {
    filling_ = false;
    status_->setText(QString::fromUtf8(e.what()));
  }
}
void WallQuantityDialog::addWall(const std::string& value) {
  const auto w = json::parse(value);
  const int r = walls_->rowCount();
  walls_->insertRow(r);
  walls_->setItem(r, 0, item(w.at("id")));
  walls_->setItem(r, 1, item(w.at("floor")));
  walls_->setItem(r, 2, item(w.at("direction")));
  for (int c = 3; c <= 6; ++c) walls_->setItem(r, c, item(nullptr));
  if (w.contains("grid")) {
    walls_->item(r, 0)->setData(Qt::UserRole, QString::fromStdString(w.dump()));
    for (int c = 3; c <= 6; ++c) {
      walls_->item(r, c)->setText(display(w.at(c < 5 ? "start_axes" : "end_axes").at((c - 3) % 2)));
      walls_->item(r, c)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      walls_->item(r, c)->setToolTip(tr("通り芯IDを参照。芯の編集に追従します。"));
    }
  } else
    for (int c = 3; c <= 6; ++c) {
      const auto v = w.value(c < 5 ? "start" : "end", json());
      walls_->item(r, c)->setText(v.is_null() ? QString() : display(v.at((c - 3) % 2)));
    }
  walls_->setItem(r, 7, item(w.value("height_mm", json())));
  auto* spec = new QComboBox(walls_);
  const auto table = json::parse(table_);
  if (!table.is_null())
    for (const auto& [id, s] : table.at("entries").items()) {
      spec->addItem(display(s.at("name")), QString::fromStdString(id));
      spec->setItemData(spec->count() - 1, display(s.at("conditions")), Qt::ToolTipRole);
    }
  const auto id = display(w.at("spec"));
  if (spec->findData(id) < 0) spec->addItem(id + tr("（表なし）"), id);
  spec->setCurrentIndex(spec->findData(id));
  walls_->setCellWidget(r, 8, spec);
  connect(spec, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, spec] {
    for (int row = 0; row < walls_->rowCount(); ++row)
      if (walls_->cellWidget(row, 8) == spec) walls_->item(row, 9)->setCheckState(Qt::Unchecked);
    changed();
  });
  walls_->setItem(r, 9, checked(w.value("verified", false)));
}
void WallQuantityDialog::reload() {
  filling_ = true;
  revision_ = window_->modeler().history().revision();
  snapshot_ = std::make_shared<const doc::Document>(window_->modeler().document());
  const auto in = json::parse(construction::normalizeWallInputs(snapshot_->settings().wallCalculation));
  table_ = in.at("table_snapshot").dump();
  frame_ = in.value("frame", std::string());
  height_->setText(display(in.value("height_m", json())));
  wind_->setText(display(in.value("wind_coefficient_cm_m2", json())));
  basis_->setText(display(in.value("input_basis", json())));
  co_->setCurrentIndex(!in.contains("co_designated") || in.at("co_designated").is_null() ? 0
                       : in.at("co_designated").get<bool>()                              ? 2
                                                                                         : 1);
  floors_->setRowCount(0);
  walls_->setRowCount(0);
  for (const auto& f : in.at("floors")) addFloor(f.dump());
  for (const auto& w : in.at("walls")) addWall(w.dump());
  filling_ = false;
  refreshNow();
}
std::string WallQuantityDialog::inputs() const {
  json in = {{"height_m", numeric(height_->text())},
             {"wind_coefficient_cm_m2", numeric(wind_->text())},
             {"co_designated", co_->currentIndex() == 0 ? json() : json(co_->currentIndex() == 2)},
             {"input_basis", basis_->text().toStdString()},
             {"table_snapshot", json::parse(table_)},
             {"floors", json::array()},
             {"walls", json::array()}};
  if (!frame_.empty()) in["frame"] = frame_;
  for (int r = 0; r < floors_->rowCount(); ++r)
    in["floors"].push_back({{"id", cell(floors_, r, 0).toStdString()},
                            {"name", cell(floors_, r, 1).toStdString()},
                            {"area_m2", numeric(cell(floors_, r, 2))},
                            {"weight_kn", numeric(cell(floors_, r, 3))},
                            {"wind_x_m2", numeric(cell(floors_, r, 4))},
                            {"wind_y_m2", numeric(cell(floors_, r, 5))},
                            {"walls_complete", floors_->item(r, 6)->checkState() == Qt::Checked}});
  for (int r = 0; r < walls_->rowCount(); ++r) {
    const auto* combo = qobject_cast<QComboBox*>(walls_->cellWidget(r, 8));
    json w = {{"id", cell(walls_, r, 0).toStdString()},
              {"floor", cell(walls_, r, 1).toStdString()},
              {"direction", cell(walls_, r, 2).toStdString()},
              {"height_mm", numeric(cell(walls_, r, 7))},
              {"spec", combo->currentData().toString().toStdString()},
              {"verified", walls_->item(r, 9)->checkState() == Qt::Checked}};
    const auto reference = walls_->item(r, 0)->data(Qt::UserRole).toString();
    if (!reference.isEmpty()) {
      const auto ref = json::parse(reference.toStdString());
      for (const auto* key : {"grid", "start_axes", "end_axes"}) w[key] = ref.at(key);
    } else {
      for (int c : {3, 5}) {
        if (!cell(walls_, r, c).isEmpty() && !cell(walls_, r, c + 1).isEmpty())
          w[c == 3 ? "start" : "end"] = {numeric(cell(walls_, r, c)), numeric(cell(walls_, r, c + 1))};
      }
    }
    in["walls"].push_back(w);
  }
  return in.dump();
}
void WallQuantityDialog::changed() {
  if (filling_) return;
  report_.clear();
  results_->setRowCount(0);
  status_->setText(tr("再計算中（入力は未保存）…"));
  debounce_->start();
}
void WallQuantityDialog::refreshNow() {
  debounce_->stop();
  report_.clear();
  results_->setRowCount(0);
  if (window_->modeler().history().revision() != revision_) {
    status_->setText(tr("モデルが変更されました。再読込が必要です。"));
    return;
  }
  try {
    const auto result =
        construction::checkWallQuantity(*snapshot_, json{{"inputs", json::parse(inputs())}}.dump());
    if (!result) throw std::runtime_error(result.error());
    auto report = json::parse(*result);
    report["revision"] = revision_;
    report_ = report.dump();
    notice_->setPlainText(display(report.at("notice")));
    results_->setRowCount(report.at("rows").size());
    int r = 0;
    for (const auto& row : report.at("rows")) {
      int c = 0;
      for (const auto* key : {"floor", "direction", "existing_m", "earthquake_m", "wind_m", "required_m"}) {
        const auto v = row.value(key, json());
        results_->setItem(r, c++,
                          new QTableWidgetItem(v.is_null()     ? tr("未評価")
                                               : v.is_number() ? QString::number(v.get<double>(), 'f', 3)
                                                               : display(v)));
      }
      QString status = row.at("status") == "pass"   ? tr("合格（壁量比較のみ）")
                       : row.at("status") == "fail" ? tr("不適合（壁量不足）")
                                                    : tr("未評価");
      status += " " + reasons(row.at("reasons"));
      for (const auto& w : report.at("walls"))
        if (w.at("floor") == row.at("floor") && w.at("direction") == row.at("direction") &&
            !w.at("reasons").empty())
          status += " " + display(w.at("id")) + ": " + reasons(w.at("reasons"));
      results_->setItem(r++, c, new QTableWidgetItem(status));
    }
    status_->setText(
        tr("開いた時点のモデル・編集中の入力による比較。入力をモデルに保存すると、保存・Undo・架構チェックに"
           "反映されます。"));
  } catch (const std::exception& e) {
    status_->setText(QString::fromUtf8(e.what()));
  }
}
Status WallQuantityDialog::applyInputs() {
  refreshNow();
  if (report_.empty()) return Status::failure(status_->text().toStdString());
  const auto result = window_->modeler().setWallCalculation(inputs());
  if (result) {
    reload();
    status_->setText(tr("壁量の入力・係数表をモデルに保存しました（Undo可）。"));
  }
  return result;
}
Status WallQuantityDialog::exportCsv(const QString& path) {
  refreshNow();
  if (report_.empty()) return Status::failure(status_->text().toStdString());
  const auto bytes = construction::wallQuantityCsv(report_);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) ||
      !file.commit())
    return Status::failure(file.errorString().toStdString());
  return Status::success();
}
}  // namespace sbr::app
