#pragma once
// Where the sun is (docs/82_sun_position): from the date, the local time,
// the place and where north is in the model. Used by the realistic display
// for its light and shadows; saved with the model (as SketchUp's shadow
// settings). The NOAA solar position formulas: about a tenth of a degree,
// plenty for shadows.

#include <array>

namespace sbr::geom {

struct SunSettings {
  int month = 4;           // 1..12
  int day = 15;            // 1..31
  double hour = 14.0;      // local clock time, 14.5 = 14:30
  double latitude = 35.68;     // degrees, north positive (Tokyo)
  double longitude = 139.69;   // degrees, east positive
  double timeZone = 9.0;       // hours from UTC (Japan)
  double northAngle = 0.0;     // degrees from the model's +Y to true north, clockwise seen from above

  bool operator==(const SunSettings& o) const {
    return month == o.month && day == o.day && hour == o.hour && latitude == o.latitude &&
           longitude == o.longitude && timeZone == o.timeZone && northAngle == o.northAngle;
  }
  bool operator!=(const SunSettings& o) const { return !(*this == o); }
};

struct SunPosition {
  double altitude = 0.0;  // degrees above the horizon (below: negative)
  double azimuth = 0.0;   // degrees from true north, clockwise (east 90)
  std::array<double, 3> towardSun{0, 0, 1};  // unit vector in model coordinates (Z up)
  bool up() const { return altitude > 0.0; }
};

// Checks the values (month, day, hour, latitude, longitude, time zone):
// empty when fine, else what is wrong.
const char* sunSettingsError(const SunSettings& s);
SunPosition sunPosition(const SunSettings& s);

}  // namespace sbr::geom
