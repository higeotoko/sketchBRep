// Phase 1 acceptance test driving the real main window with synthetic input:
// draw a rectangle, push/pull it 3000 mm, save, reopen and re-edit.

#include <cmath>

#include <BRep_Tool.hxx>

#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QAction>
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
  void selectFacesEdgesAndWholeObject();
  void pushPullDoesNotSelectWholeObject();
  void eachToolHasItsCursor();
  void revolveToolAndTypedAngle();
  void pushPullDoubleClickRepeatsDistance();
  void orbitKeepsPointUnderCursorFixed();
  void drawOnWallAndPushInCutsOpening();
  void tapeMeasureReadsDistance();
  void circleToolTypedRadius();
  void groupEditModeAndLooseFaceDelete();
  void lineToolStaysOnWallPlane();
  void offsetToolOnSolidFace();
  void paintToolAndEyedropper();
  void moveSelectedFaceStretchesSolid();
  void eraserDeletesClickedObject();
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

namespace {
void doubleClick(QWidget* w, const QPoint& pos) {
  // Qt order: press, release, double-click, release.
  click(w, pos);
  send(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
  send(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}
}  // namespace

void GuiPhase1::selectFacesEdgesAndWholeObject() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::Select);
  const QPoint top = f.vp->toWidget(gp_Pnt(500, 500, 1000));

  click(f.vp, top);  // single click: only the face
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QCOMPARE(f.window.elementSelection().front().type, TopAbs_FACE);
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{box});  // owner for the panel

  doubleClick(f.vp, top);  // face + its 4 edges
  QCOMPARE(f.window.elementSelection().size(), std::size_t(5));
  if (const QString shot = qEnvironmentVariable("SBR_SELECT_SHOT"); !shot.isEmpty()) {
    QTest::qWait(100);
    QVERIFY(f.window.screen()->grabWindow(0).copy(f.window.frameGeometry()).save(shot));
  }

  auto deleteAction = [&] {
    for (QAction* a : f.window.findChildren<QAction*>()) {
      if (a->text() == QStringLiteral("削除")) return a;
    }
    return static_cast<QAction*>(nullptr);
  }();
  QVERIFY(deleteAction);
  // Faces cannot be deleted on their own: the solid stays.
  deleteAction->trigger();
  QVERIFY(!f.window.lastError().isEmpty());
  QVERIFY(m.document().find(box));

  click(f.vp, top);  // third click right after the double-click: whole object
  QCOMPARE(f.window.elementSelection().size(), std::size_t(6 + 12));
  deleteAction->trigger();
  QTest::qWait(30);
  QVERIFY(!m.document().find(box));
  QVERIFY(m.undo());

  QTest::qWait(500);  // let the triple-click window expire
  click(f.vp, f.vp->toWidget(gp_Pnt(5000, 5000, 0)));  // empty space clears
  QVERIFY(f.window.elementSelection().empty());
  QVERIFY(f.window.selection().empty());
}

void GuiPhase1::pushPullDoesNotSelectWholeObject() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::PushPull);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1500)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(box)).CornerMax().Z() - 1500) < 1e-6);
  QVERIFY(f.window.selection().empty());
  QVERIFY(f.window.elementSelection().empty());
}

void GuiPhase1::eachToolHasItsCursor() {
  Fixture f;
  QVERIFY(f.setUp());
  f.window.activateTool(MainWindow::ToolId::Select);
  QCOMPARE(f.vp->cursor().shape(), Qt::ArrowCursor);
  for (auto id : {MainWindow::ToolId::Line, MainWindow::ToolId::Rectangle, MainWindow::ToolId::PushPull,
                  MainWindow::ToolId::Move, MainWindow::ToolId::Rotate, MainWindow::ToolId::Offset,
                  MainWindow::ToolId::Curve, MainWindow::ToolId::Circle,
                  MainWindow::ToolId::Measure, MainWindow::ToolId::Revolve}) {
    f.window.activateTool(id);
    QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
    QVERIFY(!f.vp->cursor().pixmap().isNull());
  }
  // Orbiting shows a grabbing hand and restores the tool cursor afterwards.
  const QPoint c = f.vp->rect().center();
  send(f.vp, QEvent::MouseButtonPress, c, Qt::MiddleButton, Qt::MiddleButton);
  QCOMPARE(f.vp->cursor().shape(), Qt::ClosedHandCursor);
  send(f.vp, QEvent::MouseButtonRelease, c, Qt::MiddleButton, Qt::NoButton);
  QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
}

void GuiPhase1::revolveToolAndTypedAngle() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  // Vertical profile x 0..500, z 0..2000 in the XZ plane.
  auto profile = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, 0, 0), gp_Dir(0, -1, 0)), 500, 2000);
  QVERIFY(profile.ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::Revolve);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(250, 0, 1000)));   // the profile
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));        // axis start (origin snap)
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 2000)));     // axis end (blue axis inference)
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const double full = M_PI * 500 * 500 * 2000;
  QVERIFY(std::abs(geom::volume(m.document().worldShape(*profile)) - full) < full * 1e-6);
  type(f.vp, QStringLiteral("90"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(geom::volume(m.document().worldShape(*profile)) - full / 4) < full * 1e-6);
}

void GuiPhase1::pushPullDoubleClickRepeatsDistance() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId b = f.box(2000, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::PushPull);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1400)));
  type(f.vp, QStringLiteral("400"));
  doubleClick(f.vp, f.vp->toWidget(gp_Pnt(2500, 500, 1000)));
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(a)).CornerMax().Z() - 1400) < 1e-6);
  QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(b)).CornerMax().Z() - 1400) < 1e-6);
}

void GuiPhase1::orbitKeepsPointUnderCursorFixed() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  f.box(0, 0, 1000, 1000, 1000);
  const gp_Pnt corner(1000, 0, 1000);
  const QPoint start = f.vp->toWidget(corner);  // snaps to this vertex
  send(f.vp, QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton);
  for (int i = 1; i <= 10; ++i) {
    send(f.vp, QEvent::MouseMove, start + QPoint(8 * i, 3 * i), Qt::NoButton, Qt::MiddleButton);
  }
  send(f.vp, QEvent::MouseButtonRelease, start + QPoint(80, 30), Qt::MiddleButton, Qt::NoButton);
  const QPoint after = f.vp->toWidget(corner);
  QVERIFY2((after - start).manhattanLength() <= 2,
           qPrintable(QStringLiteral("pivot moved from %1,%2 to %3,%4")
                          .arg(start.x()).arg(start.y()).arg(after.x()).arg(after.y())));
  // Toolbar shows icons.
  for (QAction* a : f.window.findChildren<QAction*>()) {
    if (a->text() == QStringLiteral("プッシュ/プル")) QVERIFY(!a->icon().isNull());
  }
}

void GuiPhase1::drawOnWallAndPushInCutsOpening() {
  Fixture f;
  QVERIFY(f.setUp(2000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId wall = f.box(0, 0, 4000, 200, 3000);
  const double full = 4000.0 * 200 * 3000;

  // Rectangle tool: first corner on the wall's front (-Y) face.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 300)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1600, 0, 1500)));
  type(f.vp, QStringLiteral("900,2100"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  // Push/pull the outline into the wall: it becomes an opening.
  f.window.activateTool(MainWindow::ToolId::PushPull);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(1450, 0, 1350)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1450, 150, 1350)));
  type(f.vp, QStringLiteral("200"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId cut = m.document().objects().front().id;
  QVERIFY(std::abs(geom::volume(m.document().worldShape(cut)) - (full - 900.0 * 2100 * 200)) < 1.0);
  QVERIFY(!m.document().find(wall));

  // Typing again re-edits the depth (niche of 100 mm).
  type(f.vp, QStringLiteral("100"));
  QVERIFY(std::abs(geom::volume(m.document().worldShape(cut)) - (full - 900.0 * 2100 * 100)) < 1.0);

  if (const QString shot = qEnvironmentVariable("SBR_OPENING_SHOT"); !shot.isEmpty()) {
    type(f.vp, QStringLiteral("200"));
    f.window.activateTool(MainWindow::ToolId::Select);
    f.vp->fitAll();
    QTest::qWait(100);
    QVERIFY(f.window.screen()->grabWindow(0).copy(f.window.frameGeometry()).save(shot));
  }
}

void GuiPhase1::tapeMeasureReadsDistance() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 1000, 2000, 3000);
  const std::size_t undo = m.history().undoCount();
  f.window.activateTool(MainWindow::ToolId::Measure);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 3000)));     // top corner
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));     // visible bottom corner
  auto* tool = dynamic_cast<MeasureTool*>(f.window.activeTool());
  QVERIFY(tool && tool->lastDistance());
  QVERIFY(std::abs(*tool->lastDistance() - std::sqrt(1e6 + 9e6)) < 1e-6);
  QCOMPARE(m.history().undoCount(), undo);  // the model is untouched
}

void GuiPhase1::circleToolTypedRadius() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Circle);
  QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));  // centre at the origin
  move(f.vp, f.vp->toWidget(gp_Pnt(300, 300, 0)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId id = m.document().objects().front().id;
  QVERIFY(std::abs(geom::area(m.document().worldShape(id)) - M_PI * 500 * 500) < 1.0);
  type(f.vp, QStringLiteral("0.6m"));  // re-edit the radius right away
  QVERIFY(std::abs(geom::area(m.document().worldShape(id)) - M_PI * 600 * 600) < 1.0);
}

void GuiPhase1::groupEditModeAndLooseFaceDelete() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 500, 500, 500);
  const doc::ObjectId b = f.box(1500, 0, 500, 500, 500);
  auto g = m.group({a, b});
  QVERIFY(g.ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::Select);
  const QPoint topB = f.vp->toWidget(gp_Pnt(1750, 250, 500));

  doubleClick(f.vp, topB);  // enter the group
  QCOMPARE(f.window.editingGroup(), *g);
  QTest::qWait(500);
  click(f.vp, topB);  // inside: the face of b, not the group
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{b});

  // Geometry drawn now goes into the group.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(3300, 300, 0)));
  type(f.vp, QStringLiteral("400,400"));
  const doc::ObjectId rect = m.document().objects().back().id;
  QCOMPARE(m.document().find(rect)->parent, *g);

  // A loose profile can be deleted from its face selection.
  f.window.activateTool(MainWindow::ToolId::Select);
  QTest::qWait(500);
  click(f.vp, f.vp->toWidget(gp_Pnt(3200, 200, 0)));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{rect});
  for (QAction* action : f.window.findChildren<QAction*>()) {
    if (action->text() == QStringLiteral("削除")) action->trigger();
  }
  QTest::qWait(30);
  QVERIFY(!m.document().find(rect));

  // Esc leaves the group; the group is selected again.
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QVERIFY(f.window.editingGroup().empty());
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{*g});
}

void GuiPhase1::lineToolStaysOnWallPlane() {
  Fixture f;
  QVERIFY(f.setUp(2000, 0));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 4000, 200, 3000);
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 1000)));  // on the wall's front face
  click(f.vp, f.vp->toWidget(gp_Pnt(1800, 0, 1000)));
  // Off the wall (above its top): must stay on the wall's plane y = 0.
  click(f.vp, f.vp->toWidget(gp_Pnt(1800, 0, 3600)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  const doc::Object& line = m.document().objects().back();
  QCOMPARE(line.kind(), doc::ObjectKind::Curve);
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(line));
  QVERIFY2(std::abs(b.CornerMin().Y()) < 1e-6 && std::abs(b.CornerMax().Y()) < 1e-6,
           qPrintable(QStringLiteral("y %1..%2").arg(b.CornerMin().Y()).arg(b.CornerMax().Y())));
  QVERIFY(b.CornerMax().Z() > 3000);
}

void GuiPhase1::offsetToolOnSolidFace() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::Offset);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));  // centre of the top face
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 300, 1000)));
  type(f.vp, QStringLiteral("100"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  const doc::Object& inset = m.document().objects().back();
  QCOMPARE(inset.kind(), doc::ObjectKind::Face);
  QVERIFY(std::abs(geom::area(inset.shape) - 800.0 * 800.0) < 1e-3);
}

void GuiPhase1::paintToolAndEyedropper() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 500, 500, 500);
  const doc::ObjectId b = f.box(1500, 0, 500, 500, 500);
  f.window.activateTool(MainWindow::ToolId::Paint);
  QTest::qWait(30);
  auto* paint = dynamic_cast<PaintTool*>(f.window.activeTool());
  QVERIFY(paint);
  const doc::Color red{0.9, 0.1, 0.1};
  paint->setColor(red);
  click(f.vp, f.vp->toWidget(gp_Pnt(250, 250, 500)));
  QCOMPARE(m.document().find(a)->color, red);
  // Alt+click picks up b's colour, then painting a with it.
  const QPoint onB = f.vp->toWidget(gp_Pnt(1750, 250, 500));
  move(f.vp, onB);
  send(f.vp, QEvent::MouseButtonPress, onB, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
  send(f.vp, QEvent::MouseButtonRelease, onB, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
  QCOMPARE(paint->color(), m.document().find(b)->color);
  click(f.vp, f.vp->toWidget(gp_Pnt(250, 250, 500)));
  QCOMPARE(m.document().find(a)->color, m.document().find(b)->color);
}

void GuiPhase1::moveSelectedFaceStretchesSolid() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId id = f.box(0, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::Select);
  const QPoint top = f.vp->toWidget(gp_Pnt(500, 500, 1000));
  click(f.vp, top);
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  f.window.activateTool(MainWindow::ToolId::Move);
  click(f.vp, top);
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1300)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(id));
  QVERIFY(std::abs(b.CornerMax().Z() - 1500) < 1e-6);
  QVERIFY(std::abs(b.CornerMax().X() - 1000) < 1e-6);  // only stretched upwards
}

void GuiPhase1::eraserDeletesClickedObject() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 500, 500, 500);
  const doc::ObjectId b = f.box(1500, 0, 500, 500, 500);
  f.window.activateTool(MainWindow::ToolId::Eraser);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(1750, 250, 500)));
  QVERIFY(!m.document().find(b));
  QVERIFY(m.document().find(a));
  QVERIFY(m.undo());
  QVERIFY(m.document().find(b));
}

QTEST_MAIN(GuiPhase1)
#include "test_gui.moc"
