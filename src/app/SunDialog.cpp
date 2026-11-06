#include "SunDialog.h"

#include <cmath>
#include <iterator>

#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace sbr::app {

namespace {
struct Place {
  const char* name;
  double latitude, longitude;
};
// Prefectural capitals and large cities across Japan (degrees).
const Place kPlaces[] = {{"札幌", 43.06, 141.35}, {"仙台", 38.27, 140.87}, {"東京", 35.68, 139.69},
                         {"名古屋", 35.18, 136.91}, {"大阪", 34.69, 135.50}, {"広島", 34.39, 132.46},
                         {"福岡", 33.59, 130.40}, {"那覇", 26.21, 127.68}};
QDoubleSpinBox* spin(double min, double max, int decimals, const QString& suffix) {
  auto* s = new QDoubleSpinBox;
  s->setRange(min, max);
  s->setDecimals(decimals);
  s->setSuffix(suffix);
  return s;
}
}  // namespace

SunDialog::SunDialog(const geom::SunSettings& sun, QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("sunDialog"));
  setWindowTitle(tr("太陽の位置（日時・場所）"));
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout;
  layout->addLayout(form);

  date_ = new QDateEdit(QDate(2001, sun.month, sun.day));  // the year does not matter
  date_->setObjectName(QStringLiteral("sunDate"));
  date_->setDisplayFormat(QStringLiteral("M月d日"));
  form->addRow(tr("日付"), date_);

  auto* timeRow = new QHBoxLayout;
  time_ = new QSlider(Qt::Horizontal);
  time_->setObjectName(QStringLiteral("sunTime"));
  time_->setRange(0, 24 * 60 - 1);
  time_->setSingleStep(10);
  time_->setPageStep(60);
  time_->setValue(static_cast<int>(std::lround(sun.hour * 60.0)));
  timeLabel_ = new QLabel;
  timeLabel_->setMinimumWidth(48);
  timeRow->addWidget(time_, 1);
  timeRow->addWidget(timeLabel_);
  form->addRow(tr("時刻"), timeRow);

  place_ = new QComboBox;
  place_->setObjectName(QStringLiteral("sunPlace"));
  for (const Place& p : kPlaces) place_->addItem(QString::fromUtf8(p.name));
  place_->addItem(tr("緯度・経度で指定"));
  form->addRow(tr("場所"), place_);
  latitude_ = spin(-90, 90, 2, QStringLiteral("°"));
  latitude_->setValue(sun.latitude);
  longitude_ = spin(-180, 180, 2, QStringLiteral("°"));
  longitude_->setValue(sun.longitude);
  timeZone_ = spin(-12, 14, 1, tr(" 時間"));
  timeZone_->setValue(sun.timeZone);
  north_ = spin(-360, 360, 1, QStringLiteral("°"));
  north_->setObjectName(QStringLiteral("sunNorth"));
  north_->setValue(sun.northAngle);
  form->addRow(tr("緯度（北が＋）"), latitude_);
  form->addRow(tr("経度（東が＋）"), longitude_);
  form->addRow(tr("時差（UTC から）"), timeZone_);
  form->addRow(tr("真北の向き（モデルの＋Y から時計回り）"), north_);
  int match = place_->count() - 1;
  for (int i = 0; i < static_cast<int>(std::size(kPlaces)); ++i) {
    if (std::abs(kPlaces[i].latitude - sun.latitude) < 0.01 && std::abs(kPlaces[i].longitude - sun.longitude) < 0.01) {
      match = i;
    }
  }
  place_->setCurrentIndex(match);

  position_ = new QLabel;
  position_->setObjectName(QStringLiteral("sunPosition"));
  layout->addWidget(position_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(place_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
    if (i >= 0 && i < static_cast<int>(std::size(kPlaces))) {
      const QSignalBlocker a(latitude_), b(longitude_), c(timeZone_);
      latitude_->setValue(kPlaces[i].latitude);
      longitude_->setValue(kPlaces[i].longitude);
      timeZone_->setValue(9.0);
    }
    update();
  });
  auto manual = [this] {
    const QSignalBlocker block(place_);
    place_->setCurrentIndex(place_->count() - 1);
    update();
  };
  connect(latitude_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, manual);
  connect(longitude_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, manual);
  connect(timeZone_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { update(); });
  connect(north_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { update(); });
  connect(date_, &QDateEdit::dateChanged, this, [this] { update(); });
  connect(time_, &QSlider::valueChanged, this, [this] { update(); });
  update();
}

geom::SunSettings SunDialog::settings() const {
  geom::SunSettings s;
  s.month = date_->date().month();
  s.day = date_->date().day();
  s.hour = time_->value() / 60.0;
  s.latitude = latitude_->value();
  s.longitude = longitude_->value();
  s.timeZone = timeZone_->value();
  s.northAngle = north_->value();
  return s;
}

void SunDialog::setMinutes(int minutes) { time_->setValue(minutes); }

void SunDialog::update() {
  const int minutes = time_->value();
  timeLabel_->setText(QStringLiteral("%1:%2").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0')));
  const geom::SunSettings s = settings();
  const geom::SunPosition p = geom::sunPosition(s);
  static const char* directions[] = {"北", "北東", "東", "南東", "南", "南西", "西", "北西"};
  const QString toward = QString::fromUtf8(directions[static_cast<int>(std::lround(p.azimuth / 45.0)) % 8]);
  position_->setText(p.up() ? tr("太陽：%1の空、高さ %2°（方位 %3°）").arg(toward).arg(p.altitude, 0, 'f', 1).arg(p.azimuth, 0, 'f', 0)
                            : tr("太陽は地平線の下です（影は出ません）"));
  emit changed(s);
}

}  // namespace sbr::app
