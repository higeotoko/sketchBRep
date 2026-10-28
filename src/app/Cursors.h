#pragma once
// Vector toolbar symbols and precise pointer cursors. Cursors use a white
// halo for contrast; palette symbols adapt to theme and device resolution.

#include <QCursor>
#include <QIcon>

namespace sbr::app {

enum class CursorKind { Select, Pencil, Rectangle, Curve, PushPull, Move, Rotate, Offset, Revolve, Measure, Circle, Paint, Eraser, Arc, Dimension, Text, Section, FollowMe, Protractor, Polygon, Scale, Wall, Slab, Opening, RotatedRectangle, SectionPlanes, SectionCuts, SectionFill };

QCursor toolCursor(CursorKind kind);
// Theme-aware vector icon, independent of the cursor pointer and hotspot.
QIcon toolIcon(CursorKind kind);

}  // namespace sbr::app
