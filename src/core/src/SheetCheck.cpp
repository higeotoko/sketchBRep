// docs/247: a sheet looked over for what makes it hard to read, as a person checks a drawing
// before it goes out: words on words, lines through words, words off the paper, words too small.
#include "sbr/draw/SheetCheck.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sbr::draw {

namespace {

// The words' box in their own frame (x along the text, y up it), on the sheet (real mm).
struct TextBox {
  const Item* item = nullptr;
  gp_Pnt2d origin;
  double c = 1, s = 0;           // direction of the text
  double x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // the box (sheet units), the inside a little shrunk
  double paperHeight = 0;
  gp_Pnt2d toLocal(const gp_Pnt2d& p) const {
    const double dx = p.X() - origin.X(), dy = p.Y() - origin.Y();
    return {dx * c + dy * s, -dx * s + dy * c};
  }
  gp_Pnt2d toSheet(double x, double y) const { return {origin.X() + x * c - y * s, origin.Y() + x * s + y * c}; }
};

// As DrawingSheet's own estimate: a full-width character is the text's height wide, others 0.6 of it.
double widthOf(const std::string& text, double height) {
  double n = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if ((ch & 0xC0) == 0x80) continue;
    n += ch >= 0xE0 ? 1.0 : 0.6;
  }
  return n * height;
}

TextBox boxOf(const Item& item, double scale) {
  TextBox b;
  b.item = &item;
  b.origin = item.points.empty() ? gp_Pnt2d() : item.points[0];
  b.c = std::cos(item.angle);
  b.s = std::sin(item.angle);
  b.paperHeight = item.height;
  const double h = item.height * scale, w = widthOf(item.text, h);
  const double left = item.hAlign == 0 ? 0.0 : item.hAlign == 1 ? -w / 2 : -w;
  const double bottom = item.vAlign == 0 ? 0.0 : item.vAlign == 1 ? -h / 2 : -h;
  // The inside: words have air around them; a line touching the box's edge is not through them.
  const double mx = 0.12 * h, my = 0.18 * h;
  b.x0 = left + mx;
  b.x1 = left + w - mx;
  b.y0 = bottom + my;
  b.y1 = bottom + h - my;
  return b;
}

// Whether the segment passes through the box's inside (Liang-Barsky in the text's frame).
bool through(const TextBox& b, const gp_Pnt2d& p, const gp_Pnt2d& q) {
  if (b.x1 <= b.x0 || b.y1 <= b.y0) return false;
  const gp_Pnt2d a = b.toLocal(p), e = b.toLocal(q);
  double t0 = 0, t1 = 1;
  const double dx = e.X() - a.X(), dy = e.Y() - a.Y();
  const double ps[4] = {-dx, dx, -dy, dy};
  const double qs[4] = {a.X() - b.x0, b.x1 - a.X(), a.Y() - b.y0, b.y1 - a.Y()};
  for (int i = 0; i < 4; ++i) {
    if (std::abs(ps[i]) < 1e-12) {
      if (qs[i] < 0) return false;
      continue;
    }
    const double t = qs[i] / ps[i];
    if (ps[i] < 0) t0 = std::max(t0, t);
    else t1 = std::min(t1, t);
    if (t0 > t1) return false;
  }
  return t1 - t0 > 1e-9;
}

// Whether two boxes' insides overlap (separating axes of the two rectangles).
bool overlap(const TextBox& a, const TextBox& b) {
  const auto corners = [](const TextBox& t) {
    return std::array<gp_Pnt2d, 4>{t.toSheet(t.x0, t.y0), t.toSheet(t.x1, t.y0), t.toSheet(t.x1, t.y1), t.toSheet(t.x0, t.y1)};
  };
  const auto ca = corners(a), cb = corners(b);
  const auto separated = [](const TextBox& t, const std::array<gp_Pnt2d, 4>& other) {
    double lx = 1e300, hx = -1e300, ly = 1e300, hy = -1e300;
    for (const gp_Pnt2d& p : other) {
      const gp_Pnt2d l = t.toLocal(p);
      lx = std::min(lx, l.X());
      hx = std::max(hx, l.X());
      ly = std::min(ly, l.Y());
      hy = std::max(hy, l.Y());
    }
    return hx <= t.x0 || lx >= t.x1 || hy <= t.y0 || ly >= t.y1;
  };
  return !separated(a, cb) && !separated(b, ca);
}

std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> segmentsOf(const Item& item) {
  std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> out;
  if (item.kind == Item::Kind::Line || item.kind == Item::Kind::Polyline) {
    for (std::size_t i = 0; i + 1 < item.points.size(); ++i) out.emplace_back(item.points[i], item.points[i + 1]);
    if (item.closed && item.points.size() > 2) out.emplace_back(item.points.back(), item.points.front());
  } else if (item.kind == Item::Kind::Arc && item.radius > 0) {
    const int n = std::max(8, static_cast<int>(std::abs(item.sweep) / (3.14159265358979323846 / 16)));
    gp_Pnt2d last(item.centre.X() + item.radius * std::cos(item.start), item.centre.Y() + item.radius * std::sin(item.start));
    for (int i = 1; i <= n; ++i) {
      const double t = item.start + item.sweep * i / n;
      const gp_Pnt2d p(item.centre.X() + item.radius * std::cos(t), item.centre.Y() + item.radius * std::sin(t));
      out.emplace_back(last, p);
      last = p;
    }
  }
  return out;
}

gp_Pnt2d onPaper(const gp_Pnt2d& p, double scale) { return {p.X() / scale, p.Y() / scale}; }

}  // namespace

std::vector<SheetFinding> checkSheet(const Sheet& sheet, std::size_t limit) {
  std::vector<SheetFinding> out;
  const double scale = sheet.scale > 0 ? sheet.scale : 1.0;
  std::vector<TextBox> texts;
  for (const Item& item : sheet.items)
    if (item.kind == Item::Kind::Text && !item.text.empty() && !item.points.empty()) texts.push_back(boxOf(item, scale));
  const auto add = [&](SheetFinding f) {
    if (out.size() < limit) out.push_back(std::move(f));
  };
  const double paperW = sheet.paperWidth * scale, paperH = sheet.paperHeight * scale;
  // docs/257: the sheet's own word that the drawing does not fit its paper; and the frame (the
  // thick border) the words should stay inside.
  for (const std::string& note : sheet.notes)
    if (note.find("does not fit") != std::string::npos) add({"does_not_fit", note, {}, {}, 0.0});
  double fx0 = 1e300, fy0 = 1e300, fx1 = -1e300, fy1 = -1e300;
  for (const Item& item : sheet.items)
    if (item.pen == Pen::Frame)
      for (const gp_Pnt2d& p : item.points) {
        fx0 = std::min(fx0, p.X());
        fy0 = std::min(fy0, p.Y());
        fx1 = std::max(fx1, p.X());
        fy1 = std::max(fy1, p.Y());
      }
  const bool framed = fx0 < fx1 && fy0 < fy1;
  for (const TextBox& t : texts) {
    if (t.paperHeight < kSmallestTextMm)
      add({"tiny_text", t.item->text, {}, onPaper(t.origin, scale), t.paperHeight});
    bool off = false;
    for (const gp_Pnt2d& p : {t.toSheet(t.x0, t.y0), t.toSheet(t.x1, t.y0), t.toSheet(t.x1, t.y1), t.toSheet(t.x0, t.y1)})
      off = off || p.X() < 0 || p.Y() < 0 || p.X() > paperW || p.Y() > paperH;
    if (off) add({"off_paper", t.item->text, {}, onPaper(t.origin, scale), t.paperHeight});
    else if (framed && t.item->pen != Pen::Frame) {
      bool outside = false;
      for (const gp_Pnt2d& p : {t.toSheet(t.x0, t.y0), t.toSheet(t.x1, t.y0), t.toSheet(t.x1, t.y1), t.toSheet(t.x0, t.y1)})
        outside = outside || p.X() < fx0 || p.Y() < fy0 || p.X() > fx1 || p.Y() > fy1;
      if (outside) add({"off_frame", t.item->text, {}, onPaper(t.origin, scale), t.paperHeight});
    }
  }
  for (std::size_t i = 0; i < texts.size(); ++i)
    for (std::size_t j = i + 1; j < texts.size(); ++j)
      if (overlap(texts[i], texts[j]))
        add({"text_on_text", texts[i].item->text, texts[j].item->text, onPaper(texts[i].origin, scale), texts[i].paperHeight});
  for (const TextBox& t : texts) {
    int crossings = 0;
    std::string pen;
    for (const Item& item : sheet.items) {
      if (item.kind != Item::Kind::Line && item.kind != Item::Kind::Polyline && item.kind != Item::Kind::Arc) continue;
      for (const auto& [p, q] : segmentsOf(item))
        if (through(t, p, q)) {
          if (crossings++ == 0) pen = penName(item.pen);
          break;
        }
    }
    if (crossings > 0) add({"line_through_text", t.item->text, pen + (crossings > 1 ? " ほか " + std::to_string(crossings - 1) : ""),
                            onPaper(t.origin, scale), t.paperHeight});
  }
  return out;
}

std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> clearOfText(const gp_Pnt2d& a, const gp_Pnt2d& b, const std::vector<const Item*>& texts,
                                                       double scale, double gap) {
  // The parts of the line (as fractions 0..1) under the words, merged, then what is left.
  std::vector<std::pair<double, double>> under;
  for (const Item* item : texts) {
    if (!item || item->kind != Item::Kind::Text || item->points.empty() || item->text.empty()) continue;
    TextBox t = boxOf(*item, scale);
    // The whole box (not its inside) and the gap around it.
    const double h = item->height * scale, g = gap * scale;
    t.x0 -= 0.12 * h + g;
    t.x1 += 0.12 * h + g;
    t.y0 -= 0.18 * h + g;
    t.y1 += 0.18 * h + g;
    const gp_Pnt2d p = t.toLocal(a), q = t.toLocal(b);
    double t0 = 0, t1 = 1;
    const double dx = q.X() - p.X(), dy = q.Y() - p.Y();
    const double ps[4] = {-dx, dx, -dy, dy};
    const double qs[4] = {p.X() - t.x0, t.x1 - p.X(), p.Y() - t.y0, t.y1 - p.Y()};
    bool hit = true;
    for (int i = 0; i < 4 && hit; ++i) {
      if (std::abs(ps[i]) < 1e-12) {
        if (qs[i] < 0) hit = false;
        continue;
      }
      const double r = qs[i] / ps[i];
      if (ps[i] < 0) t0 = std::max(t0, r);
      else t1 = std::min(t1, r);
      if (t0 > t1) hit = false;
    }
    if (hit && t1 > t0) under.emplace_back(t0, t1);
  }
  std::sort(under.begin(), under.end());
  std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> out;
  const auto at = [&](double f) { return gp_Pnt2d(a.X() + (b.X() - a.X()) * f, a.Y() + (b.Y() - a.Y()) * f); };
  double from = 0;
  for (const auto& [s, e] : under) {
    if (s > from + 1e-6) out.emplace_back(at(from), at(s));
    from = std::max(from, e);
  }
  if (from < 1 - 1e-6) out.emplace_back(at(from), at(1));
  return out;
}

const char* penName(Pen pen) {
  switch (pen) {
    case Pen::Cut: return "切断線";
    case Pen::Visible: return "見えがかり線";
    case Pen::Hidden: return "隠れ線";
    case Pen::Fill: return "塗り";
    case Pen::Grid: return "通り芯";
    case Pen::GridLabel: return "通り芯の符号";
    case Pen::Level: return "レベル";
    case Pen::Dimension: return "寸法";
    case Pen::Text: return "注記";
    case Pen::Frame: return "枠";
    case Pen::TitleBlock: return "表題欄";
  }
  return "";
}

}  // namespace sbr::draw
