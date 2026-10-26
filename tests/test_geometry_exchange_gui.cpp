#include <QtTest/QtTest>
#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <BRepPrimAPI_MakeBox.hxx>
#include "MainWindow.h"
using namespace sbr;
class GeometryExchangeGui:public QObject {
 Q_OBJECT
 QAction* action(app::MainWindow& w,const QString& text) {
   for(auto* menu:w.menuBar()->actions())if(menu->menu())for(auto* a:menu->menu()->actions())if(a->text()==text)return a;
   return nullptr;
 }
 private slots:
 void initTestCase(){QStandardPaths::setTestModeEnabled(true);QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);}
 void exportAndImportFromMenus_data(){QTest::addColumn<bool>("iges");QTest::newRow("brep")<<false;QTest::newRow("iges")<<true;}
 void exportAndImportFromMenus(){
   QFETCH(bool,iges);QTemporaryDir dir;app::MainWindow w;w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
   auto a=w.modeler().importShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),"Box","test");QVERIFY(a.ok());
   QVERIFY(w.modeler().importShape(BRepPrimAPI_MakeBox(gp_Pnt(100,0,0),10,10,10).Shape(),"Other","test").ok());
   w.setSelection({*a});const auto history=w.modeler().history().undoCount();
   const auto format=iges?QStringLiteral("IGES"):QStringLiteral("BREP");const auto path=dir.filePath(iges?"model.igs":"model.brep");
   auto* output=action(w,format+QString::fromUtf8(" エクスポート..."));QVERIFY(output);
   bool configured=false,saved=false,importing=false;
   QTimer timer;connect(&timer,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets()) {
     if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible()) {
       if(!saved){file->setDirectory(dir.path());saved=true;}
       const auto name=importing?QFileInfo(path).fileName():QStringLiteral("model");
       if(auto* edit=file->findChild<QLineEdit*>("fileNameEdit"))edit->setText(name);
       else file->selectFile(name);
       QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);
     } else if(auto* dialog=qobject_cast<QDialog*>(widget);dialog&&dialog->isVisible()&&dialog->windowTitle()==format+QString::fromUtf8(" エクスポート")) {
       for(auto* label:dialog->findChildren<QLabel*>())configured|=label->text().contains(QString::fromUtf8("形状のみ"));
       dialog->accept();
     }
   }});
   timer.start(10);output->trigger();timer.stop();
   QVERIFY(configured);QVERIFY(saved);QVERIFY2(w.lastError().isEmpty(),qPrintable(w.lastError()));QVERIFY(QFileInfo(path).size()>0);
   QCOMPARE(w.modeler().history().undoCount(),history);
   auto* input=action(w,format+QString::fromUtf8(" インポート..."));QVERIFY(input);
   importing=true;saved=false;timer.start(10);input->trigger();timer.stop();
   QVERIFY(saved);QVERIFY2(w.lastError().isEmpty(),qPrintable(w.lastError()));QVERIFY(!w.selection().empty());
   for(const auto& id:w.selection())QCOMPARE(w.modeler().document().find(id)->kind(),doc::ObjectKind::Solid);
   QCOMPARE(w.modeler().history().undoCount(),history+1);QVERIFY(w.modeler().history().undo());QCOMPARE(w.modeler().listObjects().size(),std::size_t(2));
 }
 void invalidFileAndImportCancellationLeaveModelAlone_data(){exportAndImportFromMenus_data();}
 void invalidFileAndImportCancellationLeaveModelAlone(){
   QFETCH(bool,iges);QTemporaryDir dir;app::MainWindow w;
   QVERIFY(w.modeler().importShape(BRepPrimAPI_MakeBox(1,2,3).Shape(),"Box","test"));
   const auto before=w.modeler().history().undoCount();const auto path=dir.filePath(iges?"bad.igs":"bad.brep");
   QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));f.write("not a geometry file");f.close();
   QVERIFY(!w.importGeometryFile(path,iges?app::MainWindow::GeometryFormat::Iges:app::MainWindow::GeometryFormat::Brep));
   QCOMPARE(w.modeler().history().undoCount(),before);QCOMPARE(w.modeler().listObjects().size(),std::size_t(1));
   auto* input=action(w,(iges?QStringLiteral("IGES"):QStringLiteral("BREP"))+QString::fromUtf8(" インポート..."));QVERIFY(input);
   bool cancelled=false;QTimer timer;connect(&timer,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets())
     if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible()){cancelled=true;file->reject();}});
   timer.start(10);input->trigger();timer.stop();QVERIFY(cancelled);QCOMPARE(w.modeler().history().undoCount(),before);
 }
};
QTEST_MAIN(GeometryExchangeGui)
#include "test_geometry_exchange_gui.moc"
