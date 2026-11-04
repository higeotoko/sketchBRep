// DXF of a sheet (docs/50): R12 (AC1009), the version every CAD reads,
// Jw_cad included. Real size (mm); a layer per pen with its colour and
// line type; Japanese text in Shift_JIS ($DWGCODEPAGE ANSI_932).

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "DrawingInternal.h"
#include "sbr/TextEncoding.h"
#include "sbr/draw/Drawing.h"

namespace sbr::draw {
namespace {

// AutoCAD colour numbers per pen (plotted by colour in most offices).
int penColor(Pen pen) {
  switch (pen) {
    case Pen::Cut: return 1;         // red
    case Pen::Visible: return 7;     // white / black
    case Pen::Hidden: return 8;      // grey
    case Pen::Fill: return 9;        // light grey
    case Pen::Grid: return 6;        // magenta
    case Pen::GridLabel: return 6;
    case Pen::Level: return 5;       // blue
    case Pen::Dimension: return 3;   // green
    case Pen::Text: return 7;
    case Pen::Frame: return 7;
    case Pen::TitleBlock: return 7;
  }
  return 7;
}

const char* lineType(Pen pen) {
  switch (pen) {
    case Pen::Hidden: return "HIDDEN";
    case Pen::Grid:
    case Pen::Level: return "DASHDOT";
    default: return "CONTINUOUS";
  }
}

class Out {
 public:
  void code(int c, const std::string& v) { s_ << c << "\n" << v << "\n"; }
  void code(int c, int v) { s_ << c << "\n" << v << "\n"; }
  void real(int c, double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6f", v);
    s_ << c << "\n" << b << "\n";
  }
  void point(int c, const gp_Pnt2d& p) {
    real(c, p.X());
    real(c + 10, p.Y());
    real(c + 20, 0.0);
  }
  std::string str() const { return s_.str(); }

 private:
  std::ostringstream s_;
};

}  // namespace

Status writeDxf(const Sheet& sheet, const std::string& path) {
  const double s = sheet.scale;
  Out o;
  o.code(0, "SECTION");
  o.code(2, "HEADER");
  o.code(9, "$ACADVER");
  o.code(1, "AC1009");
  o.code(9, "$DWGCODEPAGE");
  o.code(3, "ANSI_932");
  o.code(9, "$INSBASE");
  o.point(10, gp_Pnt2d(0, 0));
  o.code(9, "$EXTMIN");
  o.point(10, gp_Pnt2d(0, 0));
  o.code(9, "$EXTMAX");
  o.point(10, gp_Pnt2d(sheet.paperWidth * s, sheet.paperHeight * s));
  o.code(9, "$LIMMIN");
  o.real(10, 0);
  o.real(20, 0);
  o.code(9, "$LIMMAX");
  o.real(10, sheet.paperWidth * s);
  o.real(20, sheet.paperHeight * s);
  o.code(9, "$LTSCALE");
  o.real(40, 1.0);
  o.code(0, "ENDSEC");

  o.code(0, "SECTION");
  o.code(2, "TABLES");
  // Line types: dashes on paper, at the drawing's scale.
  o.code(0, "TABLE");
  o.code(2, "LTYPE");
  o.code(70, 3);
  struct LT {
    const char* name;
    const char* text;
    std::vector<double> dashes;
  };
  const LT types[] = {{"CONTINUOUS", "Solid line", {}},
                      {"HIDDEN", "Hidden __ __ __", penDashes(Pen::Hidden)},
                      {"DASHDOT", "Dash dot __ . __ .", penDashes(Pen::Grid)}};
  for (const LT& t : types) {
    o.code(0, "LTYPE");
    o.code(2, t.name);
    o.code(70, 0);
    o.code(3, t.text);
    o.code(72, 65);
    o.code(73, static_cast<int>(t.dashes.size()));
    double total = 0;
    for (double d : t.dashes) total += d * s;
    o.real(40, total);
    for (std::size_t k = 0; k < t.dashes.size(); ++k) o.real(49, (k % 2 == 0 ? 1 : -1) * t.dashes[k] * s);
  }
  o.code(0, "ENDTAB");
  o.code(0, "TABLE");
  o.code(2, "LAYER");
  o.code(70, kPenCount);
  for (int p = 0; p < kPenCount; ++p) {
    const Pen pen = static_cast<Pen>(p);
    o.code(0, "LAYER");
    o.code(2, penLayerName(pen));
    o.code(70, 0);
    o.code(62, penColor(pen));
    o.code(6, lineType(pen));
  }
  o.code(0, "ENDTAB");
  o.code(0, "TABLE");
  o.code(2, "STYLE");
  o.code(70, 1);
  o.code(0, "STYLE");
  o.code(2, "STANDARD");
  o.code(70, 0);
  o.real(40, 0.0);
  o.real(41, 1.0);
  o.real(50, 0.0);
  o.code(71, 0);
  o.real(42, 2.5);
  o.code(3, "txt");
  o.code(4, "");
  o.code(0, "ENDTAB");
  o.code(0, "ENDSEC");

  o.code(0, "SECTION");
  o.code(2, "ENTITIES");
  for (const Item& i : sheet.items) {
    const std::string layer = penLayerName(i.pen);
    switch (i.kind) {
      case Item::Kind::Line:
        o.code(0, "LINE");
        o.code(8, layer);
        o.point(10, i.points[0]);
        o.point(11, i.points[1]);
        break;
      case Item::Kind::Arc:
        if (i.sweep >= 2 * M_PI - 1e-9) {
          o.code(0, "CIRCLE");
          o.code(8, layer);
          o.point(10, i.centre);
          o.real(40, i.radius);
        } else {
          o.code(0, "ARC");
          o.code(8, layer);
          o.point(10, i.centre);
          o.real(40, i.radius);
          o.real(50, i.start * 180 / M_PI);
          o.real(51, (i.start + i.sweep) * 180 / M_PI);
        }
        break;
      case Item::Kind::Polyline:
        o.code(0, "POLYLINE");
        o.code(8, layer);
        o.code(66, 1);
        o.point(10, gp_Pnt2d(0, 0));
        o.code(70, i.closed ? 1 : 0);
        for (const gp_Pnt2d& p : i.points) {
          o.code(0, "VERTEX");
          o.code(8, layer);
          o.point(10, p);
        }
        o.code(0, "SEQEND");
        o.code(8, layer);
        break;
      case Item::Kind::Text: {
        o.code(0, "TEXT");
        o.code(8, layer);
        o.point(10, i.points[0]);
        o.real(40, i.height * s);
        o.code(1, toShiftJis(i.text));
        o.real(50, i.angle * 180 / M_PI);
        const int v = i.vAlign == 1 ? 2 : i.vAlign == 2 ? 3 : 0;
        if (i.hAlign != 0 || v != 0) {
          o.code(72, i.hAlign);
          o.code(73, v);
          o.point(11, i.points[0]);
        }
        break;
      }
      case Item::Kind::Fill:
        for (const auto& t : detail::triangulate(i)) {
          o.code(0, "SOLID");
          o.code(8, layer);
          o.point(10, t[0]);
          o.point(11, t[1]);
          o.point(12, t[2]);
          o.point(13, t[2]);
        }
        break;
    }
  }
  o.code(0, "ENDSEC");
  o.code(0, "EOF");
  std::ofstream f(path, std::ios::binary);
  if (!f) return Status::failure("Cannot write " + path);
  const std::string text = o.str();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) return Status::failure("Writing " + path + " failed");
  return Status::success();
}

}  // namespace sbr::draw
