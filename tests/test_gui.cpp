// Phase 1 acceptance test driving the real main window with synthetic input:
// draw a rectangle, push/pull it 3000 mm, save, reopen and re-edit.

#include <cmath>

#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Lin.hxx>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <nlohmann/json.hpp>
#include <QImage>
#include <QSettings>
#include <QListWidget>
#include <QPushButton>
#include <QTabBar>
#include <QStandardPaths>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QAction>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QScreen>
#include <QtTest/QtTest>

#include "AiBridge.h"
#include "MainWindow.h"
#include "Viewport.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/ChangeSummary.h"
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
  void initTestCase() {
    // Keep app settings (e.g. AI access) out of the user's real configuration.
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
  }
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
  void tapeMeasurePullsGuideOffEdge();
  void protractorPlacesAngledGuide();
  void gridLinesSnapAtCrossings();
  void copyPasteFollowsCursor();
  void polygonToolSidesAndRadius();
  void exportImageWritesPng();
  void typedUnitsDoNotTriggerShortcuts();
  void scaleToolFactorAndLength();
  void aiReadsKeepDrawingInProgress();
  void proposalShowsRotationAndHide();
  void benchViewportSync();
  void incrementalSyncFollowsGroupsAndUndo();
  void componentPlacementsShareOneDisplay();
  void circleToolTypedRadius();
  void groupEditModeAndLooseFaceDelete();
  void lineToolStaysOnWallPlane();
  void offsetToolOnSolidFace();
  void paintToolAndEyedropper();
  void moveSelectedFaceStretchesSolid();
  void eraserDeletesClickedObject();
  void aiBridgeOverLocalSocket();
  void arcToolSemicircleAndBulgeEdit();
  void scenesSaveRestoreAndPersist();
  void dimensionAndTextTools();
  void lineAcrossWallSplitsFace();
  void arcAndLinesCloseIntoFace();
  void sectionPlaneCutsView();
  void sectionToolAndTagsPanel();
  void pushPullCylinderSide();
  void lineAcrossRectangleSplitsProfile();
  void gate2InputConsistency();
  void boxSelectionWindowAndCrossing();
  void aiProposalApplyAndReject();
  void componentEditInPlace();
  void followMeAroundTopFace();
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
                  MainWindow::ToolId::Measure, MainWindow::ToolId::Revolve, MainWindow::ToolId::Arc,
                  MainWindow::ToolId::Paint, MainWindow::ToolId::Eraser, MainWindow::ToolId::Protractor,
                  MainWindow::ToolId::Polygon, MainWindow::ToolId::Scale}) {
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

void GuiPhase1::tapeMeasurePullsGuideOffEdge() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 6000, 2000, 3000);
  f.window.activateTool(MainWindow::ToolId::Measure);
  // Start on the middle of the top front edge (along X), click on the top
  // face: a guide parallel to the edge at the clicked offset.
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 3000)));  // mid-edge, away from the midpoint
  move(f.vp, f.vp->toWidget(gp_Pnt(1100, 700, 3000)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1100, 700, 3000)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  std::vector<doc::GuideLine> guides;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() == doc::ObjectKind::Guide) guides.push_back(std::get<doc::GuideLine>(o.recipe.base));
  }
  QCOMPARE(guides.size(), std::size_t(1));
  QVERIFY(guides[0].direction.IsParallel(gp::DX(), 1e-9));
  QVERIFY2(std::abs(guides[0].point.Y() - 700) < 100.0, qPrintable(QString::number(guides[0].point.Y())));
  QVERIFY(std::abs(guides[0].point.Z() - 3000) < 1e-6);

  // Typed offset: start on the same edge, show the side, type the distance.
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 3000)));  // mid-edge, away from the midpoint
  move(f.vp, f.vp->toWidget(gp_Pnt(2500, 600, 3000)));
  type(f.vp, QStringLiteral("1200"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const doc::Object& last = m.document().objects().back();
  QCOMPARE(last.kind(), doc::ObjectKind::Guide);
  const gp_Pnt& p = std::get<doc::GuideLine>(last.recipe.base).point;
  QVERIFY2(std::abs(p.Y() - 1200) < 1e-6 && std::abs(p.Z() - 3000) < 1e-6, qPrintable(QString("%1 %2").arg(p.Y()).arg(p.Z())));

  QVERIFY(m.clearGuides().ok());
  QCOMPARE(m.document().objects().size(), std::size_t(1));
}

void GuiPhase1::protractorPlacesAngledGuide() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Protractor);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));     // centre: origin
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));  // base: red axis
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 900, 0))); // counter-clockwise side
  type(f.vp, QStringLiteral("30"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::Object& g = m.document().objects().front();
  QCOMPARE(g.kind(), doc::ObjectKind::Guide);
  const auto& line = std::get<doc::GuideLine>(g.recipe.base);
  QVERIFY(line.point.Distance(gp::Origin()) < 1e-6);
  QVERIFY(line.direction.IsParallel(gp_Dir(std::cos(M_PI / 6), std::sin(M_PI / 6), 0), 1e-9));

  // The line tool snaps onto the guide, also far from where it was placed.
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::Line);
  const gp_Pnt onGuide = gp::Origin().Translated(gp_Vec(line.direction) * 3000);
  const gp_Lin guideLine(line.point, line.direction);
  click(f.vp, f.vp->toWidget(onGuide) + QPoint(1, 1));
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, -1000, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  bool touches = false;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() != doc::ObjectKind::Curve) continue;
    for (TopExp_Explorer v(m.document().worldShape(o), TopAbs_VERTEX); v.More(); v.Next()) {
      touches |= guideLine.Distance(BRep_Tool::Pnt(TopoDS::Vertex(v.Current()))) < 1e-6;
    }
  }
  QVERIFY(touches);
}

void GuiPhase1::gridLinesSnapAtCrossings() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  QVERIFY2(f.window.createGrid(QStringLiteral("3600*2"), QStringLiteral("3.6m")), qPrintable(f.window.lastError()));
  QTest::qWait(30);
  // A rectangle from crossing X2/Y1 to crossing X3/Y2, clicked a little off.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  click(f.vp, f.vp->toWidget(gp_Pnt(3600, 0, 0)) + QPoint(2, -2));
  click(f.vp, f.vp->toWidget(gp_Pnt(7200, 3600, 0)) + QPoint(-2, 2));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const doc::Object& room = m.document().objects().back();
  QCOMPARE(room.kind(), doc::ObjectKind::Face);
  QVERIFY(std::abs(geom::area(m.document().worldShape(room)) - 3600.0 * 3600.0) < 1e-3);
  QVERIFY(!f.window.createGrid(QStringLiteral("3600*a"), QString()));
  QVERIFY(!f.window.lastError().isEmpty());
}

void GuiPhase1::copyPasteFollowsCursor() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.window.paste();  // nothing copied yet
  QVERIFY(!f.window.lastError().isEmpty());
  f.window.setSelection({box});
  QVERIFY(f.window.copySelection());
  f.window.paste();
  QVERIFY(dynamic_cast<PasteTool*>(f.window.activeTool()));
  move(f.vp, f.vp->toWidget(gp_Pnt(2500, 0, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);  // changed my mind
  QVERIFY(dynamic_cast<SelectTool*>(f.window.activeTool()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));

  f.window.paste();
  // Onto the box's top corner (endpoint snap): stacked exactly.
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 1000)) + QPoint(2, 1));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 1000)) + QPoint(2, 1));
  QTest::qWait(30);
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  const doc::ObjectId copy = m.document().objects().back().id;
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{copy});
  QVERIFY(dynamic_cast<SelectTool*>(f.window.activeTool()));
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(copy));
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  QVERIFY2(std::abs(x0 - 1000) < 1e-6 && std::abs(y0) < 1e-6 && std::abs(z0 - 1000) < 1e-6, qPrintable(QString("%1 %2 %3").arg(x0).arg(y0).arg(z0)));

  // Cut + paste in place: same spot, one undo brings the cut back.
  QVERIFY(f.window.cutSelection());
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QVERIFY(f.window.pasteInPlace());
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(1));
}

void GuiPhase1::polygonToolSidesAndRadius() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Polygon);
  type(f.vp, QStringLiteral("8s"));
  auto* tool = dynamic_cast<PolygonTool*>(f.window.activeTool());
  QVERIFY(tool && tool->sides() == 8);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(300, 300, 0)));
  type(f.vp, QStringLiteral("1000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId id = m.document().objects().front().id;
  QVERIFY(std::abs(geom::area(m.document().worldShape(id)) - 2.0 * std::sqrt(2.0) * 1e6) < 1e-3);
  type(f.vp, QStringLiteral("4s"));  // re-edit the last polygon
  QVERIFY(std::abs(geom::area(m.document().worldShape(id)) - 2.0e6) < 1e-3);
  type(f.vp, QStringLiteral("2s"));
  QVERIFY(!f.window.lastError().isEmpty());
}

void GuiPhase1::exportImageWritesPng() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  f.box(0, 0, 1000, 1000, 1000);
  const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("sbr-export-test.png"));
  QFile::remove(path);
  QVERIFY2(f.window.exportImage(path), qPrintable(f.window.lastError()));
  QImage image(path);
  QVERIFY(!image.isNull());
  QVERIFY(image.width() > 100 && image.height() > 100);
}

void GuiPhase1::typedUnitsDoNotTriggerShortcuts() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateWindow();  // shortcuts only fire in the active window
  QTest::qWait(50);
  QVERIFY(f.window.isActiveWindow());
  f.window.activateTool(MainWindow::ToolId::Circle);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(300, 300, 0)));
  type(f.vp, QStringLiteral("0.5m"));  // "m" must not pick the move tool
  QVERIFY(dynamic_cast<CircleTool*>(f.window.activeTool()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QVERIFY(std::abs(geom::area(m.document().objects().front().shape) - M_PI * 500 * 500) < 1.0);
  // A lone letter still switches tools.
  QTest::keyClick(f.vp, Qt::Key_M);
  QTest::qWait(30);
  QVERIFY(dynamic_cast<MoveTool*>(f.window.activeTool()));
}

void GuiPhase1::scaleToolFactorAndLength() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::Scale);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));  // nothing selected yet
  QVERIFY(!f.window.lastError().isEmpty());
  f.window.setSelection({box});
  const std::size_t steps = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));     // fixed point: origin
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));  // reference: bottom corner
  move(f.vp, f.vp->toWidget(gp_Pnt(1800, 0, 0)));
  type(f.vp, QStringLiteral("2"));
  QCOMPARE(boxMismatch(m.document().worldShape(box), 2000, 2000, 2000), QString());
  type(f.vp, QStringLiteral("3m"));  // re-edit: reference length becomes 3 m
  QCOMPARE(boxMismatch(m.document().worldShape(box), 3000, 3000, 3000), QString());
  QCOMPARE(m.history().undoCount(), steps + 1);
  QVERIFY(dynamic_cast<ScaleTool*>(f.window.activeTool()));
}

// Review C / R04: AI requests that only read must not reset a half-drawn
// shape; model writes still interrupt it (cleanly, before running).
void GuiPhase1::aiReadsKeepDrawingInProgress() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(3000, 3000, 500, 500, 500);
  AiBridge* bridge = f.window.aiBridge();
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));  // first corner placed
  const std::size_t steps = m.history().undoCount();
  const std::string reads[] = {
      R"({"command": "list_objects"})",
      R"({"command": "get_object", "args": {"id": ")" + box + R"("}})",
      R"({"command": "describe"})",
      R"({"command": "get_selection"})",
      R"({"command": "list_scenes"})",
      R"({"command": "proposal_status"})",
      R"({"command": "preview", "args": {"commands": [{"command": "delete", "args": {"ids": [")" + box + R"("]}}]}})",
  };
  for (const std::string& r : reads) {
    const auto reply = nlohmann::json::parse(bridge->execute(r));
    QVERIFY2(reply.value("ok", false), r.c_str());
  }
  QCOMPARE(m.history().undoCount(), steps);
  // The rectangle started before the reads is finished with typed sizes.
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, 1000, 0)));
  type(f.vp, QStringLiteral("2000,1000"));
  QCOMPARE(m.history().undoCount(), steps + 1);
  const doc::Object& rect = m.document().objects().back();
  QCOMPARE(rect.kind(), doc::ObjectKind::Face);
  const double area = geom::area(m.document().worldShape(rect));
  QVERIFY2(std::abs(area - 2000.0 * 1000.0) < 1.0, qPrintable(QString::number(area)));

  // A write interrupts the drawing first: the next click starts anew.
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 2000, 0)));
  const auto reply = nlohmann::json::parse(bridge->execute(
      R"({"command": "create_box", "args": {"origin": [6000, 0, 0], "width": 100, "depth": 100, "height": 100}})"));
  QVERIFY(reply.value("ok", false));
  const std::size_t afterWrite = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 3000, 0)));  // only a first corner now
  QCOMPARE(m.history().undoCount(), afterWrite);
}

// Review B / R03: a proposal is summarised from the change set its trial
// run recorded, so a rotation about the origin or a hide is not lost, and
// applying it commits the same change set.
void GuiPhase1::proposalShowsRotationAndHide() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId other = f.box(3000, 0, 500, 500, 500);
  AiBridge* bridge = f.window.aiBridge();
  const std::string request =
      R"({"command": "propose", "args": {"commands": [
           {"command": "rotate", "args": {"ids": [")" + box + R"("], "axis_origin": [0, 0, 0], "angle": 90}},
           {"command": "hide", "args": {"ids": [")" + other + R"("]}}]}})";
  const auto reply = nlohmann::json::parse(bridge->execute(request));
  QVERIFY2(reply.value("ok", false), reply.dump().c_str());
  const auto& result = reply["result"];
  QCOMPARE(result["changed"].get<int>(), 2);
  QVERIFY(f.window.hasPendingProposal());
  QVERIFY(f.vp->hasProposalGhost());
  QCOMPARE(m.document().find(box)->visible, true);  // nothing applied yet

  f.window.applyProposal();
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(!m.document().find(other)->visible);
  QVERIFY(m.history().lastCommand());
  const doc::ChangeSummary committed = doc::summarize(*m.history().lastCommand());
  QCOMPARE(committed.modified.size(), std::size_t(2));
  QVERIFY(committed.modified[0].aspects & doc::kPlacement);
  QVERIFY(committed.modified[1].aspects & doc::kVisibility);
}

// Performance plan step 0: viewport cost of a model with many placements.
// Skipped unless SBR_BENCH_N is set (e.g. SBR_BENCH_N=1000); prints timings.
void GuiPhase1::benchViewportSync() {
  const int n = qEnvironmentVariableIntValue("SBR_BENCH_N");
  if (n <= 0) QSKIP("set SBR_BENCH_N to run the viewport benchmark");
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId window = f.box(0, 0, 1200, 150, 1500);
  auto instance = m.makeComponent({window}, "Window");
  QVERIFY(instance.ok());
  const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
  gp_Trsf dx, dy;
  dx.SetTranslation(gp_Vec(1800, 0, 0));
  dy.SetTranslation(gp_Vec(0, 3000, 0));
  auto row = m.copy({*instance}, dx, columns - 1);
  QVERIFY(row.ok());
  std::vector<doc::ObjectId> firstRow{*instance};
  firstRow.insert(firstRow.end(), row->begin(), row->end());
  QVERIFY(m.copy(firstRow, dy, (n + columns - 1) / columns - 1).ok());
  auto timed = [](const std::function<void()>& fn) {
    QElapsedTimer t;
    t.start();
    fn();
    return t.nsecsElapsed() / 1e6;
  };
  const double initial = timed([&] { f.vp->syncNow(); });
  const doc::ObjectId last = m.document().objects().back().id;
  const double moveCommit = timed([&] { m.move({last}, gp_Vec(10, 0, 0)); });
  const double moveSync = timed([&] { f.vp->syncNow(); });
  const double afterMove = moveCommit + moveSync;
  const double selectViewport = timed([&] { f.vp->setSelection({last}); });
  const double afterSelect = timed([&] { f.window.setSelection({}); });
  const double repaint = timed([&] { f.vp->repaint(); });
  qInfo("bench detail move_commit_ms=%.1f move_sync_ms=%.1f select_viewport_ms=%.1f repaint_ms=%.1f", moveCommit,
        moveSync, selectViewport, repaint);
  const double redraw = timed([&] {
    f.vp->fitAll();
    QCoreApplication::processEvents();
  });
  qInfo("bench viewport n=%d objects=%zu initial_sync_ms=%.1f move_one_sync_ms=%.1f select_ms=%.1f fit_redraw_ms=%.1f",
        n, m.document().objects().size(), initial, afterMove, afterSelect, redraw);
}

// Performance step 2: the viewport only updates changed objects; moving a
// group must still move what its children show and snap to, and undo,
// hiding a tag and deleting must come back exactly.
void GuiPhase1::incrementalSyncFollowsGroupsAndUndo() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId b = f.box(2000, 0, 1000, 1000, 1000);
  auto g = m.group({a, b});
  QVERIFY(g.ok());
  f.vp->syncNow();
  auto snapsTo = [&](const gp_Pnt& p) {
    const snap::SnapResult r = f.vp->snap(f.vp->toWidget(p), std::nullopt, std::nullopt, std::nullopt);
    return r.kind == snap::SnapKind::Endpoint && r.point.Distance(p) < 1e-6;
  };
  QVERIFY(snapsTo(gp_Pnt(3000, 0, 1000)));
  QVERIFY(m.move({*g}, gp_Vec(0, 0, 500)).ok());  // only the group changes
  f.vp->syncNow();
  QVERIFY(snapsTo(gp_Pnt(3000, 0, 1500)));
  QVERIFY(!snapsTo(gp_Pnt(3000, 0, 1000)));
  QVERIFY(m.undo());
  f.vp->syncNow();
  QVERIFY(snapsTo(gp_Pnt(3000, 0, 1000)));
  // Tag visibility is a full resync; deleting removes from the snap index.
  m.setTagVisible("Default", false);
  f.vp->syncNow();
  QVERIFY(!snapsTo(gp_Pnt(3000, 0, 1000)));
  m.setTagVisible("Default", true);
  QVERIFY(m.remove({b}).ok());
  f.vp->syncNow();
  QVERIFY(!snapsTo(gp_Pnt(3000, 0, 1000)));
  QVERIFY(snapsTo(gp_Pnt(1000, 0, 1000)));
}

// Performance step 2: placements of one component share a display copy;
// editing the definition replaces it for all of them.
void GuiPhase1::componentPlacementsShareOneDisplay() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  auto instance = m.makeComponent({box}, "Window");
  QVERIFY(instance.ok());
  gp_Trsf dx;
  dx.SetTranslation(gp_Vec(2000, 0, 0));
  QVERIFY(m.copy({*instance}, dx, 3).ok());
  f.vp->syncNow();
  QCOMPARE(f.vp->sharedDisplayCount(), std::size_t(1));
  auto snapsTo = [&](const gp_Pnt& p) {
    const snap::SnapResult r = f.vp->snap(f.vp->toWidget(p), std::nullopt, std::nullopt, std::nullopt);
    return r.kind == snap::SnapKind::Endpoint && r.point.Distance(p) < 1e-6;
  };
  QVERIFY(snapsTo(gp_Pnt(7000, 0, 1000)));  // corner of the last placement

  const auto& placed = std::get<doc::ComponentInstance>(m.document().find(*instance)->recipe.base);
  const std::string child = m.document().findDefinition(placed.definitionId)->objects.front().id;
  QVERIFY(m.setDefinitionParameter(placed.definitionId, child, "op0.distance", 2000).ok());
  f.vp->syncNow();
  QCOMPARE(f.vp->sharedDisplayCount(), std::size_t(1));  // the old copy is released
  QVERIFY(snapsTo(gp_Pnt(7000, 0, 2000)));
  QVERIFY(m.remove({*instance}).ok());
  f.vp->syncNow();
  QCOMPARE(f.vp->sharedDisplayCount(), std::size_t(1));  // three placements left
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

void GuiPhase1::aiBridgeOverLocalSocket() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  AiBridge* bridge = f.window.aiBridge();
  QVERIFY(!bridge->isEnabled());  // off by default

  QLocalSocket refused;
  refused.connectToServer(bridge->serverName());
  QVERIFY(!refused.waitForConnected(200));

  f.window.setAiAccess(true);
  QVERIFY(bridge->isEnabled());
  QLocalSocket socket;
  socket.connectToServer(bridge->serverName());
  QTRY_VERIFY(socket.state() == QLocalSocket::ConnectedState);

  auto request = [&](const QByteArray& line) {
    socket.write(line + '\n');
    socket.flush();
    QByteArray reply;
    QTest::qWaitFor([&] { return socket.canReadLine(); }, 5000);
    if (socket.canReadLine()) reply = socket.readLine();
    return QJsonDocument::fromJson(reply).object();
  };

  QJsonObject r = request(R"({"command": "create_box", "args": {"origin": [0, 0, 0], "width": 1000, "depth": 1000, "height": 2000}})");
  QVERIFY2(r.value("ok").toBool(), qPrintable(QJsonDocument(r).toJson()));
  const doc::ObjectId id = r.value("result").toObject().value("id").toString().toStdString();
  QVERIFY(m.document().find(id));
  QCOMPARE(QString::fromStdString(m.history().undoName()), QStringLiteral("Box"));

  // App-only commands: select, then read the selection back.
  r = request(QByteArray(R"({"command": "select", "args": {"ids": [")") + id.c_str() + R"("]}})");
  QVERIFY(r.value("ok").toBool());
  QCOMPARE(f.window.selection().size(), std::size_t(1));
  r = request(R"({"command": "get_selection"})");
  QCOMPARE(r.value("result").toObject().value("ids").toArray().first().toString().toStdString(), id);
  r = request(R"({"command": "capture_view"})");
  QVERIFY(r.value("ok").toBool());
  QVERIFY(QFileInfo(r.value("result").toObject().value("image_path").toString()).size() > 0);

  // File commands are not available to the agent; the user's Ctrl+Z undoes AI edits.
  r = request(R"({"command": "save", "args": {"path": "x.sbr"}})");
  QVERIFY(!r.value("ok").toBool());
  QVERIFY(m.undo());
  QVERIFY(!m.document().find(id));

  f.window.setAiAccess(false);
  QVERIFY(!bridge->isEnabled());
  QTRY_VERIFY(socket.state() != QLocalSocket::ConnectedState);
}

void GuiPhase1::arcToolSemicircleAndBulgeEdit() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Arc);
  QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));        // start at the origin
  move(f.vp, f.vp->toWidget(gp_Pnt(1200, 0, 0)));      // direction along X
  type(f.vp, QStringLiteral("2000"));                   // chord length
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, 990, 0)));    // near a semicircle: snaps to it
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 990, 0)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId id = m.document().objects().front().id;
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Face);  // segment face by default
  QVERIFY(std::abs(geom::area(m.document().worldShape(id)) - M_PI * 1000 * 1000 / 2) < 1.0);

  type(f.vp, QStringLiteral("500"));  // re-edit the bulge right away
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(id));
  QVERIFY(std::abs(b.CornerMax().Y() - 500) < 1e-6);

  // Ctrl: arc only (a curve), drawn on the other side with a typed bulge.
  QTest::keyClick(f.vp, Qt::Key_Control);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, -2000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, -2000, 0)));
  type(f.vp, QStringLiteral("2000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, -2300, 0)));
  type(f.vp, QStringLiteral("300"));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  const doc::ObjectId arc = m.document().objects().back().id;
  QCOMPARE(m.document().find(arc)->kind(), doc::ObjectKind::Curve);
  const Bnd_Box ab = geom::exactBounds(m.document().worldShape(arc));
  // The start click is free (no snap), so check the exact typed sizes.
  QVERIFY(std::abs(ab.CornerMax().X() - ab.CornerMin().X() - 2000) < 1e-6);
  QVERIFY(std::abs(ab.CornerMax().Y() - ab.CornerMin().Y() - 300) < 1e-6);
  const auto* recipe = std::get_if<doc::ArcCurve>(&m.document().find(arc)->recipe.base);
  QVERIFY(recipe && recipe->bulge < 0);  // on the -Y side, where the mouse was
}

void GuiPhase1::scenesSaveRestoreAndPersist() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  f.box(0, 0, 1000, 1000, 1000);
  doc::CameraState a;
  f.vp->storeCamera(a);
  f.window.addScene(QStringLiteral("外観"));
  QCOMPARE(f.window.sceneBar()->count(), 1);
  QVERIFY(f.window.isWindowModified());

  f.window.viewport().setStandardView(Viewport::StandardView::Top);
  doc::CameraState top;
  f.vp->storeCamera(top);
  QVERIFY(std::abs(top.eye[2] - a.eye[2]) > 1.0 || std::abs(top.eye[0] - a.eye[0]) > 1.0);
  QVERIFY(f.window.goToScene(0));
  doc::CameraState back;
  f.vp->storeCamera(back);
  for (int i = 0; i < 3; ++i) QVERIFY(std::abs(back.eye[i] - a.eye[i]) < 1e-3);

  // Scenes are saved with the file; AI clients can use them too.
  const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("sbr_scenes_test.sbr"));
  QVERIFY(f.window.saveFile(path));
  MainWindow other;
  other.show();
  QVERIFY(QTest::qWaitForWindowExposed(&other));
  QVERIFY(other.openFile(path));
  QCOMPARE(other.sceneBar()->count(), 1);
  QCOMPARE(other.sceneBar()->tabText(0), QStringLiteral("外観"));
  const auto r = QJsonDocument::fromJson(QByteArray::fromStdString(
                     other.aiBridge()->execute(R"({"command": "go_to_scene", "args": {"name": "外観"}})")))
                     .object();
  QVERIFY2(r.value("ok").toBool(), qPrintable(QJsonDocument(r).toJson()));
  QFile::remove(path);
}

void GuiPhase1::dimensionAndTextTools() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 2000, 1000, 1000);

  // Dimension: two corners, then the offset typed after showing the side.
  f.window.activateTool(MainWindow::ToolId::Dimension);
  QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, -400, 0)));
  type(f.vp, QStringLiteral("600"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const doc::Object* dim = nullptr;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() == doc::ObjectKind::Annotation) dim = &o;
  }
  QVERIFY(dim);
  const doc::ObjectId dimId = dim->id;
  const auto* recipe = std::get_if<doc::LinearDimension>(&dim->recipe.base);
  QVERIFY(recipe);
  QVERIFY(std::abs(recipe->offset.Y() + 600) < 1e-6);
  QTest::qWait(30);
  QCOMPARE(f.vp->annotationText(dimId), QStringLiteral("2,000"));

  // Typing right after replaces the text.
  f.window.measurementBox()->setText(QStringLiteral("2,000 (芯々)"));
  emit f.window.measurementBox()->returnPressed();
  QTest::qWait(30);
  QCOMPARE(f.vp->annotationText(dimId), QStringLiteral("2,000 (芯々)"));

  // Text on the top face: its area is the default, then replaced (IME text box).
  f.window.activateTool(MainWindow::ToolId::Text);
  const QPoint top = f.vp->toWidget(gp_Pnt(1000, 500, 1000));
  click(f.vp, top);
  click(f.vp, top);  // same spot: no leader
  QCOMPARE(f.window.measurementBox()->text(), QStringLiteral("2.00 m²"));
  QCOMPARE(f.window.focusWidget(), static_cast<QWidget*>(f.window.measurementBox()));
  const doc::ObjectId note = m.document().objects().back().id;
  QVERIFY(std::get_if<doc::TextNote>(&m.document().find(note)->recipe.base));
  f.window.measurementBox()->setText(QStringLiteral("会議室 2.00 m²"));
  emit f.window.measurementBox()->returnPressed();
  QTest::qWait(30);
  QCOMPARE(f.vp->annotationText(note), QStringLiteral("会議室 2.00 m²"));
  QVERIFY(m.undo());
  QTest::qWait(30);
  QCOMPARE(f.vp->annotationText(note), QStringLiteral("2.00 m²"));
}

void GuiPhase1::lineAcrossWallSplitsFace() {
  Fixture f;
  QVERIFY(f.setUp(2000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId wall = f.box(0, 0, 4000, 200, 3000);
  f.window.activateTool(MainWindow::ToolId::Line);
  // From the left edge to the right edge of the front face, at mid height.
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 1500)));
  click(f.vp, f.vp->toWidget(gp_Pnt(4000, 0, 1500)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));  // no separate line object
  int faces = 0;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(wall)), TopAbs_FACE); e.More(); e.Next()) ++faces;
  QCOMPARE(faces, 7);
  QCOMPARE(QString::fromStdString(m.history().undoName()), QStringLiteral("Split Face"));

  // Push/pull the upper half of the front face outwards.
  f.window.activateTool(MainWindow::ToolId::PushPull);
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 2500)));
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, -300, 2500)));
  type(f.vp, QStringLiteral("300"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const double v = geom::volume(m.document().worldShape(*m.document().find(wall)));
  QVERIFY2(std::abs(v - (4000.0 * 200 * 3000 + 4000.0 * 1500 * 300)) < 1.0, qPrintable(QString::number(v)));
}

void GuiPhase1::arcAndLinesCloseIntoFace() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();
  // An open semicircle (Ctrl = arc only) bulging to -Y...
  f.window.activateTool(MainWindow::ToolId::Arc);
  QTest::keyClick(f.vp, Qt::Key_Control);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1200, 0, 0)));
  type(f.vp, QStringLiteral("2000"));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, -990, 0)));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QCOMPARE(m.document().objects().front().kind(), doc::ObjectKind::Curve);

  // ...closed by three lines back to the arc's start: becomes one face.
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, 1200, 0)));
  type(f.vp, QStringLiteral("2000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(800, 2000, 0)));
  type(f.vp, QStringLiteral("2000"));
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::Object& face = m.document().objects().front();
  QCOMPARE(face.kind(), doc::ObjectKind::Face);
  const double a = geom::area(m.document().worldShape(face));
  QVERIFY2(std::abs(a - (2000.0 * 2000 + M_PI * 1000 * 1000 / 2)) < 1.0, qPrintable(QString::number(a)));
  // F-08: the closing line and the face are one undo step.
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QCOMPARE(m.document().objects().front().kind(), doc::ObjectKind::Curve);
}

void GuiPhase1::sectionPlaneCutsView() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 2000, 2000, 2000);
  QTest::qWait(30);
  const QPoint top = f.vp->toWidget(gp_Pnt(1000, 1000, 2000));
  auto hit = f.vp->faceAt(top);
  QVERIFY(hit && std::abs(hit->first.Z() - 2000) < 1.0);

  // Cut away everything above 1 m: picking now reaches inside the box.
  doc::SectionPlane plane;
  plane.origin = {0, 0, 1000};
  plane.normal = {0, 0, 1};
  m.setSection(plane);
  QTest::qWait(50);
  f.vp->redraw();
  if (qEnvironmentVariableIsSet("SBR_SCREENSHOT_DIR")) {
    f.vp->dumpImage(QDir(qEnvironmentVariable("SBR_SCREENSHOT_DIR")).filePath(QStringLiteral("section.png")));
  }
  hit = f.vp->faceAt(top);
  QVERIFY2(!hit || hit->first.Z() < 1000.0 + 1.0, hit ? qPrintable(QString::number(hit->first.Z())) : "");

  m.setSection(std::nullopt);
  QTest::qWait(50);
  hit = f.vp->faceAt(top);
  QVERIFY(hit && std::abs(hit->first.Z() - 2000) < 1.0);
}

void GuiPhase1::sectionToolAndTagsPanel() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 2000, 2000, 2000);
  QVERIFY(m.createDimension(gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Vec(0, -500, 0)).ok());
  QTest::qWait(30);

  // Section tool: click the top face, cut 800 below it.
  f.window.activateTool(MainWindow::ToolId::Section);
  QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 1000, 2000)));
  type(f.vp, QStringLiteral("800"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().settings().sections.size(), std::size_t(1));
  const auto& plane = m.document().settings().sections.front();
  QVERIFY(std::abs(plane.origin[2] - 1200) < 1e-6);
  QVERIFY(std::abs(plane.normal[2] - 1) < 1e-9);
  QVERIFY(m.sectionVisible());
  QVERIFY(f.window.isWindowModified());

  // Tags panel lists Default and Annotations; unticking hides the dimension.
  QTest::qWait(30);
  QListWidget* tags = f.window.tagList();
  QCOMPARE(tags->count(), 2);
  QListWidgetItem* annotations = nullptr;
  for (int i = 0; i < tags->count(); ++i) {
    if (tags->item(i)->data(Qt::UserRole).toString() == QStringLiteral("Annotations")) annotations = tags->item(i);
  }
  QVERIFY(annotations);
  annotations->setCheckState(Qt::Unchecked);
  QTest::qWait(30);
  QVERIFY(m.document().settings().hiddenTags.count("Annotations"));
  QVERIFY(m.document().isVisible(box));

  // Saved and reopened with the section and hidden tag.
  const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("sbr_section_test.sbr"));
  QVERIFY(f.window.saveFile(path));
  doc::Modeler loaded;
  QVERIFY(loaded.load(path.toStdString()).ok());
  QCOMPARE(loaded.document().settings().sections.size(), std::size_t(1));
  QVERIFY(loaded.document().settings().hiddenTags.count("Annotations"));
  QFile::remove(path);
}

void GuiPhase1::pushPullCylinderSide() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  auto c = m.createCircle(geom::SketchPlane::ground(), 500);
  QVERIFY(m.extrude(*c, 1000).ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::PushPull);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, -500, 500)));  // the side, facing the camera
  move(f.vp, f.vp->toWidget(gp_Pnt(0, -560, 500)));
  type(f.vp, QStringLiteral("100"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const auto* circle = std::get_if<doc::CircleProfile>(&m.document().find(*c)->recipe.base);
  QVERIFY(circle);
  QVERIFY2(std::abs(circle->radius - 600) < 1e-6, qPrintable(QString::number(circle->radius)));
}

void GuiPhase1::lineAcrossRectangleSplitsProfile() {
  Fixture f;
  QVERIFY(f.setUp(1000, 500));
  doc::Modeler& m = f.window.modeler();
  QVERIFY(m.createRectangle(geom::SketchPlane::ground(), 2000, 1000).ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::Line);
  // From the midpoint of the front edge to the midpoint of the back edge.
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 1000, 0)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  for (const doc::Object& o : m.document().objects()) {
    QCOMPARE(o.kind(), doc::ObjectKind::Face);
    QVERIFY(std::abs(geom::area(m.document().worldShape(o)) - 1000.0 * 1000) < 1.0);
  }
  // The right half can be pushed up on its own.
  f.window.activateTool(MainWindow::ToolId::PushPull);
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 500, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 500, 300)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  int solids = 0;
  for (const doc::Object& o : m.document().objects()) solids += o.kind() == doc::ObjectKind::Solid;
  QCOMPARE(solids, 1);
}

namespace {
void orbitBy(Viewport* vp, const QPoint& from, const QPoint& delta) {
  send(vp, QEvent::MouseButtonPress, from, Qt::MiddleButton, Qt::MiddleButton);
  for (int i = 1; i <= 5; ++i) send(vp, QEvent::MouseMove, from + delta * i / 5, Qt::NoButton, Qt::MiddleButton);
  send(vp, QEvent::MouseButtonRelease, from + delta, Qt::MiddleButton, Qt::NoButton);
  QTest::qWait(20);
}
}  // namespace

// Spec 07 Gate 2: AC-05, AC-11, AC-14, AC-15, AC-16 and failure wording (§10).
void GuiPhase1::gate2InputConsistency() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();

  // AC-15: orbit in the middle of drawing a rectangle; the first corner and
  // the typed size still apply.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  orbitBy(f.vp, f.vp->rect().center(), QPoint(60, 20));
  move(f.vp, f.vp->toWidget(gp_Pnt(800, 600, 0)));
  type(f.vp, QStringLiteral("2000,1000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const Bnd_Box rb = geom::exactBounds(m.document().worldShape(m.document().objects().front()));
  QVERIFY(std::abs(rb.CornerMin().X()) < 1e-6 && std::abs(rb.CornerMax().X() - 2000) < 1e-6);
  QVERIFY(std::abs(rb.CornerMax().Y() - 1000) < 1e-6);
  QVERIFY(m.undo());

  // AC-11: an X lock survives orbiting; the typed length goes along X.
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  QTest::keyClick(f.vp, Qt::Key_Right);
  orbitBy(f.vp, f.vp->rect().center(), QPoint(-50, 30));
  move(f.vp, f.vp->toWidget(gp_Pnt(700, 400, 0)));
  type(f.vp, QStringLiteral("1500"));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const Bnd_Box lb = geom::exactBounds(m.document().worldShape(m.document().objects().front()));
  QVERIFY2(std::abs(lb.CornerMax().X() - 1500) < 1e-6 && std::abs(lb.CornerMax().Y()) < 1e-6,
           qPrintable(QStringLiteral("%1 %2").arg(lb.CornerMax().X()).arg(lb.CornerMax().Y())));

  // AC-05: while drawing lines, the open ends of the loose line are marked.
  QTest::qWait(30);
  QCOMPARE(f.vp->openEndCount(), 2);
  f.window.activateTool(MainWindow::ToolId::Select);
  QTest::qWait(30);
  QCOMPARE(f.vp->openEndCount(), 0);
  QVERIFY(m.undo());

  // AC-14: Esc goes back one stage (arc bulge -> end point), then cancels;
  // nothing is recorded in the history.
  const std::size_t steps = m.history().undoCount();
  f.window.activateTool(MainWindow::ToolId::Arc);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 300, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);  // back to choosing the end point
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));
  type(f.vp, QStringLiteral("400"));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const Bnd_Box ab = geom::exactBounds(m.document().worldShape(m.document().objects().front()));
  // The end point was picked again (free click on the ground: pixel accuracy).
  QVERIFY2(std::abs(ab.CornerMax().X() - 2000) < 20.0, qPrintable(QString::number(ab.CornerMax().X())));
  QVERIFY(m.undo());
  QCOMPARE(m.history().undoCount(), steps);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QCOMPARE(m.history().undoCount(), steps);
  QVERIFY(m.document().objects().empty());

  // AC-16: GUI rectangle + push/pull 3000 equals the API's create_box.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(800, 600, 0)));
  type(f.vp, QStringLiteral("1200,900"));
  f.window.activateTool(MainWindow::ToolId::PushPull);
  click(f.vp, f.vp->toWidget(gp_Pnt(600, 450, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(600, 450, 500)));
  type(f.vp, QStringLiteral("3000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const TopoDS_Shape gui = m.document().worldShape(m.document().objects().front());
  doc::Modeler other;
  api::CommandApi api(other);
  const auto r = QJsonDocument::fromJson(QByteArray::fromStdString(api.execute(
      R"({"command": "create_box", "args": {"origin": [0,0,0], "width": 1200, "depth": 900, "height": 3000}})")));
  QVERIFY(r.object().value("ok").toBool());
  const TopoDS_Shape apiShape = other.document().worldShape(other.document().objects().front());
  QVERIFY(std::abs(geom::volume(gui) - geom::volume(apiShape)) < 1e-6);
  QVERIFY(geom::exactBounds(gui).CornerMax().Distance(geom::exactBounds(apiShape).CornerMax()) < 1e-6);
  QCOMPARE(QString::fromStdString(doc::describe(m.document().objects().front().recipe)),
           QString::fromStdString(doc::describe(other.document().objects().front().recipe)));

  // §10: failures say why, that the model is unchanged, in Japanese.
  f.window.reportError(QStringLiteral("Distance must be non-zero"));
  QVERIFY(f.window.lastError().contains(QStringLiteral("距離が 0")));
  QVERIFY(f.window.lastError().contains(QStringLiteral("モデルは変更されていません")));
}

void GuiPhase1::boxSelectionWindowAndCrossing() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  const doc::ObjectId a = f.box(0, 0, 500, 500, 500);
  const doc::ObjectId b = f.box(1500, 0, 500, 500, 500);
  f.window.activateTool(MainWindow::ToolId::Select);
  QTest::qWait(30);
  auto drag = [&](const QPoint& from, const QPoint& to) {
    send(f.vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 4; ++i) send(f.vp, QEvent::MouseMove, from + (to - from) * i / 4, Qt::NoButton, Qt::LeftButton);
    send(f.vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QTest::qWait(20);
  };
  // Footprints on screen.
  QRect ra, rb;
  for (int i = 0; i < 8; ++i) {
    ra = ra.united(QRect(f.vp->toWidget(gp_Pnt(i & 1 ? 500 : 0, i & 2 ? 500 : 0, i & 4 ? 500 : 0)), QSize(1, 1)));
    rb = rb.united(QRect(f.vp->toWidget(gp_Pnt(i & 1 ? 2000 : 1500, i & 2 ? 500 : 0, i & 4 ? 500 : 0)), QSize(1, 1)));
  }
  const int top = std::min(ra.top(), rb.top()) - 20;
  const int bottom = std::max(ra.bottom(), rb.bottom()) + 20;
  // Left to right around A only: window selection, A fully inside.
  drag(QPoint(ra.left() - 20, top), QPoint(ra.right() + 5, bottom));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{a});
  // Left to right covering half of B: B is not fully inside -> nothing.
  drag(QPoint(rb.left() - 5, top), QPoint(rb.center().x(), bottom));
  QVERIFY(f.window.selection().empty());
  // Right to left over half of B: crossing selects B.
  drag(QPoint(rb.center().x(), bottom), QPoint(rb.left() - 5, top));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{b});
  // A plain click on empty space clears the selection.
  click(f.vp, QPoint(5, 5));
  QVERIFY(f.window.selection().empty());
}

// Spec 07 §9.2 / AC-20 in the app: the AI proposes, the user decides.
void GuiPhase1::aiProposalApplyAndReject() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();
  AiBridge* bridge = f.window.aiBridge();
  const std::string proposal =
      R"({"command": "propose", "args": {"description": "柱 2 本", "commands": [
         {"command": "create_box", "args": {"origin": [0, 0, 0], "width": 400, "depth": 400, "height": 3000}},
         {"command": "create_box", "args": {"origin": [3000, 0, 0], "width": 400, "depth": 400, "height": 3000}}]}})";
  auto call = [&](const std::string& r) { return QJsonDocument::fromJson(QByteArray::fromStdString(bridge->execute(r))).object(); };

  QJsonObject r = call(proposal);
  QVERIFY2(r.value("ok").toBool(), qPrintable(QJsonDocument(r).toJson()));
  QVERIFY(f.window.hasPendingProposal());
  QVERIFY(f.vp->hasProposalGhost());
  QVERIFY(m.document().objects().empty());  // nothing changed yet
  QCOMPARE(r.value("result").toObject().value("added").toInt(), 2);
  f.window.findChild<QPushButton*>(QStringLiteral("proposalReject"))->click();
  QVERIFY(!f.vp->hasProposalGhost());
  QVERIFY(m.document().objects().empty());
  QCOMPARE(call(R"({"command": "proposal_status"})").value("result").toObject().value("state").toString(),
           QStringLiteral("rejected"));

  call(proposal);
  f.window.findChild<QPushButton*>(QStringLiteral("proposalApply"))->click();
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  QCOMPARE(call(R"({"command": "proposal_status"})").value("result").toObject().value("state").toString(),
           QStringLiteral("applied"));
  QVERIFY(m.undo());  // one undo step
  QVERIFY(m.document().objects().empty());

  // The user edits while a proposal waits: it is withdrawn.
  call(proposal);
  QVERIFY(f.window.hasPendingProposal());
  QVERIFY(m.createRectangle(geom::SketchPlane::ground(), 100, 100).ok());
  QTest::qWait(30);
  QVERIFY(!f.window.hasPendingProposal());
  QVERIFY(!f.vp->hasProposalGhost());

  // A proposal that deletes something says so (and shows it in red).
  QVERIFY(f.window.hasPendingProposal() == false);
  const std::string victim = m.document().objects().front().id;
  r = call(std::string(R"({"command": "propose", "args": {"commands": [{"command": "delete", "args": {"ids": [")") + victim + R"("]}}]}})");
  QVERIFY2(r.value("ok").toBool(), qPrintable(QJsonDocument(r).toJson()));
  QCOMPARE(r.value("result").toObject().value("removed").toInt(), 1);
  f.window.findChild<QPushButton*>(QStringLiteral("proposalReject"))->click();
  QVERIFY(m.document().find(victim));

  // A proposal that cannot be applied is refused up front.
  r = call(R"({"command": "propose", "args": {"commands": [{"command": "extrude", "args": {"id": "nope", "distance": 1}}]}})");
  QVERIFY(!r.value("ok").toBool());
  QVERIFY(!f.window.hasPendingProposal());
}

// AC-22: double-click a component, push/pull inside, Esc: every copy follows.
void GuiPhase1::componentEditInPlace() {
  Fixture f;
  QVERIFY(f.setUp(2000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  auto component = m.makeComponent({box}, "Block");
  QVERIFY(component.ok());
  gp_Trsf step;
  step.SetTranslation(gp_Vec(2500, 0, 0));
  QVERIFY(m.copy({*component}, step, 1).ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::Select);
  doubleClick(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  QTest::qWait(30);
  const doc::ObjectId group = m.activeGroup();
  QVERIFY(!group.empty());
  QVERIFY(m.componentEditGroup(group).has_value());

  // Push the top face up by 500 inside the component.
  f.window.activateTool(MainWindow::ToolId::PushPull);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1300)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));

  // Esc twice: back to the select tool's context, then leave the component.
  f.window.activateTool(MainWindow::ToolId::Select);
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  QVERIFY(m.activeGroup().empty());
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{*component});
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  for (const doc::Object& o : m.document().objects()) {
    QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(o)).CornerMax().Z() - 1500) < 1e-6);
  }
  // Leaving, the push/pull and entering are one undo step each.
  QVERIFY(m.undo());
  QVERIFY(m.undo());
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  for (const doc::Object& o : m.document().objects()) {
    QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(o)).CornerMax().Z() - 1000) < 1e-6);
  }
}

void GuiPhase1::followMeAroundTopFace() {
  Fixture f;
  QVERIFY(f.setUp(1000, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 2000, 1000, 500);
  // A 100 x 100 cornice profile standing on the box's corner.
  auto profile = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, 0, 500), gp::DX()), 100, 100);
  QVERIFY(profile.ok());
  QTest::qWait(30);
  f.window.activateTool(MainWindow::ToolId::FollowMe);
  QCOMPARE(f.vp->cursor().shape(), Qt::BitmapCursor);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 50, 550)));       // the profile
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 500, 500)));    // around the top face
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(!m.document().find(*profile));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  const doc::Object& ring = m.document().objects().back();
  QVERIFY(ring.id != box);
  QVERIFY(geom::validateShape(m.document().worldShape(ring), true).valid);
  QVERIFY(m.undo());
  QVERIFY(m.document().find(*profile));
}

QTEST_MAIN(GuiPhase1)
#include "test_gui.moc"
