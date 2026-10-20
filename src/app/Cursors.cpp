#include "Cursors.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace sbr::app {
namespace {

constexpr int kSize = 32;

// Strokes `path` twice: a wide white halo, then the black line.
void halo(QPainter& p, const QPainterPath& path, bool fill = false) {
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(Qt::white, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.drawPath(path);
  p.setPen(QPen(Qt::black, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  if (fill) p.setBrush(Qt::black);
  p.drawPath(path);
}

QPainterPath arrowHead(QPointF tip, QPointF from, double size = 4.5) {
  const QLineF axis(tip, from);
  QLineF a = axis;
  a.setLength(size);
  QLineF left = a, right = a;
  left.setAngle(a.angle() + 30);
  right.setAngle(a.angle() - 30);
  QPainterPath path;
  path.moveTo(left.p2());
  path.lineTo(tip);
  path.lineTo(right.p2());
  return path;
}

// Pencil pointing to the lower-left corner (hotspot at its tip).
QPainterPath pencil() {
  QPainterPath path;
  path.moveTo(2, 30);
  path.lineTo(5, 22);
  path.lineTo(19, 8);
  path.lineTo(24, 13);
  path.lineTo(10, 27);
  path.closeSubpath();
  path.moveTo(5, 22);
  path.lineTo(10, 27);
  path.moveTo(16, 11);
  path.lineTo(21, 16);
  return path;
}

QPixmap draw(CursorKind kind, QPoint& hotspot) {
  QPixmap pixmap(kSize, kSize);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing);
  hotspot = QPoint(kSize / 2, kSize / 2);

  switch (kind) {
    case CursorKind::Select: {
      QPainterPath arrow;
      arrow.moveTo(6, 3);
      arrow.lineTo(6, 26);
      arrow.lineTo(12, 20);
      arrow.lineTo(16, 29);
      arrow.lineTo(20, 27);
      arrow.lineTo(16, 18);
      arrow.lineTo(24, 18);
      arrow.closeSubpath();
      p.setBrush(Qt::white);
      halo(p, arrow);
      hotspot = QPoint(6, 3);
      break;
    }
    case CursorKind::Measure: {
      // Tape measure: case with a tape ending at the hotspot.
      QPainterPath body;
      body.addRoundedRect(QRectF(12, 4, 16, 16), 4, 4);
      halo(p, body);
      QPainterPath tape;
      tape.moveTo(14, 20);
      tape.lineTo(3, 29);
      halo(p, tape);
      QPainterPath dot;
      dot.addEllipse(QPointF(20, 12), 2.5, 2.5);
      halo(p, dot);
      hotspot = QPoint(3, 29);
      break;
    }
    case CursorKind::Revolve: {
      // Profile rectangle and an arc around a dashed axis.
      QPainterPath axis;
      axis.moveTo(6, 2);
      axis.lineTo(6, 30);
      p.setPen(QPen(Qt::black, 1.2, Qt::DashLine));
      p.drawPath(axis);
      QPainterPath profile;
      profile.addRect(QRectF(10, 8, 7, 16));
      halo(p, profile);
      QPainterPath arc;
      arc.arcMoveTo(QRectF(-10, 4, 40, 24), 20);
      arc.arcTo(QRectF(-10, 4, 40, 24), 20, -110);
      halo(p, arc);
      const QPointF end = arc.currentPosition();
      halo(p, arrowHead(end, QPointF(end.x() + 5, end.y() - 3), 5));
      hotspot = QPoint(4, 4);
      break;
    }
    case CursorKind::Pencil:
      halo(p, pencil());
      hotspot = QPoint(2, 30);
      break;
    case CursorKind::Rectangle: {
      halo(p, pencil());
      QPainterPath rect;
      rect.addRect(QRectF(20, 22, 10, 7));
      halo(p, rect);
      hotspot = QPoint(2, 30);
      break;
    }
    case CursorKind::Curve: {
      halo(p, pencil());
      QPainterPath s;
      s.moveTo(18, 30);
      s.cubicTo(22, 20, 26, 32, 30, 22);
      halo(p, s);
      hotspot = QPoint(2, 30);
      break;
    }
    case CursorKind::PushPull: {
      // A face (parallelogram) with an arrow pulled out of it.
      QPainterPath face;
      face.moveTo(4, 28);
      face.lineTo(20, 28);
      face.lineTo(28, 21);
      face.lineTo(12, 21);
      face.closeSubpath();
      halo(p, face);
      QPainterPath shaft;
      shaft.moveTo(16, 24);
      shaft.lineTo(16, 4);
      halo(p, shaft);
      halo(p, arrowHead(QPointF(16, 3), QPointF(16, 12), 6));
      hotspot = QPoint(16, 24);
      break;
    }
    case CursorKind::Move: {
      QPainterPath cross;
      cross.moveTo(16, 3);
      cross.lineTo(16, 29);
      cross.moveTo(3, 16);
      cross.lineTo(29, 16);
      halo(p, cross);
      halo(p, arrowHead(QPointF(16, 2), QPointF(16, 10)));
      halo(p, arrowHead(QPointF(16, 30), QPointF(16, 22)));
      halo(p, arrowHead(QPointF(2, 16), QPointF(10, 16)));
      halo(p, arrowHead(QPointF(30, 16), QPointF(22, 16)));
      break;
    }
    case CursorKind::Rotate: {
      // Protractor-like circular arrow around the centre (hotspot).
      QPainterPath arc;
      arc.arcMoveTo(QRectF(5, 5, 22, 22), 120);
      arc.arcTo(QRectF(5, 5, 22, 22), 120, 270);
      halo(p, arc);
      const QPointF end = arc.currentPosition();
      halo(p, arrowHead(end, QPointF(end.x() - 6, end.y() - 4), 5));
      QPainterPath centre;
      centre.addEllipse(QPointF(16, 16), 1.5, 1.5);
      halo(p, centre, true);
      break;
    }
    case CursorKind::Offset: {
      QPainterPath outer, inner;
      outer.addRect(QRectF(3, 6, 22, 22));
      inner.addRect(QRectF(9, 12, 10, 10));
      halo(p, outer);
      halo(p, inner);
      QPainterPath arrow;
      arrow.moveTo(26, 4);
      arrow.lineTo(20, 10);
      halo(p, arrow);
      halo(p, arrowHead(QPointF(19, 11), QPointF(26, 4), 4));
      hotspot = QPoint(3, 28);
      break;
    }
  }
  p.end();
  return pixmap;
}

}  // namespace

QCursor toolCursor(CursorKind kind) {
  if (kind == CursorKind::Select) return QCursor(Qt::ArrowCursor);
  QPoint hotspot;
  const QPixmap pixmap = draw(kind, hotspot);
  return QCursor(pixmap, hotspot.x(), hotspot.y());
}

QIcon toolIcon(CursorKind kind) {
  QPoint hotspot;
  return QIcon(draw(kind, hotspot));
}

}  // namespace sbr::app
