#include "sbr/doc/FireCompartments.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "sbr/doc/Modeler.h"

namespace sbr::doc {
namespace {
using Json = nlohmann::json;
constexpr const char* kBoundary = "fire_compartment_boundary";
constexpr const char* kEquipment = "fire_compartment_equipment";

std::string attr(const Object& object, const char* key) {
  const auto at = object.attributes.find(key);
  return at == object.attributes.end() ? std::string() : at->second;
}

bool lineText(const std::string& value, std::size_t bytes) {
  return value.size() <= bytes && value.find_first_of("\r\n") == std::string::npos &&
         value.find_first_not_of(" \t") != std::string::npos;
}

Json equipmentRecord(const Object& host) {
  auto value = Json::parse(attr(host, kEquipment), nullptr, false);
  return value.is_object() ? value : Json::object();
}

Json openingBasis(const Object& host, const OpeningRow& opening) {
  const auto fitting = Json::parse(attr(host, "opening_fittings"), nullptr, false);
  const auto hundredths = [](double mm) { return std::round(mm * 100.0); };
  return {{"width_centi_mm", hundredths(opening.width)}, {"height_centi_mm", hundredths(opening.height)},
          {"depth_centi_mm", hundredths(opening.depth)}, {"product", opening.product},
          {"fitting", fitting.is_object() && fitting.contains(opening.openingId) ? fitting.at(opening.openingId) : Json()}};
}

std::string levelOf(const Document& doc, const Object& object, bool floor) {
  const auto id = attr(object, floor ? "top_level" : "base_level");
  for (const auto& level : doc.settings().levels)
    if (level.id == id) return level.name;
  return {};
}

TopoDS_Shape openingMarker(const OpeningRow& opening) {
  const gp_Vec side = gp_Vec(-opening.normal.Y(), opening.normal.X(), 0) * (opening.width / 2);
  const gp_Pnt center(opening.center.X(), opening.center.Y(), opening.bottom);
  const auto a = center.Translated(-side), b = center.Translated(side);
  const gp_Vec up(0, 0, opening.height);
  BRepBuilderAPI_MakePolygon outline(a, b, b.Translated(up), a.Translated(up), true);
  return outline.IsDone() ? outline.Shape() : TopoDS_Shape();
}

struct FloorSurface {
  std::vector<TopoDS_Shape> holes;
  bool read = false;
};

FloorSurface floorOpenings(const TopoDS_Shape& shape, double top) {
  FloorSurface result;
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
    const auto face = TopoDS::Face(faces.Current());
    const BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto plane = surface.Plane();
    if (std::abs(plane.Axis().Direction().Z()) < 1 - 1e-8 ||
        std::abs(plane.Location().Z() - top) > tolerance::kLinear * 10) continue;
    const auto outer = BRepTools::OuterWire(face);
    if (outer.IsNull()) continue;
    result.read = true;
    for (TopExp_Explorer wires(face, TopAbs_WIRE); wires.More(); wires.Next())
      if (!wires.Current().IsSame(outer)) result.holes.push_back(wires.Current());
  }
  return result;
}

std::string openingStatus(const FireCompartmentBoundary& b, const FireCompartmentEquipment& equipment) {
  if (b.requirement == "no_openings") return "開口なしの指定を確認";
  if (b.requirement == "unknown") return "必要な設備種別を確認";
  if (equipment.kind.empty()) return "設備の仕様・根拠を入力";
  if (equipment.kind != b.requirement) return "必要種別と登録種別を照合";
  return "仕様記録あり";
}
}  // namespace

std::optional<FireCompartmentBoundary> fireCompartmentBoundary(const Object& object) {
  const auto value = Json::parse(attr(object, kBoundary), nullptr, false);
  if (!value.is_object()) return std::nullopt;
  try {
    FireCompartmentBoundary b{value.at("mark").get<std::string>(), value.at("kind").get<std::string>(),
                              value.at("requirement").get<std::string>(), value.at("source").get<std::string>()};
    if (!b.mark.empty() && !fireCompartmentKindName(b.kind).empty() &&
        !fireCompartmentRequirementName(b.requirement).empty() && !b.source.empty()) return b;
  } catch (const Json::exception&) {
  }
  return std::nullopt;
}

Status Modeler::setFireCompartmentBoundary(const std::vector<ObjectId>& ids, const FireCompartmentBoundary& boundary) {
  if (ids.empty()) return Status::failure("区画に使う壁・床を選択してください");
  const bool clear = boundary.mark.empty() && boundary.kind.empty() && boundary.requirement.empty() && boundary.source.empty();
  if (!clear && (!lineText(boundary.mark, 90) || !lineText(boundary.source, 450) ||
                 fireCompartmentKindName(boundary.kind).empty() || fireCompartmentRequirementName(boundary.requirement).empty()))
    return Status::failure("区画符号、区画の種類、開口部の条件、適用条項・確認資料を入力してください");
  for (const auto& id : ids) {
    const auto* object = document_.find(id);
    if (!object || object->locked || (!clear &&
        (object->shape.IsNull() || (attr(*object, "kind") != "wall" && attr(*object, "kind") != "slab"))))
      return Status::failure("ロックされていない壁・床を選択してください");
  }
  const auto value = clear ? std::string() : Json{{"mark", boundary.mark}, {"kind", boundary.kind},
      {"requirement", boundary.requirement}, {"source", boundary.source}}.dump();
  return setAttribute(ids, kBoundary, value);
}

Status Modeler::setFireCompartmentEquipment(const ObjectId& host, const std::string& opening,
                                           const FireCompartmentEquipment& equipment) {
  const auto* object = document_.find(host);
  if (!object || object->locked || !fireCompartmentBoundary(*object))
    return Status::failure("区画に登録した壁の開口部を選択してください");
  if (opening.find_first_not_of("0123456789") == std::string::npos)
    return Status::failure("旧形式の開口です。建具を設定し直してから設備を登録してください");
  const auto openings = openingSchedule(document_);
  const auto target = std::find_if(openings.rows.begin(), openings.rows.end(), [&](const auto& row) {
    return row.host == host && row.openingId == opening;
  });
  if (target == openings.rows.end()) return Status::failure("壁の開口部が見つかりません");
  if ((equipment.kind.empty() && !equipment.reference.empty()) ||
      (!equipment.kind.empty() && ((equipment.kind != "specific" && equipment.kind != "fire") ||
                                   !lineText(equipment.reference, 450))))
    return Status::failure("設備種別と認定番号または確認資料を入力してください");
  auto value = equipmentRecord(*object);
  if (equipment.kind.empty()) value.erase(opening);
  else value[opening] = {{"kind", equipment.kind}, {"reference", equipment.reference}, {"basis", openingBasis(*object, *target)}};
  return setAttribute({host}, kEquipment, value.empty() ? std::string() : value.dump());
}

std::vector<FireCompartmentRow> fireCompartmentReview(const Document& document, bool markers) {
  std::map<ObjectId, std::vector<OpeningRow>> openings;
  std::vector<FireCompartmentRow> out;
  const bool registered = std::any_of(document.objects().begin(), document.objects().end(),
                                      [](const auto& o) { return o.attributes.count(kBoundary) != 0; });
  if (!registered) return out;
  for (const auto& opening : openingSchedule(document).rows) openings[opening.host].push_back(opening);
  for (const auto& object : document.objects()) {
    const auto boundary = fireCompartmentBoundary(object);
    if (!boundary) continue;
    const bool floor = attr(object, "kind") == "slab";
    const bool wall = attr(object, "kind") == "wall";
    FireCompartmentRow member;
    member.host = object.id;
    member.hostName = object.name;
    member.boundary = *boundary;
    member.level = levelOf(document, object, floor);
    member.status = "構造・取り合い・貫通を確認";
    out.push_back(member);
    auto& review = out.back();
    if ((!wall && !floor) || object.shape.IsNull()) {
      review.status = "区画部材の形状を確認";
      continue;
    }
    if (review.level.empty()) review.status = "所属階・構造・取り合いを確認";
    if (markers) review.marker = document.worldShape(object);
    if (floor) {
      try {
        const auto bounds = document.worldBounds(object);
        const auto found = bounds.IsVoid() ? FloorSurface{} : floorOpenings(document.worldShape(object), bounds.CornerMax().Z());
        if (!found.read) review.status = "床の上面と貫通を確認";
        for (std::size_t i = 0; i < found.holes.size(); ++i) {
          auto row = member;
          row.part = FireCompartmentRow::Part::FloorOpening;
          row.openingMark = "床上面の開口 " + std::to_string(i + 1);
          row.status = boundary->requirement == "no_openings" ? "開口なしの指定を確認" : "貫通・防火措置を確認";
          if (markers) row.marker = found.holes[i];
          out.push_back(std::move(row));
        }
      } catch (const Standard_Failure&) {
        review.status = "床の形状を確認";
      }
      continue;
    }
    const auto saved = equipmentRecord(object);
    for (const auto& opening : openings[object.id]) {
      auto row = member;
      row.part = FireCompartmentRow::Part::WallOpening;
      row.openingId = opening.openingId;
      row.openingMark = opening.mark;
      row.wallOpening = opening;
      bool equipmentChanged = false;
      if (const auto it = saved.find(row.openingId); it != saved.end() && it->is_object()) {
        try {
          const auto kind = it->at("kind").get<std::string>();
          const auto ref = it->at("reference").get<std::string>();
          if ((kind == "fire" || kind == "specific") && lineText(ref, 450)) {
            row.equipment = {kind, ref};
            equipmentChanged = !it->contains("basis") || it->at("basis") != openingBasis(object, opening);
          }
        } catch (const Json::exception&) {
        }
      }
      row.status = openingStatus(*boundary, row.equipment);
      if (equipmentChanged && boundary->requirement != "no_openings") row.status = "開口寸法・建具の変更を照合";
      else if (row.status == "仕様記録あり" && opening.kind == "opening") row.status = "建具の設定を確認";
      row.attention = row.status != "仕様記録あり";
      if (markers) row.marker = openingMarker(opening);
      out.push_back(std::move(row));
    }
  }
  return out;
}

std::string fireCompartmentKindName(const std::string& kind) {
  if (kind == "area") return "面積区画";
  if (kind == "shaft") return "竪穴区画";
  if (kind == "mixed_use") return "異種用途区画";
  if (kind == "other") return "その他";
  return {};
}

std::string fireCompartmentRequirementName(const std::string& requirement) {
  if (requirement == "unknown") return "条項確認中";
  if (requirement == "fire") return "防火設備";
  if (requirement == "specific") return "特定防火設備";
  if (requirement == "no_openings") return "開口なし";
  return {};
}

draw::Table fireCompartmentTable(const std::vector<FireCompartmentRow>& rows) {
  draw::Table table{"防火区画・開口部確認表", {"区画符号", "種類", "階", "部材・建具", "設計条件・登録設備", "根拠・仕様資料", "確認事項"}, {}, {}};
  for (const auto& row : rows) {
    const bool boundary = row.part == FireCompartmentRow::Part::Boundary;
    const auto name = boundary ? row.hostName : row.hostName + " / " + row.openingMark;
    std::string equip = fireCompartmentRequirementName(row.boundary.requirement);
    if (row.part == FireCompartmentRow::Part::FloorOpening && row.boundary.requirement != "no_openings") equip = "防火措置を確認";
    if (!row.equipment.kind.empty()) equip = "設計 " + equip + " / 登録 " + fireCompartmentRequirementName(row.equipment.kind);
    table.rows.push_back({row.boundary.mark, fireCompartmentKindName(row.boundary.kind), row.level, name, equip,
                          boundary ? row.boundary.source : row.equipment.reference, row.status});
  }
  table.notes = {"建築基準法施行令 第112条、横浜市建築基準条例。支持部・取合い、常閉・自閉・連動・遮煙、風道・配管等は個別照合。",
                 "区画の適用条項・囲いの成立、認定仕様への適合は設計資料と照合する。床に表示する開口は上面の閉じた輪郭。"};
  return table;
}

}  // namespace sbr::doc
