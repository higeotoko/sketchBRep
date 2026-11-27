#include "sbr/doc/FireSpread.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <nlohmann/json.hpp>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>

#include "sbr/doc/Modeler.h"
#include "sbr/WallTypes.h"
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

namespace sbr::doc {
namespace {
using Json = nlohmann::json;
using Line = std::array<gp_Pnt, 2>;
constexpr const char* kAttr = "fire_spread_conditions";

std::string attr(const Object& object, const char* name) {
  const auto at = object.attributes.find(name);
  return at == object.attributes.end() ? std::string() : at->second;
}

gp_Pnt flat(const gp_Pnt& p) { return gp_Pnt(p.X(), p.Y(), 0); }

Json basisRecord(const SiteBoundary& site, const SiteBoundaryGeometry& geometry) {
  Json perimeter = Json::array();
  for (const auto& p : geometry.vertices) perimeter.push_back({p.X(), p.Y(), p.Z()});
  Json road;
  if (site.road) road = {{"edge", site.road->localEdge}, {"width", site.road->widthMm},
                        {"kind", site.road->kind}, {"source", site.road->source}};
  return {{"vertices", perimeter}, {"site_source", site.source}, {"road", road}};
}

std::optional<std::size_t> roadIndex(const SiteBoundaryGeometry& geometry, const std::optional<Line>& road) {
  if (!road) return std::nullopt;
  const auto a = std::find_if(geometry.vertices.begin(), geometry.vertices.end(), [&](const gp_Pnt& p) {
    return p.Distance((*road)[0]) <= tolerance::kLinear;
  });
  if (a == geometry.vertices.end()) return std::nullopt;
  return static_cast<std::size_t>(a - geometry.vertices.begin());
}

Line perimeterLine(const SiteBoundaryGeometry& geometry, std::size_t i) {
  return {flat(geometry.vertices[i]), flat(geometry.vertices[(i + 1) % geometry.vertices.size()])};
}

FireSpreadStudy::Basis roadCenter(const SiteBoundaryGeometry& geometry, std::size_t i, double width) {
  const Line edge = perimeterLine(geometry, i);
  const gp_Pnt origin = edge[0];
  const gp_Vec along = gp_Vec(edge[0], edge[1]).Normalized();
  double winding = 0, extent = width + 5000;
  for (std::size_t j = 0; j < geometry.vertices.size(); ++j) {
    const Line side = perimeterLine(geometry, j);
    const gp_Vec a(origin, side[0]), b(origin, side[1]);
    winding += a.Crossed(b).Z();
    extent = std::max(extent, std::abs(a.Dot(along)) + 5000);
  }
  const gp_Vec left(-along.Y(), along.X(), 0);
  const gp_Pnt mid = origin.Translated(left * (winding > 0 ? -width / 2 : width / 2));
  return {{mid.Translated(-along * extent), mid.Translated(along * extent)}, true,
          {mid, mid.Translated(gp_Vec(edge[0], edge[1]))}};
}

TopoDS_Shape contour(const FireSpreadStudy& study, const SiteBoundaryGeometry& geometry, double distance,
                     TopoDS_Shape& clippedBand) {
  BRepBuilderAPI_MakePolygon perimeter;
  for (const auto& point : geometry.vertices) perimeter.Add(flat(point));
  perimeter.Close();
  const TopoDS_Face site = BRepBuilderAPI_MakeFace(perimeter.Wire()).Face();
  TopoDS_Shape region;
  for (const auto& basis : study.bases) {
    const auto& [a, b] = basis.line;
    const gp_Vec shift = gp_Vec(a, b).Normalized() * distance;
    const gp_Vec side(-shift.Y(), shift.X(), 0);
    const gp_Pnt al = a.Translated(side), bl = b.Translated(side);
    const gp_Pnt ar = a.Translated(-side), br = b.Translated(-side);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(al, bl).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(bl, b.Translated(shift), br).Value()).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(br, ar).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(ar, a.Translated(-shift), al).Value()).Edge());
    const TopoDS_Face band = BRepBuilderAPI_MakeFace(wire.Wire()).Face();
    if (region.IsNull()) region = band;
    else {
      BRepAlgoAPI_Fuse merge;
      TopTools_ListOfShape argument, tool;
      argument.Append(region);
      tool.Append(band);
      merge.SetArguments(argument);
      merge.SetTools(tool);
      merge.SetNonDestructive(true);
      merge.Build();
      if (!merge.IsDone() || merge.HasErrors()) throw Standard_Failure("Fire distance contour union failed");
      region = merge.Shape();
    }
  }
  if (region.IsNull()) return {};
  ShapeUpgrade_UnifySameDomain unified(region, true, true, false);
  unified.Build();
  BRep_Builder builder;
  TopoDS_Compound boundary;
  builder.MakeCompound(boundary);
  for (TopExp_Explorer edge(unified.Shape(), TopAbs_EDGE); edge.More(); edge.Next())
    builder.Add(boundary, edge.Current());
  BRepAlgoAPI_Common fill(site, unified.Shape());
  if (!fill.IsDone() || fill.HasErrors()) throw Standard_Failure("Fire distance band clipping failed");
  if (TopExp_Explorer(fill.Shape(), TopAbs_FACE).More()) clippedBand = fill.Shape();
  // Clip the contour itself. The portions of the site boundary inside the band are not limit lines.
  BRepAlgoAPI_Common clip(site, boundary);
  if (!clip.IsDone() || clip.HasErrors()) throw Standard_Failure("Fire distance contour clipping failed");
  return TopExp_Explorer(clip.Shape(), TopAbs_EDGE).More() ? clip.Shape() : TopoDS_Shape();
}

std::vector<FireSpreadStudy::Floor> floorsIn(const Document& document) {
  std::vector<FireSpreadStudy::Floor> floors;
  const auto& levels = document.settings().levels;
  for (const auto& level : levels) {
    const auto& name = level.name;
    const auto digitsEnd = name.find_first_not_of("0123456789");
    if (digitsEnd == std::string::npos || digitsEnd == 0 || name.front() == '0' || name.substr(digitsEnd) != "FL") continue;
    floors.push_back({level.id, name, level.elevation, name == "1FL" ? 3 : 5});
  }
  std::sort(floors.begin(), floors.end(), [](const auto& a, const auto& b) { return a.z < b.z; });
  if (!floors.empty() && (floors.front().metres != 3 ||
      std::adjacent_find(floors.begin(), floors.end(), [](const auto& a, const auto& b) {
        return b.z - a.z <= tolerance::kLinear;
      }) != floors.end())) floors.clear();
  return floors;
}

void reviewOpenings(const Document& document, FireSpreadStudy& study) {
  using State = FireSpreadStudy::Status;
  const auto outline = projectOutline(document);
  const auto zone = outline.find("fire_zone");
  const bool applies = zone != outline.end() && (zone->second == "fire" || zone->second == "quasi_fire");
  std::vector<TopoDS_Edge> references;
  for (const auto& b : study.bases) references.push_back(BRepBuilderAPI_MakeEdge(b.line[0], b.line[1]).Edge());
  for (const auto& row : openingSchedule(document).rows) {
    const auto* host = document.find(row.host);
    if (!host) continue;
    const auto type = construction::wallType(attr(*host, "wall_type"));
    if (type && !type->exterior) continue;
    FireSpreadStudy::Opening result;
    result.row = row;
    const auto normal = Modeler::exteriorWallNormal(document, *host);
    const bool reverse = normal && normal->Dot(row.normal) < 0;
    const gp_Pnt face = row.center.Translated(gp_Vec(row.normal) * (reverse ? -row.depth : 0.0));
    const gp_Vec across = gp_Vec(-row.normal.Y(), row.normal.X(), 0).Normalized() * (row.width / 2);
    const gp_Pnt a = gp_Pnt(face.X(), face.Y(), row.bottom).Translated(-across);
    const gp_Pnt b = gp_Pnt(face.X(), face.Y(), row.bottom).Translated(across);
    const gp_Vec up(0, 0, row.height);
    result.marker = BRepBuilderAPI_MakePolygon(a, b, b.Translated(up), a.Translated(up), true).Wire();
    const auto floor = std::find_if(study.floors.begin(), study.floors.end(), [&](const auto& f) {
      return attr(*host, "base_level") == f.id;
    });
    if (study.changed || references.empty()) result.note = "基準線を確認";
    else if (!normal || std::abs(normal->Dot(row.normal)) < 1 - 1e-6) result.note = "壁種と外側を確認";
    else if (floor == study.floors.end() || row.bottom < floor->z - tolerance::kLinear ||
             (std::next(floor) != study.floors.end() && row.bottom + row.height > std::next(floor)->z + tolerance::kLinear))
      result.note = "開口の所属階を確認";
    else {
      const auto opening = BRepBuilderAPI_MakeEdge(flat(a), flat(b)).Edge();
      double nearest = std::numeric_limits<double>::infinity();
      for (std::size_t i = 0; i < references.size(); ++i) {
        BRepExtrema_DistShapeShape measure(opening, references[i]);
        if (measure.IsDone() && measure.Value() < nearest) {
          nearest = measure.Value();
          result.basis = study.bases[i].road ? "道路中心線" : "隣地境界線";
        }
      }
      if (std::isfinite(nearest)) {
        result.distance = nearest;
        if (nearest <= floor->metres * 1000 + tolerance::kLinear) {
          result.status = applies ? State::EquipmentReview : State::InRange;
          result.note = applies ? "防火戸・防火サッシの仕様を照合" : "防火指定と開口部に適用する規定を確認";
          // docs/335: the 防火設備 given for it, to be compared with the product's documents.
          if (applies && !row.fireSpec.empty())
            result.note = "防火設備として入力：" + row.fireSpec +
                          (row.fireCertification.empty() ? std::string() : "（" + row.fireCertification + "）") + "。製品資料で照合";
          else if (applies)
            result.note = "防火設備が未入力（set_opening_fire_equipment）。防火戸・防火サッシの仕様を照合";
        } else {
          result.status = State::BeyondStudiedLines;
          result.note = "基準線の網羅と個別条件を確認";
        }
      } else result.note = "開口と基準線の距離を取得できません";
    }
    study.openings.push_back(std::move(result));
  }
  std::stable_sort(study.openings.begin(), study.openings.end(), [](const auto& a, const auto& b) {
    return a.status < b.status;
  });
}
}  // namespace

sbr::Status Modeler::setFireSpreadConditions(const FireSpreadConditions& conditions) {
  const auto& site = document_.settings().siteBoundary;
  if (!site) return Status::failure("敷地境界を先に登録してください");
  const auto* outline = document_.find(site->outline);
  if (!outline) return Status::failure("敷地境界の輪郭が見つかりません");
  const auto geometry = document_.siteBoundaryGeometry(site->outline);
  if (!geometry) return Status::failure(geometry.error());
  const bool clear = conditions.neighbors.empty() && !conditions.parallelRoad;
  const auto road = roadIndex(*geometry, document_.siteRoadEdge(*geometry));
  auto neighbors = conditions.neighbors;
  std::sort(neighbors.begin(), neighbors.end());
  if (std::adjacent_find(neighbors.begin(), neighbors.end()) != neighbors.end() ||
      (!neighbors.empty() && neighbors.back() >= geometry->vertices.size()) ||
      (road && std::binary_search(neighbors.begin(), neighbors.end(), *road)))
    return Status::failure("道路と重ならない隣地境界を選んでください");
  if (conditions.parallelRoad && (!road || !site->road || !(site->road->widthMm > 0)))
    return Status::failure("道路境界と確認した幅員を先に登録してください");
  if (!clear && (conditions.source.size() > 300 || conditions.source.find_first_not_of(" \t") == std::string::npos ||
                 conditions.source.find_first_of("\r\n") != std::string::npos))
    return Status::failure("基準線の確認資料を1行、100字以内で入力してください");
  const std::string value = clear ? std::string() : Json{{"neighbors", neighbors}, {"road", conditions.parallelRoad},
      {"source", conditions.source}, {"basis", basisRecord(*site, *geometry)}}.dump();
  if (value == attr(*outline, kAttr)) return Status::success();
  if (outline->locked) return Status::failure("敷地境界がロックされています");
  Object changed = *outline;
  if (clear) changed.attributes.erase(kAttr);
  else changed.attributes[kAttr] = value;
  history_.execute(Command{"Fire Spread Conditions",
      {ObjectDelta{outline->id, *outline, changed, document_.indexOf(outline->id)}}, {}});
  return Status::success();
}

FireSpreadStudy fireSpreadStudy(const Document& document, bool contours, bool openings) {
  FireSpreadStudy out;
  const auto& site = document.settings().siteBoundary;
  const auto* outline = site ? document.find(site->outline) : nullptr;
  if (!outline) return out;
  const std::string stored = attr(*outline, kAttr);
  if (stored.empty()) return out;
  out.configured = true;
  out.floors = floorsIn(document);
  const auto geometry = document.siteBoundaryGeometry(site->outline);
  const Json saved = Json::parse(stored, nullptr, false);
  try {
    out.changed = !geometry || !saved.is_object() || saved.at("basis") != basisRecord(*site, *geometry);
    if (!out.changed) {
      const auto neighbors = saved.at("neighbors").get<std::vector<std::size_t>>();
      const bool useRoad = saved.at("road").get<bool>();
      const auto road = roadIndex(*geometry, document.siteRoadEdge(*geometry));
      for (const auto i : neighbors) {
        if (i >= geometry->vertices.size() || i == road) throw std::out_of_range("invalid fire boundary");
        const auto edge = perimeterLine(*geometry, i);
        out.bases.push_back({edge, false, edge});
      }
      if (useRoad) {
        if (!road || !site->road || !(site->road->widthMm > 0)) throw std::out_of_range("invalid fire road");
        out.bases.push_back(roadCenter(*geometry, *road, site->road->widthMm));
      }
      out.conditions = {neighbors, useRoad, saved.at("source").get<std::string>()};
    }
  } catch (const std::exception&) {
    out.changed = true;
    out.bases.clear();
  }
  if (out.changed) out.issue = "敷地・道路の条件が変わりました。基準線を選び直してください。";
  else if (out.floors.empty()) out.issue = "1FL などの階レベルを登録すると、階別表示と建具の照合ができます。";
  try {
    if (contours && geometry && !out.bases.empty()) {
      out.limits3 = contour(out, *geometry, 3000, out.band3);
      out.limits5 = contour(out, *geometry, 5000, out.band5);
    }
  } catch (const Standard_Failure&) {
    out.limits3.Nullify();
    out.limits5.Nullify();
    out.band3.Nullify();
    out.band5.Nullify();
    out.issue = "延焼ラインを描画できません。基準線と敷地の形状を確認してください。";
  }
  if (openings) {
    try {
      reviewOpenings(document, out);
    } catch (const Standard_Failure&) {
      out.openings.clear();
      out.issue = "開口を照合できません。開口形状と基準線を確認してください。";
    }
  }
  return out;
}

std::string fireSpreadStatus(FireSpreadStudy::Status status) {
  switch (status) {
    case FireSpreadStudy::Status::EquipmentReview: return "防火設備を照合";
    case FireSpreadStudy::Status::InRange: return "延焼範囲内";
    case FireSpreadStudy::Status::BeyondStudiedLines: return "登録線の範囲外";
    case FireSpreadStudy::Status::Unchecked: return "要確認";
  }
  return "要確認";
}

draw::Table fireSpreadTable(const FireSpreadStudy& study) {
  draw::Table table;
  table.title = "外壁開口部の延焼確認";
  table.headers = {"符号", "階", "壁", "品番", "距離 (m)", "基準線", "状態", "確認事項"};
  for (const auto& opening : study.openings) {
    const auto& row = opening.row;
    char distance[32]{};
    if (opening.distance) std::snprintf(distance, sizeof distance, "%.3f", *opening.distance / 1000);
    table.rows.push_back({row.mark, row.level, row.hostName, row.product, distance, opening.basis,
                          fireSpreadStatus(opening.status), opening.note});
  }
  table.notes = {"建築基準法 第2条第6号・第61条。1階 3 m、2階以上 5 m。開口の外壁面全幅からの水平最短距離。",
                 "水路等・同一敷地内の別棟・角度による除外は個別確認。認定番号・仕様は製品資料で照合。"};
  if (!study.conditions.source.empty()) table.notes.push_back("基準線資料　" + study.conditions.source);
  if (!study.issue.empty()) table.notes.push_back(study.issue);
  return table;
}

}  // namespace sbr::doc
