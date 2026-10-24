// Phase 1 acceptance test driving the real main window with synthetic input:
// draw a rectangle, push/pull it 3000 mm, save, reopen and re-edit.

#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <QWheelEvent>

#include <set>

#include <BRepGProp.hxx>
#include <TopExp.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Lin.hxx>

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QSlider>
#include <QMenu>
#include <QMessageBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <nlohmann/json.hpp>
#include <QImage>
#include <QSettings>
#include <QListWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTemporaryDir>
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

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include "AiBridge.h"
#include "ProgressDialog.h"
#include "WheelZoom.h"
#include "MainWindow.h"
#include "MaterialEditor.h"
#include "MaterialsPanel.h"
#include "PropertiesPanel.h"
#include "sbr/geom/FaceRef.h"
#include "Viewport.h"
#include "sbr/api/CommandApi.h"
#include "sbr/doc/NativeFormat.h"
#include "sbr/doc/ChangeSummary.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Operations.h"
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

void wheelAt(QWidget* w, const QPoint& pos, const QPoint& angle, const QPoint& pixel = QPoint(),
             Qt::ScrollPhase phase = Qt::NoScrollPhase) {
  QWheelEvent event(QPointF(pos), QPointF(w->mapToGlobal(pos)), pixel, angle, Qt::NoButton, Qt::NoModifier, phase,
                    false);
  QApplication::sendEvent(w, &event);
}

double gap(const doc::CameraState& a, const doc::CameraState& b) {
  double d = 0;
  for (int i = 0; i < 3; ++i) d = std::max({d, std::abs(a.eye[i] - b.eye[i]), std::abs(a.target[i] - b.target[i])});
  return std::max(d, std::abs(a.scale - b.scale));
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
  void copySelectedEdgeArrays();
  void copyOntoEdgeThenDivideKeepsSolid();
  void lineAcrossStripsInAGroupDividesEach();
  void typedLengthSplitsOrClosesSafely();
  void middleLeftPanAddsNoPoint();
  void rectangleOneSideSignsAndReentry();
  void cutCopyAndMoveCopyTakeOnlyPickedElements();
  void rotateByTypedAngle();
  void polarArrayFullCircleAndCtrlMidMove();
  void offsetToolInwards();
  void groupIsSelectedAsAWhole();
  void curveToolOpenAndClosed();
  void selectFacesEdgesAndWholeObject();
  void hoverDoesNotHighlightAndEdgeSelectionStays();
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
  void scaleGripsStretchEachDirection();
  void scaleGripsOnComponentsAndGroups();
  void rectangleStandsUpLikeSketchUp();
  void topEdgesCopiedDownDivideTheSides();
  void boxSelectionTakesFacesAndEdges();
  void followMeReportedSetup();
  void ux01MixedSelectionKeepsPartsAndWholes();
  void ux07CtrlAddsCtrlShiftRemoves();
  void ux04BoxOverBlankSelectsNothing();
  void ux02ScalePartsRefused();
  void ux05ScaleInvalidPlaceDoesNotCommit();
  void ux09ScaleCommasKeepTheirAxes();
  void ux03MoveInsideGroupTakesItsChild();
  void ux06SameNegativeValueTwiceSameResult();
  void ux08RotateArrowsSetThePlane();
  void ux10RotateCopyPreviewShowsTheParts();
  void followMeTracedFromTheProfile();
  void move05FaceMovesSideways();
  void state03SaveWhileDrawingKeepsTheLastState();
  void look06HiddenIsNotSnappedOrPicked();
  void look06SectionAndXRayRules();
  void measureResizesTheModel();
  void rotatedRectangleTool();
  void arcCarriesOnTangent();
  void lockedObjectsSelectableNotEditable();
  void groupEditShowsDottedBox();
  void snapTipNamesTheAdoptedPoint();
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
  void aiWaitsForLongOperation();
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
  void componentPlacementOwnSizeMirrorAndAxes();
  void moveToolReshapesCornersAndEdges();
  void moveToolLiftsRidgeIntoGable();
  void lineAgainstVolumeMakesFace();
  void eraserShiftHidesEdgeLine();
  void openingToolOverEverything();
  void lineToolCrossingLinesMakeFace();
  void eraserWorksOnLines();
  void rightClickMenuFollowsTheSelection();
  void skyAndGroundFollowTheHorizon();
  void selectionDotsKeepTheMaterialColour();
  void eraserDragErasesCrossedLines();
  void eraserDragFollowsWhatTheLineBecame();
  void wheelZoomIsConsistentAndReversible();
  void wheelZoomEmptySpaceAndLimits();
  void wheelZoomKeepsDrawing();
  void materialEditorColourTextureOpacityDuplicate();
  void brandingResourcesLoad();
  void paintNeedsAMaterialAndPresetsAddOnClick();
  void followMeAroundTopFace();
  void followMeAlongSelectedEdges();
  void wallToolAndArchitectureMenus();
  void slabToolAndOpeningTool();
  void typedRevisionOnlyRightAfter();
  void escStepsBackAndConstraintsShow();
  void autosaveRecoveryAndBackups();
  void deleteFaceLeavesOpenShape();
  void deleteLooseFaceKeepsItsOutline();
  void deleteMixedSelection();
  void pushPullOnOpenShape();
  void imageUnderlayCalibrateLockTrace();
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

// Request "線の移動コピー・等間隔配列・等分割": a selected line copied with
// Move + Ctrl and a typed distance, then "*5", "*3", "/3", "/5" typed right
// after; one edge of a solid copied without changing the solid; the left
// edge of a face copied to its right edge and "/3" divides the face.
void GuiPhase1::copySelectedEdgeArrays() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId line = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(0, 1000, 0)});
  f.vp->syncNow();
  auto xs = [&] {
    std::multiset<long> out;
    for (const doc::Object& o : m.document().objects()) {
      if (o.kind() != doc::ObjectKind::Curve) continue;
      Bnd_Box b = geom::exactBounds(m.document().worldShape(o));
      out.insert(std::lround(b.CornerMin().X()));
    }
    return out;
  };
  auto copyBy = [&](const gp_Pnt& on, double distance, const gp_Pnt& towards) {
    f.window.activateTool(MainWindow::ToolId::Select);
    click(f.vp, f.vp->toWidget(on));
    QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
    f.window.activateTool(MainWindow::ToolId::Move);
    click(f.vp, f.vp->toWidget(on));
    QTest::keyClick(f.vp, Qt::Key_Control);
    move(f.vp, f.vp->toWidget(towards));
    type(f.vp, QString::number(distance));
  };
  // A01, A03: 600 then *5 (6 lines), then *3 replaces it (4 lines).
  const std::size_t steps = m.history().undoCount();
  copyBy(gp_Pnt(0, 500, 0), 600, gp_Pnt(1200, 700, 0));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  type(f.vp, QStringLiteral("*5"));
  QCOMPARE(xs(), (std::multiset<long>{0, 600, 1200, 1800, 2400, 3000}));
  QVERIFY2(f.window.statusText().contains(QStringLiteral("元を含め 6")), qPrintable(f.window.statusText()));
  type(f.vp, QStringLiteral("3*"));
  QCOMPARE(xs(), (std::multiset<long>{0, 600, 1200, 1800}));
  // /3 divides the first 600; /5 too, not what *3 made.
  type(f.vp, QStringLiteral("/3"));
  QCOMPARE(xs(), (std::multiset<long>{0, 200, 400, 600}));
  type(f.vp, QStringLiteral("x5"));
  QCOMPARE(xs(), (std::multiset<long>{0, 600, 1200, 1800, 2400, 3000}));
  // A12: nonsense keeps what is there.
  type(f.vp, QStringLiteral("/0"));
  QVERIFY(!f.window.lastError().isEmpty());
  QCOMPARE(xs(), (std::multiset<long>{0, 600, 1200, 1800, 2400, 3000}));
  f.window.clearLastError();
  QCOMPARE(m.history().undoCount(), steps + 1);  // one step all along
  QVERIFY(m.undo());
  QCOMPARE(xs(), (std::multiset<long>{0}));

  // A02 / A04: 3000 then /3, then /5: the same 3000 in five.
  copyBy(gp_Pnt(0, 500, 0), 3000, gp_Pnt(1200, 700, 0));
  type(f.vp, QStringLiteral("/3"));
  QCOMPARE(xs(), (std::multiset<long>{0, 1000, 2000, 3000}));
  type(f.vp, QStringLiteral("5/"));
  QCOMPARE(xs(), (std::multiset<long>{0, 600, 1200, 1800, 2400, 3000}));
  QVERIFY(m.undo());
  QVERIFY(m.document().find(line));

  // A07: one edge of a solid: copied as a line, the solid unchanged.
  const doc::ObjectId box = f.box(5000, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  const double volume = geom::volume(m.document().worldShape(box));
  copyBy(gp_Pnt(5500, 0, 1000), 2000, gp_Pnt(5500, -1500, 1000));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(geom::volume(m.document().worldShape(box)) - volume) < 1e-6);
  QCOMPARE(m.document().objects().size(), std::size_t(3));  // line, box, the copied edge
}

// Report: a line copied with Move + Ctrl to the far edge of a box side,
// then "/3": the box went back to a flat face and picking stopped. The copy
// on the far edge records nothing (the edge is there), so typing "/3" must
// not take back the step before it (the box's push/pull).
void GuiPhase1::copyOntoEdgeThenDivideKeepsSolid() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 3000, 1000, 1000);
  f.vp->syncNow();
  const std::size_t steps = m.history().undoCount();
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 500)));  // the front's left edge
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  f.window.activateTool(MainWindow::ToolId::Move);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 500)));
  QTest::keyClick(f.vp, Qt::Key_Control);
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 500)));
  type(f.vp, QStringLiteral("3000"));  // onto the right edge: nothing new
  QCOMPARE(m.history().undoCount(), steps);
  type(f.vp, QStringLiteral("/3"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const TopoDS_Shape solid = m.document().worldShape(*m.document().find(box));
  QVERIFY(std::abs(geom::volume(solid) - 3e9) < 1.0);
  int faces = 0;
  for (TopExp_Explorer e(solid, TopAbs_FACE); e.More(); e.Next()) ++faces;
  QCOMPARE(faces, 8);  // the front in three
  QCOMPARE(m.history().undoCount(), steps + 1);
  // Everything is picked again: the front's parts, the top, the side.
  f.window.activateTool(MainWindow::ToolId::Select);
  for (const gp_Pnt& p : {gp_Pnt(500, 0, 500), gp_Pnt(1500, 0, 500), gp_Pnt(1500, 500, 1000), gp_Pnt(3000, 500, 500)}) {
    click(f.vp, f.vp->toWidget(gp_Pnt(6000, -3000, 0)));
    click(f.vp, f.vp->toWidget(p));
    QVERIFY2(f.window.elementSelection().size() == 1,
             qPrintable(QStringLiteral("nothing picked at %1 %2 %3").arg(p.X()).arg(p.Y()).arg(p.Z())));
  }
  // The middle part pushed out on its own.
  f.window.activateTool(MainWindow::ToolId::PushPull);
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 500)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, -300, 500)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(geom::volume(m.document().worldShape(*m.document().find(box))) - 3.5e9) < 1.0);
  QVERIFY(m.undo());
  QVERIFY(m.undo());
  QVERIFY(std::abs(geom::volume(m.document().worldShape(*m.document().find(box))) - 3e9) < 1.0);
}

// W0 / LINE-05, SC03: a line started on an edge and given its length by
// typing reaches the opposite edge and divides the face (the chain ends
// there); typing the closing side closes the loop. Nothing reads the
// finished chain afterwards, no stray rubber band, one undo step each.
void GuiPhase1::typedLengthSplitsOrClosesSafely() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 3000, 1000, 1000);
  f.vp->syncNow();
  auto faceCount = [&](const doc::ObjectId& id) {
    int n = 0;
    for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
  };
  f.window.activateTool(MainWindow::ToolId::Line);
  const std::size_t steps = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 1000)));  // the top's front edge
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 600, 1000)));  // across the top
  type(f.vp, QStringLiteral("1000"));                    // to the back edge
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(faceCount(box), 7);  // the top in two
  QCOMPARE(m.history().undoCount(), steps + 1);
  // The chain ended: moving and typing again start nothing by themselves.
  move(f.vp, f.vp->toWidget(gp_Pnt(2500, 600, 1000)));
  type(f.vp, QStringLiteral("500"));
  QCOMPARE(m.history().undoCount(), steps + 1);
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QVERIFY(!f.window.lastError().isEmpty());  // says to click a start first
  f.window.clearLastError();

  // A loose face divided the same way (its own object per part).
  const auto face = m.createRectangle(geom::SketchPlane::ground(), 2000, 1000);
  QVERIFY(face.ok());
  f.vp->syncNow();
  QTest::keyClick(f.vp, Qt::Key_Escape);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, 400, 0)));
  type(f.vp, QStringLiteral("1000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  int faces = 0;
  for (const doc::Object& o : m.document().objects()) faces += o.kind() == doc::ObjectKind::Face;
  QCOMPARE(faces, 2);

  // Typed sides closing a loop: a face, and the chain is done.
  QTest::keyClick(f.vp, Qt::Key_Escape);
  const std::size_t before = m.document().objects().size();
  click(f.vp, f.vp->toWidget(gp_Pnt(5000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(5400, 0, 0)));
  type(f.vp, QStringLiteral("1000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(6000, 400, 0)));
  type(f.vp, QStringLiteral("1000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(5600, 1000, 0)));
  type(f.vp, QStringLiteral("1000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(5000, 600, 0)));
  type(f.vp, QStringLiteral("1000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), before + 1);
  QCOMPARE(m.document().objects().back().kind(), doc::ObjectKind::Face);
  QVERIFY(std::abs(geom::area(m.document().worldShape(m.document().objects().back())) - 1e6) < 1.0);
}

// W1 / CLIP-01/02, SEL-01, SC18: a face or an edge picked on a box is what
// Ctrl+C / Ctrl+X / Move+Ctrl take, never the whole box; grouping only part
// of an object is refused rather than grouping all of it.
void GuiPhase1::cutCopyAndMoveCopyTakeOnlyPickedElements() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  auto kinds = [&](doc::ObjectKind k) {
    int n = 0;
    for (const doc::Object& o : m.document().objects()) n += o.kind() == k;
    return n;
  };
  auto faces = [&](const doc::ObjectId& id) {
    int n = 0;
    for (TopExp_Explorer e(m.document().worldShape(*m.document().find(id)), TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
  };
  f.window.activateTool(MainWindow::ToolId::Select);
  const std::size_t steps = m.history().undoCount();

  // Ctrl+X on the top face: that face goes (the box opens), the rest stays.
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QVERIFY(f.window.cutSelection());
  QVERIFY(m.document().find(box));
  QCOMPARE(faces(box), 5);
  QCOMPARE(m.history().undoCount(), steps + 1);
  // Pasted where it was: it closes the box again (one solid, no loose face).
  QVERIFY(f.window.pasteInPlace());
  QCOMPARE(kinds(doc::ObjectKind::Face), 0);
  QCOMPARE(kinds(doc::ObjectKind::Solid), 1);
  QCOMPARE(kinds(doc::ObjectKind::Shell), 0);
  QVERIFY(m.undo());
  QVERIFY(m.undo());
  QCOMPARE(faces(box), 6);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(*m.document().find(box))) - 1e9) < 1.0);

  // Ctrl+C on one edge, pasted elsewhere: a line; the box as it was.
  f.vp->syncNow();
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, -3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 1000)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QVERIFY(f.window.copySelection());
  f.window.activateTool(MainWindow::ToolId::Paste);
  click(f.vp, f.vp->toWidget(gp_Pnt(2500, -2000, 0)));
  QCOMPARE(kinds(doc::ObjectKind::Curve), 1);
  QCOMPARE(kinds(doc::ObjectKind::Solid), 1);
  QCOMPARE(faces(box), 6);

  // Move + Ctrl on the top face: a copy of that face, the box unchanged.
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, -3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  f.window.activateTool(MainWindow::ToolId::Move);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  QTest::keyClick(f.vp, Qt::Key_Control);
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 2000, 1000)));
  type(f.vp, QStringLiteral("2000"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(kinds(doc::ObjectKind::Face), 1);
  QCOMPARE(kinds(doc::ObjectKind::Solid), 1);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(*m.document().find(box))) - 1e9) < 1.0);

  // Grouping just a face of the box: refused, nothing changes.
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, -3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 500)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  const std::size_t objects = m.document().objects().size();
  f.window.clearLastError();
  for (QAction* a : f.window.findChildren<QAction*>()) {
    if (a->text() == QStringLiteral("グループ化")) {
      a->trigger();
      break;
    }
  }
  QVERIFY(!f.window.lastError().isEmpty());
  QCOMPARE(m.document().objects().size(), objects);
}

// W2 / NUM-02..05, SC02: a rectangle typed 2000,1000, then "3000," (width
// only), ",1500" (depth only) keeps its first corner and the other side;
// a negative value turns that side over; units, the keypad and an IME's
// full-width digits read the same; garbage changes nothing; all of it one
// undo step.
void GuiPhase1::rectangleOneSideSignsAndReentry() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  const std::size_t steps = m.history().undoCount();
  auto bounds = [&] {
    return geom::exactBounds(m.document().worldShape(m.document().objects().back()));
  };
  auto same = [](double a, double b) { return std::abs(a - b) < 1e-6; };
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 800, 0)));
  type(f.vp, QStringLiteral("2000,1000"));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  Bnd_Box b = bounds();
  QVERIFY(same(b.CornerMax().X(), 2000) && same(b.CornerMax().Y(), 1000));
  type(f.vp, QStringLiteral("3000,"));
  b = bounds();
  QVERIFY2(same(b.CornerMin().X(), 0) && same(b.CornerMax().X(), 3000) && same(b.CornerMax().Y(), 1000),
           qPrintable(QStringLiteral("%1 %2").arg(b.CornerMax().X()).arg(b.CornerMax().Y())));
  type(f.vp, QStringLiteral(",1500"));
  b = bounds();
  QVERIFY(same(b.CornerMax().X(), 3000) && same(b.CornerMax().Y(), 1500) && same(b.CornerMin().Y(), 0));
  // Units.
  type(f.vp, QStringLiteral("300cm,1.5m"));
  b = bounds();
  QVERIFY(same(b.CornerMax().X(), 3000) && same(b.CornerMax().Y(), 1500));
  // Garbage and zero: nothing changes.
  type(f.vp, QStringLiteral("3000,abc"));
  QVERIFY(!f.window.lastError().isEmpty());
  f.window.clearLastError();
  type(f.vp, QStringLiteral("0,"));
  QVERIFY(!f.window.lastError().isEmpty());
  f.window.clearLastError();
  b = bounds();
  QVERIFY(same(b.CornerMax().X(), 3000) && same(b.CornerMax().Y(), 1500));
  // A negative width: the other side of the first corner.
  type(f.vp, QStringLiteral("-3000,"));
  b = bounds();
  QVERIFY2(same(b.CornerMin().X(), -3000) && same(b.CornerMax().X(), 0),
           qPrintable(QStringLiteral("%1 %2").arg(b.CornerMin().X()).arg(b.CornerMax().X())));
  QCOMPARE(m.history().undoCount(), steps + 1);  // one step all along
  QCOMPARE(m.document().objects().size(), std::size_t(1));

  // The keypad, while drawing: one side typed, the other from the cursor.
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 5000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1200, 5600, 0)));
  for (const char c : std::string("2500,")) {
    QTest::keyClick(f.vp, c, c == ',' ? Qt::NoModifier : Qt::KeypadModifier);
  }
  QTest::keyClick(f.vp, Qt::Key_Enter, Qt::KeypadModifier);
  QTest::qWait(50);
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  b = bounds();
  QVERIFY2(same(b.CornerMax().X() - b.CornerMin().X(), 2500) && b.CornerMax().Y() > 5000 + 100,
           qPrintable(QStringLiteral("%1 %2").arg(b.CornerMax().X()).arg(b.CornerMax().Y())));
  // An IME's full-width entry in the box.
  f.window.measurementBox()->setText(QStringLiteral("３０００，１０００"));
  QTest::keyClick(f.window.measurementBox(), Qt::Key_Return);
  QTest::qWait(50);
  b = bounds();
  QVERIFY2(same(b.CornerMax().X() - b.CornerMin().X(), 3000) && same(b.CornerMax().Y() - b.CornerMin().Y(), 1000),
           qPrintable(QStringLiteral("%1 %2 %3").arg(b.CornerMax().X()).arg(b.CornerMax().Y()).arg(f.window.lastError())));
  // Undo: the second rectangle (with its revisions) in one step.
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(1));

  // Move: a negative distance goes the other way, typed while moving or
  // right after; zero changes nothing.
  const doc::ObjectId moved = m.document().objects().front().id;
  f.window.setSelection({moved});
  f.window.activateTool(MainWindow::ToolId::Move);
  f.window.setSelection({moved});
  const double x0 = geom::exactBounds(m.document().worldShape(*m.document().find(moved))).CornerMin().X();
  click(f.vp, f.vp->toWidget(gp_Pnt(-3000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(-2000, 0, 0)));
  type(f.vp, QStringLiteral("-500"));
  double x1 = geom::exactBounds(m.document().worldShape(*m.document().find(moved))).CornerMin().X();
  QVERIFY2(std::abs((x1 - x0) + 500) < 1.0, qPrintable(QString::number(x1 - x0)));
  type(f.vp, QStringLiteral("-800"));  // re-entered: still the other way
  x1 = geom::exactBounds(m.document().worldShape(*m.document().find(moved))).CornerMin().X();
  QVERIFY2(std::abs((x1 - x0) + 800) < 1.0, qPrintable(QString::number(x1 - x0)));
  f.window.clearLastError();
  type(f.vp, QStringLiteral("0"));
  QVERIFY(!f.window.lastError().isEmpty());
  x1 = geom::exactBounds(m.document().worldShape(*m.document().find(moved))).CornerMin().X();
  QVERIFY(std::abs((x1 - x0) + 800) < 1.0);
}

// W3 / CAM-01, SC17: while the middle button orbits, pressing the left
// one pans; neither press nor release reaches the line tool (no stray
// point), and the start point, direction and typed length carry on.
void GuiPhase1::middleLeftPanAddsNoPoint() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(800, 0, 0)));
  doc::CameraState before, after;
  f.vp->storeCamera(before);
  const QPoint a = f.vp->toWidget(gp_Pnt(800, 0, 0));
  const QPoint b = a + QPoint(120, 60);
  send(f.vp, QEvent::MouseButtonPress, a, Qt::MiddleButton, Qt::MiddleButton);
  send(f.vp, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::MiddleButton | Qt::LeftButton);
  send(f.vp, QEvent::MouseMove, b, Qt::NoButton, Qt::MiddleButton | Qt::LeftButton);
  send(f.vp, QEvent::MouseButtonDblClick, b, Qt::LeftButton, Qt::MiddleButton | Qt::LeftButton);
  send(f.vp, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::MiddleButton);
  send(f.vp, QEvent::MouseButtonRelease, b, Qt::MiddleButton, Qt::NoButton);
  QTest::qWait(30);
  f.vp->storeCamera(after);
  QVERIFY(gap(before, after) > 1.0);  // the view moved
  QCOMPARE(m.document().objects().size(), std::size_t(0));
  // The line goes on from its start: point the way again and type.
  move(f.vp, f.vp->toWidget(gp_Pnt(800, 0, 0)));
  type(f.vp, QStringLiteral("1500"));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const TopoDS_Shape line = m.document().worldShape(m.document().objects().front());
  int edges = 0;
  for (TopExp_Explorer e(line, TopAbs_EDGE); e.More(); e.Next()) ++edges;
  QCOMPARE(edges, 1);
  const Bnd_Box box = geom::exactBounds(line);
  QVERIFY2(std::abs(box.CornerMax().X() - box.CornerMin().X() - 1500) < 1e-6 && std::abs(box.CornerMin().X()) < 1e-6,
           qPrintable(QStringLiteral("%1 %2").arg(box.CornerMin().X()).arg(box.CornerMax().X())));
}

// W4 / XFORM-02, ARRAY-03, MOVE-04: a 30 degree rotated copy then "*12"
// goes round once: 11 copies, the twelfth would lie on the original and is
// not made; "/3" divides the angle given (as Move divides its distance);
// one undo step. Ctrl switched on and off while reshaping: the shape is
// only changed by what is finally clicked.
void GuiPhase1::polarArrayFullCircleAndCtrlMidMove() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId id = f.box(1000, -100, 400, 200, 200);
  f.window.setSelection({id});
  f.window.activateTool(MainWindow::ToolId::Rotate);
  f.window.setSelection({id});
  const std::size_t steps = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));     // centre
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 0)));  // reference along +X
  QTest::keyClick(f.vp, Qt::Key_Control);           // copy
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 900, 0)));
  type(f.vp, QStringLiteral("30"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  type(f.vp, QStringLiteral("*12"));
  QCOMPARE(m.document().objects().size(), std::size_t(12));  // 11 copies + the original
  // No copy on top of the original.
  int onOriginal = 0;
  const Bnd_Box original = geom::exactBounds(m.document().worldShape(*m.document().find(id)));
  for (const doc::Object& o : m.document().objects()) {
    if (o.id == id) continue;
    const Bnd_Box b = geom::exactBounds(m.document().worldShape(o));
    onOriginal += b.CornerMin().Distance(original.CornerMin()) < 1e-3 && b.CornerMax().Distance(original.CornerMax()) < 1e-3;
  }
  QCOMPARE(onOriginal, 0);
  type(f.vp, QStringLiteral("/3"));  // 30 degrees in three: copies at 10, 20, 30
  QCOMPARE(m.document().objects().size(), std::size_t(4));
  QCOMPARE(m.history().undoCount(), steps + 1);
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(1));

  // MOVE-04: grab the top edge, start lifting it, Ctrl on (copy), Ctrl off
  // again, type: the edge is lifted (reshape), no copy, one step.
  const doc::ObjectId block = f.box(-4000, -4000, 2000, 2000, 1000);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(-3000, -4000, 1000)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  f.window.activateTool(MainWindow::ToolId::Move);
  const std::size_t before = m.history().undoCount();
  const std::size_t objects = m.document().objects().size();
  click(f.vp, f.vp->toWidget(gp_Pnt(-3000, -4000, 1000)));
  QTest::keyClick(f.vp, Qt::Key_Up);  // blue axis
  move(f.vp, f.vp->toWidget(gp_Pnt(-3000, -4000, 3000)));
  QTest::keyClick(f.vp, Qt::Key_Control);
  move(f.vp, f.vp->toWidget(gp_Pnt(-3000, -4000, 3500)));
  QCOMPARE(m.document().objects().size(), objects);  // nothing committed yet
  QCOMPARE(m.history().undoCount(), before);
  QTest::keyClick(f.vp, Qt::Key_Control);
  move(f.vp, f.vp->toWidget(gp_Pnt(-3000, -4000, 3000)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), objects);
  QCOMPARE(m.history().undoCount(), before + 1);
  const double v = geom::volume(m.document().worldShape(*m.document().find(block)));
  QVERIFY2(std::abs(v - 2000.0 * 2000 * 1250) < 1.0, qPrintable(QString::number(v)));
}

// Report: the corner grip (and a typed factor) did nothing on a component
// placement (uniform scale refused placements), and a group showed no grips
// (it has no shape of its own). The placement grows alone (its own scale;
// the other placement and the definition stay); the group grows with what
// is in it, by the corner or along one direction.
// Rectangles are not only drawn on the ground: towards a corner above the
// start they stand up (the axis plane through both), and the arrow keys
// hold them to the plane across red / green / blue.
void GuiPhase1::rectangleStandsUpLikeSketchUp() {
  Fixture f;
  QVERIFY(f.setUp(1500, 0));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  auto newest = [&]() -> Bnd_Box {
    Bnd_Box b;
    const auto& objects = m.document().objects();
    if (!objects.empty()) b.Add(geom::exactBounds(m.document().worldShape(objects.back())));
    return b;
  };
  const std::size_t before = m.document().objects().size();
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  // From the ground in front of the box up to its top corner: upright,
  // across green (y = 0), 2000 wide and 1000 high.
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 1000)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 1000)));
  QCOMPARE(m.document().objects().size(), before + 1);
  Bnd_Box up = newest();
  QVERIFY(std::abs(up.CornerMax().Y() - up.CornerMin().Y()) < 1e-6);
  QVERIFY(std::abs(up.CornerMax().Z() - 1000) < 1e-6 && std::abs(up.CornerMin().Z()) < 1e-6);
  QVERIFY(std::abs(up.CornerMax().X() - up.CornerMin().X() - 2000) < 50);  // corner clicked in free space

  // Without a point above: on the ground as before.
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, -2000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(4000, -3000, 0)));
  type(f.vp, QStringLiteral("1000,500"));
  Bnd_Box ground = newest();
  QVERIFY(std::abs(ground.CornerMax().Z()) < 1e-6 && std::abs(ground.CornerMin().Z()) < 1e-6);

  // → holds it across red: a wall facing X, sized by typing.
  QTest::keyClick(f.vp, Qt::Key_Right);
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, -4000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(3000, -5000, 0)));
  type(f.vp, QStringLiteral("1500,800"));
  Bnd_Box wall = newest();
  QVERIFY(std::abs(wall.CornerMax().X() - wall.CornerMin().X()) < 1e-6);
  QVERIFY(std::abs(wall.CornerMax().Y() - wall.CornerMin().Y() - 1500) < 1e-6);
  QVERIFY(std::abs(wall.CornerMax().Z() - wall.CornerMin().Z() - 800) < 1e-6);
  // The lock was for that rectangle; ↑ after → and ↓ let go too.
  QTest::keyClick(f.vp, Qt::Key_Up);
  QTest::keyClick(f.vp, Qt::Key_Down);
  click(f.vp, f.vp->toWidget(gp_Pnt(5000, -2000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(6000, -3000, 0)));
  type(f.vp, QStringLiteral("700,700"));
  Bnd_Box flat = newest();
  QVERIFY(std::abs(flat.CornerMax().Z() - flat.CornerMin().Z()) < 1e-6);
  QCOMPARE(m.document().objects().size(), before + 4);
}

// Report: the 4 edges round a cube's top, copied down to the bottom with
// the Move tool and /4, drew lines but left each side one face.
void GuiPhase1::topEdgesCopiedDownDivideTheSides() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  const double volume = geom::volume(m.document().worldShape(box));
  auto faces = [&] {
    int n = 0;
    for (TopExp_Explorer e(m.document().worldShape(box), TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
  };
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 1000)));
  for (const gp_Pnt& p : {gp_Pnt(1000, 500, 1000), gp_Pnt(500, 1000, 1000), gp_Pnt(0, 500, 1000)}) {
    const QPoint at = f.vp->toWidget(p);
    move(f.vp, at);
    send(f.vp, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    send(f.vp, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    QTest::qWait(20);
  }
  QCOMPARE(f.window.elementSelection().size(), std::size_t(4));
  for (const ElementRef& e : f.window.elementSelection()) QCOMPARE(e.type, TopAbs_EDGE);
  f.window.activateTool(MainWindow::ToolId::Move);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 1000)));
  QTest::keyClick(f.vp, Qt::Key_Control);
  move(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));  // onto the bottom
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(faces(), 6);  // on the bottom edges: nothing new
  type(f.vp, QStringLiteral("/4"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(faces(), 4 * 4 + 2);  // each side in 4 strips
  type(f.vp, QStringLiteral("/3"));
  QCOMPARE(faces(), 4 * 3 + 2);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(box)) - volume) < 1e-3);
  for (const doc::Object& o : m.document().objects()) QCOMPARE(o.id, box);  // no lines, no loose faces
  // A strip is picked on its own (not the whole side).
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 500)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QCOMPARE(f.window.elementSelection().front().type, TopAbs_FACE);
  const Bnd_Box picked = geom::exactBounds(f.vp->elementShape(f.window.elementSelection().front()));
  QVERIFY(std::abs(picked.CornerMax().Z() - picked.CornerMin().Z() - 1000.0 / 3) < 1e-6);
}

// Report: a selection box round one face of a box and its 4 edges, or
// round lines only, took the whole box / nothing. Raw geometry is taken
// face by face and edge by edge; a group as one.
void GuiPhase1::boxSelectionTakesFacesAndEdges() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId line = *m.createPolyline({gp_Pnt(2500, -1500, 0), gp_Pnt(3500, -1500, 0), gp_Pnt(3500, -2500, 0)});
  const doc::ObjectId inner = f.box(5000, 0, 500, 500, 500);
  const doc::ObjectId group = *m.group({inner});
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  auto drag = [&](const QPoint& from, const QPoint& to) {
    send(f.vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 4; ++i) send(f.vp, QEvent::MouseMove, from + (to - from) * i / 4, Qt::NoButton, Qt::LeftButton);
    send(f.vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QTest::qWait(20);
  };
  auto around = [&](std::initializer_list<gp_Pnt> points, int margin) {
    QRect r;
    for (const gp_Pnt& p : points) r = r.united(QRect(f.vp->toWidget(p), QSize(1, 1)));
    return r.adjusted(-margin, -margin, margin, margin);
  };
  auto counts = [&] {
    int faces = 0, edges = 0;
    for (const ElementRef& e : f.window.elementSelection()) (e.type == TopAbs_FACE ? faces : edges)++;
    return std::pair<int, int>(faces, edges);
  };
  // Window round the front face (y = 0) and its 4 edges: just those.
  const QRect front = around({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 0, 1000), gp_Pnt(0, 0, 1000)}, 3);
  drag(front.topLeft(), front.bottomRight());
  // The face and its 4 edges; anything else taken lies inside the box too
  // (here the box's bottom edge behind, seen through: a window takes what
  // it holds, hidden or not).
  QCOMPARE(counts().first, 1);
  int frontEdges = 0;
  for (const ElementRef& e : f.window.elementSelection()) {
    const TopoDS_Shape shape = f.vp->elementShape(e);
    for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next()) {
      QVERIFY(front.contains(f.vp->toWidget(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())))));
    }
    const Bnd_Box b = geom::exactBounds(shape);
    if (e.type == TopAbs_EDGE && std::abs(b.CornerMin().Y()) < 1e-6 && std::abs(b.CornerMax().Y()) < 1e-6) ++frontEdges;
  }
  QCOMPARE(frontEdges, 4);
  const auto targets = f.window.selectionTargets();
  QCOMPARE(targets.size(), std::size_t(1));
  QCOMPARE(targets.front().id, box);
  QVERIFY(!targets.front().whole);  // not read as the whole box
  QCOMPARE(targets.front().faces.size(), std::size_t(1));
  // Lines only: their edges, nothing else.
  const QRect lines = around({gp_Pnt(2500, -1500, 0), gp_Pnt(3500, -1500, 0), gp_Pnt(3500, -2500, 0)}, 5);
  drag(lines.topLeft(), lines.bottomRight());
  QCOMPARE(counts(), std::make_pair(0, 2));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{line});
  // Crossing (right to left) inside the top face (no edge under it, the
  // back face behind): that face only; in X-ray the back face too.
  const QPoint mid = f.vp->toWidget(gp_Pnt(300, 300, 1000));
  drag(mid + QPoint(3, 3), mid - QPoint(3, 3));
  QCOMPARE(counts(), std::make_pair(1, 0));
  const Bnd_Box picked = geom::exactBounds(f.vp->elementShape(f.window.elementSelection().front()));
  QVERIFY(std::abs(picked.CornerMin().Z() - 1000) < 1e-6);
  f.vp->setXRay(true);
  drag(mid + QPoint(3, 3), mid - QPoint(3, 3));
  QCOMPARE(counts(), std::make_pair(2, 0));
  f.vp->setXRay(false);
  // A window round the whole box: every face and edge (the box whole).
  const QRect all = around({gp_Pnt(0, 0, 0), gp_Pnt(1000, 0, 0), gp_Pnt(1000, 1000, 0), gp_Pnt(0, 1000, 0),
                            gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000), gp_Pnt(1000, 1000, 1000), gp_Pnt(0, 1000, 1000)},
                           5);
  drag(all.topLeft(), all.bottomRight());
  QCOMPARE(counts(), std::make_pair(6, 12));
  QVERIFY(f.window.selectionTargets().front().whole);
  // The group: one unit, never its faces.
  const QRect g = around({gp_Pnt(5000, 0, 0), gp_Pnt(5500, 500, 500), gp_Pnt(5500, 0, 0), gp_Pnt(5000, 500, 500),
                          gp_Pnt(5000, 0, 500), gp_Pnt(5500, 500, 0)},
                         5);
  drag(g.topLeft(), g.bottomRight());
  QVERIFY(f.window.elementSelection().empty());
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{group});
  // Shift adds: the group and then the front face with its edges, mixed.
  const QPoint from = front.topLeft(), to = front.bottomRight();
  send(f.vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
  for (int i = 1; i <= 4; ++i) {
    send(f.vp, QEvent::MouseMove, from + (to - from) * i / 4, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
  }
  send(f.vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
  QTest::qWait(20);
  QCOMPARE(counts().first, 1);  // the front face (and its edges, as above)
  const auto mixed = f.window.selectionTargets();
  QCOMPARE(mixed.size(), std::size_t(2));
  const bool groupWhole = std::any_of(mixed.begin(), mixed.end(), [&](const doc::DeleteTarget& t) { return t.id == group && t.whole; });
  const bool boxPart = std::any_of(mixed.begin(), mixed.end(), [&](const doc::DeleteTarget& t) { return t.id == box && !t.whole; });
  QVERIFY(groupWhole && boxPart);
}

// Report: a rectangle standing at the start of a 3-segment line on the
// ground, the line's edges picked, Follow Me, the rectangle clicked: the
// rectangle runs along the whole line, joined at the corners. (It failed:
// the tool passed the profile's id by reference into the document, which
// the sweep changes - the profile id read afterwards was garbage.)
void GuiPhase1::followMeReportedSetup() {
  const std::vector<gp_Pnt> points{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0),
                                   gp_Pnt(6000, 3500, 0)};
  double length = 0;
  for (std::size_t i = 1; i < points.size(); ++i) length += points[i - 1].Distance(points[i]);
  for (bool separate : {false, true}) {
    Fixture f;
    QVERIFY(f.setUp(3000, 2000));
    doc::Modeler& m = f.window.modeler();
    // The profile: 2000 x 1500 upright on y = 0; the path from the middle
    // of its bottom edge, away from it, turning twice.
    auto profile = m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 1500), gp_Pnt(0, 0, 1500)});
    QVERIFY(profile.ok());
    if (separate) {
      for (std::size_t i = 1; i < points.size(); ++i) QVERIFY(m.createPolyline({points[i - 1], points[i]}).ok());
    } else {
      QVERIFY(m.createPolyline(points).ok());
    }
    f.vp->syncNow();
    const std::size_t objects = m.document().objects().size();
    // Pick the three segments (Shift adds), from the last to the first.
    auto pickPath = [&] {
      f.window.activateTool(MainWindow::ToolId::Select);
      for (std::size_t i = points.size() - 1; i >= 1; --i) {
        const gp_Pnt mid((points[i - 1].XYZ() + points[i].XYZ()) * 0.5);
        const QPoint at = f.vp->toWidget(mid);
        const Qt::KeyboardModifiers mods = i == points.size() - 1 ? Qt::NoModifier : Qt::ShiftModifier;
        move(f.vp, at);
        send(f.vp, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, mods);
        send(f.vp, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
        QTest::qWait(20);
      }
    };
    pickPath();
    QCOMPARE(f.window.elementSelection().size(), std::size_t(3));  // edges, not the line as a whole
    for (const ElementRef& e : f.window.elementSelection()) QCOMPARE(e.type, TopAbs_EDGE);
    // Kept through the tool change; Esc before the click changes nothing.
    f.window.activateTool(MainWindow::ToolId::FollowMe);
    QVERIFY2(f.window.statusText().contains(QStringLiteral("3 本")), qPrintable(f.window.statusText()));
    QTest::keyClick(f.vp, Qt::Key_Escape);
    QCOMPARE(m.document().objects().size(), objects);
    pickPath();
    f.window.activateTool(MainWindow::ToolId::FollowMe);
    const std::size_t steps = m.history().undoCount();
    click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 1000)));  // the profile
    QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
    QCOMPARE(m.history().undoCount(), steps + 1);
    QVERIFY(!m.document().find(*profile));  // consumed
    std::optional<doc::ObjectId> swept;
    for (const doc::Object& o : m.document().objects()) {
      if (o.kind() == doc::ObjectKind::Solid) swept = o.id;
    }
    QVERIFY(swept.has_value());
    const TopoDS_Shape shape = m.document().worldShape(*m.document().find(*swept));
    QVERIFY(geom::validateShape(shape, true).valid);
    QVERIFY(std::abs(geom::volume(shape) - 2000.0 * 1500 * length) < 1.0);  // turned at each corner
    // The same as the AI's call on the same model.
    doc::Modeler other;
    const doc::ObjectId p2 = *other.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 1500), gp_Pnt(0, 0, 1500)});
    const doc::ObjectId l2 = *other.createPolyline(points);
    api::CommandApi api(other);
    nlohmann::json args{{"profile", p2}, {"path_edges", nlohmann::json::array({nlohmann::json{{"id", l2}}})}};
    const auto r = nlohmann::json::parse(api.execute(nlohmann::json{{"command", "follow_me"}, {"args", args}}.dump()));
    QVERIFY(r["ok"].get<bool>());
    const TopoDS_Shape viaApi = other.document().worldShape(*other.document().find(r["result"]["id"].get<std::string>()));
    QVERIFY(std::abs(geom::volume(viaApi) - geom::volume(shape)) < 1e-3);
    // One undo: profile and path as they were; redo; saved and reloaded.
    QVERIFY(m.undo());
    QVERIFY(m.document().find(*profile));
    QCOMPARE(m.document().objects().size(), objects);
    QVERIFY(m.redo());
    const QString file = QDir(QDir::tempPath()).filePath(QStringLiteral("follow_me_gui.sbr"));
    QVERIFY(m.save(file.toStdString()).ok());
    doc::Modeler back;
    QVERIFY(back.load(file.toStdString()).ok());
    QVERIFY(std::abs(geom::volume(back.document().worldShape(*back.document().find(*swept))) - geom::volume(shape)) < 1e-3);
  }
  // A path with a gap: the reason and where, shown; nothing changed.
  Fixture f;
  QVERIFY(f.setUp(3000, 2000));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId profile =
      *m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 1500), gp_Pnt(0, 0, 1500)});
  const doc::ObjectId a = *m.createPolyline({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0)});
  const doc::ObjectId b = *m.createPolyline({gp_Pnt(1500, 3000, 0), gp_Pnt(4000, 4500, 0)});
  f.vp->syncNow();
  std::vector<ElementRef> path;
  for (const doc::ObjectId& id : {a, b}) {
    for (const ElementRef& e : f.vp->allElements(id)) path.push_back(e);
  }
  f.window.setElementSelection(path);
  f.window.activateTool(MainWindow::ToolId::FollowMe);
  const std::size_t objects = m.document().objects().size();
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 1000)));
  QVERIFY(f.window.lastError().contains(QStringLiteral("(1000.0, 3000.0, 0.0)")));
  QCOMPARE(m.document().objects().size(), objects);
  QVERIFY(m.document().find(profile));
}

namespace {

// What the selection means, per object: whole, or how many faces / edges.
std::map<doc::ObjectId, std::tuple<bool, int, int>> meaning(MainWindow& w) {
  std::map<doc::ObjectId, std::tuple<bool, int, int>> out;
  for (const doc::DeleteTarget& t : w.selectionTargets()) {
    out[t.id] = {t.whole, int(t.faces.size()), int(t.edges.size())};
  }
  return out;
}

void clickWith(QWidget* w, const QPoint& at, Qt::KeyboardModifiers mods) {
  move(w, at);
  send(w, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, mods);
  send(w, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
  QTest::qWait(20);
}

}  // namespace

// UX-01: a face of a loose box and a group, added to each other with Shift
// in either order: the face stays a face (not the whole box), the group a
// whole, and Delete takes just those.
void GuiPhase1::ux01MixedSelectionKeepsPartsAndWholes() {
  for (bool faceFirst : {true, false}) {
    Fixture f;
    QVERIFY(f.setUp(2000, 500));
    doc::Modeler& m = f.window.modeler();
    const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
    const doc::ObjectId inner = f.box(3000, 0, 1000, 1000, 1000);
    const doc::ObjectId group = *m.group({inner});
    f.vp->syncNow();
    f.window.activateTool(MainWindow::ToolId::Select);
    const QPoint top = f.vp->toWidget(gp_Pnt(500, 500, 1000));
    const QPoint g = f.vp->toWidget(gp_Pnt(3500, 500, 1000));
    clickWith(f.vp, faceFirst ? top : g, Qt::NoModifier);
    clickWith(f.vp, faceFirst ? g : top, Qt::ShiftModifier);
    const auto now = meaning(f.window);
    QCOMPARE(now.size(), std::size_t(2));
    QVERIFY(now.count(a) && now.count(group));
    QCOMPARE(now.at(a), std::make_tuple(false, 1, 0));  // the top face only
    QVERIFY(std::get<0>(now.at(group)));                 // the group as a whole
    // Delete: the top face goes (the box opens), the group goes.
    f.window.activateWindow();
    QTest::qWait(50);
    QTest::keyClick(f.vp, Qt::Key_Delete);
    QTest::qWait(30);
    QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
    QVERIFY(m.document().find(a));
    QCOMPARE(m.document().find(a)->kind(), doc::ObjectKind::Shell);
    QVERIFY(!m.document().find(group));
  }
}

// UX-07 (Windows SketchUp): Ctrl adds only, Shift toggles, Ctrl+Shift
// removes only - by click and by box.
void GuiPhase1::ux07CtrlAddsCtrlShiftRemoves() {
  Fixture f;
  QVERIFY(f.setUp(2000, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId inner = f.box(3000, 0, 1000, 1000, 1000);
  const doc::ObjectId group = *m.group({inner});
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  const QPoint top = f.vp->toWidget(gp_Pnt(500, 500, 1000));
  const QPoint front = f.vp->toWidget(gp_Pnt(500, 0, 500));
  const QPoint g = f.vp->toWidget(gp_Pnt(3500, 500, 1000));
  const Qt::KeyboardModifiers ctrl = Qt::ControlModifier, both = Qt::ControlModifier | Qt::ShiftModifier;
  clickWith(f.vp, top, Qt::NoModifier);
  clickWith(f.vp, g, ctrl);  // added
  QCOMPARE(meaning(f.window).size(), std::size_t(2));
  clickWith(f.vp, g, ctrl);  // Ctrl again: still selected (adds only)
  QCOMPARE(meaning(f.window).size(), std::size_t(2));
  clickWith(f.vp, top, ctrl);  // the face again: still there
  QCOMPARE(std::get<1>(meaning(f.window).at(a)), 1);
  clickWith(f.vp, front, both);  // not selected: Ctrl+Shift does not add it
  QCOMPARE(std::get<1>(meaning(f.window).at(a)), 1);
  clickWith(f.vp, g, both);  // selected: removed
  QCOMPARE(meaning(f.window).size(), std::size_t(1));
  QVERIFY(!meaning(f.window).count(group));
  clickWith(f.vp, top, both);
  QVERIFY(meaning(f.window).empty());
  // By box: Ctrl adds the group, Ctrl+Shift takes it away again.
  clickWith(f.vp, top, Qt::NoModifier);
  QRect r;
  for (int i = 0; i < 8; ++i) {
    r = r.united(QRect(f.vp->toWidget(gp_Pnt(i & 1 ? 4000 : 3000, i & 2 ? 1000 : 0, i & 4 ? 1000 : 0)), QSize(1, 1)));
  }
  r.adjust(-10, -10, 10, 10);
  auto boxWith = [&](Qt::KeyboardModifiers mods) {
    send(f.vp, QEvent::MouseButtonPress, r.topLeft(), Qt::LeftButton, Qt::LeftButton, mods);
    for (int i = 1; i <= 4; ++i) send(f.vp, QEvent::MouseMove, r.topLeft() + (r.bottomRight() - r.topLeft()) * i / 4, Qt::NoButton, Qt::LeftButton, mods);
    send(f.vp, QEvent::MouseButtonRelease, r.bottomRight(), Qt::LeftButton, Qt::NoButton, mods);
    QTest::qWait(20);
  };
  boxWith(ctrl);
  QCOMPARE(meaning(f.window).size(), std::size_t(2));
  QCOMPARE(std::get<1>(meaning(f.window).at(a)), 1);
  boxWith(ctrl);  // again: still both
  QCOMPARE(meaning(f.window).size(), std::size_t(2));
  boxWith(both);
  QCOMPARE(meaning(f.window).size(), std::size_t(1));
  QVERIFY(meaning(f.window).count(a));
}

// UX-04: a crossing box over the blank part of an L-shaped face's or a
// long diagonal line's bounding rectangle touches neither: nothing is
// selected (the old check used the projected bounding box); one touching
// the line takes the line's edge only.
void GuiPhase1::ux04BoxOverBlankSelectsNothing() {
  Fixture f;
  QVERIFY(f.setUp(3000, 3000));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId line = *m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(6000, 6000, 0)});
  QVERIFY(m.createPolygon({gp_Pnt(-4000, 0, 0), gp_Pnt(-1000, 0, 0), gp_Pnt(-1000, 1000, 0), gp_Pnt(-3000, 1000, 0),
                           gp_Pnt(-3000, 4000, 0), gp_Pnt(-4000, 4000, 0)})
              .ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  auto crossing = [&](const gp_Pnt& at) {
    const QPoint c = f.vp->toWidget(at);
    const QPoint from = c + QPoint(8, 8), to = c - QPoint(8, 8);
    send(f.vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 4; ++i) send(f.vp, QEvent::MouseMove, from + (to - from) * i / 4, Qt::NoButton, Qt::LeftButton);
    send(f.vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    QTest::qWait(20);
  };
  crossing(gp_Pnt(4500, 1500, 0));  // inside the line's box, far from it
  QVERIFY(f.window.selection().empty());
  crossing(gp_Pnt(-1500, 3000, 0));  // inside the L's box, in its notch
  QVERIFY(f.window.selection().empty());
  crossing(gp_Pnt(3000, 3000, 0));  // on the line
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{line});
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
}

namespace {

// The Scale tool with grips on `ids` (selected whole).
ScaleTool* scaleOn(MainWindow& w, const std::vector<doc::ObjectId>& ids) {
  w.activateTool(MainWindow::ToolId::Select);
  w.setSelection(ids);
  w.activateTool(MainWindow::ToolId::Scale);
  return dynamic_cast<ScaleTool*>(w.activeTool());
}

}  // namespace

// UX-02: the top face only (or one edge) selected: Scale does not stretch
// the whole box - refused with the reason, by grips and by base point.
void GuiPhase1::ux02ScalePartsRefused() {
  for (bool edge : {false, true}) {
    Fixture f;
    QVERIFY(f.setUp(500, 500));
    doc::Modeler& m = f.window.modeler();
    const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
    f.vp->syncNow();
    f.window.activateTool(MainWindow::ToolId::Select);
    click(f.vp, f.vp->toWidget(edge ? gp_Pnt(500, 0, 1000) : gp_Pnt(500, 500, 1000)));
    QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
    const std::size_t steps = m.history().undoCount();
    f.window.activateTool(MainWindow::ToolId::Scale);
    auto* tool = dynamic_cast<ScaleTool*>(f.window.activeTool());
    QVERIFY(tool);
    QVERIFY(!f.window.lastError().isEmpty() || f.window.statusText().contains(QStringLiteral("面・辺だけ")));
    if (tool->hasGrips()) {
      const gp_Pnt g = *tool->gripPoint(1, 1, 1);
      click(f.vp, f.vp->toWidget(g));
      move(f.vp, f.vp->toWidget(g.Translated(gp_Vec(200, 200, 200))));
      type(f.vp, QStringLiteral("2"));
    }
    // Base point, reference, typed factor.
    click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
    click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
    type(f.vp, QStringLiteral("2"));
    QVERIFY(std::abs(geom::volume(m.document().worldShape(box)) - 1e9) < 1e-3);
    QCOMPARE(m.history().undoCount(), steps);
    QVERIFY(!f.window.lastError().isEmpty());
  }
}

// UX-05: a grip moved to 1.5, then past the fixed side (not allowed), then
// clicked there: nothing is applied (not the earlier 1.5); a valid place
// afterwards still works. A typed size that cannot be applied (locked)
// changes nothing and keeps the drag.
void GuiPhase1::ux05ScaleInvalidPlaceDoesNotCommit() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  auto* tool = scaleOn(f.window, {box});
  QVERIFY(tool && tool->hasGrips());
  const std::size_t steps = m.history().undoCount();
  const gp_Pnt g = *tool->gripPoint(1, 0, 0);
  click(f.vp, f.vp->toWidget(g));
  move(f.vp, f.vp->toWidget(gp_Pnt(1500, 500, 500)));
  move(f.vp, f.vp->toWidget(gp_Pnt(-600, 500, 500)));  // past the fixed side
  click(f.vp, f.vp->toWidget(gp_Pnt(-600, 500, 500)));
  EXPECT_BOX(m.document().worldShape(box), 1000, 1000, 1000);
  QCOMPARE(m.history().undoCount(), steps);
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 500, 500)));  // a valid place: applied
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(box));
  QVERIFY2(std::abs(b.CornerMax().X() - 2000) < 20, qPrintable(QString::number(b.CornerMax().X())));
  QCOMPARE(m.history().undoCount(), steps + 1);

  // Locked: the typed factor is refused, nothing changes.
  QVERIFY(m.undo());
  QVERIFY(m.setLocked({box}, true).ok());
  tool = scaleOn(f.window, {box});
  if (tool && tool->hasGrips()) {
    const std::size_t before = m.history().undoCount();
    const gp_Pnt h = *tool->gripPoint(1, 0, 0);
    click(f.vp, f.vp->toWidget(h));
    type(f.vp, QStringLiteral("2"));
    EXPECT_BOX(m.document().worldShape(box), 1000, 1000, 1000);
    QCOMPARE(m.history().undoCount(), before);
    QVERIFY(!f.window.lastError().isEmpty());
  }
}

// UX-09: typed per direction with commas, an empty slot keeps its
// direction (never moves the value to another one); a wrong count is
// refused and changes nothing.
void GuiPhase1::ux09ScaleCommasKeepTheirAxes() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  auto* tool = scaleOn(f.window, {box});
  QVERIFY(tool && tool->hasGrips());
  auto grab = [&](int sx, int sy, int sz, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const gp_Pnt g = *tool->gripPoint(sx, sy, sz);
    const QPoint at = f.vp->toWidget(g);
    move(f.vp, at);
    send(f.vp, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, mods);
    send(f.vp, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
    QTest::qWait(20);
    move(f.vp, f.vp->toWidget(g.Translated(gp_Vec(sx, sy, sz) * 100)));
  };
  grab(1, 1, 0);  // the edge grip: X and Y
  type(f.vp, QStringLiteral(",2"));
  EXPECT_BOX(m.document().worldShape(box), 1000, 2000, 1000);
  QVERIFY(m.undo());
  tool->cancel();
  grab(1, 1, 0);
  type(f.vp, QStringLiteral("2,"));
  EXPECT_BOX(m.document().worldShape(box), 2000, 1000, 1000);
  QVERIFY(m.undo());
  tool->cancel();
  // A corner with Shift: each direction on its own; the middle one kept.
  grab(1, 1, 1, Qt::ShiftModifier);
  type(f.vp, QStringLiteral("2,,3"));
  EXPECT_BOX(m.document().worldShape(box), 2000, 1000, 3000);
  QVERIFY(m.undo());
  tool->cancel();
  // Too many values for the edge grip: refused, nothing changes.
  const std::size_t steps = m.history().undoCount();
  f.window.clearLastError();
  grab(1, 1, 0);
  type(f.vp, QStringLiteral("2,3,4"));
  EXPECT_BOX(m.document().worldShape(box), 1000, 1000, 1000);
  QCOMPARE(m.history().undoCount(), steps);
  QVERIFY(!f.window.lastError().isEmpty());
}

// UX-03: editing group G (A and B in it, C outside), nothing selected:
// Move on A moves A only (not G, not B); nested G / H / A editing H moves
// A only; a click on C outside the edited group moves nothing. Alt (grab
// corners) the same.
void GuiPhase1::ux03MoveInsideGroupTakesItsChild() {
  Fixture f;
  QVERIFY(f.setUp(2000, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId b = f.box(1500, 0, 1000, 1000, 1000);
  const doc::ObjectId c = f.box(4000, 0, 1000, 1000, 1000);
  const doc::ObjectId g = *m.group({a, b});
  f.vp->syncNow();
  auto origin = [&](const doc::ObjectId& id) {
    return geom::exactBounds(m.document().worldShape(*m.document().find(id))).CornerMin();
  };
  auto moveFrom = [&](const gp_Pnt& p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    f.window.setSelection({});
    f.window.activateTool(MainWindow::ToolId::Move);
    const QPoint at = f.vp->toWidget(p);
    move(f.vp, at);
    send(f.vp, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, mods);
    send(f.vp, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
    QTest::qWait(20);
    move(f.vp, f.vp->toWidget(p.Translated(gp_Vec(0, -800, 0))));
    type(f.vp, QStringLiteral("500"));
    QTest::keyClick(f.vp, Qt::Key_Escape);
  };
  f.window.enterGroup(g);
  const gp_Trsf gBefore = m.document().find(g)->transform;
  moveFrom(gp_Pnt(500, 0, 500));  // A's front
  QVERIFY(origin(a).Distance(gp_Pnt(0, -500, 0)) < 1e-6);
  QVERIFY(origin(b).Distance(gp_Pnt(1500, 0, 0)) < 1e-6);
  QVERIFY(m.document().find(g)->transform.TranslationPart().IsEqual(gBefore.TranslationPart(), 1e-9));
  // C outside the edited group: not moved, said why.
  f.window.clearLastError();
  moveFrom(gp_Pnt(4500, 0, 500));
  QVERIFY(origin(c).Distance(gp_Pnt(4000, 0, 0)) < 1e-6);
  QVERIFY(!f.window.lastError().isEmpty());
  // Alt on C's corner (reshape) from inside G: refused too.
  f.window.clearLastError();
  moveFrom(gp_Pnt(4000, 0, 1000), Qt::AltModifier);
  QVERIFY(origin(c).Distance(gp_Pnt(4000, 0, 0)) < 1e-6);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(c)) - 1e9) < 1e-3);
  f.window.exitGroup();

  // Nested G / H / A, editing H: A alone moves.
  const doc::ObjectId h = *m.group({a});
  QCOMPARE(m.document().find(h)->parent, g);
  f.window.enterGroup(g);
  f.window.enterGroup(h);
  const gp_Pnt aBefore = origin(a), bBefore = origin(b);
  moveFrom(gp_Pnt(500, -500, 500));  // A's front (moved above)
  QVERIFY(origin(a).Distance(aBefore.Translated(gp_Vec(0, -500, 0))) < 1e-6);
  QVERIFY(origin(b).Distance(bBefore) < 1e-6);
  QVERIFY(m.document().find(h)->transform.TranslationPart().Modulus() < 1e-9);
}

// UX-06: the same negative value typed again right after gives the same
// result (it counts from the way the cursor showed, not from the last
// typed sign): rectangle sides and rotation angles; all one undo step.
void GuiPhase1::ux06SameNegativeValueTwiceSameResult() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  const std::size_t steps = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 1000, 0)));  // drawn towards +X, +Y
  auto xs = [&] {
    const Bnd_Box b = geom::exactBounds(m.document().worldShape(m.document().objects().back()));
    return std::make_pair(std::lround(b.CornerMin().X()), std::lround(b.CornerMax().X()));
  };
  type(f.vp, QStringLiteral("-3000,"));
  QCOMPARE(xs(), std::make_pair(-3000L, 0L));
  type(f.vp, QStringLiteral("-3000,"));  // again: the same
  QCOMPARE(xs(), std::make_pair(-3000L, 0L));
  type(f.vp, QStringLiteral("3000,"));
  QCOMPARE(xs(), std::make_pair(0L, 3000L));
  type(f.vp, QStringLiteral("-3000,"));
  QCOMPARE(xs(), std::make_pair(-3000L, 0L));
  QCOMPARE(m.history().undoCount(), steps + 1);

  // Rotation: a box rotated 90 about its corner, then -30 typed twice.
  const doc::ObjectId box = f.box(5000, 0, 1000, 200, 200);
  f.window.activateTool(MainWindow::ToolId::Select);
  f.window.setSelection({box});
  f.window.activateTool(MainWindow::ToolId::Rotate);
  click(f.vp, f.vp->toWidget(gp_Pnt(5000, 0, 0)));     // centre
  click(f.vp, f.vp->toWidget(gp_Pnt(6000, 0, 0)));     // reference: +X
  move(f.vp, f.vp->toWidget(gp_Pnt(5000, 1000, 0)));   // towards +Y (counter-clockwise)
  type(f.vp, QStringLiteral("90"));
  auto farCorner = [&] {
    const Bnd_Box b = geom::exactBounds(m.document().worldShape(box));
    return std::make_pair(std::lround(b.CornerMax().X()), std::lround(b.CornerMax().Y()));
  };
  const auto ninety = farCorner();
  type(f.vp, QStringLiteral("-30"));
  const auto once = farCorner();
  type(f.vp, QStringLiteral("-30"));
  QCOMPARE(farCorner(), once);  // the same -30, not +30
  type(f.vp, QStringLiteral("90"));
  QCOMPARE(farCorner(), ninety);
  QVERIFY(once != ninety);
}

// UX-08: in Rotate, → / ← / ↑ hold the rotation plane across red / green /
// blue (SketchUp), also with the centre on a face; 90 typed: an uneven box
// 1000 x 200 x 100 turns about that axis.
void GuiPhase1::ux08RotateArrowsSetThePlane() {
  struct Case {
    Qt::Key key;
    gp_Pnt reference;  // a point away from the centre in that plane
    long dx, dy, dz;   // the extents afterwards
  };
  const Case cases[] = {
      {Qt::Key_Right, gp_Pnt(0, -1500, 0), 1000, 100, 200},
      {Qt::Key_Left, gp_Pnt(-1500, 0, 0), 100, 200, 1000},
      {Qt::Key_Up, gp_Pnt(-1500, 0, 0), 200, 1000, 100},
  };
  for (const Case& c : cases) {
    Fixture f;
    QVERIFY(f.setUp(0, 0));
    doc::Modeler& m = f.window.modeler();
    const doc::ObjectId box = f.box(0, 0, 1000, 200, 100);
    f.vp->syncNow();
    f.window.activateTool(MainWindow::ToolId::Select);
    f.window.setSelection({box});
    f.window.activateTool(MainWindow::ToolId::Rotate);
    QTest::keyClick(f.vp, c.key);
    click(f.vp, f.vp->toWidget(gp_Pnt(500, 100, 100)));  // the centre, on the top face
    click(f.vp, f.vp->toWidget(c.reference));
    type(f.vp, QStringLiteral("90"));
    QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
    const Bnd_Box b = geom::exactBounds(m.document().worldShape(box));
    const long dx = std::lround(b.CornerMax().X() - b.CornerMin().X());
    const long dy = std::lround(b.CornerMax().Y() - b.CornerMin().Y());
    const long dz = std::lround(b.CornerMax().Z() - b.CornerMin().Z());
    QVERIFY2(dx == c.dx && dy == c.dy && dz == c.dz,
             qPrintable(QStringLiteral("key %1: %2 x %3 x %4").arg(int(c.key)).arg(dx).arg(dy).arg(dz)));
  }
}

// UX-10: Rotate + Ctrl on the top face only (or one edge, or both): the
// preview shows just those, as the copy makes them; the box stays as it
// was. Without Ctrl nothing is previewed (parts are not turned alone).
void GuiPhase1::ux10RotateCopyPreviewShowsTheParts() {
  struct Case {
    std::vector<gp_Pnt> picks;
    int faces, edgesAlone;
  };
  const Case cases[] = {
      {{gp_Pnt(500, 500, 1000)}, 1, 0},
      {{gp_Pnt(500, 0, 1000)}, 0, 1},
      {{gp_Pnt(500, 500, 1000), gp_Pnt(0, 500, 0)}, 1, 1},
  };
  for (const Case& c : cases) {
    Fixture f;
    QVERIFY(f.setUp(500, 500));
    doc::Modeler& m = f.window.modeler();
    const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
    f.vp->syncNow();
    f.window.activateTool(MainWindow::ToolId::Select);
    for (std::size_t i = 0; i < c.picks.size(); ++i) clickWith(f.vp, f.vp->toWidget(c.picks[i]), i ? Qt::ShiftModifier : Qt::NoModifier);
    QCOMPARE(f.window.elementSelection().size(), c.picks.size());
    f.window.activateTool(MainWindow::ToolId::Rotate);
    QTest::keyClick(f.vp, Qt::Key_Up);                     // about blue
    click(f.vp, f.vp->toWidget(gp_Pnt(2000, 2000, 0)));    // centre beside the box
    click(f.vp, f.vp->toWidget(gp_Pnt(3000, 2000, 0)));    // reference +X
    move(f.vp, f.vp->toWidget(gp_Pnt(2000, 3000, 0)));     // 90
    QVERIFY(f.vp->previewShape().IsNull());                // no Ctrl: nothing shown
    QTest::keyClick(f.vp, Qt::Key_Control);
    move(f.vp, f.vp->toWidget(gp_Pnt(2001, 3000, 0)));
    const TopoDS_Shape preview = f.vp->previewShape();
    QVERIFY(!preview.IsNull());
    int faces = 0;
    for (TopExp_Explorer e(preview, TopAbs_FACE); e.More(); e.Next()) ++faces;
    int loose = 0;
    for (TopExp_Explorer e(preview, TopAbs_EDGE, TopAbs_FACE); e.More(); e.Next()) ++loose;
    QCOMPARE(faces, c.faces);
    QCOMPARE(loose, c.edgesAlone);
    const Bnd_Box shown = geom::exactBounds(preview);
    click(f.vp, f.vp->toWidget(gp_Pnt(2001, 3000, 0)));
    QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
    QVERIFY(std::abs(geom::volume(m.document().worldShape(box)) - 1e9) < 1e-3);  // the box as it was
    // What was made lies where the preview showed it.
    Bnd_Box made;
    for (const doc::Object& o : m.document().objects()) {
      if (o.id != box) made.Add(geom::exactBounds(m.document().worldShape(o)));
    }
    QVERIFY(!made.IsVoid());
    QVERIFY(made.CornerMin().Distance(shown.CornerMin()) < 1.0 && made.CornerMax().Distance(shown.CornerMax()) < 1.0);
  }
}

// Follow Me B: the profile clicked first, the path traced with the cursor
// along the lines drawn one segment at a time: it grows and shrinks with
// the cursor (highlighted, previewed), Esc stops with nothing changed, a
// click at the end makes what the preview showed - the same as when the
// edges are selected first.
void GuiPhase1::followMeTracedFromTheProfile() {
  const std::vector<gp_Pnt> points{gp_Pnt(1000, 0, 0), gp_Pnt(1000, 3000, 0), gp_Pnt(4000, 4500, 0),
                                   gp_Pnt(6000, 3500, 0)};
  double length = 0;
  for (std::size_t i = 1; i < points.size(); ++i) length += points[i - 1].Distance(points[i]);
  Fixture f;
  QVERIFY(f.setUp(3000, 2000));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId profile =
      *m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 1500), gp_Pnt(0, 0, 1500)});
  for (std::size_t i = 1; i < points.size(); ++i) QVERIFY(m.createPolyline({points[i - 1], points[i]}).ok());
  f.vp->syncNow();
  const std::size_t objects = m.document().objects().size();
  auto mid = [&](std::size_t i) { return f.vp->toWidget(gp_Pnt((points[i - 1].XYZ() + points[i].XYZ()) * 0.5)); };
  f.window.activateTool(MainWindow::ToolId::FollowMe);
  auto* tool = dynamic_cast<FollowMeTool*>(f.window.activeTool());
  QVERIFY(tool);
  const std::size_t steps = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 1000)));  // the profile
  for (std::size_t i = 1; i < points.size(); ++i) {
    move(f.vp, mid(i));
    QCOMPARE(tool->tracedEdges(), i);
    QVERIFY(!f.vp->previewShape().IsNull());
  }
  move(f.vp, mid(1));  // back: shorter again
  QCOMPARE(tool->tracedEdges(), std::size_t(1));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QCOMPARE(m.document().objects().size(), objects);
  QCOMPARE(m.history().undoCount(), steps);
  QVERIFY(f.vp->previewShape().IsNull());
  // Again, to the end, and click there.
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 1000)));
  for (std::size_t i = 1; i < points.size(); ++i) move(f.vp, mid(i));
  const double previewed = geom::volume(f.vp->previewShape());
  click(f.vp, mid(points.size() - 1));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.history().undoCount(), steps + 1);
  QVERIFY(!m.document().find(profile));
  std::optional<doc::ObjectId> swept;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() == doc::ObjectKind::Solid) swept = o.id;
  }
  QVERIFY(swept.has_value());
  const double made = geom::volume(m.document().worldShape(*swept));
  QVERIFY(std::abs(made - 2000.0 * 1500 * length) < 1.0);
  QVERIFY(std::abs(made - previewed) < 1e-3);
  QVERIFY(m.undo());
  QVERIFY(m.document().find(profile));
}

// MOVE-05: a selected face moves sideways too: ↓ lets go of the normal
// lock (the top slides over: a slanted box, same volume), → holds X (the
// front slides along X); along the normal it still stretches the box. One
// undo step each; the box stays a valid solid.
void GuiPhase1::move05FaceMovesSideways() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  auto moveFace = [&](const gp_Pnt& on, Qt::Key key, const gp_Pnt& towards, const QString& typed) {
    f.window.activateTool(MainWindow::ToolId::Select);
    click(f.vp, f.vp->toWidget(on));
    QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
    f.window.activateTool(MainWindow::ToolId::Move);
    click(f.vp, f.vp->toWidget(on));
    if (key != Qt::Key_unknown) QTest::keyClick(f.vp, key);
    move(f.vp, f.vp->toWidget(towards));
    type(f.vp, typed);
    QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
    f.vp->syncNow();
  };
  const std::size_t steps = m.history().undoCount();
  // The top, slid 300 along +X.
  moveFace(gp_Pnt(500, 500, 1000), Qt::Key_Down, gp_Pnt(800, 500, 1000), QStringLiteral("300"));
  TopoDS_Shape s = m.document().worldShape(box);
  QVERIFY(geom::validateShape(s, true).valid);
  QVERIFY(std::abs(geom::volume(s) - 1e9) < 1e-3);
  Bnd_Box b = geom::exactBounds(s);
  // Free (↓): along the cursor's way, as shown on screen (within a pixel).
  QVERIFY2(std::abs(b.CornerMax().X() - 1300) < 1.0 && std::abs(b.CornerMax().Z() - 1000) < 1e-6,
           qPrintable(QStringLiteral("%1 %2").arg(b.CornerMax().X()).arg(b.CornerMax().Z())));
  QCOMPARE(m.history().undoCount(), steps + 1);
  QVERIFY(m.undo());
  f.vp->syncNow();
  // The front (y = 0), held to X with →, 200.
  moveFace(gp_Pnt(500, 0, 500), Qt::Key_Right, gp_Pnt(800, 0, 500), QStringLiteral("200"));
  s = m.document().worldShape(box);
  QVERIFY(geom::validateShape(s, true).valid);
  QVERIFY(std::abs(geom::volume(s) - 1e9) < 1e-3);
  b = geom::exactBounds(s);
  QVERIFY(std::abs(b.CornerMax().X() - 1200) < 1e-6 && std::abs(b.CornerMin().Y()) < 1e-6);
  QVERIFY(m.undo());
  f.vp->syncNow();
  // Along the normal (the default lock): still a stretch.
  moveFace(gp_Pnt(500, 500, 1000), Qt::Key_unknown, gp_Pnt(500, 500, 1400), QStringLiteral("500"));
  QVERIFY(std::abs(geom::volume(m.document().worldShape(box)) - 1.5e9) < 1e-3);
}

// STATE-03: saved (and autosaved) with only a line's start point placed:
// the file holds what was there before, nothing half drawn; the drawing
// goes on afterwards.
void GuiPhase1::state03SaveWhileDrawingKeepsTheLastState() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 1000, 1000, 1000);
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));  // the start only
  const QString file = QDir(QDir::tempPath()).filePath(QStringLiteral("state03.sbr"));
  QVERIFY(f.window.saveFile(file));
  QVERIFY(f.window.autosaveNow());
  doc::Modeler back;
  QVERIFY(back.load(file.toStdString()).ok());
  QCOMPARE(back.document().objects().size(), std::size_t(1));  // the box, no line
  // Still drawing: the second point makes the line.
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 0, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QCOMPARE(m.document().objects().size(), std::size_t(2));
}

// LOOK-06: what is not shown is not snapped to or picked: an object hidden,
// or on a tag turned off.
void GuiPhase1::look06HiddenIsNotSnappedOrPicked() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId b = f.box(3000, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Line);
  auto tipAt = [&](const gp_Pnt& p) {
    move(f.vp, f.vp->toWidget(p));
    return f.vp->snapTip();
  };
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 1000)), QStringLiteral("端点"));
  QVERIFY(m.setVisible(a, false).ok());
  f.vp->syncNow();
  QVERIFY(tipAt(gp_Pnt(1000, 0, 1000)) != QStringLiteral("端点"));
  // A tag turned off.
  QVERIFY(m.setLayer(b, "Hidden tag").ok());
  m.setTagVisible("Hidden tag", false);
  f.vp->syncNow();
  QVERIFY(tipAt(gp_Pnt(4000, 0, 1000)) != QStringLiteral("端点"));
  // Not picked either.
  QTest::keyClick(f.vp, Qt::Key_Escape);
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 500)));
  click(f.vp, f.vp->toWidget(gp_Pnt(3500, 0, 500)));
  QVERIFY(f.window.selection().empty());
}

// LOOK-06: what is drawn is what snaps and picks. A corner hidden behind
// the box is not snapped, in X-ray it is; with a section cut, what is cut
// away is neither snapped nor picked, what is left still is.
void GuiPhase1::look06SectionAndXRayRules() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Line);
  auto tipAt = [&](const gp_Pnt& p) {
    move(f.vp, f.vp->toWidget(gp_Pnt(-3000, -3000, 0)));  // away first: no sticky point
    move(f.vp, f.vp->toWidget(p));
    return f.vp->snapTip();
  };
  const gp_Pnt hiddenCorner(1000, 1000, 0);
  QVERIFY(tipAt(hiddenCorner) != QStringLiteral("端点"));
  f.vp->setXRay(true);
  {
    const QString tip = tipAt(hiddenCorner);
    const gp_Pnt at = f.vp->snapTipPoint();
    QVERIFY2(tip == QStringLiteral("端点") && at.Distance(hiddenCorner) < 1.0,
             qPrintable(tip + QStringLiteral(" %1 %2 %3").arg(at.X()).arg(at.Y()).arg(at.Z())));
  }
  f.vp->setXRay(false);
  QVERIFY(tipAt(hiddenCorner) != QStringLiteral("端点"));

  // Everything above 500 cut away.
  doc::SectionPlane plane;
  plane.origin = {0, 0, 500};
  plane.normal = {0, 0, 1};
  m.setSection(plane);
  f.vp->syncNow();
  QVERIFY(tipAt(gp_Pnt(1000, 0, 1000)) != QStringLiteral("端点"));
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 0)), QStringLiteral("端点"));
  // In X-ray too, the cut-away part stays out.
  f.vp->setXRay(true);
  QVERIFY(tipAt(gp_Pnt(1000, 0, 1000)) != QStringLiteral("端点"));
  f.vp->setXRay(false);

  // Picking: a click on the cut-away top edge does not take it (nor the
  // top face), only what is seen through there, cut faces included.
  QTest::keyClick(f.vp, Qt::Key_Escape);
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 1000)));
  for (const auto& e : f.window.elementSelection()) {
    const Bnd_Box b = geom::exactBounds(f.vp->elementShape(e));
    QVERIFY2(b.CornerMin().Z() < 500, qPrintable(QString::number(b.CornerMin().Z())));
  }
  // What is left is picked as before.
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 250)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  m.setSection(std::nullopt);
}

// W8 (guide calibration): a length typed right after measuring resizes the
// model (after a question; No leaves it), one undo step; inside a group,
// only that group's contents.
void GuiPhase1::measureResizesTheModel() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId b = f.box(3000, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  auto sizeOf = [&](const doc::ObjectId& id) {
    const Bnd_Box box = geom::exactBounds(m.document().worldShape(*m.document().find(id)));
    return box.CornerMax().X() - box.CornerMin().X();
  };
  auto answer = [&](QMessageBox::StandardButton button, bool* asked) {
    QTimer::singleShot(200, [=] {
      auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
      if (!box) return;
      *asked = true;
      box->button(button)->click();
    });
  };
  f.window.activateTool(MainWindow::ToolId::Measure);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  bool asked = false;
  answer(QMessageBox::No, &asked);
  type(f.vp, QStringLiteral("2000"));
  QVERIFY(asked);
  QVERIFY(std::abs(sizeOf(a) - 1000) < 1e-6);
  asked = false;
  answer(QMessageBox::Yes, &asked);
  type(f.vp, QStringLiteral("2000"));
  QVERIFY(asked);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(sizeOf(a) - 2000) < 1e-6);
  QVERIFY(std::abs(sizeOf(b) - 2000) < 1e-6);
  // About the origin: b moved out too.
  QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(*m.document().find(b))).CornerMin().X() - 6000) < 1e-6);
  QVERIFY(m.undo());
  QVERIFY(std::abs(sizeOf(a) - 1000) < 1e-6 && std::abs(sizeOf(b) - 1000) < 1e-6);

  // Inside a group: only its contents, about the group's origin.
  QTest::keyClick(f.vp, Qt::Key_Escape);
  const auto group = m.group({b}, "G");
  QVERIFY(group);
  f.window.enterGroup(*group);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Measure);
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(4000, 0, 0)));
  asked = false;
  answer(QMessageBox::Yes, &asked);
  type(f.vp, QStringLiteral("500"));
  QVERIFY(asked);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(sizeOf(a) - 1000) < 1e-6);
  QVERIFY(std::abs(sizeOf(b) - 500) < 1e-6);
  f.window.exitGroup();
}

// W8: the rotated rectangle - first side in any direction (length typed),
// the other side across it (length typed, or snapped to a point of the
// model to tilt it); a rectangle profile (width / height editable).
void GuiPhase1::rotatedRectangleTool() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::RotatedRectangle);
  auto newest = [&]() -> const doc::Object& { return m.document().objects().back(); };
  // On the ground, turned 45 degrees: 2000 x 500.
  // (Cursor places far from the blue axis on screen: near it, the side
  // would rightly follow blue.)
  click(f.vp, f.vp->toWidget(gp_Pnt(5000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(8000, 3000, 0)));
  type(f.vp, QStringLiteral("2000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(3500, 4500, 0)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const auto* r = std::get_if<doc::RectangleProfile>(&newest().recipe.base);
  QVERIFY(r);
  QVERIFY(std::abs(std::abs(r->width) - 2000) < 1e-6 && std::abs(std::abs(r->height) - 500) < 1e-6);
  const TopoDS_Face face = TopoDS::Face(m.document().worldShape(newest()));
  QVERIFY(std::abs(geom::area(face) - 1e6) < 1.0);
  QVERIFY(geom::planarFaceNormal(face)->IsParallel(gp::DZ(), 1e-9));
  const Bnd_Box b = geom::exactBounds(face);
  QVERIFY2(b.CornerMax().X() - b.CornerMin().X() > 1500, "turned, not along X");  // ~1414 + 354 across

  // Tilted: corner, first side and the other side snapped to a box's corners.
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  const std::size_t before = m.document().objects().size();
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  // Esc takes the corner back; then again.
  QTest::keyClick(f.vp, Qt::Key_Escape);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 1000, 1000)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), before + 1);
  const TopoDS_Face tilted = TopoDS::Face(m.document().worldShape(newest()));
  QVERIFY2(std::abs(geom::area(tilted) - 1000.0 * 1000 * std::sqrt(2.0)) < 1.0, qPrintable(QString::number(geom::area(tilted))));
  const gp_Dir n = *geom::planarFaceNormal(tilted);
  QVERIFY(std::abs(n.X()) < 1e-9 && std::abs(std::abs(n.Y()) - std::sqrt(0.5)) < 1e-9);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(box)) - 1e9) < 1e-3);
}

// W8: an arc started at the end of a line snaps to the bulge that carries
// on from the line smoothly ([接線]); the arc's way at the start is the
// line's way exactly.
void GuiPhase1::arcCarriesOnTangent() {
  Fixture f;
  QVERIFY(f.setUp(2000, 500));
  doc::Modeler& m = f.window.modeler();
  QVERIFY(m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0)}).ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Arc);
  QTest::keyClick(f.vp, Qt::Key_Control);  // the arc as a line, not a face
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 0)));  // the line's end
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 1000, 0)));
  // The tangent bulge: half chord x tan(22.5 deg) to the right of the chord.
  move(f.vp, f.vp->toWidget(gp_Pnt(2720, 310, 0)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY2(f.window.statusText().contains(QStringLiteral("接線")), qPrintable(f.window.statusText()));
  click(f.vp, f.vp->toWidget(gp_Pnt(2720, 310, 0)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const doc::Object& arc = m.document().objects().back();
  TopExp_Explorer e(m.document().worldShape(arc), TopAbs_EDGE);
  QVERIFY(e.More());
  BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
  gp_Pnt p;
  gp_Vec d;
  c.D1(c.FirstParameter(), p, d);
  if (p.Distance(gp_Pnt(2000, 0, 0)) > 1e-6) {
    c.D1(c.LastParameter(), p, d);
    d.Reverse();
  }
  QVERIFY(p.Distance(gp_Pnt(2000, 0, 0)) < 1e-6);
  QVERIFY2(gp_Dir(d).IsParallel(gp::DX(), 1e-6) && d.X() > 0,
           qPrintable(QStringLiteral("%1 %2 %3").arg(d.X()).arg(d.Y()).arg(d.Z())));
}

void GuiPhase1::scaleGripsOnComponentsAndGroups() {
  Fixture f;
  QVERIFY(f.setUp(1500, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  auto component = m.makeComponent({box});
  QVERIFY(component.ok());
  gp_Trsf away;
  away.SetTranslation(gp_Vec(3000, 0, 0));
  auto other = m.copy({*component}, away, 1);
  QVERIFY(other.ok());
  f.vp->syncNow();
  auto size = [&](const doc::ObjectId& id) {
    Bnd_Box b;
    for (const doc::ObjectId& leaf : m.document().subtree(id)) {
      const doc::Object* o = m.document().find(leaf);
      if (o && !o->isContainer() && !o->shape.IsNull()) b.Add(geom::exactBounds(m.document().worldShape(*o)));
    }
    return b.CornerMax().X() - b.CornerMin().X();
  };
  auto grab = [&](ScaleTool* tool, int sx, int sy, int sz, const QString& typed) {
    const gp_Pnt g = *tool->gripPoint(sx, sy, sz);
    click(f.vp, f.vp->toWidget(g));
    move(f.vp, f.vp->toWidget(g.Translated(gp_Vec(sx, sy, sz) * 300)));
    type(f.vp, typed);
  };
  // The component: corner grip, typed factor.
  f.window.activateTool(MainWindow::ToolId::Select);
  f.window.setSelection({*component});
  f.window.activateTool(MainWindow::ToolId::Scale);
  auto* tool = dynamic_cast<ScaleTool*>(f.window.activeTool());
  QVERIFY(tool && tool->hasGrips());
  grab(tool, 1, 1, 1, QStringLiteral("2"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY2(std::abs(size(*component) - 2000) < 1e-6, qPrintable(QString::number(size(*component))));
  QVERIFY(std::abs(size(other->front()) - 1000) < 1e-6);
  QVERIFY(m.undo());
  QVERIFY(std::abs(size(*component) - 1000) < 1e-6);

  // A group: grips around what is in it; one direction, then the corner.
  const doc::ObjectId a = f.box(0, 3000, 1000, 1000, 1000);
  const doc::ObjectId b = f.box(1500, 3000, 500, 1000, 1000);
  auto group = m.group({a, b});
  QVERIFY(group.ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  f.window.setSelection({*group});
  f.window.activateTool(MainWindow::ToolId::Scale);
  tool = dynamic_cast<ScaleTool*>(f.window.activeTool());
  QVERIFY(tool && tool->hasGrips());
  QVERIFY(std::abs(size(*group) - 2000) < 1e-6);
  grab(tool, 1, 0, 0, QStringLiteral("4000mm"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY2(std::abs(size(*group) - 4000) < 1e-6, qPrintable(QString::number(size(*group))));
  grab(tool, 1, 1, 1, QStringLiteral("0.5"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY2(std::abs(size(*group) - 2000) < 1e-6, qPrintable(QString::number(size(*group))));
}

// Report (screenshot): inside a group, a wall front in vertical strips; a
// horizontal line drawn across all of them (from outside to outside) lay on
// top without dividing: selecting picked the whole strip. Each strip is now
// divided; a part is picked alone and pushed on its own.
void GuiPhase1::lineAcrossStripsInAGroupDividesEach() {
  Fixture f;
  QVERIFY(f.setUp(1500, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 3000, 1000, 3000);
  QVERIFY(m.splitWithLine({gp_Pnt(1000, 0, 0), gp_Pnt(1000, 0, 3000)}).ok());
  QVERIFY(m.splitWithLine({gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 3000)}).ok());
  auto group = m.group({box});
  QVERIFY(group.ok());
  f.vp->syncNow();
  f.window.enterGroup(*group);
  auto faces = [&] {
    int n = 0;
    for (TopExp_Explorer e(m.document().worldShape(*m.document().find(box)), TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
  };
  QCOMPARE(faces(), 8);
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 1200)));  // on the left edge
  QTest::keyClick(f.vp, Qt::Key_Right);              // along X
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 1200)));
  type(f.vp, QStringLiteral("3600"));                // past the right edge
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(faces(), 11);
  // A lower part of the middle strip is picked alone.
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 0, 500)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  const TopoDS_Shape picked = f.vp->elementShape(f.window.elementSelection().front());
  // The part below the line (its start was snapped on the edge near 1200),
  // not the whole 3000 high strip.
  const double area = geom::area(picked);
  QVERIFY2(area > 1000.0 * 1100 && area < 1000.0 * 1300, qPrintable(QString::number(area)));
}

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
  // "/4" divides the first distance the user gave (3000), not the array's
  // 12000 (ARR-02, provisional until compared with SketchUp).
  type(f.vp, QStringLiteral("/4"));
  all.SetVoid();
  for (const doc::Object& o : m.document().objects()) all.Add(geom::exactBounds(m.document().worldShape(o)));
  QVERIFY(std::abs(all.CornerMax().X() - 3300) < 1e-6);
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
  // Request T2 / OFF-01: the original is now the frame around it, and a
  // typed value right after moves the frame's hole along.
  QVERIFY(std::abs(geom::area(m.document().find(*rect)->shape) - (2000.0 * 2000.0 - 1800.0 * 1800.0)) < 1e-3);
  type(f.vp, QStringLiteral("300"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  QVERIFY(std::abs(geom::area(m.document().find(*rect)->shape) - (2000.0 * 2000.0 - 1400.0 * 1400.0)) < 1e-3);
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

  // 3. Typing again right after the push edits the same operation (VCB
  // re-edit); it stays one undo step with the push itself.
  const std::size_t steps = m.history().undoCount();
  type(&vp, QStringLiteral("3500"));
  EXPECT_BOX(m.document().find(id)->shape, 4000, 6000, 3500);
  QCOMPARE(m.history().undoCount(), steps);
  QVERIFY(m.undo());
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Face);
  QVERIFY(m.redo());
  EXPECT_BOX(m.document().find(id)->shape, 4000, 6000, 3500);

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

  // 4. Save, reopen, and re-edit the extrusion parametrically (3500 -> 4500).
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
  EXPECT_BOX(m2.document().objects().front().shape, 4000, 6000, 3500);
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
  // The face with its edges (SketchUp): the top and the sides it held go;
  // the bottom stays (open shape) and the 4 vertical edges stay as lines.
  deleteAction->trigger();
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Face);  // the bottom is left
  QCOMPARE(m.document().objects().size(), std::size_t(1 + 4));
  QVERIFY(m.undo());
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Solid);
  f.vp->syncNow();
  QTest::qWait(500);  // a fresh click series

  click(f.vp, top);
  doubleClick(f.vp, top);
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

// Pointing at a face or an edge shows nothing (no yellow pre-highlight);
// a selected edge stays shown as selected when the cursor moves on.
void GuiPhase1::hoverDoesNotHighlightAndEdgeSelectionStays() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  const QPoint empty = f.vp->toWidget(gp_Pnt(4000, 4000, 0));
  const QPoint top = f.vp->toWidget(gp_Pnt(500, 500, 1000));
  const gp_Pnt edgeMid(500, 0, 1000);
  const QPoint edge = f.vp->toWidget(edgeMid);
  auto shot = [&] {
    move(f.vp, empty);  // nothing under the cursor first
    f.vp->renderNow();
    const QString path = QDir::temp().filePath(QStringLiteral("sbr_hover.png"));
    f.vp->dumpImage(path);
    return QImage(path);
  };
  auto pixelsDiffer = [](const QImage& a, const QImage& b) {
    int n = 0;
    for (int y = 0; y < std::min(a.height(), b.height()); y += 2) {
      for (int x = 0; x < std::min(a.width(), b.width()); x += 2) n += a.pixel(x, y) != b.pixel(x, y);
    }
    return n;
  };
  const QImage plain = shot();
  // Hovering the top face, then an edge: the picture stays the same.
  for (const QPoint& at : {top, edge}) {
    move(f.vp, at);
    f.vp->renderNow();
    const QString path = QDir::temp().filePath(QStringLiteral("sbr_hover_at.png"));
    f.vp->dumpImage(path);
    QVERIFY2(pixelsDiffer(plain, QImage(path)) < 20, qPrintable(QString::number(pixelsDiffer(plain, QImage(path)))));
  }
  // Click the edge: selected; moving away keeps it selected and shown.
  click(f.vp, edge);
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QCOMPARE(f.window.elementSelection().front().type, TopAbs_EDGE);
  const QImage selected = shot();
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QVERIFY2(pixelsDiffer(plain, selected) > 20, "the selected edge is not shown");
  move(f.vp, top);
  QTest::qWait(30);
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  const QImage still = shot();
  QVERIFY2(pixelsDiffer(selected, still) < 20, qPrintable(QString::number(pixelsDiffer(selected, still))));

  // The shape changes (here pushed up): the selected edge no longer exists
  // as it was, so it is let go and nothing stays drawn where it was.
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = m.document().objects().front().id;
  TopoDS_Face topFace;
  for (TopExp_Explorer e(m.document().worldShape(box), TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->Z() > 0.99) topFace = TopoDS::Face(e.Current());
  }
  QVERIFY(m.pushPull(box, topFace, 500).ok());
  QTest::qWait(50);
  QVERIFY(f.window.elementSelection().empty());
  const QImage after = shot();
  f.vp->setHighlightedElements({});
  const QImage clean = shot();
  QVERIFY2(pixelsDiffer(after, clean) < 5, qPrintable(QString::number(pixelsDiffer(after, clean))));
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
                  MainWindow::ToolId::Polygon, MainWindow::ToolId::Scale, MainWindow::ToolId::Wall,
                  MainWindow::ToolId::Slab, MainWindow::ToolId::Opening, MainWindow::ToolId::RotatedRectangle}) {
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

// SketchUp's scale grips: the +X face grip makes the 1000 cube 2000 wide
// only, the top grip 3000 high only (the opposite side stays); a corner
// keeps the proportions; Ctrl scales about the centre. One undo each.
void GuiPhase1::scaleGripsStretchEachDirection() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.window.setSelection({box});
  f.window.activateTool(MainWindow::ToolId::Scale);
  f.window.setSelection({box});
  auto* tool = dynamic_cast<ScaleTool*>(f.window.activeTool());
  QVERIFY(tool);
  if (!tool->hasGrips()) {  // the selection came after activation
    f.window.activateTool(MainWindow::ToolId::Scale);
    tool = dynamic_cast<ScaleTool*>(f.window.activeTool());
  }
  QVERIFY(tool && tool->hasGrips());
  QCOMPARE(f.vp->gripCount(), std::size_t(26));  // small cubes
  const std::size_t steps = m.history().undoCount();
  auto drag = [&](int sx, int sy, int sz, const QString& typed) {
    const gp_Pnt g = *tool->gripPoint(sx, sy, sz);
    click(f.vp, f.vp->toWidget(g));
    move(f.vp, f.vp->toWidget(g.Translated(gp_Vec(sx, sy, sz) * 200)));
    type(f.vp, typed);
  };
  drag(1, 0, 0, QStringLiteral("2000mm"));
  EXPECT_BOX(m.document().worldShape(box), 2000, 1000, 1000);
  drag(0, 0, 1, QStringLiteral("3000mm"));
  EXPECT_BOX(m.document().worldShape(box), 2000, 1000, 3000);
  QCOMPARE(m.history().undoCount(), steps + 2);
  // Typed again right after: the last stretch is changed, not stacked.
  type(f.vp, QStringLiteral("2"));
  EXPECT_BOX(m.document().worldShape(box), 2000, 1000, 2000);
  QCOMPARE(m.history().undoCount(), steps + 2);
  // A corner: everything by the same factor, the opposite corner fixed.
  drag(1, 1, 1, QStringLiteral("0.5"));
  EXPECT_BOX(m.document().worldShape(box), 1000, 500, 1000);
  // Ctrl: about the centre (both sides move).
  const gp_Pnt g = *tool->gripPoint(1, 0, 0);
  QTest::keyPress(f.vp, Qt::Key_Control);
  send(f.vp, QEvent::MouseButtonPress, f.vp->toWidget(g), Qt::LeftButton, Qt::LeftButton,
       Qt::ControlModifier);
  QTest::keyRelease(f.vp, Qt::Key_Control);
  type(f.vp, QStringLiteral("3"));
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(box));
  QVERIFY2(std::abs(b.CornerMin().X() + 1000) < 1e-6 && std::abs(b.CornerMax().X() - 2000) < 1e-6,
           qPrintable(QStringLiteral("%1 %2").arg(b.CornerMin().X()).arg(b.CornerMax().X())));
  QVERIFY(m.undo());
  EXPECT_BOX(m.document().worldShape(box), 1000, 500, 1000);
  // By the mouse alone: the +Y face grip dragged 500 further, then clicked.
  tool->cancel();
  const gp_Pnt y = *tool->gripPoint(0, 1, 0);
  click(f.vp, f.vp->toWidget(y));
  click(f.vp, f.vp->toWidget(y.Translated(gp_Vec(0, 500, 0))));
  const Bnd_Box d = geom::exactBounds(m.document().worldShape(box));
  QVERIFY2(std::abs(d.CornerMax().Y() - 1000) < 15 && std::abs(d.CornerMin().Y()) < 1e-6 &&
               std::abs(d.CornerMax().X() - 1000) < 1e-6,
           qPrintable(QStringLiteral("%1 %2").arg(d.CornerMin().Y()).arg(d.CornerMax().Y())));
  QCOMPARE(f.vp->gripCount(), std::size_t(26));
  // Another tool: no grips or box left behind.
  f.window.activateTool(MainWindow::ToolId::Select);
  QCOMPARE(f.vp->gripCount(), std::size_t(0));
  f.window.activateTool(MainWindow::ToolId::Line);
  QCOMPARE(f.vp->gripCount(), std::size_t(0));
}

// ScreenTip: the label beside the cursor names the snap the tool takes
// (端点, 中点, エッジ上, 面上, 作業平面上), and the point it names is the
// one committed; it covers neither the cursor nor the point, stays in the
// view, and goes away on Esc, a tool change, leaving the view and in the
// select tool.
void GuiPhase1::snapTipNamesTheAdoptedPoint() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::CameraState cam;
  cam.eye = {500 - 6000, 500 - 8000, 9000};
  cam.target = {500, 500, 0};
  cam.up = {0, 0, 1};
  cam.perspective = false;
  cam.scale = 5000;
  f.vp->restoreCamera(cam);
  QTest::qWait(30);
  doc::Modeler& m = f.window.modeler();
  f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Line);
  auto tipAt = [&](const gp_Pnt& p) {
    const QPoint at = f.vp->toWidget(p);
    move(f.vp, at);
    const QRect r = f.vp->snapTipRect();
    const QPoint marker = f.vp->toWidget(f.vp->snapTipPoint());
    if (!f.vp->snapTip().isEmpty()) {
      if (r.contains(at) || r.contains(marker)) return QStringLiteral("covers the cursor or the point");
      if (!f.vp->rect().contains(r)) return QStringLiteral("outside the view");
    }
    return f.vp->snapTip();
  };
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 1000)), QStringLiteral("端点"));
  QVERIFY(f.vp->snapTipPoint().Distance(gp_Pnt(1000, 0, 1000)) < 1e-6);
  QCOMPARE(tipAt(gp_Pnt(500, 0, 1000)), QStringLiteral("中点"));
  QCOMPARE(tipAt(gp_Pnt(250, 0, 1000)), QStringLiteral("エッジ上"));
  QCOMPARE(tipAt(gp_Pnt(400, 600, 1000)), QStringLiteral("面上"));
  QCOMPARE(tipAt(gp_Pnt(2500, -1000, 0)), QStringLiteral("作業平面上"));

  // What the label named is where the line goes.
  QCOMPARE(tipAt(gp_Pnt(500, 0, 1000)), QStringLiteral("中点"));
  const gp_Pnt start = f.vp->snapTipPoint();
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 1000)));
  QCOMPARE(tipAt(gp_Pnt(2500, -1000, 0)), QStringLiteral("作業平面上"));
  const gp_Pnt end = f.vp->snapTipPoint();
  click(f.vp, f.vp->toWidget(gp_Pnt(2500, -1000, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);  // the line is made; the tip goes
  QVERIFY(f.vp->snapTip().isEmpty());
  // The line runs from exactly the one point to the other. It may be in
  // pieces: it may have joined the box it starts on, and on the box's
  // front face plane (which face an edge pixel picks depends on the
  // display) it divides that face on its way.
  const gp_Vec along(start, end);
  auto onLine = [&](const gp_Pnt& p) {
    const gp_Vec v(start, p);
    return v.Crossed(along).Magnitude() < 1e-6 * along.Magnitude() && v.Dot(along) > -1e-6 &&
           v.Dot(along) < along.SquareMagnitude() + 1e-6;
  };
  bool startFound = false, endFound = false;
  for (const doc::Object& o : m.document().objects()) {
    for (TopExp_Explorer e(m.document().worldShape(o), TopAbs_EDGE); e.More(); e.Next()) {
      const TopoDS_Edge edge = TopoDS::Edge(e.Current());
      const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge)), b = BRep_Tool::Pnt(TopExp::LastVertex(edge));
      if (a.Distance(b) < 1e-6 || !onLine(a) || !onLine(b)) continue;
      if (a.Distance(start) < 1e-6 || b.Distance(start) < 1e-6) startFound = true;
      if (a.Distance(end) < 1e-6 || b.Distance(end) < 1e-6) endFound = true;
    }
  }
  if (!startFound || !endFound) {
    // Which edges there are instead (to tell what became of the line).
    qWarning("start (%g,%g,%g) end (%g,%g,%g)", start.X(), start.Y(), start.Z(), end.X(), end.Y(), end.Z());
    for (const doc::Object& o : m.document().objects()) {
      for (TopExp_Explorer e(m.document().worldShape(o), TopAbs_EDGE); e.More(); e.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(e.Current());
        const gp_Pnt a = BRep_Tool::Pnt(TopExp::FirstVertex(edge)), b = BRep_Tool::Pnt(TopExp::LastVertex(edge));
        qWarning("%s: (%g,%g,%g)-(%g,%g,%g)", o.name.c_str(), a.X(), a.Y(), a.Z(), b.X(), b.Y(), b.Z());
      }
    }
  }
  QVERIFY(startFound && endFound);

  // Gone when leaving the view, on a tool change, in select.
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 1000)), QStringLiteral("端点"));
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(f.vp, &leave);
  QVERIFY(f.vp->snapTip().isEmpty());
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 1000)), QStringLiteral("端点"));
  f.window.activateTool(MainWindow::ToolId::Select);
  QVERIFY(f.vp->snapTip().isEmpty());
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 1000)), QString());
  // Other snapping tools show it the same way.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  QCOMPARE(tipAt(gp_Pnt(500, 0, 1000)), QStringLiteral("中点"));
  f.window.activateTool(MainWindow::ToolId::Move);
  QCOMPARE(tipAt(gp_Pnt(1000, 0, 1000)), QStringLiteral("端点"));
}

// SketchUp's lock: a locked object is still clicked and selected (whole,
// shown red), but not edited: delete and push/pull are refused with the
// reason, double-click does not open it; right-click locks and unlocks.
void GuiPhase1::lockedObjectsSelectableNotEditable() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  auto action = [](QMenu* menu, const QString& text) -> QAction* {
    for (QAction* a : menu->actions()) {
      if (a->text() == text) return a;
    }
    return nullptr;
  };
  f.window.activateTool(MainWindow::ToolId::Select);
  const QPoint side = f.vp->toWidget(gp_Pnt(500, 0, 500));
  QMenu* menu = f.window.buildContextMenu(side);
  QAction* lock = action(menu, QStringLiteral("ロック"));
  QVERIFY(lock);
  lock->trigger();
  delete menu;
  QVERIFY(m.document().find(box)->locked);
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{box});  // stays selected, shown red

  // Clicked: the whole object, not a face.
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 3000, 0)));
  QVERIFY(f.window.selection().empty());
  click(f.vp, side);
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{box});
  QVERIFY(f.window.elementSelection().empty());
  // Double-click does not open it or pick its faces, nor move the view;
  // neither does a double-click on empty space.
  doc::CameraState before, after;
  f.vp->storeCamera(before);
  QTest::mouseDClick(f.vp, Qt::LeftButton, Qt::NoModifier, side);
  QTest::qWait(30);
  QVERIFY(f.window.elementSelection().empty());
  QTest::mouseDClick(f.vp, Qt::LeftButton, Qt::NoModifier, f.vp->toWidget(gp_Pnt(5000, -5000, 0)));
  QTest::qWait(30);
  f.vp->storeCamera(after);
  QVERIFY(gap(before, after) < 1e-9);
  click(f.vp, side);

  // Not edited: delete is refused with the reason, nothing changes.
  const std::size_t steps = m.history().undoCount();
  f.window.clearLastError();
  menu = f.window.buildContextMenu(side);
  QVERIFY(!action(menu, QStringLiteral("削除")));  // only unlocking is offered
  QAction* unlock = action(menu, QStringLiteral("ロック解除"));
  QVERIFY(unlock);
  delete menu;
  QAction* erase = nullptr;
  for (QAction* a : f.window.findChildren<QAction*>()) {
    if (a->text() == QStringLiteral("削除")) erase = a;
  }
  QVERIFY(erase);
  erase->trigger();
  QTest::qWait(30);
  QVERIFY(m.document().find(box));
  QVERIFY(f.window.lastError().contains(QStringLiteral("ロック")));
  QCOMPARE(m.history().undoCount(), steps);

  // Unlocked from the right-click menu: editable again.
  menu = f.window.buildContextMenu(side);
  action(menu, QStringLiteral("ロック解除"))->trigger();
  delete menu;
  QVERIFY(!m.document().find(box)->locked);
  click(f.vp, side);
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));  // a face again
}

// Inside a group (double-click): a dotted box around it shows where one is
// working (SketchUp); it follows the group's contents and goes on leaving.
void GuiPhase1::groupEditShowsDottedBox() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  auto group = m.group({box}, "G");
  QVERIFY(group.ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Select);
  QVERIFY(!f.vp->hasEditBox());
  doubleClick(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));
  QTest::qWait(30);
  QCOMPARE(m.activeGroup(), *group);
  QVERIFY(f.vp->hasEditBox());
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  QVERIFY(m.activeGroup().empty());
  QVERIFY(!f.vp->hasEditBox());
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
  // Request T2 / OFF-02: drawn onto the face, which becomes frame + inner
  // face of the same solid.
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId block = m.document().objects().front().id;
  auto topFaces = [&] {
    std::vector<double> areas;
    for (TopExp_Explorer e(m.document().worldShape(block), TopAbs_FACE); e.More(); e.Next()) {
      const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
      if (n && n->Z() > 0.99) areas.push_back(geom::area(e.Current()));
    }
    std::sort(areas.begin(), areas.end());
    return areas;
  };
  QCOMPARE(topFaces().size(), std::size_t(2));
  QVERIFY(std::abs(topFaces()[1] - 800.0 * 800.0) < 1e-3 || std::abs(topFaces()[0] - 800.0 * 800.0) < 1e-3);
  // Typed right after: the distance changes in the same step.
  const std::size_t steps = m.history().undoCount();
  type(f.vp, QStringLiteral("200"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.history().undoCount(), steps);
  QVERIFY(std::abs(topFaces()[0] - 600.0 * 600.0) < 1e-3 || std::abs(topFaces()[1] - 600.0 * 600.0) < 1e-3);
  // The frame alone pushed up with the push/pull tool: a rim, centre open.
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::PushPull);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(100, 500, 1000)));
  move(f.vp, f.vp->toWidget(gp_Pnt(100, 500, 1300)));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const double rim = 1e6 - 600.0 * 600.0;
  QVERIFY2(std::abs(geom::volume(m.document().worldShape(block)) - (1e9 + rim * 500)) < 1.0,
           qPrintable(QString::number(geom::volume(m.document().worldShape(block)))));
}

// Materials with the paint tool (docs/11_materials.md): pick from the panel,
// click a face / Ctrl+click the object / Alt+click to pick up, "inherit"
// takes a face's own material away; groups paint as a whole; undo, saving
// and component placements keep their shared display.
void GuiPhase1::paintToolAndEyedropper() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId a = f.box(0, 0, 500, 500, 500);
  const doc::ObjectId b = f.box(1500, 0, 500, 500, 500);
  auto faceOf = [&](const doc::ObjectId& id, const gp_Dir& n, const gp_Pnt& p) {
    for (TopExp_Explorer e(m.document().worldShape(id), TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      const auto ref = geom::FaceRef::fromFace(face);
      if (ref && ref->normal.Angle(n) < 1e-6 && gp_Pln(p, n).Distance(ref->centroid) < 1e-6) return face;
    }
    return TopoDS_Face();
  };
  auto shows = [&](const doc::ObjectId& id, const gp_Dir& n, const gp_Pnt& p) {
    f.vp->syncNow();
    const auto c = f.vp->shownFaceColor(id, faceOf(id, n, p));
    return c ? QColor::fromRgbF((*c)[0], (*c)[1], (*c)[2]).name() : QStringLiteral("none");
  };
  auto hex = [](const doc::Color& c) { return QColor::fromRgbF(c.r, c.g, c.b).name(); };
  const QString grey = hex(doc::Color{});

  // A ready-made material: one click adds it and switches to painting.
  const auto wood = f.window.addPresetMaterial(0);
  QVERIFY(wood);
  QVERIFY(dynamic_cast<PaintTool*>(f.window.activeTool()));
  QTest::qWait(30);
  const QString woodHex = hex(m.document().findMaterial(*wood)->color);
  const QPoint topA = f.vp->toWidget(gp_Pnt(250, 250, 500));
  click(f.vp, topA);  // the top face only
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(shows(a, gp::DZ(), gp_Pnt(0, 0, 500)), woodHex);
  QCOMPARE(shows(a, -gp::DY(), gp_Pnt(0, 0, 0)), grey);
  // Ctrl+click: the whole object; the face painted on its own stays.
  const auto plaster = f.window.addPresetMaterial(2);
  const QPoint frontA = f.vp->toWidget(gp_Pnt(250, 0, 250));
  move(f.vp, frontA);
  send(f.vp, QEvent::MouseButtonPress, frontA, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
  send(f.vp, QEvent::MouseButtonRelease, frontA, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
  QCOMPARE(m.document().find(a)->material, *plaster);
  QCOMPARE(shows(a, -gp::DY(), gp_Pnt(0, 0, 0)), hex(m.document().findMaterial(*plaster)->color));
  QCOMPARE(shows(a, gp::DZ(), gp_Pnt(0, 0, 500)), woodHex);
  // Alt+click picks up what a face shows; the panel follows.
  move(f.vp, topA);
  send(f.vp, QEvent::MouseButtonPress, topA, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
  send(f.vp, QEvent::MouseButtonRelease, topA, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
  QCOMPARE(f.window.paintTool().material(), *wood);
  QCOMPARE(f.window.materialsPanel()->list()->currentItem()->data(Qt::UserRole).toString().toStdString(), *wood);
  click(f.vp, f.vp->toWidget(gp_Pnt(1750, 250, 500)));  // b's top
  QCOMPARE(shows(b, gp::DZ(), gp_Pnt(1500, 0, 500)), woodHex);
  // "Inherit" takes a's top back to the object's plaster.
  f.window.chooseMaterial({});
  click(f.vp, topA);
  QCOMPARE(shows(a, gp::DZ(), gp_Pnt(0, 0, 500)), hex(m.document().findMaterial(*plaster)->color));
  QVERIFY(m.undo());
  QCOMPARE(shows(a, gp::DZ(), gp_Pnt(0, 0, 500)), woodHex);

  // A group at the top level is painted as a whole: what has no material
  // of its own shows the group's.
  auto g = m.group({a, b});
  QVERIFY(g);
  const auto tile = f.window.addPresetMaterial(6);
  click(f.vp, f.vp->toWidget(gp_Pnt(1750, 0, 250)));  // b's front, inside the group
  QCOMPARE(m.document().find(*g)->material, *tile);
  QCOMPARE(shows(b, -gp::DY(), gp_Pnt(1500, 0, 0)), hex(m.document().findMaterial(*tile)->color));
  QCOMPARE(shows(a, -gp::DY(), gp_Pnt(0, 0, 0)), hex(m.document().findMaterial(*plaster)->color));
  QCOMPARE(shows(b, gp::DZ(), gp_Pnt(1500, 0, 500)), woodHex);

  // Saved and opened again: the same look.
  const QString path = QDir::temp().filePath(QStringLiteral("sbr_gui_materials.sbr"));
  QVERIFY(f.window.saveFile(path));
  QVERIFY(f.window.openFile(path));
  QCOMPARE(shows(a, gp::DZ(), gp_Pnt(0, 0, 500)), woodHex);
  QCOMPARE(shows(b, -gp::DY(), gp_Pnt(1500, 0, 0)), hex(m.document().findMaterial(*tile)->color));
  int library = 0;  // inherit + the document's materials (ready-made ones follow, not counted)
  for (int i = 0; i < f.window.materialsPanel()->list()->count(); ++i) {
    const QListWidgetItem* item = f.window.materialsPanel()->list()->item(i);
    library += (item->flags() & Qt::ItemIsSelectable) && !item->data(Qt::UserRole + 1).isValid();
  }
  QCOMPARE(library, 4);  // inherit + 3

  // Painting one component placement keeps the shared display.
  const doc::ObjectId c = f.box(4000, 0, 500, 500, 500);
  auto instance = m.makeComponent({c}, "Unit");
  QVERIFY(instance);
  gp_Trsf dx;
  dx.SetTranslation(gp_Vec(1000, 0, 0));
  QVERIFY(m.copy({*instance}, dx, 3).ok());
  f.vp->syncNow();
  const std::size_t shared = f.vp->sharedDisplayCount();
  f.window.chooseMaterial(*wood);
  click(f.vp, f.vp->toWidget(gp_Pnt(4250, 250, 500)));
  QCOMPARE(m.document().find(*instance)->material, *wood);
  f.vp->syncNow();
  QCOMPARE(f.vp->sharedDisplayCount(), shared);
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

  // On a line that divides a face, the eraser takes just the line away:
  // the face is one again and the object stays.
  const doc::ObjectId wall = f.box(0, 1500, 3000, 200, 2000);
  QVERIFY(m.splitWithLine({gp_Pnt(0, 1500, 800), gp_Pnt(3000, 1500, 800)}).ok());
  auto faces = [&] {
    int n = 0;
    for (TopExp_Explorer e(m.document().worldShape(wall), TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
  };
  QCOMPARE(faces(), 7);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 1500, 800)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(m.document().find(wall));
  QCOMPARE(faces(), 6);
  // A corner edge (SketchUp): the two faces meeting there go, the shape stays open.
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 1500, 2000)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(faces(), 4);
  QCOMPARE(m.document().find(wall)->kind(), doc::ObjectKind::Shell);
}

// Review 2026-10-23 P0-04: while a long operation hands events out (its
// progress dialog), AI requests wait in the socket and autosave waits, so
// nothing writes the model in the middle of it.
void GuiPhase1::aiWaitsForLongOperation() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.setAiAccess(true);
  QLocalSocket socket;
  socket.connectToServer(f.window.aiBridge()->serverName());
  QTRY_VERIFY(socket.state() == QLocalSocket::ConnectedState);
  const std::size_t before = m.listObjects().size();
  {
    const GuiProgress::Pumping operation;  // as inside a long Boolean
    socket.write(R"({"command": "create_box", "args": {"origin": [0, 0, 0], "width": 100, "depth": 100, "height": 100}})"
                 "\n");
    socket.flush();
    QTest::qWait(400);
    QVERIFY(!socket.canReadLine());  // waiting, not run
    QCOMPARE(m.listObjects().size(), before);
    // A direct write is refused rather than run inside the operation.
    const auto direct = QJsonDocument::fromJson(QByteArray::fromStdString(f.window.aiBridge()->execute(
        R"({"command": "create_box", "args": {"origin": [0, 0, 0], "width": 100, "depth": 100, "height": 100}})")));
    QVERIFY(!direct.object().value("ok").toBool());
    QVERIFY(direct.object().value("error").toString().contains("busy"));
    // Reads still answer.
    const auto read = QJsonDocument::fromJson(
        QByteArray::fromStdString(f.window.aiBridge()->execute(R"({"command": "list_objects"})")));
    QVERIFY(read.object().value("ok").toBool());
    QCOMPARE(m.listObjects().size(), before);
  }
  // Over: the waiting request runs.
  QTRY_VERIFY_WITH_TIMEOUT(socket.canReadLine(), 3000);
  QVERIFY(QJsonDocument::fromJson(socket.readLine()).object().value("ok").toBool());
  QCOMPARE(m.listObjects().size(), before + 1);
  f.window.setAiAccess(false);
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
  // Placing and retyping the note is one undo step.
  QVERIFY(m.undo());
  QTest::qWait(30);
  QVERIFY(!m.document().find(note));
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
  // Left to right covering half of B: B is not fully inside -> not B as a
  // whole; only its faces / edges that are (its left side).
  drag(QPoint(rb.left() - 5, top), QPoint(rb.center().x(), bottom));
  QVERIFY(!f.window.elementSelection().empty());
  QCOMPARE(f.window.selectionTargets().size(), std::size_t(1));
  QVERIFY(!f.window.selectionTargets().front().whole);
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
  QVERIFY(f.vp->hasEditBox());  // dotted box: working inside

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
  QVERIFY(!f.vp->hasEditBox());
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

// Request T3 / FM-01: edges of two separate lines selected first, then the
// Follow Me tool, then one click on the profile: swept along both.
void GuiPhase1::followMeAlongSelectedEdges() {
  Fixture f;
  QVERIFY(f.setUp(1000, 1000));
  doc::Modeler& m = f.window.modeler();
  auto a = m.createPolyline({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0)});
  auto b = m.createPolyline({gp_Pnt(2000, 0, 0), gp_Pnt(2000, 2000, 0)});
  auto profile = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(0, -100, 0), gp::DX()), 200, 200);
  QVERIFY(a.ok() && b.ok() && profile.ok());
  f.vp->syncNow();
  std::vector<ElementRef> path;
  for (const doc::ObjectId& id : {*b, *a}) {
    for (const ElementRef& e : f.vp->allElements(id)) {
      if (e.type == TopAbs_EDGE) path.push_back(e);
    }
  }
  f.window.setElementSelection(path);
  f.window.activateTool(MainWindow::ToolId::FollowMe);
  QVERIFY2(f.window.statusText().contains(QStringLiteral("2 本")), qPrintable(f.window.statusText()));
  const std::size_t steps = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 100)));  // the profile
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(!m.document().find(*profile));
  QCOMPARE(m.history().undoCount(), steps + 1);
  const doc::Object& swept = m.document().objects().back();
  QCOMPARE(swept.kind(), doc::ObjectKind::Solid);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(swept)) - 200.0 * 200 * 4000) < 0.02 * 200.0 * 200 * 4000);
}

void GuiPhase1::wallToolAndArchitectureMenus() {
  Fixture f;
  QVERIFY(f.setUp(2000, 1500));
  doc::Modeler& m = f.window.modeler();
  f.window.wallTool().setSettings({200.0, 3000.0, 0});
  f.window.activateTool(MainWindow::ToolId::Wall);
  // Start, show the direction, type the length.
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(2500, 0, 0)));
  type(f.vp, QStringLiteral("4000"));
  // A wrong point, taken back with Esc.
  click(f.vp, f.vp->toWidget(gp_Pnt(4000, 2000, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(20);
  QCOMPARE(m.document().objects().size(), std::size_t(0));
  click(f.vp, f.vp->toWidget(gp_Pnt(4000, 3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 3000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));  // back at the start: closes the run
  std::vector<doc::ObjectId> walls;
  double volume = 0;
  for (const doc::Object& o : m.document().objects()) {
    const auto k = o.attributes.find("kind");
    if (k != o.attributes.end() && k->second == "wall") {
      walls.push_back(o.id);
      volume += geom::volume(m.document().worldShape(o));
    }
  }
  QCOMPARE(walls.size(), std::size_t(4));
  // Ring 4200 x 3200 outside, 3800 x 2800 inside, 3 m high (clicked
  // points are within a pixel).
  QVERIFY2(std::abs(volume - 2.8e6 * 3000) < 0.005 * 2.8e6 * 3000, qPrintable(QString::number(volume)));

  // Two windows into the first wall (along X; its largest vertical face).
  f.window.activateTool(MainWindow::ToolId::Select);
  f.window.setSelection({walls.front()});
  QVERIFY(f.window.placeOpeningsOnSelection(2, 900, 1200, 900));
  const doc::ObjectId host = f.window.selection().front();
  auto openings = m.openings(host);
  QVERIFY(openings && openings->size() == 2);
  QTest::qWait(30);
  QVERIFY(f.window.findChild<QWidget*>(QStringLiteral("opening2")));  // editable in the panel
  QVERIFY2(f.window.resizeOpening(host, 1, 1000, 1300), qPrintable(f.window.lastError()));
  openings = m.openings(host);
  QVERIFY(std::abs(openings->at(0).width - 1000) < 1e-6 && std::abs(openings->at(0).height - 1300) < 1e-6);
  QVERIFY(std::abs(openings->at(1).width - 900) < 1e-6);

  // Levels for everything (nothing selected), bad text refused.
  f.window.setSelection({});
  QVERIFY(!f.window.assignLevelsFromText(QStringLiteral("1F")));
  QVERIFY(f.window.assignLevelsFromText(QStringLiteral("1F=0, 2F=3m")));
  QCOMPARE(QString::fromStdString(m.document().find(host)->attributes.at("level")), QStringLiteral("1F"));

  // Align a slab below the walls' base to it.
  auto slab = m.createSlab({gp_Pnt(0, 0, 0), gp_Pnt(4000, 0, 0), gp_Pnt(4000, 3000, 0), gp_Pnt(0, 3000, 0)}, -500, 200);
  QVERIFY(slab);
  f.window.setSelection({host, *slab});
  QVERIFY(f.window.alignSelection(2, -1));
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(*m.document().find(*slab)));
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  QVERIFY2(std::abs(z0) < 1e-6, qPrintable(QString::number(z0)));
  // Undo takes the align back as one step.
  m.undo();
  geom::exactBounds(m.document().worldShape(*m.document().find(*slab))).Get(x0, y0, z0, x1, y1, z1);
  QVERIFY(std::abs(z0 + 700) < 1e-6);
}

void GuiPhase1::slabToolAndOpeningTool() {
  Fixture f;
  QVERIFY(f.setUp(2000, 0));
  doc::Modeler& m = f.window.modeler();

  // Slab: corners on the ground, Enter closes; top at the clicked level.
  f.window.slabTool().setThickness(150);
  f.window.activateTool(MainWindow::ToolId::Slab);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, -3000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, -3000, 0)));
  type(f.vp, QStringLiteral("4000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(4000, -1500, 0)));
  type(f.vp, QStringLiteral("2000"));
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, -1000, 0)));
  type(f.vp, QStringLiteral("4000"));
  QCOMPARE(m.document().objects().size(), std::size_t(0));  // still drawing
  QTest::keyClick(f.vp, Qt::Key_Return);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::Object& slab = m.document().objects().front();
  const doc::ObjectId slabId = slab.id;
  QCOMPARE(QString::fromStdString(slab.attributes.at("kind")), QStringLiteral("slab"));
  QVERIFY2(std::abs(geom::volume(slab.shape) - 4000.0 * 2000 * 150) < 1.0,
           qPrintable(QString::number(geom::volume(slab.shape))));
  double x0, y0, z0, x1, y1, z1;
  geom::exactBounds(slab.shape).Get(x0, y0, z0, x1, y1, z1);
  QVERIFY(std::abs(z1) < 1e-6 && std::abs(z0 + 150) < 1e-6);

  // Opening tool: hover the wall's front face, click: a door where clicked.
  const doc::ObjectId wall = f.box(0, 0, 4000, 200, 3000);
  f.window.activateTool(MainWindow::ToolId::Opening);
  QTest::qWait(30);
  move(f.vp, f.vp->toWidget(gp_Pnt(1100, 0, 1500)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1100, 0, 1500)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(!m.document().find(wall));
  doc::ObjectId host;
  for (const doc::Object& o : m.document().objects()) {
    if (o.id != slabId) host = o.id;
  }
  auto openings = m.openings(host);
  QVERIFY(openings && openings->size() == 1);
  QVERIFY2(std::abs(openings->front().center.X() - 1100) < 20, qPrintable(QString::number(openings->front().center.X())));
  QVERIFY(std::abs(openings->front().width - 900) < 1e-6 && std::abs(openings->front().bottom) < 1e-6);
  // Typing right after redoes that opening at the new size (one undo step).
  const std::size_t steps = m.history().undoCount();
  type(f.vp, QStringLiteral("800,1000,800"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.history().undoCount(), steps);
  for (const doc::Object& o : m.document().objects()) {
    if (o.id != slabId) host = o.id;
  }
  openings = m.openings(host);
  QVERIFY(openings && openings->size() == 1);
  QVERIFY(std::abs(openings->front().width - 800) < 1e-6 && std::abs(openings->front().height - 1000) < 1e-6);
  QVERIFY(std::abs(openings->front().bottom - 800) < 1e-6);
  // Near the face's middle it snaps there exactly.
  click(f.vp, f.vp->toWidget(gp_Pnt(2003, 0, 1500)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  for (const doc::Object& o : m.document().objects()) {
    if (o.id != slabId) host = o.id;
  }
  openings = m.openings(host);
  QVERIFY(openings && openings->size() == 2);
  QVERIFY2(std::abs(openings->back().center.X() - 2000) < 1e-6, qPrintable(QString::number(openings->back().center.X())));
  // Next to the second opening (it snaps there), in line with the first
  // one (above it), and refused where it would run into one.
  click(f.vp, f.vp->toWidget(gp_Pnt(2810, 0, 1500)));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  for (const doc::Object& o : m.document().objects()) {
    if (o.id != slabId) host = o.id;
  }
  openings = m.openings(host);
  QVERIFY(openings && openings->size() == 3);
  QVERIFY2(std::abs(openings->back().center.X() - 2800) < 1e-6, qPrintable(QString::number(openings->back().center.X())));
  f.window.openingTool().setSettings({600, 500, 2200});
  move(f.vp, f.vp->toWidget(gp_Pnt(openings->front().center.X() + 8, 0, 2400)));
  QVERIFY2(f.window.statusText().contains(QStringLiteral("中心が揃う")), qPrintable(f.window.statusText()));
  f.window.openingTool().setSettings({900, 2100, 0});
  move(f.vp, f.vp->toWidget(gp_Pnt(2050, 0, 400)));
  QVERIFY2(f.window.statusText().contains(QStringLiteral("重なります")), qPrintable(f.window.statusText()));
  const std::size_t before = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(2050, 0, 400)));
  QVERIFY(!f.window.lastError().isEmpty());
  QCOMPARE(m.history().undoCount(), before);
  // Clicking the ground (not a wall face) does nothing.
  const std::size_t count = m.history().undoCount();
  click(f.vp, f.vp->toWidget(gp_Pnt(6000, 3000, 0)));
  QCOMPARE(m.history().undoCount(), count);
}

// Common contract for "type right after to change it" (review §7): it
// survives camera moves and AI reads, ends with any other model change or
// a tool switch, and the edit with all its revisions is one undo step.
void GuiPhase1::typedRevisionOnlyRightAfter() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  auto radiusOf = [&](const doc::ObjectId& id) {
    return std::sqrt(geom::area(m.document().find(id)->shape) / M_PI);
  };
  f.window.activateTool(MainWindow::ToolId::Circle);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(300, 0, 0)));
  type(f.vp, QStringLiteral("400"));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId circle = m.document().objects().front().id;
  const std::size_t steps = m.history().undoCount();
  type(f.vp, QStringLiteral("500"));
  QVERIFY(std::abs(radiusOf(circle) - 500) < 1e-6);
  // Orbiting and an AI reading the model keep it revisable.
  doc::CameraState cam;
  f.vp->storeCamera(cam);
  cam.eye = {3000, -9000, 6000};
  f.vp->restoreCamera(cam);
  QTest::qWait(30);
  sbr::api::CommandApi reader(m);
  reader.execute(R"({"command": "list_objects"})");
  type(f.vp, QStringLiteral("600"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(std::abs(radiusOf(circle) - 600) < 1e-6);
  QCOMPARE(m.history().undoCount(), steps);  // still the circle's one step
  // Another change in between (here: an AI edit) ends it.
  geom::SketchPlane away = geom::SketchPlane::ground();
  away.origin = gp_Pnt(-5000, -5000, 0);
  auto other = m.createRectangle(away, 100, 100);
  QVERIFY(other);
  type(f.vp, QStringLiteral("700"));
  QVERIFY(!f.window.lastError().isEmpty());
  QVERIFY(std::abs(radiusOf(circle) - 600) < 1e-6);
  // So does switching tools.
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(3300, 0, 0)));
  type(f.vp, QStringLiteral("200"));
  const doc::ObjectId second = m.document().objects().back().id;
  f.window.activateTool(MainWindow::ToolId::Line);
  f.window.activateTool(MainWindow::ToolId::Circle);
  type(f.vp, QStringLiteral("900"));
  QVERIFY(std::abs(radiusOf(second) - 200) < 1e-6);
  // Undo: the second circle, the rectangle, then the first circle with
  // all its retypes at once.
  QVERIFY(m.undo());
  QVERIFY(m.undo());
  QVERIFY(m.document().find(circle));
  QVERIFY(m.undo());
  QVERIFY(!m.document().find(circle));
}

// Interaction contract (review §7): Esc goes back exactly one stage in
// every multi-step tool, the camera does not lose the input, and the
// status bar shows what is fixed (target / working plane / direction).
void GuiPhase1::escStepsBackAndConstraintsShow() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  auto esc = [&] {
    QTest::keyClick(f.vp, Qt::Key_Escape);
    QTest::qWait(20);
  };

  // Rectangle: working plane after the first corner, arrow-key lock shown.
  f.window.activateTool(MainWindow::ToolId::Rectangle);
  QVERIFY(f.window.constraintText().isEmpty());
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 300, 0)));
  QVERIFY2(f.window.constraintText().contains(QStringLiteral("作業面: 地面")), qPrintable(f.window.constraintText()));
  esc();
  QVERIFY(f.window.constraintText().isEmpty());

  // Curve: Esc takes back one clicked point at a time; orbiting meanwhile
  // keeps the input. (Lines end with Esc, like SketchUp.)
  f.window.activateTool(MainWindow::ToolId::Curve);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  doc::CameraState cam;
  f.vp->storeCamera(cam);
  cam.eye = {4000, -7000, 5000};
  f.vp->restoreCamera(cam);
  QTest::qWait(30);
  click(f.vp, f.vp->toWidget(gp_Pnt(1500, 1000, 0)));
  esc();
  QCOMPARE(f.window.statusText(), QStringLiteral("最後の点を取り消しました"));
  QTest::keyClick(f.vp, Qt::Key_Return);  // finish with the two points left
  QTest::qWait(30);
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QCOMPARE(std::get<doc::InterpolatedCurve>(m.document().objects().front().recipe.base).points.size(), std::size_t(2));

  // Line: arrow-key lock shown and released; Esc ends the chain, keeping it.
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 2000, 0)));
  QVERIFY(f.window.constraintText().contains(QStringLiteral("作業面")));
  QTest::keyClick(f.vp, Qt::Key_Right);  // lock to X
  QTest::qWait(20);
  QVERIFY2(f.window.constraintText().contains(QStringLiteral("X 軸")), qPrintable(f.window.constraintText()));
  QTest::keyClick(f.vp, Qt::Key_Down);
  QTest::qWait(20);
  QVERIFY(!f.window.constraintText().contains(QStringLiteral("方向")));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 2000, 0)));
  esc();
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  QVERIFY(f.window.constraintText().isEmpty());

  // Rotate: centre -> reference -> rotating, and back the same way.
  const doc::ObjectId box = f.box(2000, 2000, 1000, 1000, 1000);
  f.window.setSelection({box});
  f.window.activateTool(MainWindow::ToolId::Rotate);
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 2000, 0)));
  QVERIFY2(f.window.constraintText().contains(QStringLiteral("対象:")), qPrintable(f.window.constraintText()));
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 2000, 0)));
  move(f.vp, f.vp->toWidget(gp_Pnt(2000, 3000, 0)));
  esc();  // back to choosing the reference
  QVERIFY(f.window.constraintText().contains(QStringLiteral("作業面")));
  esc();  // back to choosing the centre
  QVERIFY(f.window.constraintText().isEmpty());
  QCOMPARE(m.history().undoName(), std::string("Extrude"));  // nothing rotated
}

// Unsaved work survives a crash (docs/12_recovery.md): autosave to a
// recovery file of the session, offered back only once that session is
// gone, opened as unsaved changes to the original file; saving keeps
// previous versions as .bak1 ...
void GuiPhase1::autosaveRecoveryAndBackups() {
  QTemporaryDir recovery;
  qputenv("SBR_RECOVERY_DIR", recovery.path().toUtf8());
  QTemporaryDir work;
  const QString path = QDir(work.path()).filePath(QStringLiteral("house.sbr"));
  doc::ObjectId first;
  {
    Fixture f;
    QVERIFY(f.setUp());
    first = f.box(0, 0, 1000, 1000, 1000);
    QVERIFY(f.window.saveFile(path));
    QVERIFY(f.window.autosaveNow());
    QVERIFY(!QFileInfo::exists(f.window.recoveryFile()));  // saved: nothing to recover
    f.box(2000, 0, 500, 500, 500);  // unsaved change
    QVERIFY(f.window.autosaveNow());
    QVERIFY(QFileInfo::exists(f.window.recoveryFile()));
    // While this session runs, its file is not offered to others.
    QVERIFY(MainWindow::findRecoverable().empty());
    f.window.abandonSessionForTest();  // as if it crashed now
  }
  const auto found = MainWindow::findRecoverable();
  QCOMPARE(found.size(), std::size_t(1));
  QCOMPARE(found.front().originalPath, path);
  {
    Fixture f;
    QVERIFY(f.setUp());
    QVERIFY(f.window.recover(found.front()));
    QCOMPARE(f.window.modeler().document().objects().size(), std::size_t(2));
    QVERIFY(f.window.isWindowModified());           // still to be saved
    QVERIFY(MainWindow::findRecoverable().empty());  // taken over by this session
    // Saving writes the original file and keeps the previous version.
    QVERIFY(f.window.saveFile(path));
    QVERIFY(QFileInfo::exists(doc::backupPath(path.toStdString(), 1).c_str()));
    doc::Document previous;
    QVERIFY(doc::loadDocument(previous, doc::backupPath(path.toStdString(), 1)).ok());
    QCOMPARE(previous.objects().size(), std::size_t(1));
    QVERIFY(!QFileInfo::exists(f.window.recoveryFile()));
    // Generations: newest first, at most the set number.
    f.window.setBackupGenerations(2);
    f.box(4000, 0, 500, 500, 500);
    QVERIFY(f.window.saveFile(path));
    f.box(6000, 0, 500, 500, 500);
    QVERIFY(f.window.saveFile(path));
    doc::Document b1, b2;
    QVERIFY(doc::loadDocument(b1, doc::backupPath(path.toStdString(), 1)).ok());
    QVERIFY(doc::loadDocument(b2, doc::backupPath(path.toStdString(), 2)).ok());
    QCOMPARE(b1.objects().size(), std::size_t(3));
    QCOMPARE(b2.objects().size(), std::size_t(2));
    QVERIFY(!QFileInfo::exists(doc::backupPath(path.toStdString(), 3).c_str()));
  }
  // A normal end leaves nothing behind.
  QVERIFY(QDir(recovery.path()).entryList({QStringLiteral("*.sbr")}, QDir::Files).isEmpty());
  qunsetenv("SBR_RECOVERY_DIR");
}

// Select a face, Delete: only that face goes and the shape stays open
// (shown with its back side); filling the hole closes it again.
// Request T1 / DEL-01: a drawn rectangle, its face clicked once and
// deleted: the 4 lines stay (one undo step).
void GuiPhase1::deleteLooseFaceKeepsItsOutline() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  auto face = m.createRectangle(geom::SketchPlane::ground(), 1000, 1000);
  QVERIFY(face.ok());
  f.vp->syncNow();
  f.window.activateWindow();
  QTest::qWait(50);
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 0)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QCOMPARE(f.window.elementSelection().front().type, TopAbs_FACE);
  const std::size_t steps = m.history().undoCount();
  QTest::keyClick(f.vp, Qt::Key_Delete);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(!m.document().find(*face));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QCOMPARE(m.document().objects().front().kind(), doc::ObjectKind::Curve);
  QCOMPARE(m.history().undoCount(), steps + 1);
  QVERIFY(f.window.statusText().contains(QStringLiteral("線として残")));
  QVERIFY(m.undo());
  QCOMPARE(m.document().find(*face)->kind(), doc::ObjectKind::Face);
}

// Delete of a mixed selection in the window: five faces of a cube (not the
// top) and two of three loose lines; the top and the third line stay. A
// lone line clicked and deleted goes alone. One undo step each.
void GuiPhase1::deleteMixedSelection() {
  Fixture f;
  QVERIFY(f.setUp(1500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId cube = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId l1 = *m.createPolyline({gp_Pnt(3000, 0, 0), gp_Pnt(4000, 0, 0)});
  const doc::ObjectId l2 = *m.createPolyline({gp_Pnt(3000, 500, 0), gp_Pnt(4000, 500, 0)});
  const doc::ObjectId l3 = *m.createPolyline({gp_Pnt(3000, 1000, 0), gp_Pnt(4000, 1000, 0)});
  f.vp->syncNow();
  std::vector<ElementRef> selection;
  for (const ElementRef& e : f.vp->allElements(cube)) {
    if (e.type != TopAbs_FACE) continue;
    const auto n = geom::planarFaceNormal(TopoDS::Face(f.vp->elementShape(e)));
    if (n && n->Z() < 0.99) selection.push_back(e);
  }
  QCOMPARE(selection.size(), std::size_t(5));
  for (const doc::ObjectId& id : {l1, l2}) {
    for (const ElementRef& e : f.vp->allElements(id)) selection.push_back(e);
  }
  f.window.activateWindow();
  f.window.activateTool(MainWindow::ToolId::Select);
  f.window.setElementSelection(selection);
  QCOMPARE(f.window.elementSelection().size(), std::size_t(7));
  QAction* deleteAction = nullptr;
  for (QAction* a : f.window.findChildren<QAction*>()) {
    if (a->text() == QStringLiteral("削除")) deleteAction = a;
  }
  QVERIFY(deleteAction);
  const std::size_t steps = m.history().undoCount();
  deleteAction->trigger();
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(m.document().find(cube));
  int faces = 0;
  for (TopExp_Explorer e(m.document().worldShape(*m.document().find(cube)), TopAbs_FACE); e.More(); e.Next()) ++faces;
  QCOMPARE(faces, 1);
  QVERIFY(!m.document().find(l1) && !m.document().find(l2));
  QVERIFY(m.document().find(l3));
  QCOMPARE(m.history().undoCount(), steps + 1);

  // A lone line clicked and deleted.
  f.vp->syncNow();
  click(f.vp, f.vp->toWidget(gp_Pnt(3500, 1000, 0)));
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QTest::keyClick(f.vp, Qt::Key_Delete);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(!m.document().find(l3));
  QVERIFY(m.document().find(cube));
}

// Push/pull with the tool on a face of an open box (its top deleted): a new
// volume is made from that face; the box stays open with its other faces.
void GuiPhase1::pushPullOnOpenShape() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  TopoDS_Face top;
  for (TopExp_Explorer e(m.document().worldShape(box), TopAbs_FACE); e.More(); e.Next()) {
    const auto n = geom::planarFaceNormal(TopoDS::Face(e.Current()));
    if (n && n->Z() > 0.99) top = TopoDS::Face(e.Current());
  }
  QVERIFY(m.deleteFaces(box, {top}).ok());
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Shell);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::PushPull);
  QTest::qWait(30);
  // As on a SketchUp surface: the front moves out 400, the box grows.
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 500)));  // the front face (y = 0)
  move(f.vp, f.vp->toWidget(gp_Pnt(500, -600, 500)));
  type(f.vp, QStringLiteral("400"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Shell);
  QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(box)).CornerMin().Y() + 400) < 1e-6);
  QVERIFY(m.undo());
  f.vp->syncNow();
  // Ctrl keeps the starting face: a 400 thick volume of its own.
  QTest::keyClick(f.vp, Qt::Key_Control);
  QVERIFY(f.window.statusText().contains(QStringLiteral("開始面を残す")));
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 500)));
  move(f.vp, f.vp->toWidget(gp_Pnt(500, -600, 500)));
  type(f.vp, QStringLiteral("400"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Shell);
  const doc::Object& volume = m.document().objects().back();
  QCOMPARE(volume.kind(), doc::ObjectKind::Solid);
  QVERIFY(std::abs(geom::volume(m.document().worldShape(volume)) - 1000.0 * 1000 * 400) < 1.0);
}

void GuiPhase1::deleteFaceLeavesOpenShape() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId id = f.box(0, 0, 1000, 1000, 1000);
  f.window.activateWindow();
  QTest::qWait(50);
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 500, 1000)));  // the top face
  QCOMPARE(f.window.elementSelection().size(), std::size_t(1));
  QTest::keyClick(f.vp, Qt::Key_Delete);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Shell);
  QVERIFY(f.window.statusText().contains(QStringLiteral("穴をふさぐ")));
  if (const QString shot = qEnvironmentVariable("SBR_SHELL_SHOT"); !shot.isEmpty()) {
    f.window.setSelection({});
    f.vp->syncNow();
    QTest::qWait(100);
    QVERIFY(f.vp->dumpImage(shot));
  }
  // Fill again (the Edit menu action): a solid.
  f.window.setSelection({id});
  QVERIFY(m.fillHoles({id}).ok());
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Solid);
  QVERIFY(m.undo());
  QVERIFY(m.undo());
  QCOMPARE(m.document().find(id)->kind(), doc::ObjectKind::Solid);
}

// A plan picture to draw over (docs/13): import, two-point calibration with
// the scale tool, lock, then trace on it; saved inside the file.
void GuiPhase1::imageUnderlayCalibrateLockTrace() {
  QTemporaryDir dir;
  QImage picture(200, 100, QImage::Format_RGB32);
  picture.fill(Qt::blue);
  for (int y = 0; y < 100; ++y) {
    for (int x = 0; x < 100; ++x) picture.setPixel(x, y, qRgb(220, 30, 30));  // left half red
  }
  for (int y = 0; y < 15; ++y) {
    for (int x = 0; x < 200; ++x) picture.setPixel(x, y, qRgb(255, 255, 255));  // white strip at the top
  }
  const QString png = QDir(dir.path()).filePath(QStringLiteral("plan.png"));
  QVERIFY(picture.save(png));

  Fixture f;
  QVERIFY(f.setUp(5000, 2500));
  doc::Modeler& m = f.window.modeler();
  QVERIFY(f.window.importUnderlay(png));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const doc::ObjectId image = m.document().objects().front().id;
  QCOMPARE(m.document().find(image)->kind(), doc::ObjectKind::Image);
  QVERIFY(dynamic_cast<ScaleTool*>(f.window.activeTool()));  // straight into calibration
  // Restore the test camera (import fits the view).
  doc::CameraState cam;
  cam.eye = {5000 - 6000, 2500 - 8000, 9000};
  cam.target = {5000, 2500, 0};
  cam.perspective = false;
  cam.scale = 16000;
  f.vp->restoreCamera(cam);
  QTest::qWait(50);
  if (const QString shot = qEnvironmentVariable("SBR_UNDERLAY_SHOT"); !shot.isEmpty()) {
    f.window.setSelection({});
    f.vp->syncNow();
    QTest::qWait(100);
    QVERIFY(f.vp->dumpImage(shot));
    f.window.setSelection({image});
  }
  // Two marks 5 m apart on the 10 m wide picture are really 2.5 m apart.
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 1000, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(6000, 1000, 0)));
  type(f.vp, QStringLiteral("2500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const auto& u = std::get<doc::ImageUnderlay>(m.document().find(image)->recipe.base);
  QVERIFY2(std::abs(u.width - 5000) < 50, qPrintable(QString::number(u.width)));
  QVERIFY(std::abs(u.height - 2500) < 25);
  QVERIFY(std::abs(u.plane.origin.X() - 500) < 20);

  // Locked: clicked, it is selected whole (shown red) but not edited; the
  // line tool still traces on it.
  f.window.activateWindow();
  QTest::qWait(30);
  QTest::keyClick(f.vp, Qt::Key_L, Qt::ControlModifier);
  QTest::qWait(30);
  QVERIFY(m.document().find(image)->locked);
  f.window.activateTool(MainWindow::ToolId::Select);
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 1000, 0)));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{image});
  QVERIFY(f.window.elementSelection().empty());
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 500, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(3000, 500, 0)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  QCOMPARE(m.document().objects().size(), std::size_t(2));
  QVERIFY(std::abs(geom::exactBounds(m.document().worldShape(m.document().objects().back())).CornerMax().Z()) < 1e-6);

  // In the file, and back.
  const QString path = QDir(dir.path()).filePath(QStringLiteral("traced.sbr"));
  QVERIFY(f.window.saveFile(path));
  QVERIFY(f.window.openFile(path));
  const doc::Object* back = m.document().find(image);
  QVERIFY(back && back->locked);
  QVERIFY(m.document().findImage(std::get<doc::ImageUnderlay>(back->recipe.base).image));
}

// Components step 4: one placement's own size from the panel ("この配置だけ"),
// mirror from the menu command, and the axes tool (origin + red axis by
// clicks). Placements with the same settings keep sharing one display.
void GuiPhase1::componentPlacementOwnSizeMirrorAndAxes() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 200, 2000);
  auto first = m.makeComponent({box}, "Panel");
  QVERIFY(first.ok());
  gp_Trsf dx;
  dx.SetTranslation(gp_Vec(3000, 0, 0));
  auto copies = m.copy({*first}, dx, 2);
  QVERIFY(copies.ok());
  const doc::ObjectId second = (*copies)[0], third = (*copies)[1];

  // The panel: switch to "this placement only" and widen.
  f.window.setSelection({second});
  auto* panel = f.window.findChild<PropertiesPanel*>();
  QVERIFY(panel);
  QComboBox* scope = nullptr;
  for (QComboBox* c : panel->findChildren<QComboBox*>()) {
    if (c->count() == 2 && c->itemText(1).contains(QStringLiteral("この配置だけ"))) scope = c;
  }
  QVERIFY(scope);
  QVERIFY(scope->itemText(0).contains(QStringLiteral("3 個")));  // the affected count
  scope->setCurrentIndex(1);
  QTest::qWait(20);
  QDoubleSpinBox* width = nullptr;
  for (QFormLayout* form : panel->findChildren<QFormLayout*>()) {
    for (QDoubleSpinBox* spin : panel->findChildren<QDoubleSpinBox*>()) {
      auto* label = qobject_cast<QLabel*>(form->labelForField(spin));
      if (label && label->text().endsWith(QStringLiteral("幅"))) width = spin;
    }
  }
  QVERIFY(width);
  width->setValue(1500);
  emit width->editingFinished();
  QTest::qWait(20);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(geom::exactBounds(m.document().worldShape(second)).CornerMax().X(), 4500.0);
  QCOMPARE(geom::exactBounds(m.document().worldShape(third)).CornerMax().X(), 7000.0);

  // The same own width on the third: the two share one display again.
  const auto& ref = std::get<doc::ComponentInstance>(m.document().find(second)->recipe.base);
  QVERIFY(m.setPlacementParameter(third, ref.overrides.front().object, "width", 1500).ok());
  f.vp->syncNow();
  QCOMPARE(f.vp->sharedDisplayCount(), std::size_t(2));  // plain + widened

  // Mirror from the command, in place.
  f.window.placementAction({second}, QStringLiteral("mirror0"));
  QVERIFY(std::get<doc::ComponentInstance>(m.document().find(second)->recipe.base).mirrored);
  const Bnd_Box b = geom::exactBounds(m.document().worldShape(second));
  QCOMPARE(b.CornerMin().X(), 3000.0);
  QCOMPARE(b.CornerMax().X(), 4500.0);

  // Axes tool: origin on the first panel's far corner, red axis toward +y.
  f.window.placementAction({*first}, QStringLiteral("axes"));
  QCOMPARE(f.window.activeTool()->name(), QStringLiteral("コンポーネントの軸"));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)));
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 2500, 0)));  // away from the geometry
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  const gp_Trsf& t = m.document().find(*first)->transform;
  QVERIFY(std::abs(t.TranslationPart().X() - 1000) < 1e-6);
  QVERIFY(gp_Dir(1, 0, 0).Transformed(t).IsEqual(gp::DY(), 1e-6));
  QCOMPARE(geom::exactBounds(m.document().worldShape(*first)).CornerMax().X(), 1000.0);
  QCOMPARE(geom::exactBounds(m.document().worldShape(second)).CornerMax().X(), 4500.0);

  // Make unique from the menu command: the others keep the shared component.
  f.window.placementAction({third}, QStringLiteral("unique"));
  const auto& own = std::get<doc::ComponentInstance>(m.document().find(third)->recipe.base);
  QVERIFY(own.definitionId != ref.definitionId);
  QCOMPARE(m.instanceCount(ref.definitionId), 2);
}

// Step 5: the Move tool reshapes with faces kept flat. Alt+click grabs an
// edge (or corner / face) under the cursor; selected edges move too; a move
// that would bend a face is refused with a reason and changes nothing.
void GuiPhase1::moveToolReshapesCornersAndEdges() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  auto volume = [&] { return geom::volume(m.document().worldShape(box)); };

  f.window.activateTool(MainWindow::ToolId::Move);
  // Alt+click the middle of the front top edge, then type the distance up.
  const QPoint grab = f.vp->toWidget(gp_Pnt(500, 0, 1000));
  move(f.vp, grab);
  send(f.vp, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
  send(f.vp, QEvent::MouseButtonRelease, grab, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
  QTest::qWait(20);
  QTest::keyClick(f.vp, Qt::Key_Up);  // lock the blue axis
  move(f.vp, f.vp->toWidget(gp_Pnt(500, 0, 3500)));  // clear of other snap points
  QVERIFY2(f.window.constraintText().contains(QStringLiteral("辺")), qPrintable(f.window.constraintText()));
  type(f.vp, QStringLiteral("500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY2(std::abs(volume() - 1.25e9) < 1.0, qPrintable(QString::number(volume())));
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Solid);

  // Alt+click a corner of the back top edge: moving it alone up would bend
  // the top, so the top is folded (Autofold, request T5) and the preview
  // says so; still a valid solid, one undo step.
  const std::size_t steps = m.history().undoCount();
  const QPoint corner = f.vp->toWidget(gp_Pnt(1000, 1000, 1000));
  move(f.vp, corner);
  send(f.vp, QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
  send(f.vp, QEvent::MouseButtonRelease, corner, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
  QTest::qWait(20);
  QTest::keyClick(f.vp, Qt::Key_Up);
  move(f.vp, f.vp->toWidget(gp_Pnt(1000, 1000, 3500)));
  QVERIFY2(f.window.statusText().contains(QStringLiteral("分けます")), qPrintable(f.window.statusText()));
  type(f.vp, QStringLiteral("300"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.history().undoCount(), steps + 1);
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Solid);
  QVERIFY(geom::validateShape(m.document().worldShape(box), true).valid);
  QVERIFY(volume() > 1.25e9);
  QVERIFY(m.undo());
  QVERIFY(std::abs(volume() - 1.25e9) < 1.0);
}

// Request T5, first acceptance task in the GUI: a 6000 x 4000 x 3000 box,
// its top split down the middle, the ridge Alt+grabbed and lifted 1500 with
// the blue axis locked: a gable house, one undo step.
// A line drawn with the line tool on the side of an L-shaped volume, from
// the top of the tall part to the corner of the step, closes the notch:
// a face is made there.
void GuiPhase1::lineAgainstVolumeMakesFace() {
  Fixture f;
  QVERIFY(f.setUp(1000, 0));
  doc::Modeler& m = f.window.modeler();
  auto profile = m.createPolygon({gp_Pnt(0, 0, 0), gp_Pnt(2000, 0, 0), gp_Pnt(2000, 0, 1000), gp_Pnt(1000, 0, 1000),
                                  gp_Pnt(1000, 0, 2000), gp_Pnt(0, 0, 2000)});
  QVERIFY(profile.ok());
  QVERIFY(m.extrude(*profile, 1000).ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 2000)));
  click(f.vp, f.vp->toWidget(gp_Pnt(2000, 0, 1000)));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QTest::qWait(30);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  int faces = 0, lines = 0;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() == doc::ObjectKind::Face) {
      ++faces;
      QVERIFY(std::abs(geom::area(m.document().worldShape(o)) - 0.5e6) < 1.0);
    }
    lines += o.kind() == doc::ObjectKind::Curve;
  }
  QCOMPARE(faces, 1);
  QCOMPARE(lines, 0);
}

void GuiPhase1::moveToolLiftsRidgeIntoGable() {
  Fixture f;
  QVERIFY(f.setUp(3000, 2000));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 6000, 4000, 3000);
  QVERIFY(m.splitWithLine({gp_Pnt(0, 2000, 3000), gp_Pnt(6000, 2000, 3000)}).ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Move);
  const std::size_t steps = m.history().undoCount();
  const QPoint grab = f.vp->toWidget(gp_Pnt(3000, 2000, 3000));
  move(f.vp, grab);
  send(f.vp, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
  send(f.vp, QEvent::MouseButtonRelease, grab, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
  QTest::qWait(20);
  QTest::keyClick(f.vp, Qt::Key_Up);
  move(f.vp, f.vp->toWidget(gp_Pnt(3000, 2000, 8000)));
  type(f.vp, QStringLiteral("1500"));
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.history().undoCount(), steps + 1);
  const double v = geom::volume(m.document().worldShape(box));
  QVERIFY2(std::abs(v - (6000.0 * 4000 * 3000 + 0.5 * 4000 * 1500 * 6000)) < 10.0, qPrintable(QString::number(v)));
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Solid);
}

// Step 5: Shift+click with the eraser hides an edge's line (faces stay as
// they are); the viewport draws the object without it.
void GuiPhase1::eraserShiftHidesEdgeLine() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  QCOMPARE(f.vp->hiddenEdgeCount(box), std::size_t(0));
  f.window.activateTool(MainWindow::ToolId::Eraser);
  const QPoint edge = f.vp->toWidget(gp_Pnt(0, 0, 500));  // front left vertical edge
  move(f.vp, edge);
  send(f.vp, QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
  send(f.vp, QEvent::MouseButtonRelease, edge, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
  QTest::qWait(20);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY(m.document().find(box));  // not erased
  QCOMPARE(m.document().find(box)->hiddenEdges.size(), std::size_t(1));
  f.vp->syncNow();
  QCOMPARE(f.vp->hiddenEdgeCount(box), std::size_t(1));
  if (const QString dir = qEnvironmentVariable("SBR_SHOT_DIR"); !dir.isEmpty()) {
    QVERIFY(m.moveVertices(box, {gp_Pnt(0, 0, 1000), gp_Pnt(1000, 0, 1000)}, gp_Vec(0, 0, 600)).ok());
    f.vp->syncNow();
    f.vp->fitAll();
    f.vp->dumpImage(dir + QStringLiteral("/hidden_edge.png"));
    QVERIFY(m.undo());
  }
  QVERIFY(m.undo());
  f.vp->syncNow();
  QCOMPARE(f.vp->hiddenEdgeCount(box), std::size_t(0));
}

// The opening tool must not crash wherever it is pointed or clicked: walls,
// thin vertical faces, boxes, groups, components, sloped and hidden-edge
// solids, guides and dimensions.
void GuiPhase1::openingToolOverEverything() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  QVERIFY(m.createSlab({gp_Pnt(0, 0, 0), gp_Pnt(4000, 0, 0), gp_Pnt(4000, 3000, 0), gp_Pnt(0, 3000, 0)}, 0, 200).ok());
  QVERIFY(m.createWalls({gp_Pnt(0, 0, 0), gp_Pnt(4000, 0, 0), gp_Pnt(4000, 3000, 0)}, 200, 2500).ok());
  auto face = m.createRectangle(geom::SketchPlane::fromNormal(gp_Pnt(-2000, -1000, 0), gp::DY()), 1500, 2000);
  QVERIFY(face.ok());
  const doc::ObjectId box = f.box(-3000, 1000, 1000, 1000, 1000);
  QVERIFY(m.moveVertices(box, {gp_Pnt(-3000, 1000, 1000), gp_Pnt(-2000, 1000, 1000)}, gp_Vec(0, 0, 500)).ok());
  const doc::ObjectId grouped = f.box(1000, -3000, 1000, 1000, 1000);
  QVERIFY(m.group({grouped}).ok());
  const doc::ObjectId part = f.box(-1000, -3000, 1000, 300, 2000);
  QVERIFY(m.makeComponent({part}).ok());
  QVERIFY(m.createGuide(gp_Pnt(0, -500, 0), gp::DX()).ok());
  f.vp->fitAll();
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Opening);
  const QSize size = f.vp->size();
  QStringList failures;
  auto check = [&](const QPoint& p) {
    const QString e = f.window.lastError();
    if (e.contains(QStringLiteral("形の計算でエラー")) && !failures.contains(e)) {
      failures << QStringLiteral("(%1,%2) ").arg(p.x()).arg(p.y()) + e;
    }
  };
  for (int y = 10; y < size.height(); y += 25) {
    for (int x = 10; x < size.width(); x += 25) {
      move(f.vp, QPoint(x, y));
      check(QPoint(x, y));
    }
  }
  for (int y = 20; y < size.height(); y += 60) {
    for (int x = 20; x < size.width(); x += 60) {
      click(f.vp, QPoint(x, y));
      check(QPoint(x, y));
    }
  }
  QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
}

// SketchUp: draw a # with four lines; the fourth closes the middle square
// into a face the moment it crosses the others (no end-to-end snapping).
void GuiPhase1::lineToolCrossingLinesMakeFace() {
  Fixture f;
  QVERIFY(f.setUp(2000, 2000));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Line);
  auto line = [&](const gp_Pnt& a, const gp_Pnt& b, bool end) {
    click(f.vp, f.vp->toWidget(a));
    click(f.vp, f.vp->toWidget(b));
    if (end) QTest::keyClick(f.vp, Qt::Key_Escape);  // ends the chain, keeps the line
    QTest::qWait(20);
  };
  line(gp_Pnt(500, 1000, 0), gp_Pnt(3500, 1000, 0), true);
  line(gp_Pnt(500, 3000, 0), gp_Pnt(3500, 3000, 0), true);
  line(gp_Pnt(1000, 500, 0), gp_Pnt(1000, 3500, 0), true);
  QCOMPARE(m.document().objects().size(), std::size_t(3));
  line(gp_Pnt(3000, 500, 0), gp_Pnt(3000, 3500, 0), false);  // ends by itself: an area closed
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  int faces = 0, curves = 0;
  double area = 0;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() == doc::ObjectKind::Face) {
      ++faces;
      area = geom::area(m.document().worldShape(o));
    }
    curves += o.kind() == doc::ObjectKind::Curve;
  }
  QCOMPARE(faces, 1);
  // Clicks land on pixel-rounded ground points: within a percent.
  QVERIFY2(std::abs(area - 2000.0 * 2000) < 0.01 * 2000.0 * 2000, qPrintable(QString::number(area)));
  QCOMPARE(curves, 8);
  QVERIFY(f.window.statusText().contains(QStringLiteral("面")));
  // One undo takes back the fourth line and its face together.
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(3));
}

// The eraser works on lines like SketchUp: the corner part of an L between
// the corner and an arc's end, then a box's corner edge (its two faces go,
// highlighted first).
void GuiPhase1::eraserWorksOnLines() {
  Fixture f;
  QVERIFY(f.setUp(3000, 3000));
  doc::Modeler& m = f.window.modeler();
  auto l = m.createPolyline({gp_Pnt(500, 7500, 0), gp_Pnt(500, 500, 0), gp_Pnt(7500, 500, 0)});
  QVERIFY(l.ok());
  QVERIFY(m.createArc(gp_Pnt(500, 2300, 0), gp_Pnt(2300, 500, 0), gp::DZ(), -(1800 - 1800 / std::sqrt(2.0))).ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Eraser);
  click(f.vp, f.vp->toWidget(gp_Pnt(500, 900, 0)));  // vertical leg, between the corner and the arc
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  click(f.vp, f.vp->toWidget(gp_Pnt(900, 500, 0)));  // horizontal leg, the same
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  double length = 0;
  int curves = 0;
  for (const doc::Object& o : m.document().objects()) {
    if (o.kind() != doc::ObjectKind::Curve) continue;
    ++curves;
    GProp_GProps props;
    BRepGProp::LinearProperties(m.document().worldShape(o), props);
    length += props.Mass();
  }
  QCOMPARE(curves, 3);
  QVERIFY2(std::abs(length - (5200 + 5200 + M_PI / 2 * 1800)) < 1e-3, qPrintable(QString::number(length)));

  // A box's front top edge: hovering says what goes, clicking opens the box.
  const doc::ObjectId box = f.box(9000, 0, 2000, 2000, 2000);
  f.vp->syncNow();
  const QPoint edge = f.vp->toWidget(gp_Pnt(10000, 0, 2000));
  move(f.vp, edge);
  QVERIFY2(f.window.statusText().contains(QStringLiteral("開いたまま")), qPrintable(f.window.statusText()));
  click(f.vp, edge);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().find(box)->kind(), doc::ObjectKind::Shell);
}

// Right-click: what is under the cursor gets selected (a group or component
// as a whole) and the menu offers what applies to it.
void GuiPhase1::rightClickMenuFollowsTheSelection() {
  Fixture f;
  QVERIFY(f.setUp(1500, 1500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  const doc::ObjectId inGroup = f.box(2000, 0, 1000, 1000, 1000);
  auto group = m.group({inGroup});
  QVERIFY(group.ok());
  const doc::ObjectId part = f.box(0, 2000, 1000, 1000, 1000);
  auto instance = m.makeComponent({part}, "Unit");
  QVERIFY(instance.ok());
  f.vp->syncNow();
  auto texts = [](QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << a->text();
    return out;
  };
  f.window.activateTool(MainWindow::ToolId::Select);
  QMenu* menu = f.window.buildContextMenu(f.vp->toWidget(gp_Pnt(500, 0, 500)));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{box});
  QVERIFY(texts(menu).contains(QStringLiteral("グループ化")));
  QVERIFY(texts(menu).contains(QStringLiteral("コンポーネントを作成")));
  QVERIFY(texts(menu).contains(QStringLiteral("マテリアル")));
  delete menu;

  menu = f.window.buildContextMenu(f.vp->toWidget(gp_Pnt(2500, 0, 500)));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{*group});
  QVERIFY(texts(menu).contains(QStringLiteral("グループを編集（中に入る）")));
  delete menu;

  menu = f.window.buildContextMenu(f.vp->toWidget(gp_Pnt(500, 2000, 500)));
  QCOMPARE(f.window.selection(), std::vector<doc::ObjectId>{*instance});
  bool edit = false;
  for (const QString& t : texts(menu)) edit = edit || t.startsWith(QStringLiteral("コンポーネントを編集"));
  QVERIFY(edit);
  QVERIFY(texts(menu).contains(QStringLiteral("この配置")));
  // A material from the menu goes onto the selection.
  auto red = m.createMaterial("赤", {1, 0, 0});
  QVERIFY(red.ok());
  delete menu;
  menu = f.window.buildContextMenu(f.vp->toWidget(gp_Pnt(500, 0, 500)));
  QMenu* materials = nullptr;
  for (QAction* a : menu->actions()) {
    if (a->text() == QStringLiteral("マテリアル")) materials = a->menu();
  }
  QVERIFY(materials);
  for (QAction* a : materials->actions()) {
    if (a->text() == QStringLiteral("赤")) a->trigger();
  }
  QCOMPARE(m.document().find(box)->material, *red);
  delete menu;
}

// The paint tool starts with no material: clicking asks for one instead of
// silently painting "none"; a ready-made one in the panel is added and
// picked with one click, then clicking a face paints it.
void GuiPhase1::paintNeedsAMaterialAndPresetsAddOnClick() {
  Fixture f;
  QVERIFY(f.setUp(500, 500));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(0, 0, 1000, 1000, 1000);
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Paint);
  const QPoint face = f.vp->toWidget(gp_Pnt(500, 0, 500));
  click(f.vp, face);
  QVERIFY(f.window.lastError().contains(QStringLiteral("マテリアル")));
  QVERIFY(m.document().materials().empty());

  auto* panel = f.window.findChild<MaterialsPanel*>();
  QVERIFY(panel);
  int preset = -1;
  for (int i = 0; i < panel->list()->count(); ++i) {
    if (panel->list()->item(i)->text() == QStringLiteral("レンガ")) preset = i;
  }
  QVERIFY(preset > 0);
  panel->list()->setCurrentRow(preset);
  QTest::qWait(20);
  QCOMPARE(m.document().materials().size(), std::size_t(1));
  QCOMPARE(QString::fromStdString(m.document().materials().front().name), QStringLiteral("レンガ"));
  click(f.vp, face);
  QCOMPARE(m.document().find(box)->faceMaterials.size(), std::size_t(1));
  QCOMPARE(m.document().find(box)->faceMaterials.front().material, m.document().materials().front().id);
}

// Sky above the horizon, ground below (SketchUp): in perspective the
// horizon is where horizontal planes vanish; looking down from above in a
// parallel view it is all ground. SBR_SHOT_DIR saves the pictures.
void GuiPhase1::skyAndGroundFollowTheHorizon() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  f.box(0, 0, 1000, 1000, 1500);
  doc::CameraState cam;
  cam.eye = {-6000, -8000, 1700};
  cam.target = {1000, 1000, 600};
  cam.up = {0, 0, 1};
  cam.perspective = true;
  f.vp->restoreCamera(cam);
  f.vp->syncNow();
  const QString dir = qEnvironmentVariable("SBR_SHOT_DIR");
  const QString path = (dir.isEmpty() ? QDir::tempPath() : dir) + QStringLiteral("/sky_ground.png");
  QVERIFY(f.vp->dumpImage(path));
  const QImage shot(path);
  QVERIFY(!shot.isNull());
  // Top: sky (bluish grey, lighter than the ground); bottom: ground.
  const QColor top = shot.pixelColor(shot.width() - 5, 5);
  const QColor bottom = shot.pixelColor(shot.width() - 5, shot.height() - 5);
  QVERIFY2(top.blue() > top.red(), qPrintable(top.name()));
  QVERIFY2(bottom.red() >= bottom.blue(), qPrintable(bottom.name()));
  QVERIFY2(top != bottom, qPrintable(top.name() + bottom.name()));
}

// Selected faces keep their own colour under blue dots (SketchUp), so a
// material stays recognisable while selected.
void GuiPhase1::selectionDotsKeepTheMaterialColour() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(-1000, -1000, 2000, 2000, 2000);
  auto red = m.createMaterial("Red", {0.9, 0.1, 0.1});
  QVERIFY(red.ok());
  QVERIFY(m.assignMaterial({box}, *red).ok());
  f.window.setSelection({box});
  f.vp->syncNow();
  const QString dir = qEnvironmentVariable("SBR_SHOT_DIR");
  const QString path = (dir.isEmpty() ? QDir::tempPath() : dir) + QStringLiteral("/selection_dots.png");
  QVERIFY(f.vp->dumpImage(path));
  const QImage shot(path);
  // Around the middle of the front face: mostly the red material, some blue dots.
  const QPoint c = f.vp->toWidget(gp_Pnt(0, -1000, 1000));
  int reddish = 0, blue = 0;
  for (int dy = -8; dy < 8; ++dy) {
    for (int dx = -8; dx < 8; ++dx) {
      const QColor px = shot.pixelColor(c.x() + dx, c.y() + dy);
      if (px.red() > 2 * px.blue() && px.red() > 100) ++reddish;
      if (px.blue() > px.red() + 60) ++blue;
    }
  }
  QVERIFY2(reddish > 128, qPrintable(QString("red %1 blue %2").arg(reddish).arg(blue)));
  QVERIFY2(blue >= 4, qPrintable(QString("red %1 blue %2").arg(reddish).arg(blue)));
}

// SketchUp: hold the eraser down and sweep across lines; they are marked
// and go together on release (one undo step). A click still erases one.
void GuiPhase1::eraserDragErasesCrossedLines() {
  Fixture f;
  QVERIFY(f.setUp(1500, 1500));
  doc::Modeler& m = f.window.modeler();
  for (double y : {1000.0, 1500.0, 2000.0}) QVERIFY(m.createPolyline({gp_Pnt(500, y, 0), gp_Pnt(2500, y, 0)}).ok());
  QVERIFY(m.createPolyline({gp_Pnt(500, 3000, 0), gp_Pnt(2500, 3000, 0)}).ok());  // not crossed
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Eraser);
  const std::size_t steps = m.history().undoCount();
  const QPoint from = f.vp->toWidget(gp_Pnt(1500, 700, 0)), to = f.vp->toWidget(gp_Pnt(1500, 2300, 0));
  move(f.vp, from);
  send(f.vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
  for (int k = 1; k <= 10; ++k) {
    send(f.vp, QEvent::MouseMove, from + (to - from) * k / 10, Qt::NoButton, Qt::LeftButton);
  }
  QVERIFY2(f.window.statusText().contains(QStringLiteral("3 本")), qPrintable(f.window.statusText()));
  send(f.vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  QTest::qWait(20);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));  // only the line not crossed
  QCOMPARE(m.history().undoCount(), steps + 1);
  QVERIFY(m.undo());
  QCOMPARE(m.document().objects().size(), std::size_t(4));
}

// Review 2026-10-23 P0-01: one stroke across two legs of the same polyline.
// Erasing the first leg rebuilds the object (new ids); the second is found
// in what the first erase made of it, and nothing outside it is touched: a
// separate line elsewhere through the second leg's point stays.
void GuiPhase1::eraserDragFollowsWhatTheLineBecame() {
  Fixture f;
  QVERIFY(f.setUp(1500, 1500));
  doc::Modeler& m = f.window.modeler();
  auto u = m.createPolyline({gp_Pnt(500, 1000, 0), gp_Pnt(2500, 1000, 0), gp_Pnt(2500, 2000, 0), gp_Pnt(500, 2000, 0)});
  QVERIFY(u.ok());
  // A separate line crossing the stroke's path far from it (not crossed).
  auto other = m.createPolyline({gp_Pnt(500, 3500, 0), gp_Pnt(2500, 3500, 0)});
  QVERIFY(other.ok());
  f.vp->syncNow();
  f.window.activateTool(MainWindow::ToolId::Eraser);
  const std::size_t steps = m.history().undoCount();
  const QPoint from = f.vp->toWidget(gp_Pnt(1500, 700, 0)), to = f.vp->toWidget(gp_Pnt(1500, 2300, 0));
  move(f.vp, from);
  send(f.vp, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
  for (int k = 1; k <= 10; ++k) {
    send(f.vp, QEvent::MouseMove, from + (to - from) * k / 10, Qt::NoButton, Qt::LeftButton);
  }
  send(f.vp, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  QTest::qWait(20);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QVERIFY2(f.window.statusText().contains(QStringLiteral("2 本")), qPrintable(f.window.statusText()));
  QCOMPARE(m.history().undoCount(), steps + 1);
  // Left: the right leg of the U, and the other line untouched.
  QVERIFY(m.document().find(*other));
  double length = 0;
  for (const doc::Object& o : m.document().objects()) {
    if (o.id == *other) continue;
    GProp_GProps props;
    BRepGProp::LinearProperties(m.document().worldShape(o), props);
    length += props.Mass();
  }
  QVERIFY2(std::abs(length - 1000.0) < 1e-3, qPrintable(QString::number(length)));
}

// Wheel zoom (docs/17_wheel_zoom.md, request Z02 Z05 Z06 Z07 Z11): one
// notch divides the distance to the point under the cursor by the ratio;
// that point stays under the cursor; however the same total input is
// split, the camera ends up the same; the opposite input goes back; empty
// begin/end events do nothing; pixel-only input works and is never added
// to angle input.
void GuiPhase1::wheelZoomIsConsistentAndReversible() {
  Fixture f;
  QVERIFY(f.setUp(4000, 6000));
  f.box(0, 0, 8000, 12000, 3000);  // the request's building outline
  f.vp->syncNow();
  for (bool perspective : {true, false}) {
    doc::CameraState start;
    start.eye = {-4000, -8000, 12000};
    start.target = {4000, 6000, 3000};
    start.perspective = perspective;
    start.scale = 20000;
    f.vp->restoreCamera(start);
    f.vp->storeCamera(start);  // as the view really is
    const QPoint cursor = f.vp->toWidget(gp_Pnt(6500, 9000, 3000));  // off-centre, on the roof
    const double ratio = f.vp->wheelRatioPerNotch();

    // One notch.
    wheelAt(f.vp, cursor, QPoint(0, 120));
    doc::CameraState one;
    f.vp->storeCamera(one);
    const auto anchor = f.vp->lastWheelAnchor();
    QVERIFY(anchor);
    QCOMPARE(f.vp->lastWheelAnchorSource(), QStringLiteral("face"));
    QVERIFY2(std::abs(anchor->Z() - 3000) < 1.0, qPrintable(QString::number(anchor->Z())));
    const snap::ScreenPoint shown = f.vp->project(*anchor);
    QVERIFY2(std::hypot(shown.x - cursor.x(), shown.y - cursor.y()) <= 2.0,
             qPrintable(QStringLiteral("%1,%2 vs %3,%4").arg(shown.x).arg(shown.y).arg(cursor.x()).arg(cursor.y())));
    if (perspective) {
      const gp_Pnt eye0(start.eye[0], start.eye[1], start.eye[2]), eye1(one.eye[0], one.eye[1], one.eye[2]);
      QVERIFY(std::abs(eye0.Distance(*anchor) / eye1.Distance(*anchor) - ratio) < 1e-9);
    } else {
      QVERIFY(std::abs(start.scale / one.scale - ratio) < 1e-9);
    }

    // The same 120 split in other ways, and the pixel-only / mixed forms.
    const double tolerance = 1e-6 * 20000;
    const std::vector<std::vector<std::pair<QPoint, QPoint>>> splits = {
        std::vector<std::pair<QPoint, QPoint>>(2, {QPoint(0, 60), QPoint()}),
        std::vector<std::pair<QPoint, QPoint>>(4, {QPoint(0, 30), QPoint()}),
        std::vector<std::pair<QPoint, QPoint>>(120, {QPoint(0, 1), QPoint()}),
        std::vector<std::pair<QPoint, QPoint>>{{QPoint(0, 0), QPoint()}, {QPoint(0, 120), QPoint(0, 999)},
                                               {QPoint(0, 0), QPoint()}},
        std::vector<std::pair<QPoint, QPoint>>{{QPoint(), QPoint(0, 50)}}};
    for (const auto& split : splits) {
      f.vp->restoreCamera(start);
      for (const auto& [angle, pixel] : split) wheelAt(f.vp, cursor, angle, pixel);
      doc::CameraState after;
      f.vp->storeCamera(after);
      QVERIFY2(gap(after, one) < tolerance, qPrintable(QStringLiteral("split of %1: %2").arg(split.size()).arg(gap(after, one))));
    }
    // An empty begin / end does nothing.
    f.vp->restoreCamera(start);
    wheelAt(f.vp, cursor, QPoint(), QPoint(), Qt::ScrollBegin);
    wheelAt(f.vp, cursor, QPoint(), QPoint(), Qt::ScrollEnd);
    doc::CameraState still;
    f.vp->storeCamera(still);
    QVERIFY(gap(still, start) < tolerance);

    // Five in, five out: back where it started.
    for (int k = 0; k < 5; ++k) wheelAt(f.vp, cursor, QPoint(0, 120));
    for (int k = 0; k < 5; ++k) wheelAt(f.vp, cursor, QPoint(0, -120));
    doc::CameraState back;
    f.vp->storeCamera(back);
    QVERIFY2(gap(back, start) < tolerance, qPrintable(QString::number(gap(back, start))));
  }
}

// Request Z04 Z10: pointing at the sky zooms at the depth last used (no
// jump to the origin), keeps moving through empty space, and zooming into
// a face stops short of it: no flip, no NaN.
void GuiPhase1::wheelZoomEmptySpaceAndLimits() {
  Fixture f;
  QVERIFY(f.setUp(4000, 6000));
  f.box(0, 0, 8000, 12000, 3000);
  f.vp->syncNow();
  doc::CameraState start;
  start.eye = {4000, -20000, 2000};
  start.target = {4000, 6000, 1500};
  start.perspective = true;
  start.scale = 20000;
  f.vp->restoreCamera(start);
  const QPoint wall = f.vp->toWidget(gp_Pnt(4000, 0, 1500));
  wheelAt(f.vp, wall, QPoint(0, 120));
  QCOMPARE(f.vp->lastWheelAnchorSource(), QStringLiteral("face"));
  const gp_Pnt onWall = *f.vp->lastWheelAnchor();

  // To the sky: the depth of the wall, still under the cursor.
  const QPoint sky(f.vp->width() / 2, 10);
  wheelAt(f.vp, sky, QPoint(0, 120));
  QCOMPARE(f.vp->lastWheelAnchorSource(), QStringLiteral("depth"));
  const auto inSky = f.vp->lastWheelAnchor();
  const snap::ScreenPoint shown = f.vp->project(*inSky);
  QVERIFY(std::hypot(shown.x - sky.x(), shown.y - sky.y()) <= 2.0);
  QVERIFY2(std::abs(inSky->Y() - onWall.Y()) < 0.25 * std::abs(onWall.Y() - start.eye[1]),
           qPrintable(QString::number(inSky->Y())));
  // Empty space never stalls.
  for (int k = 0; k < 40; ++k) {
    doc::CameraState before, after;
    f.vp->storeCamera(before);
    wheelAt(f.vp, sky, QPoint(0, 120));
    f.vp->storeCamera(after);
    QVERIFY(gap(before, after) > 1.0);
  }

  // Into the wall: stops short of it, stays in front, all finite.
  f.vp->restoreCamera(start);
  for (int k = 0; k < 200; ++k) wheelAt(f.vp, wall, QPoint(0, 120));
  doc::CameraState close;
  f.vp->storeCamera(close);
  for (double v : close.eye) QVERIFY(std::isfinite(v));
  QVERIFY2(close.eye[1] < 0 && close.eye[1] > -wheel::kMinDistance - 1.0, qPrintable(QString::number(close.eye[1])));
  // And out again.
  for (int k = 0; k < 200; ++k) wheelAt(f.vp, wall, QPoint(0, -120));
  doc::CameraState away;
  f.vp->storeCamera(away);
  QVERIFY2(away.eye[1] < start.eye[1] + 1.0, qPrintable(QString::number(away.eye[1])));
}

// Request Z09: the wheel does not end a line in progress; the next click
// still finishes it from the first point.
void GuiPhase1::wheelZoomKeepsDrawing() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  f.window.activateTool(MainWindow::ToolId::Line);
  click(f.vp, f.vp->toWidget(gp_Pnt(0, 0, 0)));
  for (int k = 0; k < 3; ++k) wheelAt(f.vp, f.vp->toWidget(gp_Pnt(1000, 0, 0)), QPoint(0, 120));
  // The direction from the new view, the length typed (pixel positions
  // after the zoom are not exact enough to click the end itself).
  move(f.vp, f.vp->toWidget(gp_Pnt(700, 0, 0)));
  type(f.vp, QStringLiteral("1000"));
  QTest::keyClick(f.vp, Qt::Key_Escape);
  QVERIFY2(f.window.lastError().isEmpty(), qPrintable(f.window.lastError()));
  QCOMPARE(m.document().objects().size(), std::size_t(1));
  const Bnd_Box box = geom::exactBounds(m.document().worldShape(m.document().objects().front()));
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  QVERIFY2(std::abs(x0) < 1.0 && std::abs(x1 - 1000) < 1.0, qPrintable(QStringLiteral("%1 %2").arg(x0).arg(x1)));
}

// The material editor (SketchUp's 編集 tab): the colour is adjusted after
// painting (name and id stay, a drag is one undo step), a picture tiles the
// faces at its real size, opacity shows, and a copy edits separately.
void GuiPhase1::materialEditorColourTextureOpacityDuplicate() {
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  doc::Modeler& m = f.window.modeler();
  const doc::ObjectId box = f.box(-1000, -1000, 2000, 2000, 2000);
  auto brick = m.createMaterial("レンガ", {0.63, 0.32, 0.22});
  QVERIFY(brick.ok());
  QVERIFY(m.assignMaterial({box}, *brick).ok());
  f.window.chooseMaterial(*brick);
  auto* panel = f.window.findChild<MaterialsPanel*>();
  QVERIFY(panel);
  panel->openEditor();
  MaterialEditor* editor = panel->editor();
  QCOMPARE(editor->materialId(), *brick);

  // Colour dragged in the picker: name and id stay, one undo step.
  const std::size_t steps = m.history().undoCount();
  auto* picker = editor->findChild<QColorDialog*>(QStringLiteral("materialColor"));
  QVERIFY(picker);
  for (int r : {120, 140, 160}) picker->setCurrentColor(QColor(r, 60, 40));
  QCOMPARE(m.history().undoCount(), steps + 1);
  QCOMPARE(m.document().findMaterial(*brick)->name, std::string("レンガ"));
  QVERIFY(std::abs(m.document().findMaterial(*brick)->color.r - 160 / 255.0) < 1e-3);

  // A picture: a 2 x 2 checker of red and white, 500 mm per tile.
  QImage checker(2, 2, QImage::Format_RGB32);
  checker.setPixel(0, 0, qRgb(220, 30, 30));
  checker.setPixel(1, 1, qRgb(220, 30, 30));
  checker.setPixel(1, 0, qRgb(255, 255, 255));
  checker.setPixel(0, 1, qRgb(255, 255, 255));
  const QString png = QDir::temp().filePath(QStringLiteral("sbr_checker.png"));
  QVERIFY(checker.save(png));
  QVERIFY(f.window.setMaterialTexture(*brick, png));
  const doc::Material* withPicture = m.document().findMaterial(*brick);
  QVERIFY(withPicture->texture);
  QCOMPARE(withPicture->texture->width, 1000.0);
  QCOMPARE(withPicture->texture->height, 1000.0);  // square picture: square tile
  f.vp->syncNow();
  QVERIFY(f.vp->showsTexture(box));
  if (const QString dir = qEnvironmentVariable("SBR_SHOT_DIR"); !dir.isEmpty()) {
    f.vp->dumpImage(dir + QStringLiteral("/texture.png"));
  }

  // Opacity from the slider.
  auto sliders = editor->findChildren<QSlider*>();
  QVERIFY(!sliders.isEmpty());
  sliders.front()->setValue(40);  // the first slider is opacity
  QVERIFY(std::abs(m.document().findMaterial(*brick)->opacity - 0.4) < 1e-9);

  // Duplicate: a separate copy to change.
  const std::size_t count = m.document().materials().size();
  QVERIFY(QMetaObject::invokeMethod(editor, "duplicateRequested", Q_ARG(std::string, *brick)));
  QCOMPARE(m.document().materials().size(), count + 1);
  QVERIFY(editor->materialId() != *brick);
  QCOMPARE(m.document().findMaterial(editor->materialId())->name, std::string("レンガ 2"));
}

// The logo set (assets/branding) is built in: window icon and About box.
void GuiPhase1::brandingResourcesLoad() {
  const QPixmap icon(QStringLiteral(":/branding/app-icon-256.png"));
  QCOMPARE(icon.size(), QSize(256, 256));
  const QPixmap big(QStringLiteral(":/branding/app-icon-1024.png"));
  QCOMPARE(big.size(), QSize(1024, 1024));
  const QPixmap wordmark(QStringLiteral(":/branding/wordmark.png"));
  QVERIFY(!wordmark.isNull());
  QVERIFY(wordmark.hasAlphaChannel());
  QVERIFY(QFile::exists(QStringLiteral(SBR_SOURCE_DIR "/assets/branding/05-windows/sketchbrep.ico")));
  // The About box shows the wordmark (and closes again).
  Fixture f;
  QVERIFY(f.setUp(0, 0));
  bool shown = false;
  QTimer::singleShot(300, [&] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (!box) return;
    shown = box->iconPixmap().width() > 100;
    if (const QString dir = qEnvironmentVariable("SBR_SHOT_DIR"); !dir.isEmpty()) {
      box->grab().save(dir + QStringLiteral("/about.png"));
    }
    box->accept();
  });
  f.window.showAbout();
  QVERIFY(shown);
}

QTEST_MAIN(GuiPhase1)
#include "test_gui.moc"
