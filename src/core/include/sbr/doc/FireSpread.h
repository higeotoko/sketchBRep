#pragma once

#include "sbr/doc/Schedules.h"

namespace sbr::doc {

inline constexpr const char* kFireSpreadLaw = "https://laws.e-gov.go.jp/document?lawid=325AC0000000201";
inline constexpr const char* kFireSpreadYokohama =
    "https://www.city.yokohama.lg.jp/business/bunyabetsu/kenchiku/tetsuduki/kisoku/toriatsukai.html";

struct FireSpreadConditions {
  std::vector<std::size_t> neighbors;  // perimeter edges in the order of the confirmed outline
  bool parallelRoad = false;
  std::string source;
};

struct FireSpreadStudy {
  struct Basis {
    std::array<gp_Pnt, 2> line;
    bool road = false;
    std::array<gp_Pnt, 2> visibleLine;
  };
  struct Floor {
    std::string id, name;
    double z = 0;
    int metres = 0;
  };
  enum class Status { EquipmentReview, InRange, BeyondStudiedLines, Unchecked };
  struct Opening {
    OpeningRow row;
    Status status = Status::Unchecked;
    std::optional<double> distance;
    std::string basis, note;
    TopoDS_Shape marker;
  };
  bool configured = false;
  bool changed = false;
  std::string issue;
  FireSpreadConditions conditions;
  std::vector<Basis> bases;
  std::vector<Floor> floors;
  std::vector<Opening> openings;
  TopoDS_Shape limits3, limits5;  // contours at Z=0, clipped to the registered site
  TopoDS_Shape band3, band5;
};

FireSpreadStudy fireSpreadStudy(const Document& document, bool contours = true, bool openings = true);
std::string fireSpreadStatus(FireSpreadStudy::Status status);
draw::Table fireSpreadTable(const FireSpreadStudy& study);

}  // namespace sbr::doc
