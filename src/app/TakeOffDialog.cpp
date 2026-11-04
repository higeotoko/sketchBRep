#include "TakeOffDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <set>

#include "sbr/ConstructionQuantity.h"

namespace sbr::app {
namespace {
using json = nlohmann::json;
QString display(const json& v, int decimals = 3) {
  if (v.is_null()) return QStringLiteral("未評価");
  if (v.is_number()) return QString::number(v.get<double>(), 'f', decimals);
  return QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
}  // namespace
TakeOffDialog::TakeOffDialog(const doc::Document& document, QWidget* parent)
    : QDialog(parent), snapshot_(std::make_shared<const doc::Document>(document)) {
  setObjectName(QStringLiteral("takeOffDialog"));
  setWindowTitle(tr("木材の拾い出し"));
  resize(1050, 650);
  auto* layout = new QVBoxLayout(this);
  auto* notice =
      new QLabel(tr("開いた時点のモデルを集計します。材積（断面×長さ）と加工後の実形状体積は別です。\n定尺換"
                    "算・切上げ参考本数は木取り・発注計画ではありません。意匠・概略の検討用です。"),
                 this);
  notice->setWordWrap(true);
  layout->addWidget(notice);
  auto* form = new QFormLayout;
  layout->addLayout(form);
  within_ = new QComboBox(this);
  within_->setObjectName(QStringLiteral("takeOffWithin"));
  within_->addItem(tr("すべての架構"), QString());
  std::set<std::string> tags;
  for (const auto& object : document.objects()) {
    const auto* g = std::get_if<doc::GroupNode>(&object.recipe.base);
    if (g && !g->constructionFrame.empty())
      within_->addItem(QString::fromStdString(object.name), QString::fromStdString(object.id));
    if (!object.isContainer() &&
        ((object.attributes.count("material_class") && object.attributes.at("material_class") == "timber") ||
         std::holds_alternative<doc::ComponentInstance>(object.recipe.base)))
      tags.insert(object.layer);
  }
  form->addRow(tr("架構"), within_);
  tags_ = new QListWidget(this);
  tags_->setObjectName(QStringLiteral("takeOffTags"));
  tags_->setMaximumHeight(90);
  for (const auto& tag : tags) {
    auto* item = new QListWidgetItem(QString::fromStdString(tag), tags_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  form->addRow(tr("タグ（未選択は全体）"), tags_);
  visible_ = new QCheckBox(tr("表示中の物だけ"), this);
  visible_->setObjectName(QStringLiteral("takeOffVisibleOnly"));
  form->addRow(visible_);
  summary_ = new QLabel(this);
  summary_->setObjectName(QStringLiteral("takeOffSummary"));
  summary_->setWordWrap(true);
  summary_->setMinimumHeight(fontMetrics().lineSpacing() * 3);
  layout->addWidget(summary_);
  table_ = new QTableWidget(this);
  table_->setObjectName(QStringLiteral("takeOffRows"));
  table_->setColumnCount(10);
  table_->setHorizontalHeaderLabels({tr("符号"), tr("役割"), tr("断面"), tr("長さ mm"), tr("数量"),
                                     tr("単位"), tr("材積 m³"), tr("実形状 m³"), tr("定尺換算"),
                                     tr("切上げ参考")});
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  layout->addWidget(table_, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto* exportButton = buttons->addButton(tr("CSVに保存..."), QDialogButtonBox::ActionRole);
  exportButton->setObjectName(QStringLiteral("takeOffExport"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(exportButton, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getSaveFileName(this, tr("拾い出しを保存"), QString(), tr("CSV (*.csv)"));
    if (!path.isEmpty()) {
      const auto result = exportCsv(path);
      if (!result) summary_->setText(QString::fromStdString(result.message()));
    }
  });
  timer_ = new QTimer(this);
  timer_->setSingleShot(true);
  timer_->setInterval(200);
  poll_ = new QTimer(this);
  poll_->setInterval(30);
  connect(timer_, &QTimer::timeout, this, &TakeOffDialog::launch);
  connect(poll_, &QTimer::timeout, this, &TakeOffDialog::collect);
  connect(within_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { changed(); });
  connect(tags_, &QListWidget::itemChanged, this, [this](QListWidgetItem*) { changed(); });
  connect(visible_, &QCheckBox::toggled, this, [this] { changed(); });
  changed();
}
TakeOffDialog::~TakeOffDialog() {
  if (job_.valid()) job_.wait();
}
std::string TakeOffDialog::options() const {
  json tags = json::array();
  for (int i = 0; i < tags_->count(); ++i)
    if (tags_->item(i)->checkState() == Qt::Checked) tags.push_back(tags_->item(i)->text().toStdString());
  return json{{"within", within_->currentData().toString().toStdString()},
              {"tags", tags},
              {"visible_only", visible_->isChecked()}}
      .dump();
}
void TakeOffDialog::changed() {
  ++generation_;
  report_.clear();
  table_->setRowCount(0);
  summary_->setText(tr("集計中…"));
  timer_->start();
}
void TakeOffDialog::launch() {
  if (job_.valid()) return;
  jobGeneration_ = generation_;
  const auto args = options();
  job_ = std::async(std::launch::async,
                    [document = snapshot_, args] { return construction::takeOff(*document, args); });
  poll_->start();
}
void TakeOffDialog::collect() {
  if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  auto result = job_.get();
  poll_->stop();
  if (jobGeneration_ != generation_) {
    timer_->start();
    return;
  }
  if (!result) {
    summary_->setText(QString::fromStdString(result.error()));
    return;
  }
  report_ = *result;
  populate();
}
void TakeOffDialog::refreshNow() {
  timer_->stop();
  if (job_.valid()) {
    job_.wait();
    (void)job_.get();
    poll_->stop();
  }
  const auto result = construction::takeOff(*snapshot_, options());
  if (!result) {
    report_.clear();
    summary_->setText(QString::fromStdString(result.error()));
    return;
  }
  report_ = *result;
  populate();
}
void TakeOffDialog::populate() {
  const auto report = json::parse(report_);
  const auto& totals = report.at("totals");
  summary_->setText(tr("数量 %1（本・枚などの合計）／ 寸法未評価 %2 ／ 既知の材積 %3 m³ ／ 既知の実形状体積 "
                       "%4 m³\n表示専用の目印 %5件を除外。未評価がある場合は全量ではありません。")
                        .arg(totals.at("count").get<int>())
                        .arg(totals.at("not_evaluated_count").get<int>())
                        .arg(display(totals.at("known_design_volume_m3"), 6))
                        .arg(display(totals.at("known_net_volume_m3"), 6))
                        .arg(report.at("excluded").size()));
  table_->setRowCount(0);
  for (const auto& row : report.at("rows")) {
    const QStringList cells = {
        display(row.at("mark")),
        display(row.at("role_name")),
        display(row.at("section")),
        display(row.at("length_mm")),
        display(row.at("count"), 0),
        display(row.at("unit")),
        display(row.at("design_volume_m3"), 6),
        display(row.at("net_volume_m3"), 6),
        row.at("role") == "floor_sheathing" ? tr("対象外") : display(row.at("stock_equivalent")),
        row.at("role") == "floor_sheathing" ? tr("対象外") : display(row.at("stock_lower_bound"), 0)};
    const int index = table_->rowCount();
    table_->insertRow(index);
    for (int col = 0; col < cells.size(); ++col) {
      auto* item = new QTableWidgetItem(cells[col]);
      item->setToolTip(QString::fromStdString(row.dump(2)));
      table_->setItem(index, col, item);
    }
  }
}
Status TakeOffDialog::exportCsv(const QString& path) {
  if (report_.empty()) refreshNow();
  if (report_.empty()) return Status::failure("集計できませんでした");
  const auto bytes = construction::takeOffCsv(report_);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) ||
      !file.commit())
    return Status::failure("CSVを保存できませんでした");
  return Status::success();
}
}  // namespace sbr::app
