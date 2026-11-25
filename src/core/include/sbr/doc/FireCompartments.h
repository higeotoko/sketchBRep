#pragma once

#include "sbr/doc/Schedules.h"

namespace sbr::doc {

inline constexpr const char* kFireCompartmentLaw = "https://laws.e-gov.go.jp/law/325CO0000000338";
inline constexpr const char* kFireCompartmentYokohama =
    "https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/jorei/kijunjourei/kijunjourei.files/20260501_joreiall.pdf";

struct FireCompartmentBoundary {
  std::string mark;         // one designation can be used on several walls and floors
  std::string kind;         // area | shaft | mixed_use | other
  std::string requirement;  // unknown | fire | specific | no_openings
  std::string source;       // adopted clause and project evidence
};

struct FireCompartmentEquipment {
  std::string kind;  // fire | specific, or empty to clear
  std::string reference;  // recognition number or prescribed specification and supporting source
};

struct FireCompartmentRow {
  enum class Part { Boundary, WallOpening, FloorOpening };
  Part part = Part::Boundary;
  ObjectId host;
  std::string hostName, level;
  FireCompartmentBoundary boundary;
  std::string openingId, openingMark;
  std::optional<OpeningRow> wallOpening;
  FireCompartmentEquipment equipment;
  std::string status;
  bool attention = true;
  TopoDS_Shape marker;
};

std::optional<FireCompartmentBoundary> fireCompartmentBoundary(const Object&);
std::vector<FireCompartmentRow> fireCompartmentReview(const Document&, bool markers = false);
std::string fireCompartmentKindName(const std::string&);
std::string fireCompartmentRequirementName(const std::string&);
draw::Table fireCompartmentTable(const std::vector<FireCompartmentRow>&);

}  // namespace sbr::doc
