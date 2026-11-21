#include "PermitWorkflowPanel.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

#include <QCoreApplication>
#include <QFont>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "sbr/doc/Document.h"
#include "sbr/doc/Schedules.h"

namespace sbr::app {
namespace {

QString translated(const char* text) { return QCoreApplication::translate("PermitWorkflowPanel", text); }

QString metres(double mm) { return QString::number(mm / 1000.0, 'f', 3) + QStringLiteral(" m"); }

}  // namespace

PermitWorkflowPanel::PermitWorkflowPanel(doc::Document& document, std::function<void(Action, const QString&)> open,
                                         QWidget* parent)
    : QWidget(parent), document_(document), phases_(new QListWidget(this)), next_(new QLabel(this)),
      checks_(new QTreeWidget(this)), details_(new QLabel(this)), edit_(new QPushButton(this)),
      preview_(new QPushButton(translated("検討用の配置図"), this)), confirmation_(new QLabel(this)),
      confirm_(new QPushButton(this)), refreshTimer_(new QTimer(this)) {
  setMinimumHeight(350);
  const QStringList names{translated("敷地・配置図"), translated("通り芯"), translated("平面"),
      translated("居室・採光・換気"), translated("階段"), translated("屋根・庇・バルコニー"),
      translated("耐力壁"), translated("基礎"), translated("省エネ・設備"), translated("立面・断面"),
      translated("一式の突合せ"), translated("申請用出力")};
  for (int i = 0; i < names.size(); ++i)
    phases_->addItem(QStringLiteral("%1  %2").arg(i + 1).arg(names[i]));
  phases_->setObjectName(QStringLiteral("permitWorkflowPhases"));
  phases_->setMinimumWidth(165);
  phases_->setMaximumWidth(195);
  phases_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  phases_->setWordWrap(true);
  phases_->setAccessibleName(translated("申請図の工程"));

  QFont emphasis = font();
  emphasis.setBold(true);
  next_->setFont(emphasis);
  for (auto* label : {next_, details_}) {
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
  }
  next_->setObjectName(QStringLiteral("permitWorkflowNext"));
  details_->setObjectName(QStringLiteral("permitWorkflowDetails"));
  checks_->setObjectName(QStringLiteral("permitWorkflowChecks"));
  checks_->setHeaderLabels({translated("項目"), translated("状態")});
  checks_->setRootIsDecorated(false);
  checks_->setUniformRowHeights(true);
  checks_->setMaximumHeight(180);
  checks_->header()->setStretchLastSection(false);
  checks_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  checks_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  edit_->setObjectName(QStringLiteral("permitWorkflowEdit"));
  preview_->setObjectName(QStringLiteral("permitWorkflowPreview"));
  confirmation_->setObjectName(QStringLiteral("permitWorkflowConfirmation"));
  confirmation_->setWordWrap(true);
  confirmation_->setTextFormat(Qt::PlainText);
  confirm_->setObjectName(QStringLiteral("permitWorkflowConfirm"));

  auto* right = new QVBoxLayout;
  right->addWidget(next_);
  right->addWidget(checks_);
  right->addWidget(details_);
  right->addWidget(edit_, 0, Qt::AlignLeft);
  right->addWidget(confirmation_);
  right->addWidget(confirm_, 0, Qt::AlignLeft);
  right->addStretch(1);
  right->addWidget(preview_, 0, Qt::AlignLeft);
  auto* gate = new QLabel(translated("申請用出力は保留。申請先・適用法令・必要図書の確認が残っています。"), this);
  gate->setObjectName(QStringLiteral("permitWorkflowGate"));
  gate->setWordWrap(true);
  right->addWidget(gate);
  auto* layout = new QGridLayout(this);
  layout->addWidget(phases_, 0, 0);
  layout->addLayout(right, 0, 1);
  layout->setColumnStretch(1, 1);

  connect(phases_, &QListWidget::currentRowChanged, this, [this] { refresh(); });
  connect(checks_, &QTreeWidget::currentItemChanged, this, [this] { showSelectedCheck(); });
  connect(edit_, &QPushButton::clicked, this, [this, open] {
    const int index = checks_->indexOfTopLevelItem(checks_->currentItem());
    if (index >= 0 && rows_[index].action != Action::None) open(rows_[index].action, rows_[index].target);
  });
  connect(confirm_, &QPushButton::clicked, this, [this, open] {
    const int phase = phases_->currentRow();
    if (phase >= 0) open(confirmed_ ? Action::ClearConfirmation : Action::ConfirmStage, QString::number(phase + 1));
  });
  connect(preview_, &QPushButton::clicked, this, [open = std::move(open)] { open(Action::Preview, {}); });
  refreshTimer_->setSingleShot(true);
  connect(refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
  document_.addListener([panel = QPointer<PermitWorkflowPanel>(this)] {
    if (panel && panel->isVisible() && !panel->refreshTimer_->isActive()) panel->refreshTimer_->start(0);
  });
  phases_->setCurrentRow(0);
}

void PermitWorkflowPanel::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  refresh();
}

void PermitWorkflowPanel::readSiteChecks() {
  const auto& settings = document_.settings();
  const auto& site = settings.siteBoundary;
  const auto missing = translated("未入力");
  const auto review = translated("要再照合");
  const auto entered = translated("入力済み");
  const auto boundaryHelp = translated("敷地の閉じた折れ線か水平面を1つ選んで登録します。");
  const auto boundaryEdit = translated("選択した輪郭を登録");
  const auto geometry = site ? document_.siteBoundaryGeometry(site->outline) :
      Result<doc::SiteBoundaryGeometry>::failure({});
  canPreview_ = geometry.ok();
  if (site) {
    if (geometry) {
      rows_.push_back({translated("敷地境界"), entered,
          translated("指定輪郭 %1 m²\n確認資料 %2")
              .arg(QString::number(geometry->areaMm2 / 1e6, 'f', 3), QString::fromStdString(site->source)),
          boundaryEdit, Action::Boundary});
    } else {
      rows_.push_back({translated("敷地境界"), review, QString::fromStdString(geometry.error()),
                       boundaryEdit, Action::Boundary, true});
    }
  } else {
    rows_.push_back({translated("敷地境界"), missing, boundaryHelp, boundaryEdit, Action::Boundary, true});
  }

  const auto northEdit = translated("真北の角度を入力");
  if (const auto& north = settings.siteNorth) {
    rows_.push_back({translated("真北"), entered,
        translated("モデルの +Y から時計回り %1°\n確認資料 %2\n確認日 %3")
            .arg(QString::number(north->angle, 'g', 8), QString::fromStdString(north->source),
                 QString::fromStdString(north->checkedOn)),
        northEdit, Action::North});
  } else {
    rows_.push_back({translated("真北"), missing, translated("真北の角度と確認資料・日付を入力します。"),
                     northEdit, Action::North, true});
  }

  const auto roadLabel = translated("道路・接道");
  const auto groundLabel = translated("接地外周・高さ");
  if (!canPreview_) {
    const auto prerequisite = translated("敷地境界を登録すると入力できます。");
    rows_.push_back({roadLabel, translated("敷地境界待ち"), prerequisite, {}});
    rows_.push_back({groundLabel, translated("敷地境界待ち"), prerequisite, {}});
  } else {
    const auto& road = site->road;
    const auto roadEdit = translated("道路・接道情報を入力");
    const auto roadHelp = translated("道路側にある敷地外周の辺を1本選び、確認資料と寸法を入力します。");
    if (!road) {
      rows_.push_back({roadLabel, missing, roadHelp, roadEdit, Action::Road, true});
    } else if (!document_.siteRoadEdge(*geometry)) {
      rows_.push_back({roadLabel, review, translated("道路側の辺が変わりました。") + roadHelp,
                       roadEdit, Action::Road, true});
    } else {
      const auto kind = road->kind.empty() ? missing : QString::fromStdString(road->kind);
      const auto width = road->widthMm > 0 ? metres(road->widthMm) : missing;
      const auto frontage = road->frontageMm > 0 ? metres(road->frontageMm) : missing;
      const bool partial = road->kind.empty() || road->widthMm <= 0 || road->frontageMm <= 0;
      rows_.push_back({roadLabel, partial ? translated("入力不足") : entered,
          translated("種別 %1\n幅員 %2　接道長 %3\n確認資料 %4")
              .arg(kind, width, frontage, QString::fromStdString(road->source)),
          roadEdit, Action::SavedRoad, partial});
    }

    const auto& ground = site->ground;
    const auto groundEdit = translated("接地外周・高さを開く");
    if (!ground) {
      rows_.push_back({groundLabel, missing, translated("建物の接地外周を選び、基準点と測点の高さを入力します。"),
                       groundEdit, Action::Ground, true});
    } else {
      const auto placement = document_.checkSiteGroundPlacement(*ground);
      const auto report = document_.siteGroundReport(*ground);
      if (!placement || !report) {
        rows_.push_back({groundLabel, review, translated("旧参考平均は保留。\n") +
                            QString::fromStdString(!placement ? placement.message() : report.error()),
                        report ? groundEdit : translated("選択した接地外周を開く"),
                        report ? Action::SavedGround : Action::Ground, true});
      } else {
        const bool linked = ground->placement.has_value();
        QString details = translated("参考平均 %1（基準点 %2）\n確認資料 %3")
            .arg(metres(report->averageMm()), QString::fromStdString(ground->datum), QString::fromStdString(ground->source));
        if (!linked) details += translated("\n移動時の再照合に使う建物を関連付けてください。");
        rows_.push_back({groundLabel, linked ? translated("参考値あり") : translated("建物未関連"),
                         details, groundEdit, Action::SavedGround, !linked});
      }
    }
  }
  // docs/206: the group of use zones (for the daylight factor, docs/205).
  {
    const auto zoneLabel = translated("用途地域の区分");
    const auto zoneEdit = translated("用途地域の区分を入力");
    const doc::Object* outline = site ? document_.find(site->outline) : nullptr;
    const auto value = [&](const char* key) {
      if (!outline) return std::string();
      const auto it = outline->attributes.find(key);
      return it == outline->attributes.end() ? std::string() : it->second;
    };
    const std::string zone = value("use_zone");
    if (!canPreview_) {
      rows_.push_back({zoneLabel, translated("敷地境界待ち"), translated("敷地境界を登録すると入力できます。"), {}});
    } else if (zone.empty()) {
      rows_.push_back({zoneLabel, missing,
                       translated("住居系・工業系・商業系（指定なしを含む）のどれかと確認資料を入れます。採光補正係数の式（施行令 第 20 条 2 項）を選ぶのに使います。"),
                       zoneEdit, Action::Zone, true});
    } else {
      const auto name = zone == "residential" ? translated("住居系") : zone == "industrial" ? translated("工業系") : translated("商業系・指定なし");
      rows_.push_back({zoneLabel, entered,
                       translated("%1\n確認資料 %2\n用途地域そのもの（どの地域か）の確認は申請先で行います。")
                           .arg(name, QString::fromStdString(value("use_zone_source"))),
                       zoneEdit, Action::Zone});
    }
  }

  // docs/214: the project's items that are not in the model.
  {
    const auto checks = doc::permitSiteChecks(document_);
    const auto& c = checks.back();
    rows_.push_back({QString::fromStdString(c.item), QString::fromStdString(c.state), QString::fromStdString(c.details),
                     translated("計画概要の項目を入力"), Action::Outline, c.attention});
  }

  rows_.push_back({translated("申請条件"), translated("未評価"),
      translated("申請先・適用ルートと最新の法令を確認します。道路・接道、法令上の地盤面、高低差による区分の確認が残っています。"), {}});
}

void PermitWorkflowPanel::readRoomOpeningChecks() {
  const auto data = doc::roomOpeningInputs(document_);
  const auto missing = translated("未入力");
  const auto unevaluated = translated("未評価");
  int daylightOff = 0, daylightOpen = 0, ventilationOff = 0, ventilationOpen = 0;  // docs/203
  int mechanicalOff = 0, mechanicalOpen = 0;                                        // docs/211
  if (data.rooms.empty()) {
    rows_.push_back({translated("室"), missing, translated("平面で閉じた壁の内側に室を置き、用途・天井高を設定します。"),
                     translated("室の一覧を開く"), Action::Rooms, true});
  }
  for (const auto& entry : data.rooms) {
    const auto& room = entry.room;
    const auto area = room.counted && room.innerKnown
        ? QString::number(room.innerArea / 1e6, 'f', 3) + QStringLiteral(" m²") : unevaluated;
    QString details = translated("用途 %1　内法 %2\n天井高 %3")
        .arg(room.use.empty() ? missing : QString::fromStdString(room.use), area,
             room.ceilingHeight > 0 ? metres(room.ceilingHeight) : missing);
    if (room.use.empty() || room.ceilingHeight <= 0)
      details += translated("\n室を選び、プロパティで用途・天井高を入力します。");
    if (!room.closed) details += translated("\n室の輪郭が閉じていません。");
    else if (!room.counted) details += translated("\n同じ範囲に別の室があります。");
    details += translated("\n開口候補 %1件").arg(entry.openingIndices.size());
    for (std::size_t i = 0; i < std::min<std::size_t>(entry.openingIndices.size(), 3); ++i) {
      const auto& opening = data.openings[entry.openingIndices[i]];
      details += QStringLiteral("\n%1　%2 × %3 mm")
          .arg(QString::fromStdString(opening.mark), QString::number(opening.width, 'g', 8),
               QString::number(opening.height, 'g', 8));
    }
    if (entry.openingIndices.size() > 3) details += translated("\n残り %1件").arg(entry.openingIndices.size() - 3);
    // docs/203: daylight and ventilation from the factors and areas given for the candidates.
    const doc::RoomDaylightCheck check = doc::roomDaylightCheck(entry);
    details += translated("\n採光：%1　%2").arg(QString::fromStdString(check.daylightState), QString::fromStdString(check.daylightText));
    details += translated("\n換気：%1　%2").arg(QString::fromStdString(check.ventilationState), QString::fromStdString(check.ventilationText));
    details += translated("\n機械換気：%1　%2").arg(QString::fromStdString(check.mechanicalState), QString::fromStdString(check.mechanicalText));
    if (check.mechanicalState == "参考値を外れる") ++mechanicalOff;
    else if (check.mechanicalState != "参考値内" && check.mechanicalState != "対象外") ++mechanicalOpen;
    if (check.daylightState == "参考値を外れる") ++daylightOff;
    else if (check.daylightState != "参考値内" && check.daylightState != "対象外") ++daylightOpen;
    if (check.ventilationState == "参考値を外れる") ++ventilationOff;
    else if (check.ventilationState != "参考値内" && check.ventilationState != "対象外") ++ventilationOpen;
    QString name = QString::fromStdString(room.name);
    if (!room.level.empty()) name = QString::fromStdString(room.level) + QStringLiteral("　") + name;
    const bool incomplete = !room.counted || !room.innerKnown || room.use.empty() || room.ceilingHeight <= 0;
    rows_.push_back({name, incomplete ? translated("入力不足") : translated("室データあり"), details,
                     translated("モデルで室を選択"), Action::SelectRoom, incomplete, QString::fromStdString(room.id)});
  }
  rows_.push_back({translated("開口候補"), data.openings.empty() ? missing : translated("要照合"),
      translated("壁の開口 %1件。室の壁区間に対応しなかった開口 %2件。候補の外部への面し方と建具の開き方を照合します。")
          .arg(data.openings.size()).arg(data.unmatched.size()),
      translated("建具表を開く"), Action::Openings, true});
  // docs/203: the rooms' comparisons summed up; the factors and areas are given per opening.
  const auto summary = [&](int off, int open) {
    return off > 0 ? translated("要確認") : open > 0 ? unevaluated : data.rooms.empty() ? missing : translated("参考値内");
  };
  const auto basis = translated("\n開口ごとの採光補正係数・換気に有効な面積は AI の set_opening_daylight、室の区分は set_room_daylight で入れます。"
                                "\n根拠：建築基準法 第 28 条 1・2・4 項、施行令 第 19 条 3 項（表（三）住宅の居住のための居室 1/7）・第 20 条"
                                "（e-Gov、2026-11-20 確認）。合否の判定ではありません。");
  rows_.push_back({translated("採光"), summary(daylightOff, daylightOpen),
      translated("参考値を外れる室 %1、未入力・未評価の室 %2。採光補正係数そのもの（隣地境界線などまでの距離÷高さ）は、まだモデルから求めていません。")
              .arg(daylightOff).arg(daylightOpen) + basis, {}, Action::None, daylightOff + daylightOpen > 0});
  rows_.push_back({translated("自然換気"), summary(ventilationOff, ventilationOpen),
      translated("参考値を外れる室 %1、未入力・未評価の室 %2。ふすま・障子などで仕切った二室を一室とみなす扱い（法 28 条 4 項）は、まだ見ていません。")
              .arg(ventilationOff).arg(ventilationOpen) + basis, {}, Action::None, ventilationOff + ventilationOpen > 0});
  {
    // docs/205: the daylight factors worked out from the site and the building (令 20 条 2 項).
    const auto estimates = doc::daylightFactorEstimates(document_);
    int worked = 0;
    QString lines;
    std::set<std::string> problems;
    for (const auto& e : estimates) {
      if (e.factor >= 0) {
        ++worked;
        if (worked <= 5)
          lines += translated("\n%1（%2）　係数 %3　D/H = %4/%5%6")
                       .arg(QString::fromStdString(e.mark), QString::fromStdString(e.over))
                       .arg(e.factor, 0, 'f', 2)
                       .arg(e.horizontal, 0, 'f', 0)
                       .arg(e.vertical, 0, 'f', 0)
                       .arg(e.facesRoad ? translated("（道の反対側まで）") : QString());
      } else {
        problems.insert(e.problem);
      }
    }
    QString details = estimates.empty() ? translated("壁の開口がありません。")
                                        : translated("求められた開口 %1／%2。").arg(worked).arg(estimates.size()) + lines;
    for (const auto& p : problems) details += translated("\n求められない理由：%1").arg(QString::fromStdString(p));
    details += translated("\n敷地境界・道路と建物の形から、開口の上にある部分ごとに 水平距離÷垂直距離 の最小を採光関係比率にして、"
                          "用途地域の区分の式で求めます。天窓・縁側・公園や川に面する場合、採光上支障のない庇は見ていません。"
                          "ボタンで開口の「採光係数」に入れます（根拠つき、Ctrl+Z 1 回で戻せます）。");
    rows_.push_back({translated("採光補正係数（モデルから）"), worked > 0 ? translated("計算値あり") : unevaluated, details,
                     worked > 0 ? translated("計算した係数を開口に入れる") : QString(),
                     worked > 0 ? Action::ApplyDaylightFactors : Action::None, worked == 0 && !estimates.empty()});
  }
  rows_.push_back({translated("機械換気"), summary(mechanicalOff, mechanicalOpen),
      translated("参考値を外れる室 %1、未入力・未評価の室 %2。室ごとに 回数（住宅等の居室 0.5、その他 0.3）×床面積×天井高 を、"
                 "入れた有効換気量（AI の set_room_ventilation）と比べます（施行令 第 20 条の 8、e-Gov 2026-11-21 確認）。"
                 "給排気の経路、1台で複数の室をまかなう時の合計、機器の資料は見ていません。")
          .arg(mechanicalOff).arg(mechanicalOpen), {}, Action::None, mechanicalOff + mechanicalOpen > 0});
}

// docs/194, 197-201: stages 2 (通り芯), 3 (平面), 5 (階段), 6 (屋根・庇・バルコニー), 7 (耐力壁), 8 (基礎), 9 (省エネ・設備) and 10 (立面・断面) from the model (doc::permit*Checks).
void PermitWorkflowPanel::readModelChecks(int stage) {
  const auto checks = stage == 2   ? doc::permitGridChecks(document_)
                      : stage == 3 ? doc::permitPlanChecks(document_)
                      : stage == 5 ? doc::permitStairChecks(document_)
                      : stage == 6 ? doc::permitRoofChecks(document_)
                      : stage == 7 ? doc::permitBraceChecks(document_)
                      : stage == 8 ? doc::permitFoundationChecks(document_)
                      : stage == 9 ? doc::permitEnergyChecks(document_)
                      : stage == 10 ? doc::permitElevationChecks(document_)
                      : stage == 11 ? doc::permitSetChecks(document_)
                                    : doc::permitOutputChecks(document_);
  for (const auto& c : checks) {
    QStringList ids;
    for (const auto& id : c.targets) ids << QString::fromStdString(id);
    const bool select = !ids.isEmpty();
    if (stage == 9 && c.item == "適用ルート・地域") {  // docs/215: opens its dialog
      rows_.push_back({QString::fromStdString(c.item), QString::fromStdString(c.state), QString::fromStdString(c.details),
                       translated("適用ルート・地域を入力"), Action::EnergyRoute, c.attention});
      continue;
    }
    rows_.push_back({QString::fromStdString(c.item), QString::fromStdString(c.state), QString::fromStdString(c.details),
                     select ? translated("モデルで %1 個を選択").arg(ids.size()) : QString(),
                     select ? Action::SelectObjects : Action::None, c.attention, ids.join(QLatin1Char('\n'))});
  }
  if (stage == 3)
    rows_.push_back({translated("面積の根拠"), translated("確認材料あり"),
                     translated("壁芯・内法の面積と法の床面積は、表の窓の「面積の表」で式と注を確かめます。"),
                     translated("室の一覧を開く"), Action::Rooms});
}

void PermitWorkflowPanel::refresh() {
  const int phase = phases_->currentRow();
  if (phase < 0) return;
  int selected = phase == previousPhase_ ? checks_->indexOfTopLevelItem(checks_->currentItem()) : -1;
  const QString selectedRoom = selected >= 0 ? rows_[static_cast<std::size_t>(selected)].target : QString();
  previousPhase_ = phase;
  rows_.clear();
  canPreview_ = false;
  if (phase == 0) {
    readSiteChecks();
  } else if (phase == 3) {
    readRoomOpeningChecks();
  } else {
    readModelChecks(phase + 1);  // docs/194, 197-202: stages 2, 3 and 5-12 from the model
  }
  if (!selectedRoom.isEmpty()) {
    const auto found = std::find_if(rows_.begin(), rows_.end(), [&](const Check& row) { return row.target == selectedRoom; });
    selected = found != rows_.end() ? static_cast<int>(std::distance(rows_.begin(), found)) : -1;
  }

  const QSignalBlocker block(checks_);
  checks_->clear();
  for (const auto& row : rows_) {
    auto* item = new QTreeWidgetItem(checks_, {row.item, row.state});
    if (row.attention) {
      QFont weight = checks_->font();
      weight.setBold(true);
      item->setFont(1, weight);
    }
  }
  const auto next = std::find_if(rows_.begin(), rows_.end(), [](const Check& row) { return row.attention; });
  int defaultRow = 0;
  if (next != rows_.end()) {
    defaultRow = static_cast<int>(std::distance(rows_.begin(), next));
    next_->setText(translated("次に確認する項目  %1").arg(next->item));
  } else {
    next_->setText(phase == 0 ? translated("申請条件を確認") : phases_->currentItem()->text());
  }
  checks_->setCurrentItem(checks_->topLevelItem(selected >= 0 ? selected : defaultRow));
  preview_->setVisible(phase == 0);
  preview_->setEnabled(canPreview_);
  showConfirmations(phase);
  showSelectedCheck();
}

// docs/212: the person's marks. The list shows ✓ / 要再確認 after a stage; the current stage's mark
// with what changed, and the button to give, renew or take it away.
void PermitWorkflowPanel::showConfirmations(int phase) {
  const QSignalBlocker block(phases_);
  for (int i = 0; i < phases_->count(); ++i) {
    auto* item = phases_->item(i);
    QString text = item->text();
    const int cut = text.indexOf(QStringLiteral("　"));
    if (cut > 0) text.truncate(cut);
    const auto mark = doc::permitConfirmation(document_, i + 1);
    if (mark.state == "確認済み") text += QStringLiteral("　✓");
    else if (mark.state == "要再確認") text += QStringLiteral("　") + translated("要再確認");
    item->setText(text);
  }
  const auto mark = doc::permitConfirmation(document_, phase + 1);
  confirmed_ = mark.state == "確認済み";
  QString text;
  if (mark.state == "確認済み") {
    text = translated("人の確認：確認済み（%1%2）").arg(QString::fromStdString(mark.on),
        mark.by.empty() ? QString() : QStringLiteral("　") + QString::fromStdString(mark.by));
    confirm_->setText(translated("確認の印を外す"));
  } else if (mark.state == "要再確認") {
    QStringList changed;
    for (const auto& c : mark.changed) changed << QString::fromStdString(c);
    text = translated("人の確認：要再確認（%1 に確認した後、モデルが変わりました）\n変わった行：%2")
               .arg(QString::fromStdString(mark.on), changed.isEmpty() ? translated("（記録なし）") : changed.join(QStringLiteral("、")));
    confirm_->setText(translated("見直したので確認し直す"));
  } else {
    text = translated("人の確認：未確認");
    confirm_->setText(translated("この工程を確認した"));
  }
  if (!mark.note.empty()) text += translated("\nメモ：") + QString::fromStdString(mark.note);
  confirmation_->setText(text);
}

void PermitWorkflowPanel::showSelectedCheck() {
  const int index = checks_->indexOfTopLevelItem(checks_->currentItem());
  if (index < 0) {
    details_->clear();
    edit_->hide();
    return;
  }
  const auto& check = rows_[index];
  details_->setText(check.details);
  edit_->setText(check.button);
  edit_->setVisible(check.action != Action::None);
}

}  // namespace sbr::app
