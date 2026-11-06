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
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace sbr::app {

namespace {
struct Place {
  const char* name;
  double latitude, longitude;
};
// The prefectural offices (degrees, about to 0.01: plenty for shadows).
const Place kPlaces[] = {
    {"北海道（札幌）", 43.06, 141.35}, {"青森", 40.82, 140.74},         {"岩手（盛岡）", 39.70, 141.15},
    {"宮城（仙台）", 38.27, 140.87},   {"秋田", 39.72, 140.10},         {"山形", 38.24, 140.36},
    {"福島", 37.75, 140.47},           {"茨城（水戸）", 36.34, 140.45}, {"栃木（宇都宮）", 36.57, 139.88},
    {"群馬（前橋）", 36.39, 139.06},   {"埼玉（さいたま）", 35.86, 139.65}, {"千葉", 35.61, 140.12},
    {"東京", 35.68, 139.69},           {"神奈川（横浜）", 35.45, 139.64}, {"新潟", 37.90, 139.02},
    {"富山", 36.70, 137.21},           {"石川（金沢）", 36.59, 136.63}, {"福井", 36.07, 136.22},
    {"山梨（甲府）", 35.66, 138.57},   {"長野", 36.65, 138.18},         {"岐阜", 35.39, 136.72},
    {"静岡", 34.98, 138.38},           {"愛知（名古屋）", 35.18, 136.91}, {"三重（津）", 34.73, 136.51},
    {"滋賀（大津）", 35.00, 135.87},   {"京都", 35.02, 135.76},         {"大阪", 34.69, 135.52},
    {"兵庫（神戸）", 34.69, 135.18},   {"奈良", 34.69, 135.83},         {"和歌山", 34.23, 135.17},
    {"鳥取", 35.50, 134.24},           {"島根（松江）", 35.47, 133.05}, {"岡山", 34.66, 133.93},
    {"広島", 34.40, 132.46},           {"山口", 34.19, 131.47},         {"徳島", 34.07, 134.56},
    {"香川（高松）", 34.34, 134.04},   {"愛媛（松山）", 33.84, 132.77}, {"高知", 33.56, 133.53},
    {"福岡", 33.61, 130.42},           {"佐賀", 33.25, 130.30},         {"長崎", 32.74, 129.87},
    {"熊本", 32.79, 130.74},           {"大分", 33.24, 131.61},         {"宮崎", 31.91, 131.42},
    {"鹿児島", 31.56, 130.56},         {"沖縄（那覇）", 26.21, 127.68}};
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
  play_ = new QPushButton(tr("再生"));
  play_->setObjectName(QStringLiteral("sunPlay"));
  play_->setToolTip(tr("1 日の影の動きを再生（日の入りで止まる）"));
  timeRow->addWidget(time_, 1);
  timeRow->addWidget(timeLabel_);
  timeRow->addWidget(play_);
  // The day played: 10 minutes a step until the sun sets (or midnight).
  player_ = new QTimer(this);
  player_->setInterval(80);
  connect(player_, &QTimer::timeout, this, [this] {
    const bool wasUp = geom::sunPosition(settings()).up();
    if (time_->value() + 10 > time_->maximum()) {
      setPlaying(false);
      return;
    }
    time_->setValue(time_->value() + 10);
    if (wasUp && !geom::sunPosition(settings()).up()) setPlaying(false);
  });
  connect(play_, &QPushButton::clicked, this, [this] {
    if (!player_->isActive() && !geom::sunPosition(settings()).up()) {
      // From the night: start a little before sunrise.
      int minutes = 0;
      while (minutes < 24 * 60 - 10) {
        geom::SunSettings s = settings();
        s.hour = (minutes + 10) / 60.0;
        if (geom::sunPosition(s).up()) break;
        minutes += 10;
      }
      time_->setValue(minutes);
    }
    setPlaying(!player_->isActive());
  });
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

bool SunDialog::playing() const { return player_->isActive(); }

void SunDialog::setPlaying(bool on) {
  if (on) {
    player_->start();
  } else {
    player_->stop();
  }
  play_->setText(on ? tr("停止") : tr("再生"));
}

int SunDialog::minutes() const { return time_->value(); }

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
