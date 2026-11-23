#include <QtTest/QtTest>
#include <algorithm>
#include <cmath>
#include <QAbstractButton>
#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include "MainWindow.h"
#include "sbr/geom/Measure.h"
#include "sbr/geom/Skp.h"
using namespace sbr;
class GeometryExchangeGui:public QObject {
 Q_OBJECT
 QTemporaryDir settingsDirectory_;
 QAction* action(app::MainWindow& w,const QString& text) {
   for(auto* menu:w.menuBar()->actions())if(menu->menu())for(auto* a:menu->menu()->actions())if(a->text()==text)return a;
   return nullptr;
 }
 void choose(QFileDialog* file,const QString& directory,const QString& name,const QString& format={}) {
   file->setDirectory(directory);
   if(!format.isEmpty())for(const auto& filter:file->nameFilters())if(filter.startsWith(format)) {
     file->selectNameFilter(filter);
     QMetaObject::invokeMethod(file,"filterSelected",Qt::DirectConnection,Q_ARG(QString,filter));break;
   }
   if(auto* edit=file->findChild<QLineEdit*>("fileNameEdit"))edit->setText(name);else file->selectFile(name);
   QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);
 }
 private slots:
 void initTestCase(){
   QStandardPaths::setTestModeEnabled(true);QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
   QCoreApplication::setOrganizationName("SketchBRepTests");QSettings::setDefaultFormat(QSettings::IniFormat);
   QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDirectory_.path());
 }
 void exportAndImportFromMenus_data(){
   QTest::addColumn<QString>("format");QTest::addColumn<QString>("extension");
   QTest::newRow("brep")<<QString("BREP")<<QString("brep");QTest::newRow("iges")<<QString("IGES")<<QString("igs");
   QTest::newRow("step")<<QString("STEP")<<QString("step");
#ifdef SBR_TEST_HAS_3DM
   QTest::newRow("rhino")<<QString("Rhino")<<QString("3dm");
#endif
 }
 void exportAndImportFromMenus(){
   QFETCH(QString,format);QFETCH(QString,extension);QTemporaryDir dir;app::MainWindow w;w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
   auto a=w.modeler().importShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),"Box","test");QVERIFY(a.ok());
   QVERIFY(w.modeler().importShape(BRepPrimAPI_MakeBox(gp_Pnt(100,0,0),10,10,10).Shape(),"Other","test").ok());
   w.setSelection({*a});const auto history=w.modeler().history().undoCount();
   const auto path=dir.filePath("model."+extension);
   auto* output=action(w,QString::fromUtf8("エクスポート..."));QVERIFY(output);
   bool configured=false,saved=false,importing=false,filtersValid=true;
   QTimer timer;connect(&timer,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets()) {
     if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible()) {
       const auto filters=file->nameFilters().join(';');
       filtersValid &= importing?(filters.contains("*.dxf")&&!filters.contains("*.stl")):(filters.contains("*.stl")&&!filters.contains("*.dxf"));
       saved=true;
       // All-supported import must dispatch from the actual extension.
       choose(file,dir.path(),importing?QFileInfo(path).fileName():QStringLiteral("model"),importing?QString():format);
     } else if(auto* dialog=qobject_cast<QDialog*>(widget);dialog&&dialog->isVisible()&&
               (dialog->windowTitle()==QString::fromUtf8("IGES エクスポート")||dialog->windowTitle()==QString::fromUtf8("BREP エクスポート"))) {
       for(auto* label:dialog->findChildren<QLabel*>())configured|=label->text().contains(QString::fromUtf8("形状のみ"));
       dialog->accept();
     }
   }});
   timer.start(10);output->trigger();timer.stop();
   if(format=="IGES"||format=="BREP")QVERIFY(configured);
   QVERIFY(filtersValid);QVERIFY(saved);QVERIFY2(w.lastError().isEmpty(),qPrintable(w.lastError()));QVERIFY(QFileInfo(path).size()>0);
   QCOMPARE(w.modeler().history().undoCount(),history);
   auto* input=action(w,QString::fromUtf8("インポート..."));QVERIFY(input);
   importing=true;saved=false;timer.start(10);input->trigger();timer.stop();
   QVERIFY(saved);QVERIFY2(w.lastError().isEmpty(),qPrintable(w.lastError()));QVERIFY(!w.selection().empty());
   double volume=0;
   for(const auto& id:w.selection()) {
     const auto* object=w.modeler().document().find(id);QCOMPARE(object->kind(),doc::ObjectKind::Solid);
     GProp_GProps props;BRepGProp::VolumeProperties(object->shape,props);volume+=props.Mass();
   }
   QVERIFY(std::abs(volume-6000)<1e-3);
   QCOMPARE(w.modeler().history().undoCount(),history+1);QVERIFY(w.modeler().history().undo());QCOMPARE(w.modeler().listObjects().size(),std::size_t(2));
 }
 void ifcExportFromMenuShowsWhatWasAssumed(){
   QTemporaryDir dir;
   QVERIFY(dir.isValid());
   app::MainWindow window;
   window.show();
   QVERIFY(QTest::qWaitForWindowExposed(&window));
   QVERIFY(window.modeler().createWalls({gp_Pnt(0,0,0),gp_Pnt(4000,0,0)},200,3000,0));
   auto* output=action(window,QStringLiteral("エクスポート..."));
   QVERIFY(output);
   bool offered=false;
   QString noticeTitle,notice;
   QTimer timer;
   connect(&timer,&QTimer::timeout,[&]{
     if(auto* dialog=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())){
       const auto filters=dialog->nameFilters();
       offered=std::any_of(filters.begin(),filters.end(),[](const QString& filter){
         return filter.startsWith(QStringLiteral("IFC"))&&filter.endsWith(QStringLiteral("(*.ifc)"));
       });
       if(offered)choose(dialog,dir.path(),QStringLiteral("家"),QStringLiteral("IFC"));
       else dialog->reject();
     } else if(auto* message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){
       noticeTitle=message->windowTitle();
       notice=message->text();
       message->accept();
     }
   });
   timer.start(10);
   output->trigger();
   timer.stop();
   QVERIFY(offered);
   QVERIFY2(window.lastError().isEmpty(),qPrintable(window.lastError()));
   QCOMPARE(noticeTitle,QStringLiteral("IFC の書き出し"));
   QVERIFY2(notice.contains(QStringLiteral("書き出しました"))&&notice.contains(QStringLiteral("壁 1"))&&
                notice.contains(QStringLiteral("レベルが無い")),qPrintable(notice));
   QFile file(dir.filePath(QStringLiteral("家.ifc")));
   QVERIFY(file.open(QIODevice::ReadOnly));
   const auto contents=file.readAll();
   QVERIFY(contents.contains("FILE_SCHEMA(('IFC4'));")&&contents.contains("IFCWALL("));
 }
 void sketchUpBoxRoundTripFromMenus() {
   std::string reason;
   if (!geom::skpAvailable(&reason)) QSKIP(reason.c_str());
   qInfo().noquote() << "SketchUp SDK:" << QString::fromStdString(geom::skpLibrary());
   QTemporaryDir dir;
   QVERIFY(dir.isValid());
   const QString name = QStringLiteral("検証用の箱");
   const QString path = dir.filePath(name + QStringLiteral(".skp"));

   auto runDialog = [&](QAction* entry, const QString& filename) {
     bool offered = false;
     QTimer timer;
     connect(&timer, &QTimer::timeout, [&] {
       if (auto* file = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
         const auto filters = file->nameFilters();
         offered = filters.contains(QStringLiteral("SketchUp (*.skp)"));
         if (offered) choose(file, dir.path(), filename, QStringLiteral("SketchUp"));
         else file->reject();
       } else if (auto* error = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
         error->reject();
       }
     });
     timer.start(10);
     entry->trigger();
     return offered;
   };
   {
     app::MainWindow source;
     source.show();
     QVERIFY(QTest::qWaitForWindowExposed(&source));
     QVERIFY(source.modeler().importShape(BRepPrimAPI_MakeBox(1000, 800, 700).Shape(), "Box", "test"));
     auto* output = action(source, QStringLiteral("エクスポート..."));
     QVERIFY(output);
     QVERIFY(runDialog(output, name));
     QVERIFY2(source.lastError().isEmpty(), qPrintable(source.lastError()));
     QVERIFY(QFileInfo(path).size() > 0);
   }
   app::MainWindow destination;
   destination.show();
   QVERIFY(QTest::qWaitForWindowExposed(&destination));
   auto* input = action(destination, QStringLiteral("インポート..."));
   QVERIFY(input);
   QVERIFY(runDialog(input, QFileInfo(path).fileName()));
   QVERIFY2(destination.lastError().isEmpty(), qPrintable(destination.lastError()));
   QCOMPARE(destination.selection().size(), std::size_t(1));
   const auto& document = destination.modeler().document();
   const auto* object = document.find(destination.selection().front());
   QVERIFY(object);
   QCOMPARE(object->kind(), doc::ObjectKind::Solid);
   const auto shape = document.worldShape(*object);
   int faces = 0;
   for (TopExp_Explorer face(shape, TopAbs_FACE); face.More(); face.Next()) ++faces;
   QCOMPARE(faces, 6);
   const auto bounds = geom::exactBounds(shape);
   QVERIFY(bounds.CornerMin().Distance(gp_Pnt(0, 0, 0)) < 1e-6);
   QVERIFY(bounds.CornerMax().Distance(gp_Pnt(1000, 800, 700)) < 1e-6);
 }
 void cancellingSketchUpSyncPreservesHistory_data() {
   QTest::addColumn<bool>("moveSurvivor");
   QTest::newRow("deletion-only") << false;
   QTest::newRow("move-and-deletion") << true;
 }
 void cancellingSketchUpSyncPreservesHistory() {
   QFETCH(bool, moveSurvivor);
   std::string reason;
   if (!geom::skpAvailable(&reason)) QSKIP(reason.c_str());
   QTemporaryDir dir;
   QVERIFY(dir.isValid());
   const auto path = dir.filePath(QStringLiteral("sync.skp")).toUtf8().toStdString();
   app::MainWindow window;
   window.show();
   QVERIFY(QTest::qWaitForWindowExposed(&window));
   auto& modeler = window.modeler();
   const auto first = modeler.importShape(BRepPrimAPI_MakeBox(100, 100, 100).Shape(), "First", "test");
   const auto second = modeler.importShape(BRepPrimAPI_MakeBox(gp_Pnt(300, 0, 0), 100, 100, 100).Shape(), "Second", "test");
   QVERIFY(first && second);
   QVERIFY(modeler.exportSkp(path));
   QVERIFY(geom::skpEditGroup(path, *second, nullptr, true));
   if (moveSurvivor) {
     gp_Trsf shift;
     shift.SetTranslation(gp_Vec(0, 500, 0));
     QVERIFY(geom::skpEditGroup(path, *first, &shift, false));
   }
   const auto redoObject = modeler.importShape(BRepPrimAPI_MakeBox(30, 40, 50).Shape(), "Redo", "test");
   QVERIFY(redoObject);
   auto& history = modeler.history();
   QVERIFY(history.undo());
   const auto revision = history.revision();
   const auto steps = history.undoCount();
   const auto redo = history.redoName();
   auto* sync = action(window, QStringLiteral("SketchUp で直した物を戻す..."));
   QVERIFY(sync);
   bool confirmed = false;
   QTimer timer;
   connect(&timer, &QTimer::timeout, [&] {
     if (auto* file = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
       choose(file, dir.path(), QStringLiteral("sync.skp"));
     } else if (auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
       if (auto* cancel = question->button(QMessageBox::Cancel)) {
         confirmed = true;
         cancel->click();
       } else {
         question->reject();
       }
     }
   });
   timer.start(10);
   sync->trigger();
   timer.stop();
   QVERIFY(confirmed);
   QCOMPARE(history.revision(), revision);
   QCOMPARE(history.undoCount(), steps);
   QCOMPARE(history.redoName(), redo);
   QVERIFY(modeler.document().find(*first));
   QVERIFY(modeler.document().find(*second));
   QVERIFY(history.redo());
   QVERIFY(modeler.document().find(*redoObject));
 }
 void invalidFileAndImportCancellationLeaveModelAlone_data(){exportAndImportFromMenus_data();}
 void invalidFileAndImportCancellationLeaveModelAlone(){
   QFETCH(QString,format);QFETCH(QString,extension);QTemporaryDir dir;app::MainWindow w;
   QVERIFY(w.modeler().importShape(BRepPrimAPI_MakeBox(1,2,3).Shape(),"Box","test"));
   const auto before=w.modeler().history().undoCount();const auto path=dir.filePath("bad."+extension);
   QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));f.write("not a geometry file");f.close();
   auto* input=action(w,QString::fromUtf8("インポート..."));QVERIFY(input);
   QTimer select;connect(&select,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets())
     if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible())choose(file,dir.path(),QFileInfo(path).fileName(),format);});
   select.start(10);input->trigger();select.stop();QVERIFY(!w.lastError().isEmpty());
   QCOMPARE(w.modeler().history().undoCount(),before);QCOMPARE(w.modeler().listObjects().size(),std::size_t(1));
   bool cancelled=false;QTimer timer;connect(&timer,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets())
     if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible()){cancelled=true;file->reject();}});
   timer.start(10);input->trigger();timer.stop();QVERIFY(cancelled);QCOMPARE(w.modeler().history().undoCount(),before);
 }
 void pngTypeNormalizesExtensionAndRejectingOverwritePreservesTheFile(){
   QTemporaryDir dir;app::MainWindow w;w.show();QVERIFY(QTest::qWaitForWindowExposed(&w));
   QVERIFY(w.modeler().importShape(BRepPrimAPI_MakeBox(10,20,30).Shape(),"Box","test"));
   auto* output=action(w,QString::fromUtf8("エクスポート..."));QVERIFY(output);
   QTimer timer;connect(&timer,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets())
     if(auto* file=qobject_cast<QFileDialog*>(widget);file&&file->isVisible())choose(file,dir.path(),"view.step","PNG");});
   timer.start(10);output->trigger();timer.stop();
   const auto png=dir.filePath("view.png");QVERIFY(!QImage(png).isNull());QVERIFY(!QFile::exists(dir.filePath("view.step")));
   QFile file(png);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=file.readAll();file.close();
   bool asked=false;QTimer reject;connect(&reject,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets())
     if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->isVisible()){asked=true;box->done(QMessageBox::No);}});
   timer.start(10);reject.start(10);output->trigger();timer.stop();reject.stop();QVERIFY(asked);
   QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),before);file.close();
   const auto history=w.modeler().history().undoCount();
   auto* input=action(w,QString::fromUtf8("インポート..."));QVERIFY(input);
   QTimer import;connect(&import,&QTimer::timeout,[&]{for(auto* widget:QApplication::topLevelWidgets())
     if(auto* dialog=qobject_cast<QFileDialog*>(widget);dialog&&dialog->isVisible())choose(dialog,dir.path(),"view.png");});
   import.start(10);input->trigger();import.stop();
   QVERIFY2(w.lastError().isEmpty(),qPrintable(w.lastError()));QCOMPARE(w.modeler().document().objects().back().kind(),doc::ObjectKind::Image);
   QCOMPARE(w.modeler().history().undoCount(),history+1);QVERIFY(w.modeler().history().undo());
 }
};
QTEST_MAIN(GeometryExchangeGui)
#include "test_geometry_exchange_gui.moc"
