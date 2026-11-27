// Laying a view out on paper (docs/50): annotations sized on paper, the
// paper chosen, the view centred in the frame, the title block filled in.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

#include "sbr/draw/Drawing.h"
#include "sbr/draw/SheetCheck.h"

namespace sbr::draw {
namespace {

struct Paper {
  const char* name;
  double w, h;  // landscape, mm
};
constexpr Paper kPapers[] = {{"A4", 297, 210}, {"A3", 420, 297}, {"A2", 594, 420}, {"A1", 841, 594}};

// Paper-mm constants of the layout.
constexpr double kLeftMargin = 20.0;  // binding side
constexpr double kMargin = 10.0;
constexpr double kTitleW = 130.0, kTitleH = 32.0;
constexpr double kPad = 8.0;  // around the drawing inside the frame

struct Box {
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  bool empty() const { return x0 > x1; }
  void add(double x, double y) {
    x0 = std::min(x0, x); y0 = std::min(y0, y);
    x1 = std::max(x1, x); y1 = std::max(y1, y);
  }
  void add(const gp_Pnt2d& p) { add(p.X(), p.Y()); }
};

Item line(Pen pen, const gp_Pnt2d& a, const gp_Pnt2d& b) {
  Item i;
  i.kind = Item::Kind::Line;
  i.pen = pen;
  i.points = {a, b};
  return i;
}
Item circle(Pen pen, const gp_Pnt2d& c, double r) {
  Item i;
  i.kind = Item::Kind::Arc;
  i.pen = pen;
  i.centre = c;
  i.radius = r;
  i.start = 0;
  i.sweep = 2 * M_PI;
  return i;
}
Item text(Pen pen, const gp_Pnt2d& at, const std::string& s, double height, int h = 1, int v = 1, double angle = 0) {
  Item i;
  i.kind = Item::Kind::Text;
  i.pen = pen;
  i.points = {at};
  i.text = s;
  i.height = height;
  i.hAlign = h;
  i.vAlign = v;
  i.angle = angle;
  return i;
}

std::string mm(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.0f", std::abs(v) < 0.5 ? 0.0 : v);
  return b;
}

// docs/296: dimension values as Japanese drawings write them: 7,280.
std::string mmGrouped(double v) {
  std::string digits = mm(std::abs(v));
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<std::size_t>(i), ",");
  return (v < -0.5 ? "-" : "") + digits;
}

void appendNorthMark(std::vector<Item>& items, const gp_Pnt2d& centre, double scale,
                     const std::optional<gp_Dir2d>& trueNorth) {
  const gp_Dir2d north = trueNorth.value_or(gp_Dir2d(0, 1));
  const gp_Vec2d forward(north), right(north.Y(), -north.X());
  auto at = [&](double side, double along) {
    return centre.Translated((right * side + forward * along) * scale);
  };
  items.push_back(circle(Pen::Text, centre, 6 * scale));
  items.push_back(line(Pen::Text, at(0, -6), at(0, 6)));
  Item arrow;
  arrow.kind = Item::Kind::Fill;
  arrow.pen = Pen::Text;
  arrow.points = {at(0, 7), at(-2.5, 1), at(0, 2.5), at(2.5, 1)};
  items.push_back(std::move(arrow));
  items.push_back(text(Pen::Text, at(0, 10.5), "N", 3.5));
  if (!trueNorth) items.push_back(text(Pen::Text, at(0, -9), "仮", 2.5));
}

// Characters, for the width text takes (UTF-8: count lead bytes; wide ones twice).
double textWidth(const std::string& s, double height) {
  double n = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c & 0xC0) == 0x80) continue;
    n += c >= 0xE0 ? 1.0 : 0.6;
  }
  return n * height;
}

void extentOf(const Item& i, double scale, Box& box) {
  switch (i.kind) {
    case Item::Kind::Arc:
      box.add(i.centre.X() - i.radius, i.centre.Y() - i.radius);
      box.add(i.centre.X() + i.radius, i.centre.Y() + i.radius);
      break;
    case Item::Kind::Text: {
      const double w = textWidth(i.text, i.height) * scale, h = i.height * scale;
      const double x = i.points[0].X() - w * i.hAlign / 2.0, y = i.points[0].Y() - h * i.vAlign / 2.0;
      box.add(x, y);
      box.add(x + w, y + h);
      break;
    }
    default:
      for (const gp_Pnt2d& p : i.points) box.add(p);
  }
}

void translate(Item& i, double dx, double dy) {
  for (gp_Pnt2d& p : i.points) p.SetCoord(p.X() + dx, p.Y() + dy);
  for (auto& hole : i.holes) {
    for (gp_Pnt2d& p : hole) p.SetCoord(p.X() + dx, p.Y() + dy);
  }
  i.centre.SetCoord(i.centre.X() + dx, i.centre.Y() + dy);
}

// A dimension: extension lines, the dimension line with ticks, the value.
void dimension(std::vector<Item>& out, const gp_Pnt2d& a, const gp_Pnt2d& b, const gp_Vec2d& offset,
               const std::string& value, double s) {
  const gp_Pnt2d da(a.XY() + offset.XY()), db(b.XY() + offset.XY());
  if (da.Distance(db) < 1e-9) return;
  if (offset.Magnitude() > 1e-9) {
    const gp_Vec2d o = offset.Normalized();
    out.push_back(line(Pen::Dimension, gp_Pnt2d(a.XY() + o.XY() * (1.0 * s)), gp_Pnt2d(da.XY() + o.XY() * (1.5 * s))));
    out.push_back(line(Pen::Dimension, gp_Pnt2d(b.XY() + o.XY() * (1.0 * s)), gp_Pnt2d(db.XY() + o.XY() * (1.5 * s))));
  }
  out.push_back(line(Pen::Dimension, da, db));
  // Ticks: short 45 degree strokes (architectural dimensions).
  const gp_Vec2d d = gp_Vec2d(da, db).Normalized();
  const gp_Vec2d tick = (d + gp_Vec2d(-d.Y(), d.X())).Normalized() * (1.2 * s);
  for (const gp_Pnt2d& p : {da, db}) {
    out.push_back(line(Pen::Dimension, gp_Pnt2d(p.XY() - tick.XY()), gp_Pnt2d(p.XY() + tick.XY())));
  }
  double angle = std::atan2(d.Y(), d.X());
  if (angle > M_PI / 2 + 1e-9) angle -= M_PI;
  if (angle < -M_PI / 2 + 1e-9) angle += M_PI;
  const gp_Vec2d up(-std::sin(angle), std::cos(angle));
  const gp_Pnt2d mid((da.XY() + db.XY()) / 2 + up.XY() * (1.0 * s));
  out.push_back(text(Pen::Dimension, mid, value, 2.5, 1, 0, angle));
}

constexpr double kLabelGap = 0.8;  // paper mm

template <typename IsClear>
bool findNearbyPosition(Item& label, double scale, int maxSteps, IsClear isClear) {
  const gp_Pnt2d original = label.points.front();
  constexpr double step = 3.5;  // paper mm
  // Try positions along the axes, diagonals and the spaces between them.
  for (int ring = 1; ring <= maxSteps; ++ring) {
    for (int x = 0; x <= ring; ++x) {
      const int y = ring - x;
      const std::array<gp_Vec2d, 4> offsets{
          gp_Vec2d(x, y), gp_Vec2d(x, -y), gp_Vec2d(-x, y), gp_Vec2d(-x, -y)};
      for (const gp_Vec2d& offset : offsets) {
        label.points.front() = original.Translated(offset.Multiplied(step * scale));
        if (isClear(label)) return true;
      }
    }
  }
  label.points.front() = original;
  return false;
}

void placeRoomTags(std::vector<Item>& items, const std::vector<View::RoomTag>& tags, double scale) {
  for (const auto& tag : tags) {
    Item& slot = items[tag.item];
    Item label = std::exchange(slot, Item{});
    if (!textIsClear(label, items, scale, kLabelGap)) {
      findNearbyPosition(label, scale, 6, [&](const Item& candidate) {
        return textFitsInside(candidate, tag.outline, scale, kLabelGap) &&
               textIsClear(candidate, items, scale, kLabelGap);
      });
    }
    slot = std::move(label);
  }
}

void placeBracingMarks(std::vector<Item>& items, const std::vector<View::BracingMark>& marks, double scale) {
  if (marks.empty()) return;
  Box occupied;
  for (const Item& item : items) extentOf(item, scale, occupied);
  std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> leaders;
  for (const View::BracingMark& mark : marks) {
    Item label = text(Pen::Visible, mark.at, mark.text, mark.height, 1, 1);
    const auto isClear = [&](const Item& candidate) { return textIsClear(candidate, items, scale, kLabelGap); };
    const bool clear = isClear(label) || findNearbyPosition(label, scale, 12, isClear);
    // When the plan is dense, reserve another line above the drawing. The paper-fit calculation
    // below includes it, just like the other annotations.
    if (!clear) label.points.front().SetY(occupied.y1 + (mark.height / 2 + 2 * kLabelGap) * scale);
    if (!mark.at.IsEqual(label.points.front(), 1e-9)) leaders.emplace_back(mark.at, label.points.front());
    extentOf(label, scale, occupied);
    items.push_back(std::move(label));
  }
  std::vector<const Item*> words;
  for (const Item& item : items)
    if (item.kind == Item::Kind::Text) words.push_back(&item);
  std::vector<Item> leaderItems;
  for (const auto& [a, b] : leaders)
    for (const auto& [p, q] : clearOfText(a, b, words, scale, kLabelGap))
      leaderItems.push_back(line(Pen::Text, p, q));
  items.insert(items.end(), leaderItems.begin(), leaderItems.end());
}

}  // namespace

namespace {
// The frame and the title block (bottom right, 4 rows), in sheet units (paper mm x s).
void frameAndTitle(std::vector<Item>& items, const Paper* paper, const SheetSpec& spec, const std::string& title, double s) {
  auto P = [&](double x, double y) { return gp_Pnt2d(x * s, y * s); };
    Item border;
    border.kind = Item::Kind::Polyline;
    border.pen = Pen::Frame;
    border.closed = true;
    border.points = {P(kLeftMargin, kMargin), P(paper->w - kMargin, kMargin), P(paper->w - kMargin, paper->h - kMargin),
                     P(kLeftMargin, paper->h - kMargin)};
    items.push_back(border);
    // Title block, bottom right: 4 rows.
    const double tx1 = paper->w - kMargin, tx0 = tx1 - kTitleW, ty0 = kMargin, ty1 = kMargin + kTitleH;
    const double row = kTitleH / 4, label = 20.0, half = (kTitleW - 2 * label) / 2;
    Item boxLine;
    boxLine.kind = Item::Kind::Polyline;
    boxLine.pen = Pen::TitleBlock;
    boxLine.closed = true;
    boxLine.points = {P(tx0, ty0), P(tx1, ty0), P(tx1, ty1), P(tx0, ty1)};
    items.push_back(boxLine);
    for (int r = 1; r < 4; ++r) items.push_back(line(Pen::TitleBlock, P(tx0, ty0 + r * row), P(tx1, ty0 + r * row)));
    items.push_back(line(Pen::TitleBlock, P(tx0 + label, ty0), P(tx0 + label, ty1)));
    const double xMidLabel = tx0 + label + half;
    items.push_back(line(Pen::TitleBlock, P(xMidLabel, ty0), P(xMidLabel, ty0 + 2 * row)));
    items.push_back(line(Pen::TitleBlock, P(xMidLabel + label, ty0), P(xMidLabel + label, ty0 + 2 * row)));
    char scaleRow[48];
    std::snprintf(scaleRow, sizeof scaleRow, "1:%g (%s)", s, paper->name);
    struct Cell {
      double x, y;
      const char* label;
      std::string value;
    };
    const Cell cells[] = {
        {tx0, ty0 + 3 * row, "工事名", spec.title.project}, {tx0, ty0 + 2 * row, "図面名", title},
        {tx0, ty0 + 1 * row, "縮尺", scaleRow},             {xMidLabel, ty0 + 1 * row, "日付", spec.title.date},
        {tx0, ty0, "図番", spec.title.number},              {xMidLabel, ty0, "作成者", spec.title.author},
    };
    for (const Cell& c : cells) {
      items.push_back(text(Pen::TitleBlock, P(c.x + label / 2, c.y + row / 2), c.label, 2.5, 1, 1));
      if (!c.value.empty()) items.push_back(text(Pen::TitleBlock, P(c.x + label + 2, c.y + row / 2), c.value, 3.5, 0, 1));
    }
  }

// docs/303: the legend (凡例) of the lines whose meaning is not written beside them, from the pens the
// sheet uses: a short sample in the pen itself (so it follows the pen table) and its name, in the
// strip left of the title block (it takes no room from the drawing).
void appendLegend(std::vector<Item>& items, const Paper* paper, double s, const std::set<int>& hatchPatterns = {},
                  const std::set<std::string>& symbols = {}) {
  auto P = [&](double x, double y) { return gp_Pnt2d(x * s, y * s); };
  static constexpr std::pair<Pen, const char*> kLegend[] = {
      {Pen::Hidden, "隠れ線"},          {Pen::Grid, "通り芯・中心線"},   {Pen::Boundary, "敷地境界線"},
      {Pen::Ground, "地盤線（GL）"},    {Pen::HeightLimit, "高さ制限"}, {Pen::HeightReview, "高さ制限（未確認）"},
      {Pen::FireLimit3, "延焼のおそれのある部分（1階 3m）"}, {Pen::FireLimit5, "延焼のおそれのある部分（2階以上 5m）"},
      {Pen::FireCompartment, "防火区画"}};
  bool used[kPenCount] = {};
  for (const Item& i : items)
    if (i.kind == Item::Kind::Line || i.kind == Item::Kind::Polyline || i.kind == Item::Kind::Arc) used[static_cast<int>(i.pen)] = true;
  std::vector<std::pair<Pen, const char*>> rows;
  for (const auto& row : kLegend)
    if (used[static_cast<int>(row.first)]) rows.push_back(row);
  // docs/326: the cut layers' hatching drawn on the sheet: a small cut box hatched the same way, after the lines.
  std::vector<std::pair<int, const char*>> hatches;
  if (hatchPatterns.count(2)) hatches.push_back({2, "構造用の木材の切り口（柱・間柱・土台など）"});
  if (hatchPatterns.count(1)) hatches.push_back({1, "合板・野地板の切り口"});
  if (symbols.count("through_column")) hatches.push_back({3, "通し柱"});  // docs/327: a ringed column
  // docs/333: the bracing plan's lines and columns, in the manual's legend words.
  if (symbols.count("wall_general")) hatches.push_back({6, "一般壁"});
  if (symbols.count("wall_opening")) hatches.push_back({7, "開口部"});
  if (symbols.count("bearing_wall")) hatches.push_back({8, "耐力壁"});
  if (symbols.count("column_box")) hatches.push_back({9, "柱"});
  if (symbols.count("through_column_box")) hatches.push_back({10, "通し柱"});
  // docs/331: the MLIT manual's legend words for the brace marks.
  if (symbols.count("brace_double")) hatches.push_back({5, "筋かいダブル"});
  if (symbols.count("brace")) hatches.push_back({4, "筋かいシングル"});
  if (!rows.empty() || !hatches.empty()) {
    constexpr double rowH = 5.0, sample = 14.0, colW = 75.0;
    const int perColumn = static_cast<int>((kTitleH - rowH) / rowH);
    const double x0 = kLeftMargin + 4, top = kMargin + kTitleH - 2;
    const double room = paper->w - kMargin - kTitleW - 4 - x0;
    items.push_back(text(Pen::Text, P(x0, top - rowH / 2), "凡例", 3.0, 0, 1));
    for (std::size_t k = 0; k < rows.size(); ++k) {
      const int col = static_cast<int>(k) / perColumn, r = static_cast<int>(k) % perColumn;
      const double x = x0 + col * colW, y = top - (r + 1.5) * rowH;
      if (x + sample + 4 > x0 + room) break;  // no more room beside the title block
      items.push_back(line(rows[k].first, P(x, y), P(x + sample, y)));
      items.push_back(text(Pen::Text, P(x + sample + 2, y), rows[k].second, 2.5, 0, 1));
    }
    for (std::size_t h = 0; h < hatches.size(); ++h) {
      const std::size_t k = rows.size() + h;
      const int col = static_cast<int>(k) / perColumn, r = static_cast<int>(k) % perColumn;
      const double x = x0 + col * colW, y = top - (r + 1.5) * rowH;
      if (x + sample + 4 > x0 + room) break;
      const double y0 = y - 1.4, y1 = y + 1.4;
      if (hatches[h].first >= 6 && hatches[h].first <= 10) {  // docs/333: as the bracing plan draws them
        const int code = hatches[h].first;
        const double cx = x + sample / 2;
        if (code == 6) items.push_back(line(Pen::Visible, P(x + 2, y), P(x + sample - 2, y)));
        if (code == 7) {
          items.push_back(line(Pen::Visible, P(x + 2, y - 0.5), P(x + sample - 2, y - 0.5)));
          items.push_back(line(Pen::Visible, P(x + 2, y + 0.5), P(x + sample - 2, y + 0.5)));
        }
        if (code == 8) items.push_back(line(Pen::Cut, P(x + 2, y), P(x + sample - 2, y)));
        if (code == 9 || code == 10) {
          Item square;
          square.kind = Item::Kind::Polyline;
          square.pen = Pen::Visible;
          square.closed = true;
          square.points = {P(cx - 1.2, y - 1.2), P(cx + 1.2, y - 1.2), P(cx + 1.2, y + 1.2), P(cx - 1.2, y + 1.2)};
          items.push_back(square);
          items.push_back(text(Pen::Text, P(cx, y), "n", 1.8, 1, 1));
          if (code == 10) items.push_back(circle(Pen::Visible, P(cx, y), 1.2 * std::sqrt(2.0) * 1.1 * s));
        }
        items.push_back(text(Pen::Text, P(x + sample + 2, y), hatches[h].second, 2.5, 0, 1));
        continue;
      }
      if (hatches[h].first == 4 || hatches[h].first == 5) {  // docs/331: the open triangles, as on the plans
        const double cx = x + sample / 2;
        Item edge;
        edge.kind = Item::Kind::Polyline;
        edge.pen = Pen::Visible;
        edge.closed = true;
        if (hatches[h].first == 5) {
          edge.points = {P(cx - 2.0, y - 0.7), P(cx + 2.0, y - 0.7), P(cx, y + 0.7)};
        } else {
          // The head (柱頭) at the upright side on the left, as the manual's legend draws it, named at each end.
          edge.points = {P(cx - 2.0, y + 0.7), P(cx - 2.0, y - 0.7), P(cx + 2.0, y - 0.7)};
          items.push_back(text(Pen::Text, P(cx - 2.6, y), "柱頭", 1.8, 2, 1));
          items.push_back(text(Pen::Text, P(cx + 2.6, y), "柱脚", 1.8, 0, 1));
        }
        items.push_back(edge);
        items.push_back(text(Pen::Text, P(x + sample + 2, y), hatches[h].second, 2.5, 0, 1));
        continue;
      }
      if (hatches[h].first == 3) {  // docs/327: the column's cut (a small square) in its ring
        Item square;
        square.kind = Item::Kind::Polyline;
        square.pen = Pen::Cut;
        square.closed = true;
        const double cx = x + sample / 2, half = 0.9;
        square.points = {P(cx - half, y - half), P(cx + half, y - half), P(cx + half, y + half), P(cx - half, y + half)};
        items.push_back(square);
        // docs/328: the column's cut crossed (structural timber) and the circle just round it.
        items.push_back(line(Pen::Visible, P(cx - half, y - half), P(cx + half, y + half)));
        items.push_back(line(Pen::Visible, P(cx - half, y + half), P(cx + half, y - half)));
        items.push_back(circle(Pen::Visible, P(cx, y), half * std::sqrt(2.0) * 1.08 * s));
        items.push_back(text(Pen::Text, P(x + sample + 2, y), hatches[h].second, 2.5, 0, 1));
        continue;
      }
      Item box;
      box.kind = Item::Kind::Polyline;
      box.pen = Pen::Cut;
      box.closed = true;
      box.points = {P(x, y0), P(x + sample, y0), P(x + sample, y1), P(x, y1)};
      items.push_back(box);
      // 45-degree strokes 1.5 mm apart (as the cut layers on the sheet), both ways for timber.
      for (double u = x - 3.0; u < x + sample; u += 1.5) {
        const auto clipped = [&](double ua, double va, double ub, double vb) {
          // Clip the segment to the box (x..x+sample, y0..y1) along its own direction.
          double t0 = 0, t1 = 1;
          const double du = ub - ua, dv = vb - va;
          for (const auto& [p, q] : {std::pair<double, double>{-du, ua - x}, {du, x + sample - ua}, {-dv, va - y0}, {dv, y1 - va}}) {
            if (std::abs(p) < 1e-12) {
              if (q < 0) return;
              continue;
            }
            const double t = q / p;
            if (p < 0) t0 = std::max(t0, t);
            else t1 = std::min(t1, t);
          }
          if (t1 > t0) items.push_back(line(Pen::Visible, P(ua + du * t0, va + dv * t0), P(ua + du * t1, va + dv * t1)));
        };
        clipped(u, y0, u + (y1 - y0), y1);
        if (hatches[h].first == 2) clipped(u, y1, u + (y1 - y0), y0);
      }
      items.push_back(text(Pen::Text, P(x + sample + 2, y), hatches[h].second, 2.5, 0, 1));
    }
  }
}

}  // namespace

static Result<Sheet> layoutViewOnly(const View& view, const SheetSpec& spec) {
  using R = Result<Sheet>;
  const double s = spec.scale;
  if (!(s > 0) || !std::isfinite(s)) return R::failure("The scale must be positive");
  std::vector<Item> items = view.items;
  Box model;
  for (const Item& i : view.items) extentOf(i, s, model);
  for (const GridMark& g : view.grids) model.add(g.point);
  for (const LevelMark& l : view.levels) model.add((model.empty() ? 0.0 : model.x0), l.v);
  if (model.empty()) return R::failure("Nothing to lay out");

  // Grids: lines over the model with a margin, bubbles beyond one end.
  // docs/296: bubbles of 6 mm across, drawn thin (JIS A 0150 10.2: a circle in thin line at the end of a
  // reference line); they were 10 mm, larger than the drawings submitted give them.
  const double bubble = 3.0 * s, margin = 6.0 * s;
  std::map<int, std::vector<std::pair<double, std::string>>> spans;  // family -> (coordinate, name)
  double gridLow = model.y0, gridLeft = model.x0;
  for (const GridMark& g : view.grids) {
    const gp_Vec2d d(g.direction.X(), g.direction.Y());
    double t0 = 1e300, t1 = -1e300;
    for (const gp_Pnt2d& c : {gp_Pnt2d(model.x0, model.y0), gp_Pnt2d(model.x1, model.y0), gp_Pnt2d(model.x0, model.y1),
                              gp_Pnt2d(model.x1, model.y1)}) {
      const double t = gp_Vec2d(g.point, c).Dot(d);
      t0 = std::min(t0, t);
      t1 = std::max(t1, t);
    }
    gp_Pnt2d e0(g.point.XY() + d.XY() * (t0 - margin)), e1(g.point.XY() + d.XY() * (t1 + margin));
    // The bubble at the bottom (X axes, and every axis in sections) or the left (Y axes).
    const bool byLeft = g.inPlan && g.family == 1;
    const bool swap = byLeft ? e1.X() < e0.X() : e1.Y() < e0.Y();
    if (swap) std::swap(e0, e1);
    items.push_back(line(Pen::Grid, e0, e1));
    const gp_Vec2d away(e1, e0);
    const gp_Pnt2d c(e0.XY() + away.Normalized().XY() * bubble);
    items.push_back(circle(Pen::GridLabel, c, bubble));
    items.push_back(text(Pen::GridLabel, c, g.name, 2.5, 1, 1));
    const double coord = byLeft ? g.point.Y() : g.point.X();
    spans[byLeft ? 1 : 0].push_back({coord, g.name});
    if (byLeft) gridLeft = std::min(gridLeft, e0.X());
    else gridLow = std::min(gridLow, e0.Y());
  }
  for (auto& [family, axes] : spans) {
    std::sort(axes.begin(), axes.end());
    // The spans between the drawing and the bubbles; the whole length on the other side, as the submitted
    // plans give both (docs/296). Neither lengthens the grid lines (the paper size stays).
    const double near = family == 0 ? (model.y0 + gridLow) / 2 : (model.x0 + gridLeft) / 2;
    const double far = family == 0 ? model.y1 + 3.5 * s : model.x1 + 3.5 * s;
    const auto put = [&](double a, double b, double at) {
      if (family == 0) dimension(items, gp_Pnt2d(a, at), gp_Pnt2d(b, at), gp_Vec2d(0, 0), mmGrouped(b - a), s);
      else dimension(items, gp_Pnt2d(at, a), gp_Pnt2d(at, b), gp_Vec2d(0, 0), mmGrouped(b - a), s);
    };
    for (std::size_t k = 0; k + 1 < axes.size(); ++k) {
      const double a = axes[k].first, b = axes[k + 1].first;
      if (b - a < 1e-6) continue;
      put(a, b, near);
    }
    if (axes.size() >= 3 && axes.back().first - axes.front().first > 1e-6) put(axes.front().first, axes.back().first, far);
  }
  placeRoomTags(items, view.roomTags, s);
  // Section cuts (docs/132): a line across the drawing, at each end an arrow the way the section
  // looks, the label and (under it) the sheet it is on.
  for (const View::CutLine& c : view.cuts) {
    const gp_Vec2d d(c.along.X(), c.along.Y()), look(c.look.X(), c.look.Y());
    double t0 = 1e300, t1 = -1e300;
    for (const gp_Pnt2d& corner : {gp_Pnt2d(model.x0, model.y0), gp_Pnt2d(model.x1, model.y0), gp_Pnt2d(model.x0, model.y1),
                                   gp_Pnt2d(model.x1, model.y1)}) {
      const double t = gp_Vec2d(c.point, corner).Dot(d);
      t0 = std::min(t0, t);
      t1 = std::max(t1, t);
    }
    // docs/255: with grids, the cut's ends go beyond the grid bubbles (a cut on a grid line put its
    // "A" / "B" on the bubble of Y1 / X1: the hands-on run's sheet check).
    const double cutMargin = view.grids.empty() ? margin : margin + 2.0 * bubble + 3.0 * s;
    const gp_Pnt2d e0(c.point.XY() + d.XY() * (t0 - cutMargin)), e1(c.point.XY() + d.XY() * (t1 + cutMargin));
    // docs/250: the label below an end is written up from its baseline 4 mm beyond the end, so it
    // reaches 1 mm back over the end: there the line stops 1.5 mm short (the sheet check found the
    // line through the lower "B"). The labels stay where they were (the paper does not grow).
    const double shortBy = 1.5 * s * std::abs(d.Y());
    const gp_Pnt2d l0(d.Y() > 0 ? gp_Pnt2d(e0.XY() + d.XY() * shortBy) : e0);
    const gp_Pnt2d l1(d.Y() < 0 ? gp_Pnt2d(e1.XY() - d.XY() * shortBy) : e1);
    {
      // docs/250: the line stops short of the words it would run through (room tags on the cut).
      std::vector<const Item*> words;
      for (const Item& i : items)
        if (i.kind == Item::Kind::Text) words.push_back(&i);
      const auto pieces = clearOfText(l0, l1, words, s, 0.8);
      for (const auto& [p, q] : pieces) items.push_back(line(Pen::Grid, p, q));
    }
    for (const gp_Pnt2d& e : {e0, e1}) {
      // A thick stub at the end, then the arrow.
      const gp_Pnt2d stub = e.IsEqual(e0, 1e-9) ? l0 : l1;
      const gp_Pnt2d tip(stub.XY() + look.XY() * (6.0 * s));
      items.push_back(line(Pen::Cut, stub, tip));
      Item arrow;
      arrow.kind = Item::Kind::Fill;
      arrow.pen = Pen::Text;
      const gp_Vec2d side(look.Y(), -look.X());
      arrow.points = {gp_Pnt2d(tip.XY() + look.XY() * (2.5 * s)), gp_Pnt2d(tip.XY() + side.XY() * (1.5 * s)),
                      gp_Pnt2d(tip.XY() - side.XY() * (1.5 * s))};
      items.push_back(arrow);
      const bool first = e.IsEqual(e0, 1e-9);
      const gp_Pnt2d at(e.XY() + d.XY() * ((first ? -4.0 : 4.0) * s));
      items.push_back(text(Pen::Text, at, c.label, 5.0, 1, 0));
      if (!c.sheet.empty()) items.push_back(text(Pen::Text, gp_Pnt2d(at.X(), at.Y() - 1.0 * s), c.sheet, 2.5, 1, 2));
    }
  }
  // Levels: a line across with its name above, the height below; levels
  // at one height share the line ("GL・1FL").
  std::vector<LevelMark> levels;
  for (const LevelMark& l : view.levels) {
    auto same = std::find_if(levels.begin(), levels.end(), [&](const LevelMark& m) { return std::abs(m.v - l.v) < 0.5; });
    if (same != levels.end()) same->name += "・" + l.name;
    else levels.push_back(l);
  }
  // docs/295: levels closer than their labels' height (基礎天端・土台天端・1FL) step their labels to the
  // left, each past the one before, with the level line drawn out to it.
  std::sort(levels.begin(), levels.end(), [](const LevelMark& a, const LevelMark& b) { return a.v < b.v; });
  double previousV = -1e300, previousLeft = 0.0;
  for (const LevelMark& l : levels) {
    const double u0 = model.x0 - 3 * margin, u1 = model.x1 + margin;
    const std::string h = std::abs(l.elevation) < 0.5 ? "±0" : (l.elevation > 0 ? "+" : "") + mm(l.elevation);
    // docs/300: a level is marked with a triangle on its line (▽, the ground ▼) and its name beside it,
    // as Japanese elevations and sections write heights (▽2FL); the height under the name.
    const double mark = 2.5 * s;  // the triangle's width on paper
    const double width = mark + 0.8 * s + std::max(textWidth(l.name, 3.5), textWidth(h, 2.5)) * s;
    double at = u0;
    if (l.v - previousV < 7.5 * s) at = std::min(u0, previousLeft - width - 2.0 * s);
    items.push_back(line(Pen::Level, gp_Pnt2d(at, l.v), gp_Pnt2d(u1, l.v)));
    {
      Item triangle;
      triangle.pen = Pen::Level;
      triangle.points = {gp_Pnt2d(at, l.v + 0.9 * mark), gp_Pnt2d(at + mark, l.v + 0.9 * mark),
                         gp_Pnt2d(at + mark / 2, l.v)};
      const bool ground = l.name == "GL" || l.name.find("地盤面") != std::string::npos ||
                          (l.name.size() > 3 && l.name.compare(l.name.size() - 3, 3, " GL") == 0);
      triangle.kind = ground ? Item::Kind::Fill : Item::Kind::Polyline;
      triangle.closed = true;
      items.push_back(std::move(triangle));
    }
    const double textAt = at + mark + 0.8 * s;
    items.push_back(text(Pen::Level, gp_Pnt2d(textAt, l.v + 1.0 * s), l.name, 3.5, 0, 0));
    items.push_back(text(Pen::Level, gp_Pnt2d(textAt, l.v - 1.0 * s), h, 2.5, 0, 2 /*top*/));
    previousV = l.v;
    previousLeft = at;
  }
  for (const DimensionMark& d : view.dimensions) dimension(items, d.a, d.b, d.offset, d.text, s);

  // Keep labels legible in paper units. Try alternate leader positions
  // before accepting a crowded position; never move the model itself.
  std::vector<Box> labels;
  bool crowded = false;
  for (const auto& mark : view.members) {
    const double height = 2.5, width = textWidth(mark.text, height) * s;
    gp_Pnt2d at;
    Box box;
    bool found = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
      const int ring = attempt / 4;
      const double dy = (3 + ring * 4) * s;
      const bool right = attempt % 4 < 2, above = attempt % 2 == 0;
      at = gp_Pnt2d(mark.at.X() + (right ? 2 * s : -width - 2 * s),
                    mark.at.Y() + (above ? dy : -dy - height * s));
      box = {at.X() - s, at.Y() - s, at.X() + width + s, at.Y() + height * s + s};
      found = std::none_of(labels.begin(), labels.end(), [&](const Box& b) {
        return box.x0 < b.x1 && box.x1 > b.x0 && box.y0 < b.y1 && box.y1 > b.y0;
      });
      if (found) break;
    }
    crowded |= !found;
    labels.push_back(box);
    items.push_back(line(Pen::Text, mark.at, at));
    items.push_back(text(Pen::Text, at, mark.text, height, 0, 0));
  }
  // docs/333: lines stop at the view's blank boxes (as if the marks there were drawn on white).
  if (!view.blanks.empty()) {
    std::vector<Item> kept;
    kept.reserve(items.size());
    for (Item& item : items) {
      if (item.kind != Item::Kind::Line || item.points.size() != 2) {
        kept.push_back(std::move(item));
        continue;
      }
      const gp_Pnt2d a = item.points[0], b = item.points[1];
      std::vector<std::pair<double, double>> inside;  // parts of the line (0..1) in a box
      for (const auto& [lo, hi] : view.blanks) {
        double t0 = 0.0, t1 = 1.0;
        bool hit = true;
        const double p[4] = {-(b.X() - a.X()), b.X() - a.X(), -(b.Y() - a.Y()), b.Y() - a.Y()};
        const double q[4] = {a.X() - lo.X(), hi.X() - a.X(), a.Y() - lo.Y(), hi.Y() - a.Y()};
        for (int k = 0; k < 4 && hit; ++k) {
          if (std::abs(p[k]) < 1e-12) {
            hit = q[k] >= 0;
          } else {
            const double r = q[k] / p[k];
            if (p[k] < 0) t0 = std::max(t0, r);
            else t1 = std::min(t1, r);
            hit = t0 < t1;
          }
        }
        if (hit) inside.push_back({t0, t1});
      }
      if (inside.empty()) {
        kept.push_back(std::move(item));
        continue;
      }
      std::sort(inside.begin(), inside.end());
      double from = 0.0;
      const auto at = [&](double t) { return gp_Pnt2d(a.X() + (b.X() - a.X()) * t, a.Y() + (b.Y() - a.Y()) * t); };
      for (const auto& [i0, i1] : inside) {
        if (i0 > from + 1e-9) {
          Item piece = item;
          piece.points = {at(from), at(i0)};
          kept.push_back(std::move(piece));
        }
        from = std::max(from, i1);
      }
      if (from < 1.0 - 1e-9) {
        Item piece = item;
        piece.points = {at(from), at(1.0)};
        kept.push_back(std::move(piece));
      }
    }
    items = std::move(kept);
  }
  placeBracingMarks(items, view.bracingMarks, s);
  Box all;
  for (const Item& i : items) extentOf(i, s, all);
  if (spec.compactDiagram && spec.north && (view.spec.kind == ViewKind::Plan || view.spec.kind == ViewKind::SitePlan)) {
    appendNorthMark(items, gp_Pnt2d(all.x1 + 10 * s, all.y1 - 8 * s), s, view.trueNorth);
    // A scale bar remains correct when the diagram is resized in another document.
    const double metres = 0.025 * s;
    const double decade = std::pow(10.0, std::floor(std::log10(metres)));
    const double normalized = metres / decade;
    const double nice = decade * (normalized >= 5.0 ? 5.0 : normalized >= 2.0 ? 2.0 : 1.0);
    const double x = all.x0, y = all.y0 - 6 * s, end = x + nice * 1000.0;
    items.push_back(line(Pen::Visible, gp_Pnt2d(x, y), gp_Pnt2d(end, y)));
    for (double at : {x, (x + end) / 2.0, end})
      items.push_back(line(Pen::Visible, gp_Pnt2d(at, y - s), gp_Pnt2d(at, y + s)));
    char metresText[32];
    std::snprintf(metresText, sizeof metresText, "%g m", nice);
    items.push_back(text(Pen::Text, gp_Pnt2d(end + 2 * s, y), metresText, 2.7, 0, 1));
    for (const Item& i : items) extentOf(i, s, all);
  }
  // The drawing's caption below it.
  const std::string title = view.spec.title.empty() ? spec.title.drawing : view.spec.title;
  char scaleText[32];
  std::snprintf(scaleText, sizeof scaleText, "S=1:%g", s);
  const double captionGap = 9.0 * s;
  const std::string caption = spec.compactDiagram ? title
      : title.empty() ? std::string(scaleText) : title + "  " + scaleText;
  const double captionW = textWidth(caption, 5.0) * s;
  const double width = std::max(all.x1 - all.x0, captionW);
  const double height = all.y1 - all.y0 + captionGap;

  // The paper: the smallest that holds it (paper mm = real / scale).
  const bool frame = spec.frame;
  std::vector<std::string> calculationLines;
  for (const auto& note : view.calculationNotes) {
    std::string line;
    int count = 0;
    for (unsigned char c : note) {
      if ((c & 0xC0) != 0x80 && count++ == 90) {
        calculationLines.push_back(line);
        line.clear();
        count = 1;
      }
      line += static_cast<char>(c);
    }
    if (!line.empty()) calculationLines.push_back(line);
  }
  auto areaOf = [&](const Paper& p, double& x0, double& y0, double& x1, double& y1) {
    x0 = frame ? kLeftMargin : kMargin;
    x1 = p.w - kMargin;
    y0 = frame ? kMargin + kTitleH + 4 : kMargin;
    y1 = p.h - kMargin - calculationLines.size() * 4.0;
  };
  const Paper* paper = nullptr;
  Sheet sheet;
  if (crowded) sheet.notes.push_back("部材符号が混み合っています。縮尺・タグ・符号の表示を調整してください");
  sheet.scale = s;
  for (const Paper& p : kPapers) {
    if (spec.paper != "auto" && spec.paper != p.name) continue;
    double x0, y0, x1, y1;
    areaOf(p, x0, y0, x1, y1);
    const bool fits = width / s <= x1 - x0 - 2 * kPad && height / s <= y1 - y0 - 2 * kPad;
    if (spec.paper != "auto" || fits) {
      paper = &p;
      if (!fits) sheet.notes.push_back(std::string("The drawing does not fit on ") + p.name + " at this scale");
      break;
    }
  }
  if (!paper) {
    if (spec.paper != "auto") return R::failure("Unknown paper '" + spec.paper + "' (A4, A3, A2, A1 or auto)");
    paper = &kPapers[3];
    sheet.notes.push_back("The drawing does not fit on A1 at this scale: choose a smaller scale");
  }
  sheet.paper = paper->name;
  sheet.paperWidth = paper->w;
  sheet.paperHeight = paper->h;
  sheet.title = title;
  double ax0, ay0, ax1, ay1;
  areaOf(*paper, ax0, ay0, ax1, ay1);
  // Centre the drawing (with its caption) in the area.
  const double cx = (ax0 + ax1) / 2 * s, cy = (ay0 + ay1) / 2 * s;
  const double dx = cx - (all.x0 + all.x1) / 2;
  const double dy = cy - (all.y0 - captionGap + all.y1) / 2;
  for (Item& i : items) translate(i, dx, dy);
  (void)width;
  items.push_back(text(Pen::Text, gp_Pnt2d((all.x0 + all.x1) / 2 + dx, all.y0 + dy - captionGap / 2), caption, 5.0, 1, 1));

  auto P = [&](double x, double y) { return gp_Pnt2d(x * s, y * s); };  // paper mm -> sheet
  for (std::size_t i = 0; i < calculationLines.size(); ++i)
    items.push_back(text(Pen::Text,
                         spec.compactDiagram
                             ? gp_Pnt2d(std::min(kLeftMargin * s + 6 * s, all.x0 + dx), all.y1 + dy + (calculationLines.size() - i + 1) * 4.0 * s)
                             : P(kLeftMargin + 2, paper->h - kMargin - 3 - i * 4.0),
                         calculationLines[i], spec.compactDiagram ? 2.8 : 2.2, 0, 0));
  if (!spec.compactDiagram && spec.north && (view.spec.kind == ViewKind::Plan || view.spec.kind == ViewKind::SitePlan)) {
    appendNorthMark(items, P(ax1 - 15, ay1 - 16), s, view.trueNorth);
  }
  sheet.hatchPatterns = view.hatchPatterns;  // docs/326
  sheet.symbols = view.symbols;              // docs/327
  if (frame && !spec.compactDiagram && view.spec.framingPreset.empty())
    appendLegend(items, paper, s, view.hatchPatterns, view.symbols);
  if (frame) frameAndTitle(items, paper, spec, title, s);
  if (!view.spec.framingPreset.empty()) {
    items.push_back(
        text(Pen::Text, P(kLeftMargin + 2, kMargin + 4), "概略検討用／断面・接合未照査", 2.5, 0, 0));
    if (view.spec.framingPreset == "foundation")
      items.push_back(
          text(Pen::Text, P(kLeftMargin + 2, kMargin + 9), "基礎形状の有無・寸法は別途確認", 2.5, 0, 0));
  }
  sheet.items = std::move(items);
  return sheet;
}

}  // namespace sbr::draw

namespace sbr::draw {

namespace {
constexpr double kTextH = 3.0, kRowH = 7.0, kCellPad = 2.0, kTableTitleH = 10.0, kNoteH = 5.0;

// A table's column widths (paper mm) and the room it takes (with its title and notes).
struct TableSize {
  std::vector<double> widths;
  double tableW = 0, w = 0, h = 0;
};
TableSize tableSize(const Table& table) {
  TableSize out;
  out.widths.assign(table.headers.size(), 0.0);
  auto widen = [&](const std::vector<std::string>& cells) {
    for (std::size_t c = 0; c < out.widths.size() && c < cells.size(); ++c)
      out.widths[c] = std::max(out.widths[c], textWidth(cells[c], kTextH) + 2 * kCellPad);
  };
  widen(table.headers);
  for (const auto& row : table.rows) widen(row);
  for (double w : out.widths) out.tableW += w;
  double noteW = 0;
  for (const std::string& n : table.notes) noteW = std::max(noteW, textWidth(n, 2.5));
  out.w = std::max(out.tableW, noteW);
  out.h = kTableTitleH + kRowH * (table.rows.size() + 1) + kNoteH * table.notes.size();
  return out;
}

// The table drawn with its top left corner at (left, top) on paper (mm), into sheet units.
void drawTable(const Table& table, const TableSize& size, double left, double top, double s, std::vector<Item>& items) {
  auto P = [&](double x, double y) { return gp_Pnt2d(x * s, y * s); };
  items.push_back(text(Pen::Text, P(left, top - kTableTitleH / 2), table.title, 5.0, 0, 1));
  const double gridTop = top - kTableTitleH;
  const std::size_t lines = table.rows.size() + 1;
  // Rules: across under the title, under the headings (heavier), under each row; and the columns.
  for (std::size_t r = 0; r <= lines; ++r) {
    const double y = gridTop - kRowH * r;
    items.push_back(line(r == 1 ? Pen::Cut : Pen::Visible, P(left, y), P(left + size.tableW, y)));
  }
  double x = left;
  for (std::size_t c = 0; c <= size.widths.size(); ++c) {
    items.push_back(line(Pen::Visible, P(x, gridTop), P(x, gridTop - kRowH * lines)));
    if (c < size.widths.size()) x += size.widths[c];
  }
  auto row = [&](std::size_t r, const std::vector<std::string>& cells) {
    double cx = left;
    for (std::size_t c = 0; c < size.widths.size(); ++c) {
      if (c < cells.size() && !cells[c].empty())
        items.push_back(text(Pen::Text, P(cx + kCellPad, gridTop - kRowH * r - kRowH / 2), cells[c], kTextH, 0, 1));
      cx += size.widths[c];
    }
  };
  row(0, table.headers);
  for (std::size_t r = 0; r < table.rows.size(); ++r) row(r + 1, table.rows[r]);
  for (std::size_t n = 0; n < table.notes.size(); ++n)
    items.push_back(text(Pen::Text, P(left, gridTop - kRowH * lines - kNoteH * (n + 0.8)), table.notes[n], 2.5, 0, 1));
}
}  // namespace

Result<Sheet> tableSheet(const Table& table, const SheetSpec& spec) {
  using R = Result<Sheet>;
  const double s = spec.scale;
  if (!(s > 0) || !std::isfinite(s)) return R::failure("The scale must be positive");
  if (table.headers.empty()) return R::failure("The table has no columns");
  const TableSize size = tableSize(table);
  const double needW = size.w, needH = size.h;
  // The paper: the smallest that holds it inside the frame, above the title block.
  Sheet sheet;
  sheet.scale = s;
  const Paper* paper = nullptr;
  auto area = [&](const Paper& p, double& x0, double& y0, double& x1, double& y1) {
    x0 = spec.frame ? kLeftMargin : kMargin;
    x1 = p.w - kMargin;
    y0 = spec.frame ? kMargin + kTitleH + 4 : kMargin;
    y1 = p.h - kMargin;
  };
  for (const Paper& p : kPapers) {
    if (spec.paper != "auto" && spec.paper != p.name) continue;
    double x0, y0, x1, y1;
    area(p, x0, y0, x1, y1);
    const bool fits = needW <= x1 - x0 - 2 * kPad && needH <= y1 - y0 - 2 * kPad;
    if (spec.paper != "auto" || fits) {
      paper = &p;
      if (!fits) sheet.notes.push_back(std::string("The table does not fit on ") + p.name + ": it runs off the frame");
      break;
    }
  }
  if (!paper) {
    if (spec.paper != "auto") return R::failure("Unknown paper '" + spec.paper + "' (A4, A3, A2, A1 or auto)");
    paper = &kPapers[3];
    sheet.notes.push_back("The table does not fit on A1: it runs off the frame");
  }
  sheet.paper = paper->name;
  sheet.paperWidth = paper->w;
  sheet.paperHeight = paper->h;
  sheet.title = table.title;
  double ax0, ay0, ax1, ay1;
  area(*paper, ax0, ay0, ax1, ay1);
  std::vector<Item> items;
  drawTable(table, size, ax0 + kPad, ay1 - kPad, s, items);
  if (spec.frame) frameAndTitle(items, paper, spec, table.title, s);
  sheet.items = std::move(items);
  return sheet;
}

Result<Sheet> layoutSheet(const View& view, const SheetSpec& spec) {
  return layoutSheetWithTables(view, {}, spec);
}

Result<Sheet> combineSheets(const std::vector<Sheet>& drawings, const SheetSpec& spec, const std::optional<gp_Dir2d>& trueNorth) {
  using R = Result<Sheet>;
  const double s = spec.scale;
  if (!(s > 0) || !std::isfinite(s)) return R::failure("The scale must be positive");
  if (drawings.empty()) return R::failure("No drawing to put on the sheet");
  // Each drawing's extent (paper mm).
  std::vector<Box> boxes;
  for (const Sheet& d : drawings) {
    Box box;
    for (const Item& i : d.items) extentOf(i, s, box);
    if (box.x1 < box.x0) box = Box{0, 0, 0, 0};
    boxes.push_back(box);
  }
  constexpr double gap = 15.0;  // paper mm between drawings
  // Rows within `width` (paper mm): where each drawing goes (its left, its row) and the rows' heights.
  struct Placement {
    std::vector<std::pair<double, int>> at;
    std::vector<double> rowH;
    double w = 0;
  };
  const auto arrange = [&](double width) {
    Placement p;
    double x = 0;
    for (const Box& b : boxes) {
      const double w = (b.x1 - b.x0) / s, h = (b.y1 - b.y0) / s;
      if (p.rowH.empty() || (x > 0 && x + w > width)) {
        p.rowH.push_back(0);
        x = 0;
      }
      p.at.push_back({x, static_cast<int>(p.rowH.size()) - 1});
      p.rowH.back() = std::max(p.rowH.back(), h);
      x += w + gap;
      p.w = std::max(p.w, x - gap);
    }
    return p;
  };
  const auto heightOf = [&](const Placement& p) {
    double h = 0;
    for (double r : p.rowH) h += r;
    return h + gap * (p.rowH.size() - 1);
  };
  auto area = [&](const Paper& p, double& x0, double& y0, double& x1, double& y1) {
    x0 = kLeftMargin;
    x1 = p.w - kMargin;
    y0 = kMargin + kTitleH + 4;
    y1 = p.h - kMargin;
  };
  Sheet sheet;
  sheet.scale = s;
  for (const Sheet& d : drawings)
    for (const std::string& n : d.notes) sheet.notes.push_back(n);
  const Paper* paper = nullptr;
  Placement placed;
  for (const Paper& p : kPapers) {
    if (spec.paper != "auto" && spec.paper != p.name) continue;
    double x0, y0, x1, y1;
    area(p, x0, y0, x1, y1);
    const double w = x1 - x0 - 2 * kPad, h = y1 - y0 - 2 * kPad;
    placed = arrange(w);
    const bool fits = placed.w <= w && heightOf(placed) <= h;
    if (spec.paper != "auto" || fits) {
      paper = &p;
      if (!fits) sheet.notes.push_back(std::string("The drawings do not fit on ") + p.name + ": they run off the frame");
      break;
    }
  }
  if (!paper) {
    if (spec.paper != "auto") return R::failure("Unknown paper '" + spec.paper + "' (A4, A3, A2, A1 or auto)");
    paper = &kPapers[3];
    double x0, y0, x1, y1;
    area(*paper, x0, y0, x1, y1);
    placed = arrange(x1 - x0 - 2 * kPad);
    sheet.notes.push_back("The drawings do not fit on A1 at this scale: choose a smaller scale");
  }
  sheet.paper = paper->name;
  sheet.paperWidth = paper->w;
  sheet.paperHeight = paper->h;
  sheet.title = spec.title.drawing;
  double ax0, ay0, ax1, ay1;
  area(*paper, ax0, ay0, ax1, ay1);
  // Centred in the area as a whole; each drawing at the top of its row.
  const double totalH = heightOf(placed);
  const double left = (ax0 + ax1) / 2 - placed.w / 2, top = (ay0 + ay1) / 2 + totalH / 2;
  std::vector<double> rowTop{top};
  for (std::size_t r = 1; r < placed.rowH.size(); ++r) rowTop.push_back(rowTop.back() - placed.rowH[r - 1] - gap);
  std::vector<Item> items;
  for (std::size_t k = 0; k < drawings.size(); ++k) {
    const double dx = (left + placed.at[k].first) * s - boxes[k].x0;
    const double dy = rowTop[static_cast<std::size_t>(placed.at[k].second)] * s - boxes[k].y1;
    for (Item i : drawings[k].items) {
      translate(i, dx, dy);
      items.push_back(std::move(i));
    }
  }
  if (spec.north) appendNorthMark(items, gp_Pnt2d((ax1 - 15) * s, (ay1 - 16) * s), s, trueNorth);
  for (const Sheet& d : drawings) {
    sheet.hatchPatterns.insert(d.hatchPatterns.begin(), d.hatchPatterns.end());
    sheet.symbols.insert(d.symbols.begin(), d.symbols.end());
  }
  if (spec.frame) {
    appendLegend(items, paper, s, sheet.hatchPatterns, sheet.symbols);
    frameAndTitle(items, paper, spec, sheet.title, s);
  }
  sheet.items = std::move(items);
  return sheet;
}

Result<Sheet> layoutSheetWithTables(const View& view, const std::vector<Table>& additionalTables, const SheetSpec& spec) {
  using R = Result<Sheet>;
  if (!view.siteGroundTable && additionalTables.empty()) return layoutViewOnly(view, spec);
  auto tables = additionalTables;
  if (view.siteGroundTable) tables.insert(tables.begin(), *view.siteGroundTable);
  const double s = spec.scale;
  if (!(s > 0) || !std::isfinite(s)) return R::failure("The scale must be positive");
  for (const Table& t : tables)
    if (t.headers.empty()) return R::failure("The table has no columns");
  // The drawing on its own (no frame, no north mark: it goes beside the drawing here).
  SheetSpec bare = spec;
  bare.frame = false;
  bare.north = false;
  bare.paper = "auto";
  auto drawn = layoutViewOnly(view, bare);
  if (!drawn) return drawn;
  std::vector<Item> items = drawn->items;
  Box box;
  for (const Item& i : items) extentOf(i, s, box);
  if (spec.north && (view.spec.kind == ViewKind::Plan || view.spec.kind == ViewKind::SitePlan)) {
    const std::size_t first = items.size();
    appendNorthMark(items, gp_Pnt2d(box.x1 + 14 * s, box.y1 - 8 * s), s, view.trueNorth);
    for (std::size_t i = first; i < items.size(); ++i) extentOf(items[i], s, box);
  }
  const double dW = (box.x1 - box.x0) / s, dH = (box.y1 - box.y0) / s;
  constexpr double gap = 10.0;  // paper mm between the drawing and the tables, and between tables
  std::vector<TableSize> sizes;
  double colW = 0, colH = 0;
  for (const Table& t : tables) {
    sizes.push_back(tableSize(t));
    colW = std::max(colW, sizes.back().w);
    colH += sizes.back().h + (sizes.size() > 1 ? gap : 0.0);
  }
  const double rightW = dW + gap + colW, rightH = std::max(dH, colH);
  const double belowW = std::max(dW, colW), belowH = dH + gap + colH;
  auto area = [&](const Paper& p, double& x0, double& y0, double& x1, double& y1) {
    x0 = spec.frame ? kLeftMargin : kMargin;
    x1 = p.w - kMargin;
    y0 = spec.frame ? kMargin + kTitleH + 4 : kMargin;
    y1 = p.h - kMargin;
  };
  Sheet sheet;
  sheet.scale = s;
  for (const std::string& n : drawn->notes) sheet.notes.push_back(n);
  const Paper* paper = nullptr;
  bool below = false;
  for (const Paper& p : kPapers) {
    if (spec.paper != "auto" && spec.paper != p.name) continue;
    double x0, y0, x1, y1;
    area(p, x0, y0, x1, y1);
    const double w = x1 - x0 - 2 * kPad, h = y1 - y0 - 2 * kPad;
    const bool right = rightW <= w && rightH <= h, under = belowW <= w && belowH <= h;
    if (spec.paper != "auto" || right || under) {
      paper = &p;
      below = !right && under;
      if (!right && !under) sheet.notes.push_back(std::string("The drawing and its tables do not fit on ") + p.name + ": they run off the frame");
      break;
    }
  }
  if (!paper) {
    if (spec.paper != "auto") return R::failure("Unknown paper '" + spec.paper + "' (A4, A3, A2, A1 or auto)");
    paper = &kPapers[3];
    sheet.notes.push_back("The drawing and its tables do not fit on A1: choose a smaller scale");
  }
  sheet.paper = paper->name;
  sheet.paperWidth = paper->w;
  sheet.paperHeight = paper->h;
  // The sheet's name (the drawing's and its tables', when given) in the title block.
  const std::string sheetName = spec.title.drawing.empty() ? drawn->title : spec.title.drawing;
  sheet.title = sheetName;
  double ax0, ay0, ax1, ay1;
  area(*paper, ax0, ay0, ax1, ay1);
  // The drawing at the top left of the frame.
  const double left = ax0 + kPad, top = ay1 - kPad;
  const double dx = left * s - box.x0, dy = top * s - box.y1;
  for (Item& i : items) translate(i, dx, dy);
  // The tables to its right, or under it.
  double tx = below ? left : left + dW + gap, ty = below ? top - dH - gap : top;
  for (std::size_t k = 0; k < tables.size(); ++k) {
    drawTable(tables[k], sizes[k], tx, ty, s, items);
    ty -= sizes[k].h + gap;
  }
  sheet.hatchPatterns = view.hatchPatterns;  // docs/326
  sheet.symbols = view.symbols;              // docs/327
  if (spec.frame && view.spec.framingPreset.empty()) appendLegend(items, paper, s, view.hatchPatterns, view.symbols);
  if (spec.frame) frameAndTitle(items, paper, spec, sheetName, s);
  sheet.items = std::move(items);
  return sheet;
}

}  // namespace sbr::draw
