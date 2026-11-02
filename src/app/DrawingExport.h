#pragma once
// 2D drawings (docs/50): the dialog that makes them (kind, where to cut,
// scale, paper, frame and title block, with a preview), and the PDF writer.
// The preview and the PDF are painted by the same code.

#include <optional>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <QDialog>
#include <QRectF>
#include <QString>

#include "sbr/Types.h"
#include "sbr/draw/Drawing.h"

class QPainter;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QTimer;

namespace sbr::doc {
class Document;
}

namespace sbr::app {

// Paints the sheet into `target` (keeping the paper's proportions).
void renderSheet(QPainter& painter, const draw::Sheet& sheet, const QRectF& target);
Status writePdf(const draw::Sheet& sheet, const QString& path);

class DrawingDialog : public QDialog {
  Q_OBJECT
 public:
  DrawingDialog(const doc::Document& document, QWidget* parent = nullptr);
  // For checks and the AI: what would be written now.
  const std::optional<draw::Sheet>& sheet() const { return sheet_; }
  QString lastError() const { return error_; }
  // Writes the sheet (format by extension: .pdf, .dxf, .jww).
  Status exportTo(const QString& path);
  // Recalculates at once (normally after a short pause in the changes).
  void refreshNow();

 private:
  void viewChanged();
  void layoutChanged();
  void showPreview();
  void chooseFileAndExport();
  std::optional<draw::ViewSpec> viewSpec(QString* why) const;
  draw::SheetSpec sheetSpec() const;
  void updateVisibleOptions();

  // Where a section can be cut: the active section plane, the grid's
  // axes, else across the model's middle; two sides to look from.
  struct SectionPosition {
    QString label;
    gp_Pnt point;
    gp_Dir viewers[2];
    QString sides[2];
  };
  std::vector<SectionPosition> positions_;
  const doc::Document& document_;
  QComboBox* kind_ = nullptr;
  QComboBox* level_ = nullptr;
  QDoubleSpinBox* cutHeight_ = nullptr;
  QComboBox* position_ = nullptr;
  QComboBox* side_ = nullptr;
  QComboBox* scale_ = nullptr;
  QComboBox* paper_ = nullptr;
  QCheckBox* hidden_ = nullptr;
  QCheckBox* fill_ = nullptr;
  QCheckBox* grid_ = nullptr;
  QCheckBox* levels_ = nullptr;
  QCheckBox* annotations_ = nullptr;
  QCheckBox* frame_ = nullptr;
  QLineEdit* project_ = nullptr;
  QLineEdit* drawingName_ = nullptr;
  QLineEdit* number_ = nullptr;
  QLineEdit* author_ = nullptr;
  QLineEdit* date_ = nullptr;
  QLabel* preview_ = nullptr;
  QLabel* notes_ = nullptr;
  QTimer* timer_ = nullptr;
  QWidget* planRow_ = nullptr;
  QWidget* sectionRow_ = nullptr;
  QWidget* elevationRow_ = nullptr;
  bool viewDirty_ = true;
  bool nameEdited_ = false;
  std::optional<draw::View> view_;
  std::optional<draw::Sheet> sheet_;
  QString error_;
};

}  // namespace sbr::app
