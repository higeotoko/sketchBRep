#pragma once
// Text for files read by Japanese CAD (DXF for Jw_cad / AutoCAD, JWW):
// UTF-8 to Shift_JIS (Windows code page 932). Characters it has no code
// for become '?'.

#include <optional>
#include <string>

namespace sbr {

std::string toShiftJis(const std::string& utf8);
// Shift_JIS (code page 932, as Jw_cad and Japanese AutoCAD write DXF) to
// UTF-8; nothing when the bytes are not Shift_JIS.
std::optional<std::string> fromShiftJis(const std::string& sjis);

}  // namespace sbr
