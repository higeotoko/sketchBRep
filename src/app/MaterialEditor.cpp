#include "MaterialEditor.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace sbr::app {
namespace {

QColor toQColor(const doc::Color& c) { return QColor::fromRgbF(c.r, c.g, c.b); }

QSlider* unitSlider() {
  auto* s = new QSlider(Qt::Horizontal);
  s->setRange(0, 100);
  return s;
}

QImage pictureOf(const doc::Document& document, const doc::Material& material) {
  if (!material.texture) return {};
  const doc::Document::Image* image = document.findImage(material.texture->image);
  if (!image) return {};
  return QImage::fromData(reinterpret_cast<const uchar*>(image->bytes.data()), static_cast<int>(image->bytes.size()));
}

// The picture tinted with the colour (SketchUp's "colorize"): its
// brightness keeps, the hue comes from the colour.
QImage tinted(const QImage& picture, const QColor& color) {
  QImage out = picture.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < out.height(); ++y) {
    auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
    for (int x = 0; x < out.width(); ++x) {
      const double v = qGray(row[x]) / 255.0;
      row[x] = qRgba(static_cast<int>(color.red() * v), static_cast<int>(color.green() * v),
                     static_cast<int>(color.blue() * v), qAlpha(row[x]));
    }
  }
  return out;
}

}  // namespace

QPixmap MaterialEditor::swatch(const doc::Document& document, const doc::Material& material, const QSize& size) {
  QPixmap pixmap(size);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  // See-through materials over a checkerboard.
  if (material.opacity < 1.0) {
    const int cell = std::max(4, size.height() / 4);
    for (int y = 0; y < size.height(); y += cell) {
      for (int x = 0; x < size.width(); x += cell) {
        p.fillRect(x, y, cell, cell, ((x + y) / cell) % 2 ? QColor(200, 200, 200) : Qt::white);
      }
    }
  }
  p.setOpacity(material.opacity);
  const QImage picture = pictureOf(document, material);
  if (!picture.isNull()) {
    const QImage shown = material.texture->colorize ? tinted(picture, toQColor(material.color)) : picture;
    p.fillRect(pixmap.rect(), QBrush(QPixmap::fromImage(shown.scaled(size, Qt::KeepAspectRatioByExpanding,
                                                                         Qt::SmoothTransformation))));
  } else {
    p.fillRect(pixmap.rect(), toQColor(material.color));
  }
  p.setOpacity(1.0);
  p.setPen(QColor(90, 90, 90));
  p.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
  return pixmap;
}

MaterialEditor::MaterialEditor(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);

  // Preview and name; the copy button saves it as a new material to edit.
  auto* top = new QHBoxLayout;
  preview_ = new QLabel;
  preview_->setFixedSize(72, 72);
  top->addWidget(preview_);
  auto* nameColumn = new QVBoxLayout;
  auto* nameRow = new QHBoxLayout;
  nameRow->addWidget(new QLabel(tr("名前")));
  name_ = new QLineEdit;
  name_->setObjectName(QStringLiteral("materialName"));
  nameRow->addWidget(name_);
  duplicate_ = new QToolButton;
  duplicate_->setText(tr("複製"));
  duplicate_->setToolTip(tr("このマテリアルを複製して、別名で編集します（元のマテリアルとそれを使う面はそのまま）"));
  nameRow->addWidget(duplicate_);
  nameColumn->addLayout(nameRow);
  auto* hint = new QLabel(tr("色・テクスチャを変えても名前と ID は変わりません。塗った面はすべて変わります。"));
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color: gray; font-size: 9pt"));
  nameColumn->addWidget(hint);
  top->addLayout(nameColumn);
  layout->addLayout(top);

  // Colour: field + RGB / HSV values + code, live.
  auto* colorBox = new QGroupBox(tr("色"));
  auto* colorLayout = new QVBoxLayout(colorBox);
  color_ = new QColorDialog;
  color_->setObjectName(QStringLiteral("materialColor"));
  color_->setOptions(QColorDialog::NoButtons | QColorDialog::DontUseNativeDialog);
  color_->setWindowFlags(Qt::Widget);
  colorLayout->addWidget(color_);
  layout->addWidget(colorBox);

  // Texture picture at its real size.
  auto* textureBox = new QGroupBox(tr("テクスチャ"));
  auto* textureLayout = new QFormLayout(textureBox);
  useTexture_ = new QCheckBox(tr("テクスチャイメージを使用する"));
  textureLayout->addRow(useTexture_);
  auto* fileRow = new QHBoxLayout;
  textureName_ = new QLineEdit;
  textureName_->setReadOnly(true);
  fileRow->addWidget(textureName_);
  browse_ = new QToolButton;
  browse_->setText(tr("参照..."));
  fileRow->addWidget(browse_);
  textureLayout->addRow(tr("画像"), fileRow);
  auto lengthSpin = [] {
    auto* s = new QDoubleSpinBox;
    s->setRange(1, 1e6);
    s->setDecimals(0);
    s->setSuffix(QStringLiteral(" mm"));
    return s;
  };
  tileWidth_ = lengthSpin();
  tileHeight_ = lengthSpin();
  lockAspect_ = new QCheckBox(tr("縦横比を固定"));
  lockAspect_->setChecked(true);
  textureLayout->addRow(tr("幅"), tileWidth_);
  textureLayout->addRow(tr("高さ"), tileHeight_);
  textureLayout->addRow(lockAspect_);
  auto* colorRow = new QHBoxLayout;
  colorize_ = new QCheckBox(tr("着色"));
  colorize_->setToolTip(tr("画像の明暗を残して、上の色で色付けします"));
  resetColor_ = new QPushButton(tr("色をリセット"));
  resetColor_->setToolTip(tr("着色をやめ、色を画像の平均の色に戻します"));
  colorRow->addWidget(colorize_);
  colorRow->addWidget(resetColor_);
  textureLayout->addRow(colorRow);
  layout->addWidget(textureBox);

  // Opacity.
  auto* opacityBox = new QGroupBox(tr("不透明度"));
  auto* opacityLayout = new QHBoxLayout(opacityBox);
  opacity_ = unitSlider();
  opacityValue_ = new QSpinBox;
  opacityValue_->setRange(0, 100);
  opacityLayout->addWidget(opacity_);
  opacityLayout->addWidget(opacityValue_);
  layout->addWidget(opacityBox);

  // Rendering values, kept for a renderer (not shown while modelling).
  auto* renderBox = new QGroupBox(tr("レンダリング用（保存されます・モデリング表示には未反映）"));
  auto* renderLayout = new QFormLayout(renderBox);
  metallic_ = unitSlider();
  roughness_ = unitSlider();
  reflectance_ = unitSlider();
  renderLayout->addRow(tr("金属感"), metallic_);
  renderLayout->addRow(tr("粗さ"), roughness_);
  renderLayout->addRow(tr("反射率"), reflectance_);
  layout->addWidget(renderBox);
  layout->addStretch();

  // --- changes -------------------------------------------------------------
  connect(name_, &QLineEdit::editingFinished, this, [this] {
    if (!has_ || updating_ || name_->text().trimmed().isEmpty()) return;
    const std::string name = name_->text().trimmed().toStdString();
    if (name == current_.name) return;
    current_.name = name;
    emitEdit(false);
  });
  connect(duplicate_, &QToolButton::clicked, this, [this] {
    if (has_) emit duplicateRequested(current_.id);
  });
  connect(color_, &QColorDialog::currentColorChanged, this, [this](const QColor& c) {
    if (!has_ || updating_ || !c.isValid()) return;
    current_.color = doc::Color{c.redF(), c.greenF(), c.blueF()};
    emitEdit(true);
  });
  connect(useTexture_, &QCheckBox::toggled, this, [this](bool on) {
    if (!has_ || updating_) return;
    if (on && !current_.texture) {
      chooseTexture();
      return;
    }
    if (!on && current_.texture) {
      current_.texture.reset();
      emitEdit(false);
    }
  });
  connect(browse_, &QToolButton::clicked, this, [this] {
    if (has_) chooseTexture();
  });
  connect(tileWidth_, &QDoubleSpinBox::editingFinished, this, [this] {
    if (!has_ || updating_ || !current_.texture || std::abs(tileWidth_->value() - current_.texture->width) < 1e-9) return;
    if (lockAspect_->isChecked()) current_.texture->height *= tileWidth_->value() / current_.texture->width;
    current_.texture->width = tileWidth_->value();
    emitEdit(false);
  });
  connect(tileHeight_, &QDoubleSpinBox::editingFinished, this, [this] {
    if (!has_ || updating_ || !current_.texture || std::abs(tileHeight_->value() - current_.texture->height) < 1e-9) return;
    if (lockAspect_->isChecked()) current_.texture->width *= tileHeight_->value() / current_.texture->height;
    current_.texture->height = tileHeight_->value();
    emitEdit(false);
  });
  connect(colorize_, &QCheckBox::toggled, this, [this](bool on) {
    if (!has_ || updating_ || !current_.texture) return;
    current_.texture->colorize = on;
    emitEdit(false);
  });
  connect(resetColor_, &QPushButton::clicked, this, [this] {
    if (!has_ || !current_.texture || picture_.isNull()) return;
    const QColor average = QColor(picture_.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixel(0, 0));
    current_.color = doc::Color{average.redF(), average.greenF(), average.blueF()};
    current_.texture->colorize = false;
    emitEdit(false);
  });
  connect(opacity_, &QSlider::valueChanged, this, [this](int v) {
    {
      const QSignalBlocker block(opacityValue_);
      opacityValue_->setValue(v);
    }
    if (!has_ || updating_) return;
    current_.opacity = v / 100.0;
    emitEdit(true);
  });
  connect(opacityValue_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) { opacity_->setValue(v); });
  auto unit = [this](QSlider* slider, double doc::Material::*field) {
    connect(slider, &QSlider::valueChanged, this, [this, field](int v) {
      if (!has_ || updating_) return;
      current_.*field = v / 100.0;
      emitEdit(true);
    });
  };
  unit(metallic_, &doc::Material::metallic);
  unit(roughness_, &doc::Material::roughness);
  unit(reflectance_, &doc::Material::reflectance);
  setEnabled(false);
}

void MaterialEditor::chooseTexture() {
  const QString path = QFileDialog::getOpenFileName(this, tr("テクスチャ画像を選ぶ"), QString(),
                                                    tr("画像 (*.png *.jpg *.jpeg *.bmp *.gif)"));
  if (path.isEmpty()) {
    // Nothing chosen: the box shows what the material really has.
    const QSignalBlocker block(useTexture_);
    useTexture_->setChecked(current_.texture.has_value());
    return;
  }
  emit textureFileChosen(current_.id, path);
}

void MaterialEditor::emitEdit(bool continuous) {
  // Changes in quick succession (dragging) merge into one undo step.
  const bool continuing = continuous && sinceEdit_.isValid() && sinceEdit_.elapsed() < 1500;
  sinceEdit_.start();
  if (!continuous) sinceEdit_.invalidate();
  updatePreview();
  emit edited(current_, continuing);
}

void MaterialEditor::updatePreview() {
  if (!document_ || !has_) {
    preview_->clear();
    return;
  }
  preview_->setPixmap(swatch(*document_, current_, preview_->size()));
}

void MaterialEditor::showMaterial(const doc::Document& document, const doc::Material* material) {
  document_ = &document;
  updating_ = true;
  has_ = material != nullptr;
  setEnabled(has_);
  if (!has_) {
    current_ = doc::Material{};
    name_->clear();
    preview_->clear();
    updating_ = false;
    return;
  }
  const bool other = current_.id != material->id;
  current_ = *material;
  if (other) sinceEdit_.invalidate();
  picture_ = pictureOf(document, current_);
  if (!name_->hasFocus()) name_->setText(QString::fromStdString(current_.name));
  if (color_->currentColor() != toQColor(current_.color)) color_->setCurrentColor(toQColor(current_.color));
  useTexture_->setChecked(current_.texture.has_value());
  const doc::Document::Image* image = current_.texture ? document.findImage(current_.texture->image) : nullptr;
  textureName_->setText(image ? QString::fromStdString(image->name) : QString());
  for (QWidget* w : {static_cast<QWidget*>(tileWidth_), static_cast<QWidget*>(tileHeight_),
                     static_cast<QWidget*>(lockAspect_), static_cast<QWidget*>(colorize_),
                     static_cast<QWidget*>(resetColor_)}) {
    w->setEnabled(current_.texture.has_value());
  }
  if (current_.texture) {
    tileWidth_->setValue(current_.texture->width);
    tileHeight_->setValue(current_.texture->height);
    colorize_->setChecked(current_.texture->colorize);
  }
  opacity_->setValue(static_cast<int>(std::lround(current_.opacity * 100)));
  opacityValue_->setValue(opacity_->value());
  metallic_->setValue(static_cast<int>(std::lround(current_.metallic * 100)));
  roughness_->setValue(static_cast<int>(std::lround(current_.roughness * 100)));
  reflectance_->setValue(static_cast<int>(std::lround(current_.reflectance * 100)));
  updatePreview();
  updating_ = false;
}

}  // namespace sbr::app
