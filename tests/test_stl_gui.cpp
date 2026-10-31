#include <QtTest/QtTest>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <BRepPrimAPI_MakeSphere.hxx>
#include "MainWindow.h"
using namespace sbr;
class StlExportGui : public QObject {
  Q_OBJECT
  QAction* exportAction(app::MainWindow& w) {
    for(auto* menu:w.menuBar()->actions()) if(menu->menu()) for(auto* action:menu->menu()->actions())
      if(action->text()==QString::fromUtf8("エクスポート...")) return action;
    return nullptr;
  }
 private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  }
  void selectionAndVisibleModelUseTheSameExportPath() {
    QTemporaryDir dir;app::MainWindow w;w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto plane=geom::SketchPlane::ground();auto a=w.modeler().createRectangle(plane,10,20);QVERIFY(a.ok());QVERIFY(w.modeler().extrude(*a,30));
    plane.origin=gp_Pnt(50,0,0);auto b=w.modeler().createRectangle(plane,10,20);QVERIFY(b.ok());QVERIFY(w.modeler().extrude(*b,30));
    const auto history=w.modeler().history().undoCount();
    w.setSelection({*a});const auto path=dir.filePath(QString::fromUtf8("模型.stl"));
    QVERIFY(w.exportStlFile(path));QCOMPARE(QFile(path).size(),qint64(84+12*50));
    w.setSelection({});QVERIFY(w.exportStlFile(path));QCOMPARE(QFile(path).size(),qint64(84+24*50));
    QCOMPARE(w.modeler().history().undoCount(),history);
  }
  void exportMenuWritesWithChosenToleranceAndCancelIsHarmless() {
    QTemporaryDir dir;app::MainWindow w;w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
    QVERIFY(w.modeler().importShape(BRepPrimAPI_MakeSphere(10).Shape(),"Sphere","test").ok());
    const auto coarse=dir.filePath("coarse.stl"),fine=dir.filePath("fine.stl");
    QVERIFY2(w.exportStlFile(coarse,0.5),qPrintable(w.lastError()));auto* action=exportAction(w);QVERIFY(action);
    bool configured=false,fileChosen=false;
    QTimer timer;
    connect(&timer,&QTimer::timeout,[&]{
      for(auto* widget:QApplication::topLevelWidgets()) {
        if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible()) {
          file->setDirectory(dir.path());
          for(const auto& filter:file->nameFilters())if(filter.startsWith("STL")){file->selectNameFilter(filter);QMetaObject::invokeMethod(file,"filterSelected",Qt::DirectConnection,Q_ARG(QString,filter));}
          if(auto* edit=file->findChild<QLineEdit*>("fileNameEdit"))edit->setText("fine");else file->selectFile("fine");
          fileChosen=true;
          QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);
        } else if(auto* dialog=qobject_cast<QDialog*>(widget);dialog&&dialog->isVisible()&&dialog->windowTitle()==QString::fromUtf8("STL エクスポート")) {
          if(auto* accuracy=dialog->findChild<QDoubleSpinBox*>()){accuracy->setValue(0.02);configured=true;dialog->accept();}
        }
      }
    });
    timer.start(10);action->trigger();timer.stop();
    QVERIFY(configured);QVERIFY(fileChosen);QVERIFY(QFile(fine).size()>QFile(coarse).size());
    const auto history=w.modeler().history().undoCount();
    bool cancelled=false;QTimer cancel;
    connect(&cancel,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets()) {
      if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible()) {
        file->setDirectory(dir.path());for(const auto& filter:file->nameFilters())if(filter.startsWith("STL")){file->selectNameFilter(filter);QMetaObject::invokeMethod(file,"filterSelected",Qt::DirectConnection,Q_ARG(QString,filter));}
        if(auto* edit=file->findChild<QLineEdit*>("fileNameEdit"))edit->setText("cancelled");else file->selectFile("cancelled");
        QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);
      } else if(auto* dialog=qobject_cast<QDialog*>(widget);dialog&&dialog->isVisible()&&dialog->findChild<QDoubleSpinBox*>()){
        cancelled=true;dialog->reject();
      }
    }});
    cancel.start(10);action->trigger();cancel.stop();QVERIFY(cancelled);QVERIFY(!QFile::exists(dir.filePath("cancelled.stl")));QCOMPARE(w.modeler().history().undoCount(),history);
  }
  void drawingLinesHaveAnActionableError() {
    QTemporaryDir dir;app::MainWindow w;
    QVERIFY(w.modeler().createPolyline({gp_Pnt(0,0,0),gp_Pnt(10,0,0)}).ok());
    const auto path=dir.filePath("lines.stl");QVERIFY(!w.exportStlFile(path));
    QVERIFY(w.lastError().contains(QString::fromUtf8("面や立体がありません")));QVERIFY(!QFile::exists(path));
  }
};
QTEST_MAIN(StlExportGui)
#include "test_stl_gui.moc"
