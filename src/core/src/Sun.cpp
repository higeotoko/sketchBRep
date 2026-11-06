#include "sbr/geom/Sun.h"

#include <cmath>

namespace sbr::geom {
namespace {
constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }
int dayOfYear(int month, int day) {
  static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  return before[(month - 1) % 12] + day;  // a common year: shadows do not need the leap day
}
}  // namespace

const char* sunSettingsError(const SunSettings& s) {
  static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (s.month < 1 || s.month > 12) return "month must be 1..12";
  if (s.day < 1 || s.day > days[s.month - 1]) return "no such day in that month";
  if (!(s.hour >= 0.0 && s.hour < 24.0)) return "hour must be 0..24";
  if (!(s.latitude >= -90.0 && s.latitude <= 90.0)) return "latitude must be -90..90";
  if (!(s.longitude >= -180.0 && s.longitude <= 180.0)) return "longitude must be -180..180";
  if (!(s.timeZone >= -12.0 && s.timeZone <= 14.0)) return "time zone must be -12..14 hours";
  if (!std::isfinite(s.northAngle)) return "north angle must be a number";
  return "";
}

SunPosition sunPosition(const SunSettings& s) {
  // NOAA: fractional year, equation of time and declination.
  const double gamma = 2.0 * kPi / 365.0 * (dayOfYear(s.month, s.day) - 1 + (s.hour - 12.0) / 24.0);
  const double eqTime = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) -
                                  0.014615 * std::cos(2 * gamma) - 0.040849 * std::sin(2 * gamma));
  const double decl = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) -
                      0.006758 * std::cos(2 * gamma) + 0.000907 * std::sin(2 * gamma) -
                      0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
  const double trueSolarMinutes = s.hour * 60.0 + eqTime + 4.0 * s.longitude - 60.0 * s.timeZone;
  const double hourAngle = rad(trueSolarMinutes / 4.0 - 180.0);
  const double lat = rad(s.latitude);
  double cosZenith = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(hourAngle);
  cosZenith = std::fmax(-1.0, std::fmin(1.0, cosZenith));
  const double zenith = std::acos(cosZenith);
  SunPosition p;
  p.altitude = 90.0 - deg(zenith);
  // Azimuth from north, clockwise.
  const double sinZ = std::sin(zenith);
  double az = 0.0;
  if (sinZ > 1e-9) {
    double c = (std::sin(decl) - std::sin(lat) * cosZenith) / (std::cos(lat) * sinZ);
    c = std::fmax(-1.0, std::fmin(1.0, c));
    az = deg(std::acos(c));
    if (hourAngle > 0) az = 360.0 - az;  // afternoon: west of south
  }
  p.azimuth = az;
  // In the model: true north is +Y turned clockwise by the north angle.
  const double n = rad(s.northAngle);
  const double northX = std::sin(n), northY = std::cos(n);
  const double eastX = northY, eastY = -northX;  // north turned 90 degrees clockwise
  const double h = rad(p.altitude), a = rad(az);
  const double horizontal = std::cos(h);
  p.towardSun = {horizontal * (std::sin(a) * eastX + std::cos(a) * northX),
                 horizontal * (std::sin(a) * eastY + std::cos(a) * northY), std::sin(h)};
  return p;
}

}  // namespace sbr::geom
