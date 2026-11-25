#pragma once

#include "sbr/doc/Schedules.h"

namespace sbr::doc::detail {

struct AreaGeometry {
  struct Floor {
    TopoDS_Shape enclosed, counted;
    double balconyMm2 = 0.0;
    bool assumedFloor = false;
  };
  std::map<std::string, Floor> floors;
  TopoDS_Shape building, storeys;
};

struct AreaEvidencePage {
  draw::View view;
  std::vector<draw::Table> tables;
  std::vector<draw::Table> continuation;
};

std::vector<AreaEvidencePage> areaEvidencePages(const Document& document, const AreaTable& areas,
                                                 const AreaGeometry& geometry, double scale);

}  // namespace sbr::doc::detail
