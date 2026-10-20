// Phase 1 acceptance test driving the real main window with synthetic input:
// draw a rectangle, push/pull it 3000 mm, save, reopen and re-edit.

#include <cmath>

#include <BRep_Tool.hxx>

#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScreen>
#include <QtTest/QtTest>

#include "MainWindow.h"
#include "Viewport.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Validation.h"

using namespace sbr;
using namespace sbr::app;

namespace {

void send(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
          Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QMouseEvent event(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, buttons, mods);
  QApplication::sendEvent(w, &event);
}

void move(QWidget* w, const QPoint& pos) {
  send(w, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton);
}

void click(QWidget* w, const QPoint& pos) {
  move(w, pos);
  send(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
  send(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
  QTest::qWait(20);
}

void type(QWidget* w, const QString& text) {
  QTest::keyClicks(w, text);
  QTest::keyClick(w, Qt::Key_Return);
  QTest::qWait(50);
}

// Returns an empty string when the bounds match.
QString boxMismatch(const TopoDS_Shape& s, double x, double y, double z) {
  const Bnd_Box b = geom::exactBounds(s);
  if (b.IsVoid()) return QStringLiteral("empty shape");
  const double tol = 1e-6;
  const gp_Pnt mn = b.CornerMin();
  const gp_Pnt mx = b.CornerMax();
  if (std::abs(mn.X()) > tol || std::abs(mn.Y()) > tol || std::abs(mn.Z()) > tol ||
      std::abs(mx.X() - x) > tol || std::abs(mx.Y() - y) > tol || std::abs(mx.Z() - z) > tol) {
    return QStringLiteral("bounds (%1 %2 %3)-(%4 %5 %6), expected (0 0 0)-(%7 %8 %9)")
        .arg(mn.X()).arg(mn.Y()).arg(mn.Z()).arg(mx.X()).arg(mx.Y()).arg(mx.Z())
        .arg(x).arg(y).arg(z);
  }
  return {};
}

#define EXPECT_BOX(shape, x, y, z)                                     \
  do {                                                                 \
    const QString mismatch = boxMismatch((shape), (x), (y), (z));      \
    QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));                \
  } while (false)

}  // namespace

class GuiPhase1 : public QObject {
  Q_OBJECT
 private slots:
  void drawPushPullSaveReopen();
  void lineToolClosesLoopIntoFace();
  void moveCopyAndArray();
  void rotateByTypedAngle();
  void offsetToolInwards();
  void groupIsSelectedAsAWhole();
  void curveToolOpenAndClosed();
};

namespace {

// Window with a fixed top-down orthographic camera around the origin.
struct Fixture {
  MainWindow window;
  Viewport* vp = nullptr;
  bool setUp(double cx = 3000, double cy = 3000) {
    window.resize(1280, 860);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) return false;
    vp = &window.viewport();
    doc::CameraState cam;
    cam.eye = {cx - 6000, cy - 8000, 9000};
    cam.target = {cx, cy, 0};
    cam.up = {0, 0, 1};
    cam.perspective = false;
    cam.scale = 16000;
    vp->restoreCamera(cam);
    QTest::qWait(50);
    return true;
  }
  doc::ObjectId box(double x, double y, double w, double d, double h) {
    geom::SketchPlane plane = geom::SketchPlane::ground();
    plane.origin = gp_Pnt(x, y, 0);
    auto id = window.modeler().createRectangle(plane, w, d);
    window.modeler().extrude(*id, h);
    QTest::qWait(20);
    return *id;
  }
};

}  // namespace

void GuiPhase1::moveCopyAndArray() {
  Fixture f;
  QVERIFY(f.setUp());
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId column = f.box(0, 0, 300, 300, 3000);
  f.window.setSelection({column});
  f.window.activateTool(MainWindow::ToolId::Move);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));        // base point: column corner
  QTest::keyClick(f.vp, Qt::Key_Control);                 // copy mode
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 0)));      // direction along X
  type(f.vp, QStringLiteral("3000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  type(f.vp, QStringLiteral("x4"));                       // array of four at 3000 spacing
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(5));
  Bnd_Box all;
  for (const doc::Object& o : m.document().objects()) all.Add(geom::exactBounds(m.document().worldShape(o)));
  QVERIFY(std::abs(all.CornerMax().X() - 12300) < 1e-6);
  QCOMPARE(m.history().undoName(), std::string("Array"));
  type(f.vp, QStringLiteral("/4"));                       // divide the 12000 into four
  all.SetVoid();
  for (const doc::Object& o : m.document().objects()) all.Add(geom::exactBounds(m.document().worldShape(o)));
  QVERIFY(std::abs(all.CornerMax().X() - 12300) < 1e-6);
  QCOMPARE(m.document().objects().size(), std::size_t(5));

  const QString shot = qEnvironmentVariable("SBR_PHASE2_SHOT");
  if (!shot.isEmpty()) {
    // Add a wall with a door opening and an offset slab for the screenshot.
    const doc::ObjectId wall = f.box(-1000, 2000, 14000, 200, 3000);
    const doc::ObjectId door = f.box(2000, 1900, 900, 400, 2100);
    auto cut = m.boolean(doc::BooleanKind::Subtract, {wall, door});
    QVERIFY(cut.ok());
    geom::SketchPlane plane = geom::SketchPlane::ground();
    plane.origin = gp_Pnt(-1000, -1000, 0);
    auto slab = m.createRectangle(plane, 14000, 3200);
    auto inner = m.offset(*slab, -400);
    QVERIFY(inner.ok());
    f.window.setSelection({*cut});
    f.window.activateTool(MainWindow::ToolId::Select);
    f.vp->fitAll();
    QTest::qWait(150);
    QVERIFY(f.window.screen()->grabWindow(0).copy(f.window.frameGeometry()).save(shot));
  }
}

void GuiPhase1::rotateByTypedAngle() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId id = f.box(0, 0, 2000, 500, 500);
  f.window.setSelection({id});
  f.window.activateTool(MainWindow::ToolId::Rotate);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));        // centre
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));     // reference along +X
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 1500, 0)));   // counter-clockwise side
  type(f.vp, QStringLiteral("90"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(id));
  QVERIFY(std::abs(b.CornerMin().X() + 500) < 1e-6);
  QVERIFY(std::abs(b.CornerMax().Y() - 2000) < 1e-6);
}

void GuiPhase1::offsetToolInwards() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();
  auto rect = m.createRectangle(geom::SketchPlane::ground(), 2000, 2000);
  QTest::qWait(20);
  f.window.activateTool(MainWindow::ToolId::Offset);
  QTest::qWait(20);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 1000, 0)));  // inside the face
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, 300, 0)));
  type(f.vp, QStringLiteral("100"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  const doc::Object& offset = m.document().objects().back();
  QVERIFY(offset.id != *rect);
  QVERIFY(std::abs(geom::area(offset.shape) - 1800.0 * 1800.0) < 1e-3);
}

void GuiPhase1::groupIsSelectedAsAWhole() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 500, 500, 500);
  const doc::ObjectId b = f.box(1500, 0, 500, 500, 500);
  auto g = m.group({a, b});
  QVERIFY(g.ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(1750, 250, 500)));  // top of b
  QCOMPARE(f.window.selection().size(), std::size_t(1));
  QCOMPARE(f.window.selection().front(), *g);
}


void GuiPhase1::drawPushPullSaveReopen() {
  MainWindow window;
  window.resize(1280, 860);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  QTest::qWait(100);
  Viewport& vp = window.viewport();
  QVERIFY(vp.isInitialized());
  vp.setStandardView(Viewport::StandardView::Iso);
  {
    // Frame a ~5 m area so that the test geometry is comfortably visible.
    doc::CameraState cam;
    cam.eye = {9000, -9000, 9000};
    cam.target = {2000, 3000, 0};
    cam.up = {0, 0, 1};
    cam.perspective = true;
    vp.restoreCamera(cam);
  }
  doc::Modeler& m = window.modeler();

  // 1. Rectangle: click the origin (snaps exactly), indicate direction, type size.
  window.activateTool(MainWindow::ToolId::Rectangle);
  click(&vp, vp.toWidget(gp_Pnt(0, 0, 0)));
  move(&vp, vp.toWidget(gp_Pnt(3000, 3000, 0)));
  type(&vp, QStringLiteral("4000,6000"));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId id = m.document().objects().front().id;
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Face);
  QVERIFY(std::abs(geom::area(m.document().find(id)->shape) - 24e6) < 1e-6);

  // 2. Push/pull the face up by 3000 mm.
  window.activateTool(MainWindow::ToolId::PushPull);
  QTest::qWait(50);
  const QPoint center = vp.toWidget(gp_Pnt(2000, 3000, 0));
  click(&vp, center);
  move(&vp, vp.toWidget(gp_Pnt(2000, 3000, 1500)));
  type(&vp, QStringLiteral("3000"));
  QVERIFY2(window.lastError().isEmpty(), qPrintable(window.lastError()));
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Solid);
  EXPECT_BOX(m.document().find(id)->shape, 4000, 6000, 3000);
  QVERIFY(geom::validateShape(m.document().find(id)->shape, true).valid);

  // 3. Typing again right after the push edits the same operation (VCB re-edit).
  type(&vp, QStringLiteral("3500"));
  EXPECT_BOX(m.document().find(id)->shape, 4000, 6000, 3500);
  QVERIFY(m.undo());
  EXPECT_BOX(m.document().find(id)->shape, 4000, 6000, 3000);

  QTest::qWait(100);
  const QString shot = qEnvironmentVariable("SBR_SCREENSHOT");
  if (!shot.isEmpty()) QVERIFY(vp.dumpImage(shot));
  const QString windowShot = qEnvironmentVariable("SBR_WINDOW_SHOT");
  if (!windowShot.isEmpty()) {
    window.activateTool(MainWindow::ToolId::Select);
    window.setSelection({id});
    QTest::qWait(100);
    QVERIFY(window.screen()->grabWindow(0).copy(window.frameGeometry()).save(windowShot));
  }

  // 4. Save, reopen, and re-edit the extrusion parametrically (3000 -> 4500).
  const QString path = QDir::temp().filePath(QStringLiteral("sbr_gui_test.sbr"));
  QVERIFY(window.saveFile(path));
  QVERIFY(!window.isWindowModified());

  MainWindow reopened;
  reopened.resize(1280, 860);
  reopened.show();
  QVERIFY(QTest::qWaitForWindowExposed(&reopened));
  QVERIFY(reopened.openFile(path));
  doc::Modeler& m2 = reopened.modeler();
  QCOMPARE(m2.document().objects().size(), std::size_t(1));
  QCOMPARE(m2.document().objects().front().id, id);
  EXPECT_BOX(m2.document().objects().front().shape, 4000, 6000, 3000);
  QVERIFY(m2.setParameter(id, "op0.distance", 4500).ok());
  EXPECT_BOX(m2.document().find(id)->shape, 4000, 6000, 4500);
  QTest::qWait(50);
  const QString shot2 = qEnvironmentVariable("SBR_SCREENSHOT2");
  if (!shot2.isEmpty()) QVERIFY(reopened.viewport().dumpImage(shot2));
}

void GuiPhase1::lineToolClosesLoopIntoFace() {
  MainWindow window;
  window.resize(1280, 860);
  window.show();
  QVERIFY(QTest::qWaitForWindowExposed(&window));
  Viewport& vp = window.viewport();
  doc::CameraState cam;
  cam.eye = {500, 500, 20000};
  cam.target = {500, 500, 0};
  cam.up = {0, 1, 0};
  cam.perspective = false;
  vp.restoreCamera(cam);
  vp.fitObjects({});  // empty selection -> default framing around the origin
  doc::Modeler& m = window.modeler();

  window.activateTool(MainWindow::ToolId::Line);
  click(&vp, vp.toWidget(gp_Pnt(0, 0, 0)));
  const gp_Pnt corners[] = {gp_Pnt(1000, 0, 0), gp_Pnt(1000, 2000, 0), gp_Pnt(0, 2000, 0)};
  const QString lengths[] = {QStringLiteral("1000"), QStringLiteral("2m"), QStringLiteral("1000")};
  gp_Pnt last(0, 0, 0);
  for (int i = 0; i < 3; ++i) {
    // Indicate the direction roughly, the typed length gives the exact point.
    const gp_Vec dir(last, corners[i]);
    move(&vp, vp.toWidget(last.Translated(dir * 0.4)));
    type(&vp, lengths[i]);
    QVERIFY2(window.lastError().isEmpty(), qPrintable(window.lastError()));
    last = corners[i];
  }
  click(&vp, vp.toWidget(gp_Pnt(0, 0, 0)));  // close the loop on the start point
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::Object& face = m.document().objects().front();
  QCOMPARE(face.kind(), doc::ObjectKind::Face);
  QVERIFY(std::abs(geom::area(face.shape) - 2e6) < 1e-6);
}

void GuiPhase1::curveToolOpenAndClosed() {
  Fixture f;
  QVERIFY(f.setUp(2000, 1000));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Curve);
  // Open curve through three points, finished with Enter.
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 1500, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(4000, 0, 0)));
  QTest::keyClick(f.vp, Qt::Key_Return);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QCOMPARE(m.document().objects().front().kind(), doc::ObjectKind::Curve);
  const Bnd_Box open = geom::exactBounds(m.document().objects().front().shape);
  // The start snaps to the origin exactly; free clicks on the work plane are
  // quantised to pixels (about 18 mm per pixel at this zoom).
  QVERIFY(std::abs(open.CornerMin().X()) < 1e-6);
  QVERIFY(std::abs(open.CornerMax().X() - 4000) < 30.0);

  // Closed curve: click back on the first point.
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 3800, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 3000, 0)));
  QTest::qWait(30);
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  const TopoDS_Shape closed = m.document().objects().back().shape;
  QVERIFY(BRep_Tool::IsClosed(closed));
}

QTEST_MAIN(GuiPhase1)
#include "test_gui.moc"
