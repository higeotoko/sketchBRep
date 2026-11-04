#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include "AiBridge.h"
#include "MainWindow.h"
#include "sbr/ConstructionGuide.h"

namespace sbr::app {
using nlohmann::json;

bool MainWindow::previewTimberFrame(const std::string& inputs, const std::string& replace,
                                    bool preserveEdited) {
  json command = {{"command", "generate_frame"},
                  {"args",
                   {{"system", "timber"},
                    {"recipe", "post_beam_2storey"},
                    {"inputs", json::parse(inputs)},
                    {"preview", false}}}};
  if (!replace.empty()) command["args"]["replace"] = replace;
  if (preserveEdited) command["args"]["on_modified"] = "preserve";
  const json request = {
      {"command", "propose"},
      {"args", {{"commands", json::array({command})}, {"description", "木造架構のプレビュー"}}}};
  const json response = json::parse(aiBridge_->execute(request.dump()));
  if (!response.value("ok", false)) {
    lastError_ = QString::fromStdString(response.value("error", std::string("生成できませんでした")));
    return false;
  }
  proposalLabel_->setText(tr("木造架構のプレビュー — 「適用」で確定、「却下」で取り消し"));
  setStatus(
      tr("木造架構を青色でプレビューしています。寸法・接合は概略値です。確認して「適用」を押してください"));
  return true;
}

void MainWindow::showTimberFrameDialog() {
  const auto& patches = modeler_.document().settings().constructionOverrides;
  const auto found = patches.find("timber");
  const auto guide =
      json::parse(construction::guideData("timber", found == patches.end() ? "{}" : found->second));
  const auto value = [&](const char* key) {
    return guide.at("presets").at("entries").at(key).at("value").get<double>();
  };
  std::string grid, frame;
  for (const auto& id : selection_) {
    if (modeler_.gridSpec(id)) grid = id;
    const doc::Object* o = modeler_.document().find(id);
    while (o) {
      if (const auto* g = std::get_if<doc::GroupNode>(&o->recipe.base); g && !g->constructionFrame.empty()) {
        frame = o->id;
        break;
      }
      o = o->parent.empty() ? nullptr : modeler_.document().find(o->parent);
    }
  }
  json initial = guide.at("recipes").at("entries").at("post_beam_2storey").at("defaults");
  if (!frame.empty()) {
    const auto& state =
        std::get<doc::GroupNode>(modeler_.document().find(frame)->recipe.base).constructionFrame;
    initial.merge_patch(json::parse(state).at("inputs"));
    if (grid.empty() && initial.contains("grid")) grid = initial.at("grid").get<std::string>();
  }
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("timberFrameDialog"));
  dialog.setWindowTitle(tr("木造架構を作成"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* intro =
      new QLabel(tr("在来軸組・和小屋の概略形を作ります。プレビューしてから確定できます。<br>"
                    "梁せいは仮の値、金物の製品と継手は未選定です。構造の照査は行いません。"));
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* form = new QFormLayout;
  layout->addLayout(form);
  auto* useGrid = new QCheckBox(tr("選択中の通り芯を使う"));
  useGrid->setObjectName(QStringLiteral("timberUseGrid"));
  useGrid->setChecked(!grid.empty());
  useGrid->setEnabled(!grid.empty());
  form->addRow(useGrid);
  auto* replacing = new QCheckBox(tr("選択した架構を作り直す"));
  replacing->setObjectName(QStringLiteral("timberReplace"));
  replacing->setChecked(!frame.empty());
  replacing->setEnabled(!frame.empty());
  form->addRow(replacing);
  auto* x = new QSpinBox;
  x->setObjectName(QStringLiteral("timberXBays"));
  x->setRange(1, 40);
  x->setValue(initial.value("x_bays", 10));
  auto* y = new QSpinBox;
  y->setObjectName(QStringLiteral("timberYBays"));
  y->setRange(1, 40);
  y->setValue(initial.value("y_bays", 8));
  form->addRow(tr("X方向のマス数"), x);
  form->addRow(tr("Y方向のマス数"), y);
  auto numeric = [&](const char* id, double initial, double minimum, double maximum, const QString& suffix) {
    auto* spin = new QDoubleSpinBox;
    spin->setObjectName(QString::fromLatin1(id));
    spin->setRange(minimum, maximum);
    spin->setDecimals(1);
    spin->setValue(initial);
    spin->setSuffix(suffix);
    return spin;
  };
  auto* module = numeric("timberModule", initial.value("module", value("module")), 100, 10000, tr(" mm"));
  form->addRow(tr("モジュール"), module);
  auto* storeys = new QComboBox;
  storeys->setObjectName(QStringLiteral("timberStoreys"));
  storeys->addItem(tr("平屋"), 1);
  storeys->addItem(tr("2階建て"), 2);
  storeys->setCurrentIndex(initial.value("storeys", 2) == 2 ? 1 : 0);
  form->addRow(tr("階数"), storeys);
  auto* roof = new QComboBox;
  roof->setObjectName(QStringLiteral("timberRoof"));
  roof->addItem(tr("切妻"), "gable");
  roof->addItem(tr("片流れ"), "shed");
  roof->setCurrentIndex(initial.value("roof", std::string("gable")) == "shed" ? 1 : 0);
  form->addRow(tr("屋根"), roof);
  auto* axis = new QComboBox;
  axis->addItem(tr("X方向"), "x");
  axis->addItem(tr("Y方向"), "y");
  axis->setCurrentIndex(initial.value("ridge_axis", std::string("x")) == "y" ? 1 : 0);
  form->addRow(tr("棟・母屋の向き"), axis);
  auto* slope =
      numeric("timberRoofPitch", initial.value("roof_pitch", value("roof_pitch")), 0.1, 30, tr(" 寸"));
  form->addRow(tr("屋根勾配"), slope);
  auto* over = numeric("timberEaveProjection", initial.value("eave_projection", value("eave_projection")), 0,
                       10000, tr(" mm"));
  form->addRow(tr("軒の出（柱芯から）"), over);
  auto* gable = numeric("timberGableProjection", initial.value("gable_projection", value("gable_projection")),
                        0, 10000, tr(" mm"));
  form->addRow(tr("けらばの出（柱芯から）"), gable);
  auto* height = numeric("timberStoreyHeight", initial.value("storey_height", value("storey_height")), 100,
                         10000, tr(" mm"));
  form->addRow(tr("1FL→2FL"), height);
  auto* eave =
      numeric("timberEaveHeight", initial.value("eave_height", value("eave_height")), 100, 10000, tr(" mm"));
  form->addRow(tr("最上階FL→桁天端"), eave);
  auto floor = [&](const char* id, const char* preset) {
    auto* box = new QComboBox;
    box->setObjectName(QString::fromLatin1(id));
    box->addItem(tr("根太床"), "joist");
    box->addItem(tr("根太レス（合板）"), "joistless");
    box->setCurrentIndex(box->findData(QString::fromStdString(
        initial.value(preset, guide.at("presets").at("entries").at(preset).at("value").get<std::string>()))));
    return box;
  };
  auto* first = floor("timberFirstFloor", "first_floor_system");
  auto* second = floor("timberSecondFloor", "second_floor_system");
  form->addRow(tr("1階の床"), first);
  form->addRow(tr("2階の床"), second);
  auto* range = new QCheckBox(tr("2階の範囲を通り芯の番号で指定（先頭は0）"));
  form->addRow(range);
  auto* rangeRow = new QWidget;
  auto* row = new QHBoxLayout(rangeRow);
  row->setContentsMargins(0, 0, 0, 0);
  QSpinBox* limits[4];
  for (int i = 0; i < 4; ++i) {
    limits[i] = new QSpinBox;
    limits[i]->setRange(0, 40);
    row->addWidget(limits[i]);
  }
  const auto* selectedGrid = grid.empty() ? nullptr : modeler_.gridSpec(grid);
  limits[1]->setValue(selectedGrid ? static_cast<int>(selectedGrid->x.size()) - 1 : x->value());
  limits[3]->setValue(selectedGrid ? static_cast<int>(selectedGrid->y.size()) - 1 : y->value());
  if (initial.contains("second_floor")) {
    const auto& r = initial.at("second_floor");
    limits[0]->setValue(r.at("x").at(0));
    limits[1]->setValue(r.at("x").at(1));
    limits[2]->setValue(r.at("y").at(0));
    limits[3]->setValue(r.at("y").at(1));
    range->setChecked(true);
  }
  rangeRow->setEnabled(range->isChecked());
  form->addRow(tr("Xの先頭・末尾 / Yの先頭・末尾"), rangeRow);
  connect(range, &QCheckBox::toggled, rangeRow, &QWidget::setEnabled);
  auto update = [&] {
    const bool gridMode = useGrid->isChecked(), regen = replacing->isChecked();
    x->setEnabled(!gridMode);
    y->setEnabled(!gridMode);
    module->setEnabled(!gridMode);
    height->setEnabled(!regen);
    eave->setEnabled(!regen);
    second->setEnabled(storeys->currentData().toInt() == 2);
    range->setEnabled(storeys->currentData().toInt() == 2);
  };
  connect(useGrid, &QCheckBox::toggled, &dialog, [&] { update(); });
  connect(replacing, &QCheckBox::toggled, &dialog, [&] { update(); });
  connect(storeys, &QComboBox::currentIndexChanged, &dialog, [&] { update(); });
  update();
  auto* note = new QLabel(
      tr("作り直すときの高さは現在のレベルを使います。高さは「レベル（GL・FL）を編集」で変更できます。"));
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  auto* preview = buttons->addButton(tr("プレビュー"), QDialogButtonBox::AcceptRole);
  preview->setObjectName(QStringLiteral("timberPreview"));
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted) return;
  json inputs = {{"storeys", storeys->currentData().toInt()},
                 {"roof", roof->currentData().toString().toStdString()},
                 {"ridge_axis", axis->currentData().toString().toStdString()},
                 {"roof_pitch", slope->value()},
                 {"eave_projection", over->value()},
                 {"gable_projection", gable->value()},
                 {"first_floor_system", first->currentData().toString().toStdString()},
                 {"second_floor_system", second->currentData().toString().toStdString()}};
  if (useGrid->isChecked())
    inputs["grid"] = grid;
  else {
    inputs["x_bays"] = x->value();
    inputs["y_bays"] = y->value();
    inputs["module"] = module->value();
  }
  if (!replacing->isChecked()) {
    inputs["storey_height"] = height->value();
    inputs["eave_height"] = eave->value();
  }
  if (range->isChecked() && storeys->currentData().toInt() == 2)
    inputs["second_floor"] = {{"x", {limits[0]->value(), limits[1]->value()}},
                              {"y", {limits[2]->value(), limits[3]->value()}}};
  const std::string replace = replacing->isChecked() ? frame : std::string();
  if (previewTimberFrame(inputs.dump(), replace)) return;
  if (lastError_.contains(QStringLiteral("requires_decision"))) {
    const auto choice = QMessageBox::question(this, tr("手で編集した部材があります"),
                                              tr("手で編集・追加・削除した部材を保持して、残りを作り直します"
                                                 "か？\n保持した部材と同じ場所には新しい部材を重ねません。"),
                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice == QMessageBox::Yes && previewTimberFrame(inputs.dump(), replace, true)) return;
  }
  reportError(lastError_);
}
}  // namespace sbr::app
