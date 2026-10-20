#pragma once
// Tool cursors drawn in code (no image assets): black strokes with a white
// halo so they read on both dark edges and light faces.

#include <QCursor>

namespace sbr::app {

enum class CursorKind { Select, Pencil, Rectangle, Curve, PushPull, Move, Rotate, Offset };

QCursor toolCursor(CursorKind kind);

}  // namespace sbr::app
