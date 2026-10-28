#include "Cursors.h"

#include <QGuiApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <cmath>
#include <algorithm>
#include <QtMath>

namespace sbr::app {
namespace {
constexpr qreal pi = 3.14159265358979323846;

void arrow(QPainterPath& path, QPointF tip, QPointF from, qreal size = 3) {
  QLineF line(tip, from); line.setLength(size);
  QLineF left(line), right(line);
  left.setAngle(line.angle() + 35); right.setAngle(line.angle() - 35);
  path.moveTo(left.p2()); path.lineTo(tip); path.lineTo(right.p2());
}

// Toolbar symbols use one stroke system in a 24-unit canvas. They do not
// carry a cursor's pointer, hotspot or white halo.
QPainterPath symbol(CursorKind kind) {
  QPainterPath p;
  switch (kind) {
    case CursorKind::Select:
      p.moveTo(5, 3); p.lineTo(5, 20); p.lineTo(10, 15); p.lineTo(14, 22);
      p.lineTo(17, 20); p.lineTo(13, 13); p.lineTo(21, 13); p.closeSubpath(); break;
    case CursorKind::Pencil:
      p.moveTo(5, 19); p.lineTo(19, 5); p.addEllipse(QPointF(5, 19), 1.5, 1.5);
      p.addEllipse(QPointF(19, 5), 1.5, 1.5); break;
    case CursorKind::Rectangle: p.addRect(QRectF(4, 5, 16, 14)); break;
    case CursorKind::RotatedRectangle:
      p.moveTo(3, 10); p.lineTo(15, 4); p.lineTo(21, 15); p.lineTo(9, 21); p.closeSubpath(); break;
    case CursorKind::Circle: p.addEllipse(QPointF(12, 12), 8, 8); break;
    case CursorKind::Arc: p.moveTo(4, 19); p.quadTo(12, -7, 20, 19); break;
    case CursorKind::Curve:
      p.moveTo(3, 19); p.cubicTo(7, -4, 16, 28, 21, 5); break;
    case CursorKind::Polygon:
      for (int k = 0; k < 6; ++k) {
        QPointF v(12 + 9 * std::cos(k * pi / 3), 12 + 9 * std::sin(k * pi / 3));
        if (!k) p.moveTo(v); else p.lineTo(v);
      }
      p.closeSubpath(); break;
    case CursorKind::PushPull:
      p.moveTo(3, 17); p.lineTo(12, 13); p.lineTo(21, 17); p.lineTo(12, 21); p.closeSubpath();
      p.moveTo(12, 17); p.lineTo(12, 3); arrow(p, {12, 3}, {12, 11}); break;
    case CursorKind::Move:
      p.moveTo(12, 3); p.lineTo(12, 21); p.moveTo(3, 12); p.lineTo(21, 12);
      arrow(p, {12, 3}, {12, 10}); arrow(p, {12, 21}, {12, 14});
      arrow(p, {3, 12}, {10, 12}); arrow(p, {21, 12}, {14, 12}); break;
    case CursorKind::Rotate: {
      p.arcMoveTo(QRectF(4, 4, 16, 16), 110); p.arcTo(QRectF(4, 4, 16, 16), 110, 285);
      const auto end = p.currentPosition(), from = p.pointAtPercent(.92); arrow(p, end, from); break;
    }
    case CursorKind::Scale:
      p.addRect(QRectF(4, 12, 8, 8)); p.moveTo(9, 15); p.lineTo(21, 3);
      arrow(p, {21, 3}, {14, 10}, 4); break;
    case CursorKind::Offset: p.addRect(QRectF(3, 3, 18, 18)); p.addRect(QRectF(7, 7, 10, 10)); break;
    case CursorKind::Revolve:
      p.moveTo(5, 3); p.lineTo(5, 21); p.moveTo(9, 5); p.lineTo(15, 5); p.lineTo(15, 14);
      p.arcMoveTo(QRectF(3, 12, 18, 8), 165); p.arcTo(QRectF(3, 12, 18, 8), 165, 285);
      arrow(p, p.currentPosition(), p.pointAtPercent(.92), 2.5); break;
    case CursorKind::FollowMe:
      p.addRect(QRectF(3, 15, 6, 6)); p.moveTo(6, 15); p.lineTo(6, 9);
      p.quadTo(6, 4, 12, 4); p.lineTo(21, 4); arrow(p, {21, 4}, {14, 4}); break;
    case CursorKind::Measure:
      p.moveTo(3, 17); p.lineTo(17, 3); p.lineTo(21, 7); p.lineTo(7, 21); p.closeSubpath();
      for (int i = 0; i < 3; ++i) { p.moveTo(7 + 4*i, 13 - 4*i); p.lineTo(9 + 4*i, 15 - 4*i); } break;
    case CursorKind::Protractor:
      p.moveTo(3, 20); p.arcTo(QRectF(3, 3, 18, 34), 180, -180); p.closeSubpath();
      p.moveTo(12, 20); p.lineTo(18, 8); break;
    case CursorKind::Dimension:
      p.moveTo(4, 4); p.lineTo(4, 20); p.moveTo(20, 4); p.lineTo(20, 20);
      p.moveTo(4, 12); p.lineTo(20, 12); arrow(p, {4, 12}, {11, 12}); arrow(p, {20, 12}, {13, 12}); break;
    case CursorKind::Text:
      p.moveTo(4, 5); p.lineTo(20, 5); p.moveTo(12, 5); p.lineTo(12, 21);
      p.moveTo(8, 21); p.lineTo(16, 21); break;
    case CursorKind::Paint:
      p.moveTo(5, 9); p.lineTo(12, 3); p.lineTo(21, 12); p.lineTo(14, 18); p.closeSubpath();
      p.moveTo(4, 15); p.cubicTo(0, 20, 4, 24, 7, 20); p.quadTo(8, 19, 4, 15); break;
    case CursorKind::Eraser:
      p.moveTo(3, 15); p.lineTo(12, 4); p.lineTo(21, 11); p.lineTo(13, 21); p.lineTo(10, 21); p.closeSubpath();
      p.moveTo(8, 9); p.lineTo(17, 16); p.moveTo(3, 21); p.lineTo(21, 21); break;
    case CursorKind::Wall:
      p.addRect(QRectF(3, 5, 18, 15)); p.moveTo(3, 12); p.lineTo(21, 12);
      p.moveTo(10, 5); p.lineTo(10, 12); p.moveTo(15, 12); p.lineTo(15, 20); break;
    case CursorKind::Slab:
      p.moveTo(3, 10); p.lineTo(12, 5); p.lineTo(21, 10); p.lineTo(12, 15); p.closeSubpath();
      p.moveTo(3, 10); p.lineTo(3, 14); p.lineTo(12, 19); p.lineTo(21, 14); p.lineTo(21, 10);
      p.moveTo(12, 15); p.lineTo(12, 19); break;
    case CursorKind::Opening:
      p.addRect(QRectF(4, 3, 16, 18)); p.addRect(QRectF(8, 7, 8, 10));
      p.moveTo(12, 7); p.lineTo(12, 17); break;
    case CursorKind::Section:
      p.moveTo(4, 7); p.lineTo(15, 3); p.lineTo(15, 20); p.lineTo(4, 23); p.closeSubpath();
      p.moveTo(9, 12); p.lineTo(22, 12); arrow(p, {22, 12}, {15, 12}); break;
    case CursorKind::SectionPlanes:
      p.moveTo(3, 8); p.lineTo(16, 3); p.lineTo(21, 16); p.lineTo(8, 21); p.closeSubpath(); break;
    case CursorKind::SectionCuts:
    case CursorKind::SectionFill:
      p.moveTo(3, 11); p.lineTo(10, 6); p.lineTo(21, 9); p.lineTo(14, 14); p.closeSubpath();
      p.moveTo(3, 11); p.lineTo(3, 18); p.lineTo(14, 21); p.lineTo(21, 16); p.lineTo(21, 9);
      p.moveTo(14, 14); p.lineTo(14, 21); break;
  }
  return p;
}

void drawSymbol(QPainter& painter, CursorKind kind, const QRectF& rect, QColor color, bool cursorHalo = false) {
  painter.save();
  painter.translate(rect.topLeft());
  painter.scale(rect.width() / 24, rect.height() / 24);
  const auto path = symbol(kind);
  painter.setBrush(Qt::NoBrush);
  if (cursorHalo) {
    painter.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
  }
  painter.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);
  if (kind == CursorKind::SectionFill) {
    QPainterPath fill;
    fill.moveTo(3, 11); fill.lineTo(10, 6); fill.lineTo(21, 9); fill.lineTo(14, 14); fill.closeSubpath();
    painter.fillPath(fill, color);
  }
  painter.restore();
}

class ToolIconEngine : public QIconEngine {
 public:
  explicit ToolIconEngine(CursorKind kind) : kind_(kind) {}
  QIconEngine* clone() const override { return new ToolIconEngine(kind_); }
  void paint(QPainter* p, const QRect& r, QIcon::Mode mode, QIcon::State) override {
    const QPalette palette = QGuiApplication::palette();
    const QColor color = mode == QIcon::Disabled ? palette.color(QPalette::Disabled, QPalette::ButtonText)
        : palette.color(mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::ButtonText);
    p->setRenderHint(QPainter::Antialiasing);
    const int side = std::min(r.width(), r.height());
    drawSymbol(*p, kind_, QRectF(r.center().x() - side/2.0 + .5, r.center().y() - side/2.0 + .5, side, side), color);
  }
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1); }
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    const QSize logical(qRound(size.width() / scale), qRound(size.height() / scale));
#else
    const QSize logical = size;
#endif
    QPixmap image(QSize(qCeil(logical.width() * scale), qCeil(logical.height() * scale)));
    image.setDevicePixelRatio(scale); image.fill(Qt::transparent);
    QPainter painter(&image); paint(&painter, QRect(QPoint(), logical), mode, state);
    return image;
  }
 private:
  CursorKind kind_;
};
}

QCursor toolCursor(CursorKind kind) {
  if (kind == CursorKind::Select) return QCursor(Qt::ArrowCursor);
  const auto* screen = QGuiApplication::screenAt(QCursor::pos());
  const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
  QPixmap image(QSize(qCeil(32 * dpr), qCeil(32 * dpr)));
  image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
  QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
  if (kind == CursorKind::Move) {
    // The empty center is intentional: a single point marks the exact
    // pick/grab position, rather than the intersection of four busy arms.
    QPainterPath arrows;
    arrows.moveTo(16, 3); arrows.lineTo(16, 10);
    arrows.moveTo(16, 22); arrows.lineTo(16, 29);
    arrows.moveTo(3, 16); arrows.lineTo(10, 16);
    arrows.moveTo(22, 16); arrows.lineTo(29, 16);
    arrow(arrows, {16, 3}, {16, 10}, 4); arrow(arrows, {16, 29}, {16, 22}, 4);
    arrow(arrows, {3, 16}, {10, 16}, 4); arrow(arrows, {29, 16}, {22, 16}, 4);
    p.setPen(QPen(Qt::white, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPath(arrows);
    p.setPen(QPen(Qt::black, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPath(arrows);
    p.setPen(Qt::NoPen); p.setBrush(Qt::white); p.drawEllipse(QPointF(16, 16), 2.5, 2.5);
    p.setBrush(Qt::black); p.drawEllipse(QPointF(16, 16), 1, 1);
    p.end();
    return QCursor(image, 16, 16);
  }
  drawSymbol(p, kind, QRectF(16, 16, 15, 15), Qt::black, true);
  QPainterPath pointer;
  pointer.moveTo(3, 2); pointer.lineTo(3, 19); pointer.lineTo(7, 15);
  pointer.lineTo(10, 22); pointer.lineTo(13, 20); pointer.lineTo(10, 13);
  pointer.lineTo(17, 13); pointer.closeSubpath();
  p.setBrush(Qt::white); p.setPen(QPen(Qt::white, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.drawPath(pointer);
  p.setPen(QPen(Qt::black, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPath(pointer);
  p.end();
  return QCursor(image, 3, 2);
}

QIcon toolIcon(CursorKind kind) { return QIcon(new ToolIconEngine(kind)); }

}  // namespace sbr::app
