#pragma once
// docs/247: a sheet looked over for what makes it hard to read, as a person checks a drawing
// before it goes out. Estimates (the words' width as the sheet layout estimates it), to point at
// places to look at, not a judgement.

#include <cstddef>
#include <string>
#include <vector>

#include <gp_Pnt2d.hxx>

#include "sbr/draw/Drawing.h"

namespace sbr::draw {

// Words smaller than this on paper (mm) are pointed at.
constexpr double kSmallestTextMm = 1.8;

struct SheetFinding {
  // text_on_text (words on other words), line_through_text (a line through the words; `other`
  // names the kind of line), off_paper (words beyond the paper), tiny_text (smaller than
  // kSmallestTextMm), off_frame (words beyond the sheet's frame, docs/257), does_not_fit (the
  // sheet's own note that the drawing does not fit its paper, docs/257).
  std::string kind;
  std::string text, other;
  gp_Pnt2d at;          // the words' anchor on paper (mm from the lower left)
  double height = 0.0;  // the words' height on paper (mm)
};

// At most `limit` findings, in the order above (per kind).
std::vector<SheetFinding> checkSheet(const Sheet& sheet, std::size_t limit = 200);
const char* penName(Pen pen);

// Whether a proposed text item has 'gap' of paper space around it, clear of text and lines.
bool textIsClear(const Item& label, const std::vector<Item>& items, double scale, double gap);
bool textFitsInside(const Item& label, const std::vector<gp_Pnt2d>& outline, double scale, double gap);

// The pieces of the line a-b left when it is kept `gap` (paper mm) clear of the words of `texts`
// (Text items; heights on paper, the sheet at 1:`scale`). Empty when nothing is left.
std::vector<std::pair<gp_Pnt2d, gp_Pnt2d>> clearOfText(const gp_Pnt2d& a, const gp_Pnt2d& b, const std::vector<const Item*>& texts,
                                                       double scale, double gap);

}  // namespace sbr::draw
