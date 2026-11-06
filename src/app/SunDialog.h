#pragma once
// 表示 > 太陽の位置 (docs/82_sun_position): date, time, place and north for
// the realistic display's sun, as SketchUp's shadow settings. The time
// slider moves the shadows live; OK keeps the values in the model (one
// undo step), Cancel leaves it as it was.

#include <QDialog>

#include "sbr/geom/Sun.h"

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QSlider;

namespace sbr::app {

class SunDialog : public QDialog {
  Q_OBJECT
 public:
  explicit SunDialog(const geom::SunSettings& sun, QWidget* parent = nullptr);
  geom::SunSettings settings() const;
  // For checks: the time as on the slider (minutes from midnight).
  void setMinutes(int minutes);

 signals:
  void changed(const sbr::geom::SunSettings& sun);

 private:
  void update();
  QDateEdit* date_ = nullptr;
  QSlider* time_ = nullptr;
  QLabel* timeLabel_ = nullptr;
  QComboBox* place_ = nullptr;
  QDoubleSpinBox* latitude_ = nullptr;
  QDoubleSpinBox* longitude_ = nullptr;
  QDoubleSpinBox* timeZone_ = nullptr;
  QDoubleSpinBox* north_ = nullptr;
  QLabel* position_ = nullptr;
};

}  // namespace sbr::app
