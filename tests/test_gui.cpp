// Phase 1 acceptance test driving the real main window with synthetic input:
// draw a rectangle, push/pull it 3000 mm, save, reopen and re-edit.

#include <cmath>

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
};

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

QTEST_MAIN(GuiPhase1)
#include "test_gui.moc"
