#pragma once
// Text for files read by Japanese CAD (DXF for Jw_cad / AutoCAD, JWW):
// UTF-8 to Shift_JIS (Windows code page 932). Characters it has no code
// for become '?'.

#include <string>

namespace sbr {

std::string toShiftJis(const std::string& utf8);

}  // namespace sbr
