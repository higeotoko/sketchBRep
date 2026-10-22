#include <QApplication>
#include <QByteArray>
#include <QStringList>

#include "MainWindow.h"

int main(int argc, char** argv) {
#if !defined(_WIN32) && !defined(__APPLE__)
  // The OCCT viewer needs an X11 native window (XWayland is fine).
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "xcb");
#endif
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("SketchBRep"));
  QApplication::setOrganizationName(QStringLiteral("SketchBRep"));
  QApplication::setApplicationVersion(QStringLiteral(SBR_VERSION));

  sbr::app::MainWindow window;
  window.resize(1400, 900);
  window.show();

  const QStringList args = QApplication::arguments();
  if (args.size() > 1) {
    window.openFile(args.at(1));
  } else {
    window.offerRecovery();  // work left by a session that did not end normally
  }
  return QApplication::exec();
}
