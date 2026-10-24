#pragma once
// The "編集" tab of the materials panel (SketchUp's material editor): the
// current material's name, colour (picker with colour field and RGB / HSV
// values), texture picture at real size, opacity and the rendering values.
// Every change goes out as the whole edited material; continuous changes
// (a slider being dragged) say so, to become one undo step.

#include <string>

#include <QElapsedTimer>
#include <QImage>
#include <QWidget>

#include "sbr/doc/Document.h"

class QCheckBox;
class QColorDialog;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;

namespace sbr::app {

class MaterialEditor : public QWidget {
  Q_OBJECT
 public:
  explicit MaterialEditor(QWidget* parent = nullptr);

  // Shows `material` (nullptr: nothing to edit) without emitting anything.
  void showMaterial(const doc::Document& document, const doc::Material* material);
  const std::string& materialId() const { return current_.id; }

  // The swatch for a material: its colour, or its picture (tinted when
  // colourised), over a checkerboard where it is see-through.
  static QPixmap swatch(const doc::Document& document, const doc::Material& material, const QSize& size);

 signals:
  void edited(const doc::Material& changed, bool continuing);
  void textureFileChosen(const std::string& materialId, const QString& path);
  void duplicateRequested(const std::string& materialId);

 private:
  void emitEdit(bool continuous);
  void updatePreview();
  void chooseTexture();

  doc::Material current_;
  bool has_ = false;
  bool updating_ = false;
  const doc::Document* document_ = nullptr;
  QImage picture_;  // the texture's picture, if any
  QElapsedTimer sinceEdit_;

  QLabel* preview_ = nullptr;
  QLineEdit* name_ = nullptr;
  QToolButton* duplicate_ = nullptr;
  QColorDialog* color_ = nullptr;
  QCheckBox* useTexture_ = nullptr;
  QLineEdit* textureName_ = nullptr;
  QToolButton* browse_ = nullptr;
  QDoubleSpinBox* tileWidth_ = nullptr;
  QDoubleSpinBox* tileHeight_ = nullptr;
  QCheckBox* lockAspect_ = nullptr;
  QCheckBox* colorize_ = nullptr;
  QPushButton* resetColor_ = nullptr;
  QSlider* opacity_ = nullptr;
  QSpinBox* opacityValue_ = nullptr;
  QSlider* metallic_ = nullptr;
  QSlider* roughness_ = nullptr;
  QSlider* reflectance_ = nullptr;
};

}  // namespace sbr::app
