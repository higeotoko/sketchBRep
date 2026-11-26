#pragma once

// docs/312: which 建築士 may design and supervise the building (建築士法第3条・第3条の2・第3条の3, read on
// e-Gov 2026-11-27): from the model's floor area, storeys and height and the outline's structure, main use
// and kind of work, and the designer's and supervisor's grades given in the outline. New buildings only;
// for other kinds of work the part concerned is taken as new (第3条第2項) and is left to the person.

#include <string>

namespace sbr::doc {
class Document;

struct ArchitectCheck {
  // first: 一級建築士 / second: 一級又は二級 / wood: 一級・二級・木造 / none: 建築士でなくてもよい / unknown.
  std::string required = "unknown";
  std::string article;  // e.g. 建築士法第3条第1項第4号
  std::string basis;    // the figures it was found from, in words
  std::string problem;  // what is missing or unclear (empty when found)
};
ArchitectCheck architectCheck(const Document& document);
// Whether a grade ("first" / "second" / "wood", as the outline keeps it) may do what `required` asks.
bool gradeMeets(const std::string& grade, const std::string& required);
const char* requiredLabel(const std::string& required);

inline constexpr char kArchitectActSource[] = "https://laws.e-gov.go.jp/law/325AC1000000202";
inline constexpr char kArchitectActCheckedOn[] = "2026-11-27";

}  // namespace sbr::doc
