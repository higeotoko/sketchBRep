#include "PermitSubmission.h"
#include "PermitApplicationArea.h"

#include "sbr/doc/Schedules.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTableWidget>
#include <QVBoxLayout>

namespace sbr::app {
namespace {

QString tr(const char* text) { return QApplication::translate("PermitSubmission", text); }

struct Requirement {
  QString name;
  QString files;
  QString drawings;
  QString notes;
  bool conditional = false;
  bool feeWorksheet = false;
};

QList<Requirement> requirements() {
  return {
    {tr("確認申請書"), QStringLiteral("確認申請書"), {}, tr("Word様式の第二号様式")},
    {tr("建築計画概要書"), QStringLiteral("建築計画概要書"), {}, tr("Word様式の第三号様式")},
    {tr("意匠・設備受付チェックシート"), QStringLiteral("(意匠.*設備|設備.*意匠).*チェック"), {},
     tr("市の書式（三号物件以外）")},
    {tr("構造受付チェックシート"), QStringLiteral("構造.*チェック"), {}, tr("市の書式。構造の申請ルートを確認")},
    {tr("確認申請手数料算定表"), QStringLiteral("手数料.*算定"), {},
     tr("確認申請書 第三面11欄イの申請部分と照合。金額・加算は市の手数料表で確認"), false, true},
    {tr("建築協定等手続状況届出書"), QStringLiteral("建築協定.*(届出|手続)"), {}, tr("市の書式。協定等の該当状況を記入")},
    {tr("図面一式"), QStringLiteral("図面一式|確認申請図"), {}, tr("一式のファイル候補。個別図の有無は内容と照合")},
    {tr("付近見取図"), QStringLiteral("付近見取図"), QStringLiteral("付近見取図"), tr("地図・敷地の位置を確認")},
    {tr("配置図"), QStringLiteral("配置図"), QStringLiteral("^配置図"), tr("道路・境界・地盤面を確認")},
    {tr("各階平面図"), QStringLiteral("平面図"), QStringLiteral("平面図$"), tr("申請する各階がそろっているか確認")},
    {tr("立面図"), QStringLiteral("立面図"), QStringLiteral("立面図$"), tr("高さ・地盤を各面で照合")},
    {tr("断面図"), QStringLiteral("断面図"), QStringLiteral("断面図$"), tr("高さ・地盤・道路を各図と照合")},
    {tr("求積図"), QStringLiteral("求積図|面積計算"), QStringLiteral("求積図"), tr("敷地・建築面積・各階の床面積を確認")},
    {tr("構造図・構造計算書"), QStringLiteral("伏図|軸組図|接合部|耐力壁|構造計算|壁量"), QStringLiteral("耐力壁配置図|壁量"), tr("申請ルートに応じて必要図書を確認")},
    {tr("使用建築材料・機械換気の図書"), QStringLiteral("使用建築材料|機械換気|換気設備"), QStringLiteral("換気|仕上げ"),
     tr("市の書式、仕上げ・換気設備の図書を確認")},
    {tr("省エネの根拠図書・適判書類"), QStringLiteral("省エネ|断熱|設計内容説明|適合判定"), QStringLiteral("省エネ|断熱"),
     tr("仕様基準・性能基準、適判の要否を確認"), true},
    {tr("適判・消防・その他の添付書類"), QStringLiteral("適合性判定|消防同意|許可|認定|委任状"), {},
     tr("構造適判、消防同意、委任状、許可・認定などは案件に応じて確認"), true},
  };
}

}  // namespace

void showPermitSubmissionReview(const doc::Document& document, const doc::DrawingSetSpec& spec, QWidget* parent) {
  QDialog dialog(parent);
  dialog.setWindowTitle(tr("横浜市の確認申請・提出図書の照合"));
  dialog.resize(1050, 640);
  auto* layout = new QVBoxLayout(&dialog);
  auto* intro = new QLabel(tr("横浜市に直接申請する木造2階建ての新築戸建てを基準に、モデルの図面と提出フォルダを照合します。"
                             "ファイル名や図面名が合ったものは候補として表示します。提出前に内容と適用条件を確認してください。"));
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* links = new QLabel(tr("<a href=\"https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/dl/dl.html\">横浜市の受付書式</a>"
                              "　<a href=\"https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/tetsuzuki.html\">確認申請の手続き</a>"
                              "　<a href=\"https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kakunin/tesuryoudir/tesuryou.html\">手数料表</a>"));
  links->setTextInteractionFlags(Qt::TextBrowserInteraction);
  links->setOpenExternalLinks(true);
  layout->addWidget(links);
  auto* feeScope = new QCheckBox(tr("手数料確認用に、単一棟の新築の申請部分に全床面積を含める"));
  feeScope->setObjectName(QStringLiteral("permitSubmissionFeeScope"));
  layout->addWidget(feeScope);
  auto* table = new QTableWidget(0, 4);
  table->setObjectName(QStringLiteral("permitSubmissionTable"));
  table->setHorizontalHeaderLabels({tr("図書"), tr("モデル／図面一式"), tr("提出フォルダ"), tr("確認事項")});
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setColumnWidth(0, 240);
  table->setColumnWidth(1, 215);
  table->setColumnWidth(2, 175);
  layout->addWidget(table);
  auto* folderLabel = new QLabel(tr("提出フォルダ未選択"));
  folderLabel->setTextFormat(Qt::PlainText);
  layout->addWidget(folderLabel);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  buttons->button(QDialogButtonBox::Close)->setText(tr("閉じる"));
  auto* scan = buttons->addButton(tr("提出フォルダを選ぶ…"), QDialogButtonBox::ActionRole);
  auto* inspect = buttons->addButton(tr("モデルの図面を確認"), QDialogButtonBox::ActionRole);
  scan->setObjectName(QStringLiteral("permitSubmissionFolder"));
  inspect->setObjectName(QStringLiteral("permitSubmissionModel"));
  layout->addWidget(buttons);

  const auto items = requirements();
  const auto openedAt = document.sequence();
  table->setRowCount(items.size());
  QString folder, feeArea;
  const auto isNewBuilding = [&] {
    const auto outline = doc::projectOutline(document);
    const auto kind = outline.find("work_kind");
    return kind != outline.end() && kind->second == "new";
  };
  feeScope->setEnabled(isNewBuilding());
  QStringList files, drawings;
  bool checkedModel = false;
  QString drawingError;
  const auto matches = [](const QStringList& list, const QString& pattern) {
    if (pattern.isEmpty()) return QStringList{};
    return list.filter(QRegularExpression(pattern));
  };
  const auto refresh = [&] {
    for (int i = 0; i < items.size(); ++i) {
      const auto& item = items[i];
      const auto matchingFiles = matches(files, item.files);
      const auto matchingDrawings = matches(drawings, item.drawings);
      QString model = tr("未照合");
      if (item.drawings.isEmpty()) model = tr("提出ファイルで確認");
      else if (checkedModel)
        model = !drawingError.isEmpty() ? tr("図面一式を確認できません") :
                matchingDrawings.isEmpty() ? tr("該当図面なし") : tr("%1枚の候補").arg(matchingDrawings.size());
      if (item.feeWorksheet) model = !feeScope->isEnabled() ? tr("工事種別・申請部分を確認")
          : !feeScope->isChecked() ? tr("単一棟の申請範囲を確認")
          : feeArea.isEmpty() ? tr("床面積の根拠を確認")
                             : tr("申請部分 %1（候補）").arg(feeArea);
      QString delivery = folder.isEmpty() ? tr("未照合") :
                         matchingFiles.isEmpty() ? (item.conditional ? tr("該当時に確認") : tr("候補ファイルなし"))
                                                 : tr("%1件の候補").arg(matchingFiles.size());
      const QString values[] = {item.name, model, delivery, item.notes};
      const QString hints[] = {item.name, matchingDrawings.isEmpty() ? drawingError : matchingDrawings.join('\n'),
                               matchingFiles.join('\n'), item.notes};
      for (int c = 0; c < 4; ++c) {
        auto* cell = new QTableWidgetItem(values[c]);
        cell->setToolTip(hints[c].isEmpty() ? values[c] : hints[c]);
        table->setItem(i, c, cell);
      }
    }
  };
  const auto refreshFee = [&] {
    feeScope->setEnabled(isNewBuilding());
    feeArea.clear();
    if (feeScope->isEnabled() && feeScope->isChecked()) {
      const auto area = doc::areaTable(document);
      if (hasApplicationFloorArea(area)) feeArea = permitSquareMetres(area.legalTotal);
    }
    refresh();
  };
  QObject::connect(feeScope, &QCheckBox::toggled, &dialog, refreshFee);
  QObject::connect(scan, &QPushButton::clicked, &dialog, [&] {
    QFileDialog chooser(&dialog, tr("提出フォルダを選ぶ"), folder);
    chooser.setObjectName(QStringLiteral("permitSubmissionDirectoryChooser"));
    chooser.setFileMode(QFileDialog::Directory);
    chooser.setOption(QFileDialog::ShowDirsOnly);
    if (chooser.exec() != QDialog::Accepted) return;
    folder = chooser.selectedFiles().value(0);
    if (folder.isEmpty()) return;
    files = QDir(folder).entryList({"*.pdf", "*.docx", "*.xlsx", "*.doc", "*.xls"}, QDir::Files | QDir::Readable, QDir::Name);
    folderLabel->setText(tr("提出フォルダ  %1").arg(QDir::toNativeSeparators(folder)));
    refresh();
  });
  QObject::connect(inspect, &QPushButton::clicked, &dialog, [&] {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto restore = qScopeGuard([] { QApplication::restoreOverrideCursor(); });
    auto currentSpec = spec;
    if (document.sequence() != openedAt) currentSpec.heightEvidence = nullptr;
    const auto set = doc::drawingSet(document, currentSpec);
    checkedModel = true;
    drawings.clear();
    drawingError.clear();
    if (set) {
      for (const auto& name : set->names) drawings.push_back(QString::fromStdString(name));
    } else {
      drawingError = QString::fromStdString(set.error());
    }
    refreshFee();
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refresh();
  dialog.exec();
}

}  // namespace sbr::app
