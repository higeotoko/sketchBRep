#include <QtTest/QtTest>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QProgressDialog>
#include <QTimer>
#include <QInputDialog>
#include <QMessageBox>
#include <QAbstractButton>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QMenu>
#include <QAction>
#include <algorithm>
#include "MainWindow.h"
#include "Viewport.h"
#include "Tools.h"
#include "DwgConverter.h"

using namespace sbr;
class DrawingImport : public QObject {
  Q_OBJECT
 private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
  }
  void conversionAndGuiImport() {
    QTemporaryDir dir;
    const auto input = dir.filePath(QString::fromUtf8("図面 with spaces.dwg"));
    QFile source(input); QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("AC1032fixture"); source.close();
    const auto result = app::convertDwgToDxf(input, QString::fromUtf8(SBR_FAKE_ODA), dir.filePath("output"));
    QVERIFY2(result.ok(), result.error().c_str());
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), QByteArray("AC1032fixture"));
    source.close();
    app::MainWindow window;
    window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.importDxfFile(*result));
    QCOMPARE(window.modeler().listObjects().size(), std::size_t(1));
    QCOMPARE(window.modeler().history().undoCount(), std::size_t(1));
    const auto id = window.modeler().listObjects().front().id;
    QCOMPARE(QString::fromStdString(window.modeler().document().find(id)->layer), QString("Walls"));
    QCOMPARE(window.selection().size(), std::size_t(1));
    QVERIFY(window.modeler().history().undo());
    QVERIFY(window.modeler().listObjects().empty());
  }
  void unitsAndPartialImportCanBeCancelled() {
    QTemporaryDir dir;
    const auto path = dir.filePath("unitless.dxf");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("0\nSECTION\n2\nENTITIES\n0\nLINE\n10\n0\n20\n0\n11\n10\n21\n0\n"
               "0\nTEXT\n1\nNote\n0\nENDSEC\n0\nEOF\n"); file.close();
    app::MainWindow window;
    QTimer cancelUnits;
    connect(&cancelUnits, &QTimer::timeout, [] {
      for (auto* w : QApplication::topLevelWidgets())
        if (auto* dialog = qobject_cast<QInputDialog*>(w)) dialog->reject();
    });
    cancelUnits.start(10);
    QVERIFY(!window.importDxfFile(path));
    cancelUnits.stop();
    QVERIFY(window.modeler().listObjects().empty());
    QVERIFY(!window.modeler().history().canUndo());
    QTimer timer;
    connect(&timer, &QTimer::timeout, [] {
      for (auto* w : QApplication::topLevelWidgets()) {
        if (auto* dialog = qobject_cast<QInputDialog*>(w)) { dialog->setDoubleValue(1000); dialog->accept(); }
        if (auto* dialog = qobject_cast<QMessageBox*>(w)) dialog->button(QMessageBox::No)->click();
      }
    });
    timer.start(10);
    QVERIFY(!window.importDxfFile(path));
    QVERIFY(window.modeler().listObjects().empty());
    QVERIFY(!window.modeler().history().canUndo());
    timer.stop();
  }
  void cameraMotionRestoresQuality() {
    app::MainWindow window; window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto& view = window.viewport();
    view.zoomWheel(view.rect().center(), 1);
    QVERIFY(view.interactiveRendering());
    QTRY_VERIFY_WITH_TIMEOUT(!view.interactiveRendering(), 2000);
    const auto center = view.rect().center();
    QTest::mousePress(&view, Qt::MiddleButton, Qt::NoModifier, center);
    QTest::mouseMove(&view, center + QPoint(10, 5), 0);
    QVERIFY(view.interactiveRendering());
    QTest::qWait(220);
    QVERIFY(view.interactiveRendering());
    QTest::mouseRelease(&view, Qt::MiddleButton, Qt::NoModifier, center);
    QTRY_VERIFY_WITH_TIMEOUT(!view.interactiveRendering(), 2000);
    view.zoomWheel(center, -1);
    QTemporaryDir image;
    QVERIFY(view.dumpImage(image.filePath("view.png")));
    QVERIFY(!view.interactiveRendering());
  }
  void selectedDxfOutlineCreatesFaceFromContextMenu_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("grouped");
    QTest::newRow("closed-polyline") << QString("small_closed_polyline.dxf") << false;
    QTest::newRow("individual-lines") << QString("small_closed_lines.dxf") << false;
    QTest::newRow("layer-batch") << QString("small_closed_lines.dxf") << true;
    QTest::newRow("arc-layer") << QString("small_arc_outline.dxf") << true;
    QTest::newRow("selected-edges-in-layer") << QString("small_layer_with_extra_line.dxf") << true;
  }
  void selectedDxfOutlineCreatesFaceFromContextMenu() {
    QFETCH(QString, fixture); QFETCH(bool, grouped);
    app::MainWindow window; window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto path=QString::fromUtf8(SBR_TEST_DATA_DIR)+"/dxf/"+fixture;
    geom::DxfImportOptions options; options.groupByLayer=grouped;
    auto ids=window.modeler().importDxf(path.toStdString(),options); QVERIFY(ids.ok());
    window.setSelection(*ids); window.viewport().syncNow();
    if (fixture=="small_layer_with_extra_line.dxf") {
      std::vector<app::ElementRef> boundary;
      for (const auto& e:window.viewport().allElements(ids->front()))
        if (e.type==TopAbs_EDGE && e.index<=4) boundary.push_back(e);
      QCOMPARE(boundary.size(),std::size_t(4));
      window.setElementSelection(boundary);
    }
    window.viewport().setStandardView(app::Viewport::StandardView::Top);
    const auto before=window.modeler().listObjects().size();
    auto* menu=window.buildContextMenu(QPoint(2,2));
    QAction* create=nullptr;
    for(auto* a:menu->actions()) if(a->text()==QString::fromUtf8("選択した線から面を生成")) create=a;
    QVERIFY(create); create->trigger(); delete menu;
    QVERIFY2(window.lastError().isEmpty(),qPrintable(window.lastError()));
    QCOMPARE(window.selection().size(),std::size_t(1));
    const auto face=window.selection().front();
    QCOMPARE(window.modeler().document().find(face)->kind(),doc::ObjectKind::Face);
    QCOMPARE(window.modeler().listObjects().size(),before+1);
    QVERIFY(window.modeler().history().undo()); QCOMPARE(window.modeler().listObjects().size(),before);
    QVERIFY(window.modeler().history().redo()); QVERIFY(window.modeler().extrude(face,100));
  }
  void importedLinesMakeFaceWhenClosedWithLineTool() {
    app::MainWindow window; window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.importDxfFile(QString::fromUtf8(SBR_TEST_DATA_DIR)+"/dxf/small_open_rectangle.dxf"));
    window.activateTool(app::MainWindow::ToolId::Line);
    QTest::mouseClick(&window.viewport(),Qt::LeftButton,Qt::NoModifier,window.viewport().toWidget(gp_Pnt(0,1000,0)));
    QTest::mouseClick(&window.viewport(),Qt::LeftButton,Qt::NoModifier,window.viewport().toWidget(gp_Pnt(0,0,0)));
    const auto objects=window.modeler().listObjects();
    QCOMPARE(objects.size(),std::size_t(1));
    QCOMPARE(objects.front().kind,doc::ObjectKind::Face);
  }
  void conversionFailureAndTimeout_data() {
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("exit-failure") << QByteArray("AC1032FAIL");
    QTest::newRow("missing-output") << QByteArray("AC1032MISSING");
    QTest::newRow("timeout") << QByteArray("AC1032TIMEOUT");
    QTest::newRow("not-dwg") << QByteArray("not a drawing");
  }
  void largeDrawingUsesLayerBatches() {
    QTemporaryDir dir;
    QString path = dir.filePath("large.dxf");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n");
    for (int i = 0; i < 1500; ++i)
      file.write(QString("0\nLINE\n8\nWalls\n10\n%1\n20\n0\n11\n%1\n21\n1000\n").arg(i).toUtf8());
    file.write("0\nENDSEC\n0\nEOF\n"); file.close();
    // Optional local measurement with a real drawing; never retain its data.
    const auto real = qEnvironmentVariable("SBR_DXF_BENCH");
    if (!real.isEmpty()) path = real;
    app::MainWindow window; window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTimer timer;
    connect(&timer, &QTimer::timeout, [] {
      for (auto* w : QApplication::topLevelWidgets())
        if (auto* dialog = qobject_cast<QMessageBox*>(w)) {
          if (dialog->checkBox()) QVERIFY(dialog->checkBox()->isChecked());
          dialog->button(QMessageBox::Yes)->click();
        }
    });
    timer.start(10);
    QElapsedTimer clock; clock.start();
    QVERIFY(window.importDxfFile(path));
    qInfo() << "DXF import/display ms:" << clock.elapsed() << "objects:" << window.modeler().listObjects().size();
    if (real.isEmpty()) QCOMPARE(window.modeler().listObjects().size(), std::size_t(1));
    else QVERIFY(window.modeler().listObjects().size() < 1000);
    if (!real.isEmpty()) {
      clock.restart();
      for (int i = 0; i < 10; ++i) {
        window.viewport().zoomWheel(QPointF(300, 300), i % 2 ? -0.2 : 0.2);
        window.viewport().renderNow();
      }
      qInfo() << "DXF 10 zoom/render frames ms:" << clock.elapsed();
      window.setSelection({});
      auto measure = [&](const char* name, const auto& operation) {
        std::vector<double> times;
        for (int y = 1; y <= 5; ++y) for (int x = 1; x <= 8; ++x) {
          const QPoint p(window.viewport().width() * x / 9, window.viewport().height() * y / 6);
          QElapsedTimer sample; sample.start(); operation(p);
          times.push_back(sample.nsecsElapsed() / 1e6);
        }
        std::sort(times.begin(), times.end());
        qInfo() << name << "median/p95/max ms:" << times[20] << times[37] << times.back();
      };
      measure("DXF snap", [&](const QPoint& p) { window.viewport().snap(p, {}, {}, {}); });
      window.viewport().setPickMode(app::PickMode::Elements);
      measure("DXF pick", [&](const QPoint& p) { window.viewport().pickElement(p); });
      window.viewport().setPickMode(app::PickMode::Objects);
      measure("DXF object pick", [&](const QPoint& p) { window.viewport().pickObject(p); });
      window.viewport().setPickMode(app::PickMode::Elements);
      measure("DXF hover", [&](const QPoint& p) { QTest::mouseMove(&window.viewport(), p, 0); });
      const QPoint center = window.viewport().rect().center();
      clock.restart();
      QTest::mousePress(&window.viewport(), Qt::MiddleButton, Qt::NoModifier, center);
      qInfo() << "DXF orbit start ms:" << clock.elapsed();
      measure("DXF orbit/render", [&](const QPoint& p) {
        QTest::mouseMove(&window.viewport(), p, 0); window.viewport().renderNow();
      });
      QTest::mouseRelease(&window.viewport(), Qt::MiddleButton, Qt::NoModifier, center);
      QTest::mousePress(&window.viewport(), Qt::MiddleButton, Qt::ShiftModifier, center);
      measure("DXF pan/render", [&](const QPoint& p) {
        QTest::mouseMove(&window.viewport(), p, 0); window.viewport().renderNow();
      });
      QTest::mouseRelease(&window.viewport(), Qt::MiddleButton, Qt::ShiftModifier, center);
      window.viewport().setStandardView(app::Viewport::StandardView::Top);
      window.activateTool(app::MainWindow::ToolId::Line);
      measure("DXF line hover", [&](const QPoint& p) { QTest::mouseMove(&window.viewport(), p, 0); });
      std::vector<doc::ObjectId> all;
      for (const auto& o : window.modeler().listObjects()) all.push_back(o.id);
      clock.restart(); window.setSelection(all); window.viewport().syncNow(); window.viewport().renderNow();
      qInfo() << "DXF select all/sync/render ms:" << clock.elapsed();
      clock.restart(); const auto moved = window.modeler().move(all, gp_Vec(1000, 500, 0));
      QVERIFY2(moved.ok(), moved.message().c_str());
      window.viewport().syncNow(); window.viewport().renderNow();
      qInfo() << "DXF move all/sync/render ms:" << clock.elapsed();
      QVERIFY(window.modeler().history().undo());
      window.viewport().syncNow();
    }
    timer.stop();
    QVERIFY(window.modeler().history().undo());
    QVERIFY(window.modeler().listObjects().empty());
  }
  void conversionFailureAndTimeout() {
    QFETCH(QByteArray, content);
    QTemporaryDir dir;
    QFile source(dir.filePath("drawing.dwg")); QVERIFY(source.open(QIODevice::WriteOnly));
    source.write(content); source.close();
    const auto result = app::convertDwgToDxf(source.fileName(), QString::fromUtf8(SBR_FAKE_ODA),
                                            dir.filePath("output"), nullptr, 500);
    QVERIFY(!result);
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), content);
  }
  void cancelConversion() {
    QTemporaryDir dir;
    QFile source(dir.filePath("drawing.dwg")); QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("AC1032TIMEOUT"); source.close();
    QTimer::singleShot(100, [] {
      for (auto* w : QApplication::topLevelWidgets())
        if (auto* p = qobject_cast<QProgressDialog*>(w)) QMetaObject::invokeMethod(p, "canceled");
    });
    const auto result = app::convertDwgToDxf(source.fileName(), QString::fromUtf8(SBR_FAKE_ODA), dir.filePath("output"));
    QVERIFY(!result);
    QVERIFY(QString::fromStdString(result.error()).contains(QString::fromUtf8("中止")));
  }
};
QTEST_MAIN(DrawingImport)
#include "test_drawing_import.moc"
