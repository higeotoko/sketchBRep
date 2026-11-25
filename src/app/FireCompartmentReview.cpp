#include "MainWindow.h"

#include "PanelDock.h"
#include "Viewport.h"
#include "sbr/doc/FireCompartments.h"

#include <algorithm>
#include <memory>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

namespace sbr::app {
namespace {
using Row = doc::FireCompartmentRow;

void addChoice(QComboBox* combo, const char* code, const std::string& label) {
  combo->addItem(QString::fromStdString(label), QString::fromLatin1(code));
}

QString text(const std::string& value) { return QString::fromStdString(value); }

bool sameRow(const Row& a, const Row& b) {
  return a.host == b.host && a.part == b.part && a.openingId == b.openingId &&
         (a.part != Row::Part::FloorOpening || a.openingMark == b.openingMark);
}
}  // namespace

void MainWindow::showFireCompartmentReview() {
  const auto name = QStringLiteral("fireCompartmentDock");
  if (auto* existing = findChild<QDockWidget*>(name)) {
    existing->show();
    existing->raise();
    return;
  }
  auto* dock = new PanelDock(tr("防火区画・開口部"), this);
  dock->setObjectName(name);
  auto* panel = new QWidget(dock);
  auto* layout = new QVBoxLayout(panel);
  auto* toolbar = new QHBoxLayout;
  auto* edit = new QPushButton(tr("選択部材を登録・編集…"));
  edit->setObjectName(QStringLiteral("editFireCompartment"));
  auto* equipment = new QPushButton(tr("設備仕様…"));
  equipment->setObjectName(QStringLiteral("editFireCompartmentEquipment"));
  equipment->setEnabled(false);
  auto* display = new QCheckBox(tr("3D表示"));
  display->setObjectName(QStringLiteral("showFireCompartmentOverlay"));
  display->setChecked(true);
  toolbar->addWidget(edit);
  toolbar->addWidget(equipment);
  toolbar->addStretch(1);
  toolbar->addWidget(display);
  layout->addLayout(toolbar);
  auto* guidance = new QLabel;
  guidance->setWordWrap(true);
  layout->addWidget(guidance);
  auto* table = new QTableWidget(0, 4);
  table->setObjectName(QStringLiteral("fireCompartmentRows"));
  table->setHorizontalHeaderLabels({tr("区画"), tr("階"), tr("部材・開口"), tr("確認事項")});
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setAlternatingRowColors(true);
  table->setMinimumHeight(table->horizontalHeader()->sizeHint().height() + table->verticalHeader()->defaultSectionSize() * 3 + 8);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  layout->addWidget(table, 1);
  auto* detail = new QLabel;
  detail->setObjectName(QStringLiteral("fireCompartmentDetail"));
  detail->setTextFormat(Qt::PlainText);
  detail->setWordWrap(true);
  detail->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  detail->setMinimumHeight(detail->fontMetrics().lineSpacing() * 3);
  layout->addWidget(detail);
  auto* sources = new QLabel(tr("<a href=\"%1\">施行令 第112条</a>　 <a href=\"%2\">横浜市 建築基準条例</a>")
      .arg(QString::fromLatin1(doc::kFireCompartmentLaw), QString::fromLatin1(doc::kFireCompartmentYokohama)));
  sources->setOpenExternalLinks(true);
  layout->addWidget(sources);

  auto rows = std::make_shared<std::vector<Row>>();
  const QPointer<Viewport> viewport = viewport_;
  const auto current = [table, rows]() -> const Row* {
    const int index = table->currentRow();
    return index >= 0 && static_cast<std::size_t>(index) < rows->size() ? &(*rows)[index] : nullptr;
  };
  const auto update = [this, viewport, dock, display, current, rows, detail, equipment] {
    const Row* row = current();
    equipment->setEnabled(row && row->part == Row::Part::WallOpening);
    if (row) {
      const auto& b = row->boundary;
      detail->setText(tr("%1　%2　%3\n根拠　%4\n%5")
          .arg(text(row->hostName), text(doc::fireCompartmentKindName(b.kind)),
               text(doc::fireCompartmentRequirementName(b.requirement)), text(b.source),
               row->equipment.reference.empty() ? text(row->status)
                                                : tr("%1　%2").arg(text(row->status), text(row->equipment.reference))));
    } else {
      detail->setText(tr("図面と確認表には、ここで登録した区画符号が出ます。"));
    }
    if (!viewport) return;
    if (!dock->isVisible() || !display->isChecked()) {
      viewport->setFireCompartmentShapes({});
      return;
    }
    const Quantity_Color teal(.05, .65, .63, Quantity_TOC_RGB);
    const Quantity_Color amber(.96, .46, .16, Quantity_TOC_RGB);
    const Quantity_Color yellow(.99, .78, .12, Quantity_TOC_RGB);
    BRep_Builder builder;
    TopoDS_Compound boundaries, openings;
    builder.MakeCompound(boundaries);
    builder.MakeCompound(openings);
    for (const auto& entry : *rows) {
      if (entry.marker.IsNull() || !modeler_.document().isVisible(entry.host)) continue;
      builder.Add(entry.part == Row::Part::Boundary ? boundaries : openings, entry.marker);
    }
    std::vector<std::pair<TopoDS_Shape, Quantity_Color>> shapes;
    if (boundaries.NbChildren() > 0) shapes.emplace_back(boundaries, teal);
    if (openings.NbChildren() > 0) shapes.emplace_back(openings, amber);
    if (row && !row->marker.IsNull() && modeler_.document().isVisible(row->host)) {
      TopoDS_Compound outline;
      builder.MakeCompound(outline);
      for (TopExp_Explorer edges(row->marker, TopAbs_EDGE); edges.More(); edges.Next())
        builder.Add(outline, edges.Current());
      if (outline.NbChildren() > 0) shapes.emplace_back(outline, yellow);
    }
    viewport->setFireCompartmentShapes(shapes);
  };
  const auto refresh = [this, viewport, dock, display, table, guidance, rows, current, update] {
    if (!dock->isVisible()) {
      if (viewport) viewport->setFireCompartmentShapes({});
      return;
    }
    const std::optional<Row> previous = current() ? std::optional<Row>(*current()) : std::nullopt;
    *rows = doc::fireCompartmentReview(modeler_.document(), display->isChecked());
    guidance->setText(rows->empty() ? tr("モデルで区画に使う壁・床を選んで登録します。")
                                   : tr("青緑　区画の壁・床　　橙　開口・貫通候補\n行を選ぶと該当箇所が黄色で強調されます。"));
    const QSignalBlocker blocked(table);
    table->setRowCount(0);
    table->setRowCount(static_cast<int>(rows->size()));
    int selectedRow = -1;
    for (std::size_t i = 0; i < rows->size(); ++i) {
      const auto& row = (*rows)[i];
      const int index = static_cast<int>(i);
      const QStringList values{text(row.boundary.mark), text(row.level),
          text(row.part == Row::Part::Boundary ? row.hostName : row.openingMark), text(row.status)};
      for (int col = 0; col < values.size(); ++col) table->setItem(index, col, new QTableWidgetItem(values[col]));
      if (previous && sameRow(*previous, row)) selectedRow = index;
    }
    table->resizeColumnsToContents();
    if (selectedRow >= 0) table->setCurrentCell(selectedRow, 0);
    update();
  };
  connect(table, &QTableWidget::currentCellChanged, panel, [this, current, update] {
    if (const auto* row = current()) setSelection({row->host});
    update();
  });
  connect(display, &QCheckBox::toggled, panel, refresh);
  connect(dock, &QDockWidget::visibilityChanged, panel, refresh);
  connect(this, &MainWindow::fireReviewModelChanged, panel, refresh);

  connect(edit, &QPushButton::clicked, panel, [this] {
    const auto ids = selection_;
    if (ids.empty()) {
      reportError(tr("先にモデルで区画に使う壁・床を選択してください。"));
      return;
    }
    const auto& document = modeler_.document();
    const auto sequence = document.sequence();
    doc::FireCompartmentBoundary existing;
    if (const auto* object = document.find(ids.front())) {
      if (const auto b = doc::fireCompartmentBoundary(*object)) existing = *b;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fireCompartmentBoundaryDialog"));
    dialog.setWindowTitle(tr("防火区画の壁・床を登録"));
    auto* form = new QFormLayout(&dialog);
    auto* mark = new QLineEdit(text(existing.mark));
    mark->setObjectName(QStringLiteral("fireCompartmentMark"));
    mark->setPlaceholderText(tr("例　区画A"));
    auto* kind = new QComboBox;
    kind->setObjectName(QStringLiteral("fireCompartmentKind"));
    for (const auto* code : {"area", "shaft", "mixed_use", "other"})
      addChoice(kind, code, doc::fireCompartmentKindName(code));
    auto* required = new QComboBox;
    required->setObjectName(QStringLiteral("fireCompartmentRequired"));
    for (const auto* code : {"unknown", "fire", "specific", "no_openings"})
      addChoice(required, code, doc::fireCompartmentRequirementName(code));
    if (!existing.kind.empty()) kind->setCurrentIndex(kind->findData(text(existing.kind)));
    if (!existing.requirement.empty()) required->setCurrentIndex(required->findData(text(existing.requirement)));
    auto* source = new QLineEdit(text(existing.source));
    source->setObjectName(QStringLiteral("fireCompartmentSource"));
    source->setPlaceholderText(tr("適用条項と採用した設計資料"));
    form->addRow(tr("区画符号"), mark);
    form->addRow(tr("区画の種類"), kind);
    form->addRow(tr("開口部の条件"), required);
    form->addRow(tr("根拠"), source);
    auto* status = new QLabel(tr("選択した %1 個の部材に同じ区画を登録します。").arg(ids.size()));
    status->setWordWrap(true);
    form->addRow(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto* clear = buttons->addButton(tr("区画登録を解除"), QDialogButtonBox::ResetRole);
    clear->setEnabled(std::any_of(ids.begin(), ids.end(), [&](const auto& id) {
      const auto* o = document.find(id);
      return o && doc::fireCompartmentBoundary(*o).has_value();
    }));
    form->addRow(buttons);
    const auto save = [&](const doc::FireCompartmentBoundary& value) {
      if (sequence != document.sequence()) {
        status->setText(tr("モデルが変わりました。この画面を開き直してください。"));
        return;
      }
      const auto result = modeler_.setFireCompartmentBoundary(ids, value);
      if (result) dialog.accept();
      else status->setText(text(result.message()));
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
      save({mark->text().trimmed().toStdString(), kind->currentData().toString().toStdString(),
            required->currentData().toString().toStdString(), source->text().trimmed().toStdString()});
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(clear, &QPushButton::clicked, &dialog, [&] { save({}); });
    dialog.setMinimumWidth(470);
    dialog.exec();
  });
  connect(equipment, &QPushButton::clicked, panel, [this, current] {
    const auto* row = current();
    if (!row || row->part != Row::Part::WallOpening) return;
    const Row entry = *row;
    const auto sequence = modeler_.document().sequence();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("fireCompartmentEquipmentDialog"));
    dialog.setWindowTitle(tr("防火設備の仕様　%1").arg(text(entry.openingMark)));
    auto* form = new QFormLayout(&dialog);
    auto* kind = new QComboBox;
    kind->setObjectName(QStringLiteral("fireCompartmentEquipmentKind"));
    kind->addItem(tr("未登録"), QString());
    for (const auto* code : {"fire", "specific"}) addChoice(kind, code, doc::fireCompartmentRequirementName(code));
    kind->setCurrentIndex(std::max(0, kind->findData(text(entry.equipment.kind))));
    auto* reference = new QLineEdit(text(entry.equipment.reference));
    reference->setObjectName(QStringLiteral("fireCompartmentEquipmentReference"));
    reference->setPlaceholderText(tr("認定番号・告示仕様と製品資料"));
    form->addRow(tr("登録する設備種別"), kind);
    form->addRow(tr("仕様資料"), reference);
    auto* status = new QLabel(tr("設計上の開口条件　%1").arg(text(doc::fireCompartmentRequirementName(entry.boundary.requirement))));
    status->setWordWrap(true);
    form->addRow(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
      if (sequence != modeler_.document().sequence()) {
        status->setText(tr("モデルが変わりました。この画面を開き直してください。"));
        return;
      }
      const auto code = kind->currentData().toString();
      const auto result = modeler_.setFireCompartmentEquipment(entry.host, entry.openingId,
          {code.toStdString(), code.isEmpty() ? std::string() : reference->text().trimmed().toStdString()});
      if (result) dialog.accept();
      else status->setText(text(result.message()));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setMinimumWidth(450);
    dialog.exec();
  });
  dock->setWidget(panel);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  dock->show();
  dock->raise();
  refresh();
}
}  // namespace sbr::app
