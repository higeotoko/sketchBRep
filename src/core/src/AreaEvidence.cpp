#include "AreaEvidence.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"

namespace sbr::doc::detail {
namespace {
using Loop = std::vector<gp_Pnt2d>;
constexpr double kTol = 1e-5;

struct Footprint {
  struct Polygon {
    Loop outer;
    std::vector<Loop> holes;
  };
  std::vector<Polygon> faces;
  bool straight = true;
};

Footprint footprint(const TopoDS_Shape& shape) {
  Footprint out;
  if (shape.IsNull()) return out;
  ShapeUpgrade_UnifySameDomain unify(shape, false, true, false);
  unify.SetSafeInputMode(true);
  unify.Build();
  for (TopExp_Explorer f(unify.Shape(), TopAbs_FACE); f.More(); f.Next()) {
    const auto face = TopoDS::Face(f.Current());
    const BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto plane = surface.Plane();
    if (std::abs(plane.Axis().Direction().Z()) < 1 - 1e-9 || std::abs(plane.Location().Z() - 1) > kTol) continue;
    const auto outer = BRepTools::OuterWire(face);
    Footprint::Polygon polygon;
    for (TopExp_Explorer wires(face, TopAbs_WIRE); wires.More(); wires.Next()) {
      const auto wire = TopoDS::Wire(wires.Current());
      Loop loop;
      const auto add = [&](const gp_Pnt& p) { loop.emplace_back(p.X(), p.Y()); };
      for (BRepTools_WireExplorer edges(wire, face); edges.More(); edges.Next()) {
        const auto edge = edges.Current();
        const BRepAdaptor_Curve curve(edge);
        const bool reversed = edge.Orientation() == TopAbs_REVERSED;
        if (curve.GetType() == GeomAbs_Line) {
          add(curve.Value(reversed ? curve.LastParameter() : curve.FirstParameter()));
          continue;
        }
        out.straight = false;
        const GCPnts_QuasiUniformDeflection points(curve, 0.15);
        if (!points.IsDone()) continue;
        for (int i = 1; i < points.NbPoints(); ++i) add(points.Value(reversed ? points.NbPoints() - i + 1 : i));
      }
      if (loop.size() < 3) continue;
      if (wire.IsSame(outer)) polygon.outer = std::move(loop);
      else polygon.holes.push_back(std::move(loop));
    }
    if (!polygon.outer.empty()) out.faces.push_back(std::move(polygon));
  }
  return out;
}

struct Cell {
  double x0, x1, lower0, lower1, upper0, upper1;
  double width() const { return x1 - x0; }
  double height0() const { return upper0 - lower0; }
  double height1() const { return upper1 - lower1; }
  double area() const { return width() * (height0() + height1()) / 2; }
  gp_Pnt2d center() const { return {(x0 + x1) / 2, (lower0 + lower1 + upper0 + upper1) / 4}; }
};

struct Side {
  gp_Pnt2d a, b;
  double y(double x) const { return a.Y() + (b.Y() - a.Y()) * ((x - a.X()) / (b.X() - a.X())); }
};

std::optional<std::vector<Cell>> dimensionedCells(const Footprint& outlines, double area) {
  if (!outlines.straight || outlines.faces.empty()) return std::nullopt;
  std::vector<Cell> result;
  for (const auto& face : outlines.faces) {
    std::vector<Side> sides;
    std::vector<double> coordinates;
    const auto gather = [&](const Loop& loop) {
      for (std::size_t i = 0; i < loop.size(); ++i) {
        const auto& a = loop[i];
        const auto& b = loop[(i + 1) % loop.size()];
        coordinates.push_back(a.X());
        if (std::abs(a.X() - b.X()) > kTol) sides.push_back(a.X() < b.X() ? Side{a, b} : Side{b, a});
      }
    };
    gather(face.outer);
    for (const auto& hole : face.holes) gather(hole);
    if (coordinates.size() > 4096) return std::nullopt;
    std::sort(coordinates.begin(), coordinates.end());
    coordinates.erase(std::unique(coordinates.begin(), coordinates.end(),
                                  [](double a, double b) { return b - a <= kTol; }), coordinates.end());
    for (std::size_t i = 1; i < coordinates.size(); ++i) {
      const double x0 = coordinates[i - 1], x1 = coordinates[i], mid = (x0 + x1) / 2;
      std::vector<const Side*> hits;
      for (const auto& side : sides)
        if (side.a.X() < mid && mid < side.b.X()) hits.push_back(&side);
      std::stable_sort(hits.begin(), hits.end(), [&](const Side* a, const Side* b) { return a->y(mid) < b->y(mid); });
      if (hits.size() % 2 != 0) return std::nullopt;
      for (std::size_t j = 0; j < hits.size(); j += 2) {
        const auto* lower = hits[j];
        const auto* upper = hits[j + 1];
        Cell cell{x0, x1, lower->y(x0), lower->y(x1), upper->y(x0), upper->y(x1)};
        if (cell.height0() < -kTol || cell.height1() < -kTol) return std::nullopt;
        if (cell.area() > kTol) result.push_back(cell);
      }
      if (result.size() > 240) return std::nullopt;
    }
  }
  const double sum = std::accumulate(result.begin(), result.end(), 0.0, [](double a, const Cell& cell) { return a + cell.area(); });
  if (result.empty() || std::abs(sum - area) > std::max(1.0, area * 1e-8)) return std::nullopt;
  return result;
}

std::string fixed(double value, int places) {
  char buf[64]{};
  std::snprintf(buf, sizeof buf, "%.*f", places, value);
  return buf;
}

std::string mm(double value) { return fixed(std::max(0.0, value), std::abs(value - std::round(value)) < 0.0001 ? 0 : 2); }
std::string sqm(double value) { return fixed(value / 1e6, 3); }

void outline(draw::View& view, const Loop& points, draw::Pen pen) {
  draw::Item item;
  item.kind = draw::Item::Kind::Polyline;
  item.pen = pen;
  item.points = points;
  item.closed = true;
  view.items.push_back(std::move(item));
}

void mark(draw::View& view, const Cell& cell, const std::string& code, double scale) {
  if (cell.width() < 6.5 * scale || std::min(cell.height0(), cell.height1()) < 3.5 * scale) {
    view.bracingMarks.push_back({cell.center(), code, 2.5});
    return;
  }
  draw::Item label;
  label.kind = draw::Item::Kind::Text;
  label.pen = draw::Pen::Text;
  label.points = {cell.center()};
  label.text = code;
  label.height = 2.5;
  label.hAlign = label.vAlign = 1;
  view.items.push_back(std::move(label));
}

void areaParts(draw::View& view, draw::Table& formulas, const TopoDS_Shape& shape, const std::string& prefix,
               const std::string& description, bool counted, double scale, bool shade = true) {
  if (shape.IsNull()) return;
  const double exact = geom::volume(shape);
  if (exact <= kTol) return;
  Footprint plan;
  try { plan = footprint(shape); } catch (const Standard_Failure&) {}
  const auto cells = dimensionedCells(plan, exact);
  for (const auto& polygon : plan.faces) {
    if (counted && shade) {
      draw::Item fill;
      fill.kind = draw::Item::Kind::Fill;
      fill.pen = draw::Pen::Fill;
      fill.points = polygon.outer;
      fill.holes = polygon.holes;
      view.items.push_back(std::move(fill));
    }
    if (!cells) {
      outline(view, polygon.outer, counted ? draw::Pen::Visible : draw::Pen::Hidden);
      for (const auto& hole : polygon.holes) outline(view, hole, draw::Pen::Hidden);
    }
  }
  if (!cells) {
    formulas.rows.push_back({prefix, description, "モデル実測。寸法・算式の分割は未評価", sqm(exact)});
    if (plan.faces.empty()) view.calculationNotes.push_back(description + "の輪郭を作成できませんでした。");
    return;
  }
  for (std::size_t i = 0; i < cells->size(); ++i) {
    const auto& c = (*cells)[i];
    const auto code = prefix + std::to_string(i + 1);
    outline(view, {{c.x0, c.lower0}, {c.x1, c.lower1}, {c.x1, c.upper1}, {c.x0, c.upper0}},
            counted ? draw::Pen::Visible : draw::Pen::Hidden);
    mark(view, c, code, scale);
    const std::string height = std::abs(c.height0() - c.height1()) <= kTol ? mm(c.height0())
        : "(" + mm(c.height0()) + " + " + mm(c.height1()) + ") ÷ 2";
    formulas.rows.push_back({code, description, mm(c.width()) + " × " + height, sqm(c.area())});
  }
}

TopoDS_Shape difference(const TopoDS_Shape& from, const TopoDS_Shape& subtract) {
  if (from.IsNull()) return {};
  if (subtract.IsNull()) return from;
  const auto difference = geom::booleanCombine(geom::BooleanOp::Subtract, {from, subtract});
  return difference ? *difference : TopoDS_Shape();
}

AreaEvidencePage pageFor(const Document& document, const std::string& title) {
  AreaEvidencePage page;
  page.view.spec = draw::planView(0);
  page.view.spec.title = title;
  if (const auto& north = document.settings().siteNorth) {
    const double angle = north->angle * std::acos(-1.0) / 180;
    page.view.trueNorth = gp_Dir2d(std::sin(angle), std::cos(angle));
  }
  page.tables.push_back({"求積表", {"符号", "範囲", "算式 (mm)", "面積 (m²)"}, {}, {}});
  return page;
}

void paginate(AreaEvidencePage& page, std::vector<std::string> total) {
  auto& main = page.tables.front();
  constexpr std::size_t rowsPerPage = 20;
  if (main.rows.size() <= rowsPerPage) {
    main.rows.push_back(std::move(total));
    return;
  }
  const auto rows = std::move(main.rows);
  main.rows.assign(rows.begin(), rows.begin() + rowsPerPage);
  main.rows.push_back(total);
  main.notes.push_back("残りの算式は同名の求積表（続き）を参照。");
  for (std::size_t start = rowsPerPage; start < rows.size(); start += rowsPerPage) {
    draw::Table table;
    table.title = page.view.spec.title + " 求積表（続き " + std::to_string(start / rowsPerPage) + "）";
    table.headers = main.headers;
    table.rows.assign(rows.begin() + start, rows.begin() + std::min(rows.size(), start + rowsPerPage));
    if (start + rowsPerPage >= rows.size()) table.rows.push_back(total);
    table.notes = {"範囲は求積図の同じ符号で確認できます。"};
    page.continuation.push_back(std::move(table));
  }
}
}  // namespace

std::vector<AreaEvidencePage> areaEvidencePages(const Document& document, const AreaTable& areas,
                                                 const AreaGeometry& geometry, double scale) {
  std::vector<AreaEvidencePage> result;
  for (const auto& row : areas.rows) {
    const auto at = geometry.floors.find(row.level);
    if (!row.enclosedKnown || at == geometry.floors.end() || at->second.enclosed.IsNull()) continue;
    const auto& floor = at->second;
    auto page = pageFor(document, row.level + " 床面積求積図");
    auto& table = page.tables.front();
    areaParts(page.view, table, floor.counted, "F", "算入", true, scale);
    if (row.legalKnown) {
      if (!floor.assumedFloor && geom::volume(floor.enclosed) - geom::volume(floor.counted) > 1.0) {
        const auto excluded = difference(floor.enclosed, floor.counted);
        areaParts(page.view, table, excluded, "X", "床なし・除外", false, scale);
        if (excluded.IsNull()) table.notes.push_back("床のない範囲の輪郭を作成できませんでした。");
      }
    } else {
      areaParts(page.view, table, floor.enclosed, "R", "壁芯参考", false, scale);
      table.notes.push_back("既存の面積計算が未評価の階です。図は計算可能な範囲を示します。");
    }
    if (floor.assumedFloor) table.notes.push_back("スラブがない階は、壁芯で囲まれた全面を床と仮定しています。");
    if (floor.balconyMm2 > 0)
      table.rows.push_back({"", "バルコニー加算", "面積表の加算値。算入範囲の図示は未評価", sqm(floor.balconyMm2)});
    table.notes.push_back("塗り：算入。X・破線：床がなく除外した範囲。算式の寸法は mm、面積表示は丸めています。");
    paginate(page, {"", "床面積 合計", floor.balconyMm2 > 0 ? "F の合計 ＋ バルコニー加算" : "F の合計",
                    row.legalKnown ? sqm(row.legalFloor) : "未評価"});
    if (!page.view.items.empty()) result.push_back(std::move(page));
  }
  if (!geometry.building.IsNull()) {
    auto page = pageFor(document, "建築面積求積図");
    auto& table = page.tables.front();
    if (!geometry.storeys.IsNull()) {
      areaParts(page.view, table, geometry.storeys, "B", "各階の重なりを整理", true, scale);
      const auto addition = difference(geometry.building, geometry.storeys);
      if (!addition.IsNull()) areaParts(page.view, table, addition, "E", "軒等の追加範囲", true, scale, false);
      else if (std::abs(areas.buildingArea - geom::volume(geometry.storeys)) > 1.0) {
        page = pageFor(document, "建築面積求積図");
        areaParts(page.view, page.tables.front(), geometry.building, "B", "算入範囲", true, scale);
      }
    } else areaParts(page.view, table, geometry.building, "B", "算入範囲", true, scale);
    auto& finalTable = page.tables.front();
    finalTable.notes.push_back("塗り：各階の算入範囲。E：軒・庇等の追加範囲。重複は一度だけ算入。適用条件は面積表を参照。");
    if (!areas.buildingKnown) finalTable.notes.push_back("計算できた範囲を表示。建築面積の合計は未評価です。");
    paginate(page, {"", "建築面積 合計", "B・E の合計", areas.buildingKnown ? sqm(areas.buildingArea) : "未評価"});
    if (!page.view.items.empty()) result.push_back(std::move(page));
  }
  return result;
}
}  // namespace sbr::doc::detail
