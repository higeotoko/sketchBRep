#include "FrameCheckPanel.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include "MainWindow.h"
#include "sbr/ConstructionCheck.h"
#include "sbr/ConstructionGuide.h"

namespace sbr::app {
namespace {
using json = nlohmann::json;
QString string(const std::string& s) { return QString::fromStdString(s); }
QString outcome(const std::string& s) {
  if (s == "pass") return QStringLiteral("合格");
  if (s == "fail") return QStringLiteral("不適合");
  if (s == "not_evaluated") return QStringLiteral("未評価");
  return QStringLiteral("対象外");
}
QString measures(const json& row) {
  const auto& m = row.at("measured");
  const auto& r = row.at("required");
  auto number = [](const json& v) { return QString::number(v.get<double>(), 'f', 1); };
  if (m.contains("length"))
    return QStringLiteral("長さ %1 mm ／ 定尺 %2 mm")
        .arg(number(m.at("length")), number(r.at("stock_length")));
  if (m.contains("max_offset"))
    return QStringLiteral("通り芯から %1 mm ／ 許容 %2 mm")
        .arg(number(m.at("max_offset")), number(r.at("tolerance")));
  if (m.contains("max_plan_offset"))
    return QStringLiteral("割付から %1 mm ／ 許容 %2 mm")
        .arg(number(m.at("max_plan_offset")), number(r.at("tolerance")));
  if (m.contains("intersection_volume"))
    return QStringLiteral("重複体積 %1 mm³").arg(number(m.at("intersection_volume")));
  if (m.contains("at")) {
    const auto& at = m.at("at");
    return QStringLiteral("端部 (%1, %2, %3) mm ／ 受け材 %4件")
        .arg(number(at[0]), number(at[1]), number(at[2]))
        .arg(m.at("supports").size());
  }
  if (m.contains("start_z"))
    return QStringLiteral("柱 %1〜%2 mm ／ 1FL %3・2FL %4 mm")
        .arg(number(m.at("start_z")), number(m.at("end_z")), number(r.at("first_fl")),
             number(r.at("second_fl")));
  if (m.contains("candidate_pairs"))
    return QStringLiteral("候補 %1組 ／ 衝突 %2組 ／ 接合候補 %3組")
        .arg(m.at("candidate_pairs").get<int>())
        .arg(m.at("collisions").get<int>())
        .arg(m.at("excluded_end_connections").get<int>());
  return {};
}
QString csv(QString s) {
  s.replace('"', QStringLiteral("\"\""));
  return '"' + s + '"';
}
}  // namespace
FrameCheckPanel::FrameCheckPanel(MainWindow* window)
    : PanelDock(tr("架構チェック"), window), window_(window) {
  setObjectName(QStringLiteral("frameCheckPanel"));
  setAllowedAreas(Qt::NoDockWidgetArea);
  auto* content = new QWidget(this);
  auto* layout = new QVBoxLayout(content);
  auto* notice = new QLabel(tr("意匠・概略の検討用。構造の安全・法令適合・施工可否は判定しません。\n接触は形"
                               "の確認で、接合耐力の評価ではありません。"),
                            content);
  notice->setWordWrap(true);
  layout->addWidget(notice);
  status_ = new QLabel(content);
  status_->setObjectName(QStringLiteral("frameCheckStatus"));
  layout->addWidget(status_);
  issues_ = new QCheckBox(tr("不適合・未評価だけ表示"), content);
  issues_->setChecked(true);
  layout->addWidget(issues_);
  table_ = new QTableWidget(content);
  table_->setObjectName(QStringLiteral("frameCheckResults"));
  table_->setColumnCount(5);
  table_->setHorizontalHeaderLabels({tr("判定"), tr("ルール"), tr("対象"), tr("理由"), tr("測定値・必要値")});
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table_->setColumnWidth(0, 75);
  table_->setColumnWidth(1, 85);
  table_->setColumnWidth(2, 210);
  table_->setColumnWidth(3, 330);
  table_->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table_);
  auto* refresh = new QPushButton(tr("再チェック"), content);
  layout->addWidget(refresh);
  auto* exportButton = new QPushButton(tr("結果をCSVに保存..."), content);
  layout->addWidget(exportButton);
  connect(refresh, &QPushButton::clicked, this, &FrameCheckPanel::invalidate);
  connect(issues_, &QCheckBox::toggled, this, [this] { populate(); });
  connect(exportButton, &QPushButton::clicked, this, [this] {
    if (result_.empty()) return;
    const auto path =
        QFileDialog::getSaveFileName(this, tr("チェック結果を保存"), QString(), tr("CSV (*.csv)"));
    if (!path.isEmpty() && !exportCsv(path)) status_->setText(tr("CSVを保存できませんでした"));
  });
  connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
    const auto ids = table_->item(row, 0)->data(Qt::UserRole).toStringList();
    std::vector<doc::ObjectId> targets;
    for (const auto& id : ids)
      if (window_->modeler().document().find(id.toStdString())) targets.push_back(id.toStdString());
    if (targets.empty()) return;
    window_->setSelection(targets);
    window_->viewport().fitObjects(targets);
  });
  setWidget(content);
  setFloating(true);
  resize(900, 500);
  debounce_ = new QTimer(this);
  debounce_->setObjectName(QStringLiteral("frameCheckDebounce"));
  debounce_->setSingleShot(true);
  debounce_->setInterval(250);
  poll_ = new QTimer(this);
  poll_->setInterval(30);
  connect(debounce_, &QTimer::timeout, this, &FrameCheckPanel::launch);
  connect(poll_, &QTimer::timeout, this, &FrameCheckPanel::collect);
  connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    if (visible)
      invalidate();
    else if (cancel_)
      *cancel_ = true;
  });
  invalidate();
}
FrameCheckPanel::~FrameCheckPanel() {
  if (cancel_) *cancel_ = true;
  if (job_.valid()) job_.wait();
}
void FrameCheckPanel::invalidate() {
  ++epoch_;
  dirty_ = true;
  result_.clear();
  table_->setRowCount(0);
  status_->setText(tr("モデル変更後の結果は未評価です。更新を待っています…"));
  if (cancel_) *cancel_ = true;
  if (isVisible()) debounce_->start();
}
void FrameCheckPanel::launch() {
  if (!isVisible() || !dirty_ || job_.valid()) return;
  auto snapshot = std::make_shared<doc::Document>();
  const auto& document = window_->modeler().document();
  snapshot->settings() = document.settings();
  for (const auto& def : document.definitions()) snapshot->putDefinition(def);
  std::vector<std::pair<doc::Object, std::optional<std::size_t>>> objects;
  for (const auto& object : document.objects()) objects.push_back({object, std::nullopt});
  snapshot->insertMany(std::move(objects));
  jobEpoch_ = epoch_;
  jobRevision_ = window_->modeler().history().revision();
  dirty_ = false;
  cancel_ = std::make_shared<std::atomic<bool>>(false);
  job_ = std::async(std::launch::async, [snapshot, cancel = cancel_] {
    return construction::checkFrame(*snapshot, "{}", cancel.get());
  });
  status_->setText(tr("チェック中…その間も編集できます"));
  poll_->start();
}
void FrameCheckPanel::collect() {
  if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const auto result = job_.get();
  poll_->stop();
  if (jobEpoch_ != epoch_) {
    if (isVisible()) debounce_->start();
    return;
  }
  if (!result) {
    status_->setText(tr("未評価：%1").arg(string(result.error())));
    return;
  }
  auto data = json::parse(*result);
  data["revision"] = jobRevision_;
  result_ = data.dump();
  populate();
}
void FrameCheckPanel::populate() {
  table_->setRowCount(0);
  if (result_.empty()) return;
  const auto data = json::parse(result_);
  const auto& counts = data.at("counts");
  status_->setText(tr("合格 %1 ／ 不適合 %2 ／ 未評価 %3 ／ 対象外 %4（%5 ms、版 %6）")
                       .arg(counts.at("pass").get<int>())
                       .arg(counts.at("fail").get<int>())
                       .arg(counts.at("not_evaluated").get<int>())
                       .arg(counts.at("not_applicable").get<int>())
                       .arg(data.at("elapsed_ms").get<double>(), 0, 'f', 0)
                       .arg(string(data.at("checker_version"))));
  const auto& patches = window_->modeler().document().settings().constructionOverrides;
  const auto patch = patches.find("timber");
  const auto guide =
      json::parse(construction::guideData("timber", patch == patches.end() ? "{}" : patch->second));
  table_->setUpdatesEnabled(false);
  for (const auto& r : data.at("results")) {
    const std::string state = r.at("status");
    if (issues_->isChecked() && state != "fail" && state != "not_evaluated") continue;
    QStringList ids, names;
    for (const auto& id : r.at("targets")) {
      ids << string(id);
      const auto* object = window_->modeler().document().find(id.get<std::string>());
      if (object && object->attributes.count("frame_key") && object->attributes.count("kind") &&
          guide.at("roles").at("entries").contains(object->attributes.at("kind"))) {
        const auto role = object->attributes.at("kind");
        names << string(guide.at("roles").at("entries").at(role).at("name")) +
                     QStringLiteral("（%1・%2）").arg(string(object->layer), string(object->id.substr(0, 8)));
      } else
        names << (object ? string(object->name) : string(id));
    }
    const QStringList fields = {outcome(state), string(r.at("rule")), names.join(QStringLiteral(" / ")),
                                string(r.at("message")), measures(r)};
    const int row = table_->rowCount();
    table_->insertRow(row);
    for (int col = 0; col < fields.size(); ++col) {
      auto* item = new QTableWidgetItem(fields[col]);
      item->setToolTip(string(r.dump(2)));
      table_->setItem(row, col, item);
    }
    table_->item(row, 0)->setData(Qt::UserRole, ids);
  }
  table_->setUpdatesEnabled(true);
}
bool FrameCheckPanel::exportCsv(const QString& path) const {
  if (result_.empty()) return false;
  const auto data = json::parse(result_);
  QString text = QStringLiteral(
      "rule,status,targets,reason_code,message,measured,required,source,checker_version,revision,notice\r\n");
  for (const auto& r : data.at("results")) {
    QStringList fields;
    for (const auto* key : {"rule", "status", "targets", "reason_code", "message", "measured", "required",
                            "source", "checker_version"})
      fields << csv(r.at(key).is_string() ? string(r.at(key)) : string(r.at(key).dump()));
    fields << csv(string(data.at("revision").dump())) << csv(string(r.value("notice", data.at("notice"))));
    text += fields.join(',') + QStringLiteral("\r\n");
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const auto bytes = QString(QChar(0xfeff)).toUtf8() + text.toUtf8();
  return file.write(bytes) == bytes.size() && file.commit();
}
}  // namespace sbr::app
