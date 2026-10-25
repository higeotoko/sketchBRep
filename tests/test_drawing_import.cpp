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
#include "MainWindow.h"
#include "Viewport.h"
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
