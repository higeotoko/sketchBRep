// Laying a view out on paper (docs/50): annotations sized on paper, the
// paper chosen, the view centred in the frame, the title block filled in.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "sbr/draw/Drawing.h"

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

}  // namespace

Result<Sheet> layoutSheet(const View& view, const SheetSpec& spec) {
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
  const double bubble = 5.0 * s, margin = 6.0 * s;
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
    items.push_back(text(Pen::GridLabel, c, g.name, 4.0, 1, 1));
    const double coord = byLeft ? g.point.Y() : g.point.X();
    spans[byLeft ? 1 : 0].push_back({coord, g.name});
    if (byLeft) gridLeft = std::min(gridLeft, e0.X());
    else gridLow = std::min(gridLow, e0.Y());
  }
  for (auto& [family, axes] : spans) {
    std::sort(axes.begin(), axes.end());
    for (std::size_t k = 0; k + 1 < axes.size(); ++k) {
      const double a = axes[k].first, b = axes[k + 1].first;
      if (b - a < 1e-6) continue;
      if (family == 0) {
        const double v = (model.y0 + gridLow) / 2;
        dimension(items, gp_Pnt2d(a, v), gp_Pnt2d(b, v), gp_Vec2d(0, 0), mm(b - a), s);
      } else {
        const double u = (model.x0 + gridLeft) / 2;
        dimension(items, gp_Pnt2d(u, a), gp_Pnt2d(u, b), gp_Vec2d(0, 0), mm(b - a), s);
      }
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
  for (const LevelMark& l : levels) {
    const double u0 = model.x0 - 3 * margin, u1 = model.x1 + margin;
    items.push_back(line(Pen::Level, gp_Pnt2d(u0, l.v), gp_Pnt2d(u1, l.v)));
    items.push_back(text(Pen::Level, gp_Pnt2d(u0, l.v + 1.0 * s), l.name, 3.5, 0, 0));
    const std::string h = std::abs(l.elevation) < 0.5 ? "±0" : (l.elevation > 0 ? "+" : "") + mm(l.elevation);
    items.push_back(text(Pen::Level, gp_Pnt2d(u0, l.v - 1.0 * s), h, 2.5, 0, 2 /*top*/));
  }
  for (const DimensionMark& d : view.dimensions) dimension(items, d.a, d.b, d.offset, d.text, s);

  Box all;
  for (const Item& i : items) extentOf(i, s, all);
  // The drawing's caption below it.
  const std::string title = view.spec.title.empty() ? spec.title.drawing : view.spec.title;
  char scaleText[32];
  std::snprintf(scaleText, sizeof scaleText, "S=1:%g", s);
  const double captionGap = 9.0 * s;
  const std::string caption = title.empty() ? std::string(scaleText) : title + "  " + scaleText;
  const double captionW = textWidth(caption, 5.0) * s;
  const double width = std::max(all.x1 - all.x0, captionW);
  const double height = all.y1 - all.y0 + captionGap;

  // The paper: the smallest that holds it (paper mm = real / scale).
  const bool frame = spec.frame;
  auto areaOf = [&](const Paper& p, double& x0, double& y0, double& x1, double& y1) {
    x0 = frame ? kLeftMargin : kMargin;
    x1 = p.w - kMargin;
    y0 = frame ? kMargin + kTitleH + 4 : kMargin;
    y1 = p.h - kMargin;
  };
  const Paper* paper = nullptr;
  Sheet sheet;
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
  if (view.spec.kind == ViewKind::Plan && spec.north) {
    const gp_Pnt2d c = P(ax1 - 14, ay1 - 16);
    items.push_back(circle(Pen::Text, c, 6 * s));
    items.push_back(line(Pen::Text, P(ax1 - 14, ay1 - 22), P(ax1 - 14, ay1 - 10)));
    Item arrow;
    arrow.kind = Item::Kind::Fill;
    arrow.pen = Pen::Text;
    arrow.points = {P(ax1 - 14, ay1 - 9), P(ax1 - 16.5, ay1 - 15), P(ax1 - 14, ay1 - 13.5), P(ax1 - 11.5, ay1 - 15)};
    items.push_back(arrow);
    items.push_back(text(Pen::Text, P(ax1 - 14, ay1 - 6.5), "N", 3.5, 1, 0));
  }
  if (frame) {
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
  sheet.items = std::move(items);
  return sheet;
}

}  // namespace sbr::draw
