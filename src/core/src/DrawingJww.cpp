// JWW (Jw_cad) of a sheet (docs/50). Written as Jw_cad's version 6.00
// data, following Jw_cad's own description of the format ("Jw_cadのデータ
// 形式", jwdatafmt.txt) and checked against files Jw_cad saved.
//
// Jw_cad keeps coordinates on paper (mm, the paper's centre at 0, 0) with
// the scale on the layer group: the sheet (real size) is divided by the
// scale. Each pen gets a layer of group 0 and a Jw line colour (print
// widths set in 1/100 mm) and line type.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "DrawingInternal.h"
#include "sbr/TextEncoding.h"
#include "sbr/draw/Drawing.h"

namespace sbr::draw {
namespace {

constexpr std::uint32_t kVersion = 600;

class Archive {
 public:
  void u8(std::uint8_t v) { b_.push_back(v); }
  void u16(std::uint16_t v) { raw(&v, 2); }
  void u32(std::uint32_t v) { raw(&v, 4); }
  void i32(std::int32_t v) { raw(&v, 4); }
  void f64(double v) { raw(&v, 8); }
  void bytes(const char* p, std::size_t n) { raw(p, n); }
  // MFC CString (ANSI): length prefix, then the bytes (Shift_JIS).
  void str(const std::string& utf8) {
    const std::string s = toShiftJis(utf8);
    if (s.size() < 0xFF) {
      u8(static_cast<std::uint8_t>(s.size()));
    } else if (s.size() < 0xFFFE) {
      u8(0xFF);
      u16(static_cast<std::uint16_t>(s.size()));
    } else {
      u8(0xFF);
      u16(0xFFFF);
      u32(static_cast<std::uint32_t>(s.size()));
    }
    raw(s.data(), s.size());
  }
  // MFC CObList counts.
  void count(std::size_t n) {
    if (n < 0xFFFF) {
      u16(static_cast<std::uint16_t>(n));
    } else {
      u16(0xFFFF);
      u32(static_cast<std::uint32_t>(n));
    }
  }
  // MFC CArchive object header: the class the first time (new class tag,
  // schema, name), afterwards its index; objects take indices too.
  void object(const char* className) {
    for (const auto& [name, index] : classes_) {
      if (name == className) {
        if (index < 0x7FFF) {
          u16(static_cast<std::uint16_t>(0x8000 | index));
        } else {
          u16(0x7FFF);  // MFC's big class tag
          u32(0x80000000u | index);
        }
        ++mapCount_;
        return;
      }
    }
    u16(0xFFFF);
    u16(static_cast<std::uint16_t>(kVersion));
    const std::size_t n = std::strlen(className);
    u16(static_cast<std::uint16_t>(n));
    raw(className, n);
    classes_.push_back({className, mapCount_});
    mapCount_ += 2;  // the class, then the object
  }
  const std::vector<std::uint8_t>& data() const { return b_; }

 private:
  void raw(const void* p, std::size_t n) {
    const auto* c = static_cast<const std::uint8_t*>(p);
    b_.insert(b_.end(), c, c + n);
  }
  std::vector<std::uint8_t> b_;
  std::vector<std::pair<std::string, std::uint32_t>> classes_;
  std::uint32_t mapCount_ = 1;
};

// Jw line colour (1..9) and type (1 solid, 2 dotted 1, 5 chain 1) per pen.
struct JwPen {
  std::uint16_t color;
  std::uint8_t type;
  std::uint16_t layer;
};
JwPen jwPen(Pen pen) {
  switch (pen) {
    case Pen::Cut: return {5, 1, 0};
    case Pen::Visible: return {2, 1, 1};
    case Pen::Hidden: return {1, 2, 2};
    case Pen::Fill: return {9, 1, 3};
    case Pen::Grid: return {1, 5, 4};
    case Pen::GridLabel: return {2, 1, 5};
    case Pen::Level: return {1, 5, 6};
    case Pen::Dimension: return {1, 1, 7};
    case Pen::Text: return {2, 1, 8};
    case Pen::Frame: return {6, 1, 9};
    case Pen::TitleBlock: return {3, 1, 10};
  }
  return {2, 1, 1};
}
const char* const kLayerNames[] = {"断面", "見え掛かり", "隠れ線", "断面塗り", "通り芯", "通り芯符号",
                                   "レベル", "寸法", "文字", "図面枠", "表題欄"};

int paperCode(const std::string& name) {
  if (name == "A0") return 0;
  if (name == "A1") return 1;
  if (name == "A2") return 2;
  if (name == "A3") return 3;
  return 4;
}

void header(Archive& a, const Sheet& sheet) {
  a.bytes("JwwData.", 8);
  a.u32(kVersion);
  a.str(sheet.title);
  a.u32(static_cast<std::uint32_t>(paperCode(sheet.paper)));
  // Layer groups and layers: group 0 written to, all at the sheet's scale.
  a.u32(0);
  for (int g = 0; g < 16; ++g) {
    a.u32(g == 0 ? 3 : 2);
    a.u32(0);
    a.f64(sheet.scale);
    a.u32(0);
    for (int l = 0; l < 16; ++l) {
      a.u32(g == 0 && l == 0 ? 3 : 2);
      a.u32(0);
    }
  }
  for (int i = 0; i < 14; ++i) a.u32(0);
  for (int i = 0; i < 5; ++i) a.u32(0);  // dimension settings: not kept in the file
  a.u32(0);
  a.i32(-201);  // line widths in 1/100 mm (Ver.6.00: -(previous max width + 200))
  a.f64(0); a.f64(0);  // print origin
  a.f64(1.0);          // print magnification
  a.u32(0);            // print rotation / reference point
  a.u32(0); a.f64(15.0); a.f64(5.0); a.f64(5.0); a.f64(0); a.f64(0);  // scale marks (off)
  for (int g = 0; g < 16; ++g) {
    for (int l = 0; l < 16; ++l) a.str(g == 0 && l < 11 ? kLayerNames[l] : "");
  }
  for (int g = 0; g < 16; ++g) a.str(g == 0 ? sheet.title : "");
  a.f64(0); a.f64(36.0); a.u32(0); a.f64(0);  // shadow: height, latitude, 9-15h, wall
  a.f64(0); a.f64(0);                         // sky view
  a.u32(1);                                   // 2.5D in mm
  a.f64(1.0); a.f64(0); a.f64(0);             // view on saving
  a.f64(1.0); a.f64(0); a.f64(0);             // remembered range
  for (int n = 1; n <= 8; ++n) {
    a.f64(1.0); a.f64(0); a.f64(0);
    a.u32(0xFFFFFFFFu);
  }
  for (int i = 0; i < 3; ++i) a.f64(0);
  a.u32(0); a.f64(0); a.f64(0); a.f64(0); a.u32(0);  // text drawing state
  for (int n = 0; n < 10; ++n) a.f64(0);  // parallel line distances
  a.f64(0);
  // Line colours on screen (colour, width) and on paper (colour, width in
  // 1/100 mm, point radius): Jw_cad's colours, our widths.
  static const std::uint32_t screen[10] = {16777215, 12632064, 0, 49152, 49344, 12583104, 16711680, 8421376, 8388863, 12632256};
  for (int n = 0; n < 10; ++n) {
    a.u32(screen[n]);
    a.u32(1);
  }
  static const std::uint32_t paperColor[10] = {16777215, 0, 0, 0, 0, 0, 0, 0, 0, 12632256};
  static const std::uint32_t widths[10] = {13, 13, 18, 25, 35, 50, 70, 18, 18, 13};
  for (int n = 0; n < 10; ++n) {
    a.u32(paperColor[n]);
    a.u32(widths[n]);
    a.f64(0.3);
  }
  // Line types 2-9, random lines 11-15, long types 16-19 (Jw_cad's patterns).
  static const std::uint32_t types[8][4] = {{2576980377u, 4, 1, 5},   {3284386755u, 8, 1, 10}, {3890735079u, 8, 1, 10},
                                            {4188010911u, 16, 1, 10}, {4294549503u, 32, 1, 10}, {4065325647u, 16, 1, 10},
                                            {4294070271u, 32, 1, 10}, {572662306u, 4, 1, 10}};
  for (const auto& t : types) {
    for (std::uint32_t v : t) a.u32(v);
  }
  static const std::uint32_t random[5][5] = {{3434263338u, 1, 3, 1, 5},  {3434263338u, 1, 4, 1, 10}, {3434263338u, 2, 5, 2, 15},
                                             {3434263338u, 2, 6, 2, 20}, {3434263338u, 2, 7, 2, 25}};
  for (const auto& t : random) {
    for (std::uint32_t v : t) a.u32(v);
  }
  static const std::uint32_t longTypes[4][4] = {
      {4294549503u, 32, 2, 20}, {4294070271u, 32, 2, 20}, {4294868991u, 32, 2, 20}, {4294868991u, 32, 4, 40}};
  for (const auto& t : longTypes) {
    for (std::uint32_t v : t) a.u32(v);
  }
  // Points, drawing order, colour print (on) and the rest of the print settings.
  static const std::uint32_t print[10] = {0, 0, 1, 0, 0, 1, 1, 1, 0, 0};
  for (std::uint32_t v : print) a.u32(v);
  a.u32(10);  // 300 dpi line width base
  a.u32(0);   // drawing time
  a.u32(0);   // 2.5D eye positions not set
  for (int i = 0; i < 3; ++i) a.u32(static_cast<std::uint32_t>(-4500));
  static const double eye[9] = {2000.0, 1000000.0, 1000000.0, 1000000.0, 35.264389682754654, 0.0, 0.0, 0.0, 0.0};
  for (double v : eye) a.f64(v);
  a.u32(0);         // solids in a colour of their own: not by default
  a.u32(12632256);  // that colour
  // SXF colours: screen (colour, width), then paper (name, colour, width, radius).
  static const char* const sxfNames[17] = {"",        "black",   "red",   "green",  "blue",       "yellow",
                                           "magenta", "cyan",    "white", "deeppink", "brown",    "orange",
                                           "lightgreen", "lightblue", "lavender", "lightgray", "darkgray"};
  static const std::uint32_t sxfColors[17] = {0,       0,       255,     65280,   16711680, 65535,   16711935, 16776960, 16777215,
                                              8388800, 4227264, 33023,   8437888, 16744448, 16728192, 12632256, 8421504};
  auto sxfColor = [&](int n) -> std::uint32_t { return n < 17 ? sxfColors[n] : (n <= 32 ? 12632256u : 0u); };
  for (int n = 0; n <= 256; ++n) {
    a.u32(sxfColor(n));
    a.u32(18);
  }
  for (int n = 0; n <= 256; ++n) {
    a.str(n < 17 ? sxfNames[n] : "");
    a.u32(sxfColor(n));
    a.u32(18);
    a.f64(0.18);
  }
  // SXF line types: patterns, then names and pitches (the standard ones).
  for (int n = 0; n <= 32; ++n) {
    a.u32(0xFFFFFFFFu);
    a.u32(32);
    a.u32(1);
    a.u32(10);
  }
  struct SxfType {
    const char* name;
    std::vector<double> pitch;
  };
  static const SxfType sxfTypes[] = {{"continuous", {}},
                                     {"dashed", {6.0, 1.5}},
                                     {"dashed spaced", {6.0, 6.0}},
                                     {"long dashed dotted", {12.0, 1.5, 0.25, 1.5}},
                                     {"long dashed double-dotted", {12.0, 1.5, 0.25, 1.5, 0.25, 1.5}},
                                     {"long dashed triplicate-dotted", {12.0, 1.5, 0.25, 1.5, 0.25, 1.5, 0.25, 1.5}},
                                     {"dotted", {0.25, 1.5}},
                                     {"chain", {12.0, 1.5, 3.5, 1.5}},
                                     {"chain double dash", {12.0, 1.5, 3.5, 1.5, 3.5, 1.5}},
                                     {"dashed dotted", {6.0, 1.5, 0.25, 1.5}},
                                     {"double-dashed dotted", {6.0, 1.5, 6.0, 1.5, 0.25, 1.5}},
                                     {"dashed double-dotted", {6.0, 1.5, 0.25, 1.5, 0.25, 1.5}},
                                     {"double-dashed double-dotted", {6.0, 1.5, 6.0, 1.5, 0.25, 1.5, 0.25, 1.5}},
                                     {"dashed triplicate-dotted", {6.0, 1.5, 0.25, 1.5, 0.25, 1.5, 0.25, 1.5}}};
  constexpr int kSxfTypes = static_cast<int>(sizeof sxfTypes / sizeof sxfTypes[0]);
  for (int n = 0; n <= 32; ++n) {
    // The first entry is the default (unnamed), then the standard types.
    const SxfType* t = n >= 1 && n <= kSxfTypes ? &sxfTypes[n - 1] : nullptr;
    a.str(t ? t->name : "");
    a.u32(t ? static_cast<std::uint32_t>(t->pitch.size()) : 0);
    for (int j = 1; j <= 10; ++j) a.f64(t && j <= static_cast<int>(t->pitch.size()) ? t->pitch[j - 1] : 0.0);
  }
  // Text kinds 1-10 (width, height, spacing, colour): Jw_cad's sizes.
  static const double kinds[10][3] = {{2, 2, 0}, {2.5, 2.5, 0}, {3, 3, 0.5}, {4, 4, 0.5}, {5, 5, 0.5},
                                      {6, 6, 0.5}, {7, 7, 1},  {8, 8, 1},    {9, 9, 1},   {10, 10, 1}};
  static const std::uint32_t kindColors[10] = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
  for (int i = 0; i < 10; ++i) {
    a.f64(kinds[i][0]);
    a.f64(kinds[i][1]);
    a.f64(kinds[i][2]);
    a.u32(kindColors[i]);
  }
  a.f64(4.0); a.f64(4.0); a.f64(0.5); a.u32(2); a.u32(4);  // text being written
  a.f64(5.0); a.f64(0.0);                                  // text tidying
  a.u32(0);                                                // base point shifts: off
  for (double v : {-1.0, 0.0, 1.0}) a.f64(v);
  for (double v : {-1.0, 0.0, 1.0}) a.f64(v);
}

void base(Archive& a, std::uint8_t type, std::uint16_t color, std::uint16_t layer) {
  a.u32(0);      // curve attribute
  a.u8(type);
  a.u16(color);
  a.u16(0);      // width: the colour's
  a.u16(layer);
  a.u16(0);      // layer group 0
  a.u16(0);      // flags
}

}  // namespace

Status writeJww(const Sheet& sheet, const std::string& path) {
  const double s = sheet.scale;
  if (!(s > 0)) return Status::failure("The scale must be positive");
  // Sheet (real) -> Jw paper coordinates.
  const double cx = sheet.paperWidth / 2, cy = sheet.paperHeight / 2;
  auto P = [&](const gp_Pnt2d& p) { return gp_Pnt2d(p.X() / s - cx, p.Y() / s - cy); };
  struct Element {
    enum { Line, Arc, Text, Solid } kind;
    Pen pen;
    gp_Pnt2d a, b, c, d;
    double radius = 0, start = 0, sweep = 0;
    std::string text;
    double size = 0, angle = 0;
  };
  std::vector<Element> elements;
  for (const Item& i : sheet.items) {
    switch (i.kind) {
      case Item::Kind::Line:
        elements.push_back({Element::Line, i.pen, P(i.points[0]), P(i.points[1]), {}, {}});
        break;
      case Item::Kind::Polyline:
        for (std::size_t k = 1; k < i.points.size(); ++k) {
          elements.push_back({Element::Line, i.pen, P(i.points[k - 1]), P(i.points[k]), {}, {}});
        }
        if (i.closed && i.points.size() > 2) {
          elements.push_back({Element::Line, i.pen, P(i.points.back()), P(i.points.front()), {}, {}});
        }
        break;
      case Item::Kind::Arc: {
        Element e{Element::Arc, i.pen, P(i.centre), {}, {}, {}};
        e.radius = i.radius / s;
        e.start = i.start;
        e.sweep = i.sweep;
        elements.push_back(e);
        break;
      }
      case Item::Kind::Text: {
        gp_Pnt2d start, end;
        detail::textBaseline(i, s, start, end);
        Element e{Element::Text, i.pen, P(start), P(end), {}, {}};
        e.text = i.text;
        e.size = i.height;
        e.angle = i.angle * 180 / M_PI;
        elements.push_back(e);
        break;
      }
      case Item::Kind::Fill:
        for (const auto& t : detail::triangulate(i)) {
          elements.push_back({Element::Solid, i.pen, P(t[0]), P(t[2]), P(t[1]), P(t[2])});
        }
        break;
    }
  }
  Archive a;
  header(a, sheet);
  a.count(elements.size());
  for (const Element& e : elements) {
    const JwPen pen = jwPen(e.pen);
    switch (e.kind) {
      case Element::Line:
        a.object("CDataSen");
        base(a, pen.type, pen.color, pen.layer);
        a.f64(e.a.X()); a.f64(e.a.Y()); a.f64(e.b.X()); a.f64(e.b.Y());
        break;
      case Element::Arc: {
        a.object("CDataEnko");
        base(a, pen.type, pen.color, pen.layer);
        const bool full = e.sweep >= 2 * M_PI - 1e-9;
        a.f64(e.a.X()); a.f64(e.a.Y());
        a.f64(e.radius);
        a.f64(full ? 0.0 : e.start);
        a.f64(full ? 2 * M_PI : e.sweep);
        a.f64(0.0);  // tilt
        a.f64(1.0);  // flatness (a circle)
        a.u32(full ? 1 : 0);
        break;
      }
      case Element::Text:
        a.object("CDataMoji");
        base(a, 1, pen.color, pen.layer);
        a.f64(e.a.X()); a.f64(e.a.Y()); a.f64(e.b.X()); a.f64(e.b.Y());
        a.u32(0);  // text kind: any size
        a.f64(e.size); a.f64(e.size); a.f64(0.0);
        a.f64(e.angle);
        a.str("ＭＳ ゴシック");
        a.str(e.text);
        break;
      case Element::Solid:
        a.object("CDataSolid");
        base(a, 1, 10, pen.layer);  // colour 10: its own colour (grey)
        // First, fourth, second and third points, then the colour (RGB).
        a.f64(e.a.X()); a.f64(e.a.Y()); a.f64(e.d.X()); a.f64(e.d.Y());
        a.f64(e.c.X()); a.f64(e.c.Y()); a.f64(e.b.X()); a.f64(e.b.Y());
        a.u32(0x00C8C8C8);
        break;
    }
  }
  a.count(0);  // no block definitions
  std::ofstream f(path, std::ios::binary);
  if (!f) return Status::failure("Cannot write " + path);
  f.write(reinterpret_cast<const char*>(a.data().data()), static_cast<std::streamsize>(a.data().size()));
  if (!f) return Status::failure("Writing " + path + " failed");
  return Status::success();
}

}  // namespace sbr::draw
