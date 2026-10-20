#pragma once
// Tool cursors drawn in code (no image assets): black strokes with a white
// halo so they read on both dark edges and light faces.

#include <QCursor>
#include <QIcon>

namespace sbr::app {

enum class CursorKind { Select, Pencil, Rectangle, Curve, PushPull, Move, Rotate, Offset, Revolve, Measure, Circle };

QCursor toolCursor(CursorKind kind);
// Toolbar icon with the same drawing as the cursor.
QIcon toolIcon(CursorKind kind);

}  // namespace sbr::app
