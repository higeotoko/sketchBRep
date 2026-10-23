#include <QApplication>
#include <QIcon>
#include <QByteArray>
#include <QStringList>

#include <QMessageBox>
#include <Standard_Failure.hxx>

#include "MainWindow.h"

namespace {

// Last line of defence: an exception escaping an event handler (a menu
// command, a dialog) is reported instead of ending the program. Tools have
// their own guard (Viewport::toolCall); autosave keeps the work.
class SafeApplication : public QApplication {
 public:
  using QApplication::QApplication;
  bool notify(QObject* receiver, QEvent* event) override {
    QString failure;
    try {
      return QApplication::notify(receiver, event);
    } catch (const Standard_Failure& e) {
      failure = QString::fromUtf8(e.GetMessageString());
    } catch (const std::exception& e) {
      failure = QString::fromUtf8(e.what());
    } catch (...) {
      failure = QStringLiteral("unknown error");
    }
    qWarning("SketchBRep: exception in an event handler: %s", qPrintable(failure));
    if (!reporting_) {
      reporting_ = true;
      QMessageBox::warning(nullptr, QStringLiteral("SketchBRep"),
                           QObject::tr("操作中に内部エラーが起きました（%1）。モデルはそのままです。"
                                       "念のため保存してください。").arg(failure));
      reporting_ = false;
    }
    return false;
  }

 private:
  bool reporting_ = false;
};

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32) && !defined(__APPLE__)
  // The OCCT viewer needs an X11 native window (XWayland is fine).
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "xcb");
#endif
  SafeApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("SketchBRep"));
  QApplication::setOrganizationName(QStringLiteral("SketchBRep"));
  QApplication::setApplicationVersion(QStringLiteral(SBR_VERSION));
  // Title bar, taskbar and Alt+Tab (assets/branding; the .exe icon itself
  // comes from the Windows resource).
  QIcon icon;
  icon.addFile(QStringLiteral(":/branding/app-icon-256.png"));
  icon.addFile(QStringLiteral(":/branding/app-icon-1024.png"));
  QApplication::setWindowIcon(icon);

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
