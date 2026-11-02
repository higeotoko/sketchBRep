#include "CrashLog.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSysInfo>

#include <Standard_Version.hxx>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sbr::app {
namespace {

// Fixed buffers: written from a crash handler, where nothing may allocate.
constexpr int kLines = 64;
constexpr int kLineBytes = 200;
char g_lines[kLines][kLineBytes];
std::atomic<unsigned> g_next{0};
char g_path[1024] = {};
char g_header[1024] = {};
QString g_directory;
QtMessageHandler g_previousHandler = nullptr;
std::atomic<bool> g_written{false};  // one report per crash (terminate -> abort -> signal)

void writeAll(const char* text, std::size_t n);

#ifdef _WIN32
HANDLE g_file = INVALID_HANDLE_VALUE;
void openReport() {
  wchar_t wide[1024];
  MultiByteToWideChar(CP_UTF8, 0, g_path, -1, wide, 1024);
  g_file = CreateFileW(wide, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void writeAll(const char* text, std::size_t n) {
  if (g_file == INVALID_HANDLE_VALUE) return;
  DWORD done = 0;
  WriteFile(g_file, text, static_cast<DWORD>(n), &done, nullptr);
}
void closeReport() {
  if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
  g_file = INVALID_HANDLE_VALUE;
}
#else
int g_fd = -1;
void openReport() { g_fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
void writeAll(const char* text, std::size_t n) {
  while (g_fd >= 0 && n > 0) {
    const ssize_t w = ::write(g_fd, text, n);
    if (w <= 0) return;
    text += w;
    n -= static_cast<std::size_t>(w);
  }
}
void closeReport() {
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
}
#endif

void writeText(const char* s) { writeAll(s, std::strlen(s)); }

// The whole report; only fixed buffers and plain writes.
void writeReport(const char* reason, const char* detail) {
  if (!g_path[0] || g_written.exchange(true)) return;
  openReport();
  writeText(g_header);
  writeText("Failure: ");
  writeText(reason);
  writeText("\n");
  if (detail && detail[0]) {
    writeText(detail);
    writeText("\n");
  }
  writeText("\nLast actions (oldest first):\n");
  const unsigned next = g_next.load();
  const unsigned first = next > kLines ? next - kLines : 0;
  for (unsigned i = first; i < next; ++i) {
    writeText("  ");
    writeText(g_lines[i % kLines]);
    writeText("\n");
  }
  closeReport();
}

#ifdef _WIN32
LONG WINAPI onUnhandled(EXCEPTION_POINTERS* info) {
  char detail[512] = {};
  if (info && info->ExceptionRecord) {
    const void* address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
      GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* base = std::strrchr(name, '\\');
    std::snprintf(detail, sizeof detail, "Exception 0x%08lX at %s+0x%llX",
                  static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), base ? base + 1 : name,
                  static_cast<unsigned long long>(reinterpret_cast<const char*>(address) -
                                                  reinterpret_cast<const char*>(module)));
  }
  writeReport("the program stopped (Windows exception)", detail);
  return EXCEPTION_EXECUTE_HANDLER;
}
#else
void onSignal(int sig) {
  const char* name = sig == SIGSEGV ? "SIGSEGV (memory access)" : sig == SIGABRT ? "SIGABRT (abort)"
                     : sig == SIGFPE  ? "SIGFPE (arithmetic)"   : sig == SIGILL  ? "SIGILL"
                                                                                 : "signal";
  writeReport(name, nullptr);
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}
#endif

void onTerminate() {
  const char* what = "unknown";
  try {
    if (auto e = std::current_exception()) std::rethrow_exception(e);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
  }
  writeReport("an error nobody caught (std::terminate)", what);
  std::abort();
}

void onMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
    noteActivity((type == QtWarningMsg ? QStringLiteral("warning: ") : QStringLiteral("error: ")) + message.left(150));
  }
  if (type == QtFatalMsg) writeReport("fatal error", message.toUtf8().constData());
  if (g_previousHandler) g_previousHandler(type, context, message);
}

}  // namespace

void installCrashLog(const QString& directory) {
  g_directory = directory;
  QDir().mkpath(directory);
  // Keep the newest reports only.
  const QFileInfoList old = QDir(directory).entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time);
  for (int i = 20; i < old.size(); ++i) QFile::remove(old.at(i).absoluteFilePath());
  const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
  const QByteArray path = QDir(directory).filePath(QStringLiteral("crash-%1.txt").arg(stamp)).toUtf8();
  std::snprintf(g_path, sizeof g_path, "%s", path.constData());
  const QByteArray header =
      QStringLiteral("SketchBRep %1 crash report\nStarted: %2\nSystem: %3 (%4)\nQt %5, OCCT %6\n\n")
          .arg(QCoreApplication::applicationVersion(), QDateTime::currentDateTime().toString(Qt::ISODate),
               QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), QString::fromLatin1(qVersion()),
               QStringLiteral(OCC_VERSION_COMPLETE))
          .toUtf8();
  std::snprintf(g_header, sizeof g_header, "%s", header.constData());
  std::set_terminate(onTerminate);
#ifdef _WIN32
  SetUnhandledExceptionFilter(onUnhandled);
#else
  for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) std::signal(sig, onSignal);
#endif
  g_previousHandler = qInstallMessageHandler(onMessage);
  noteActivity(QStringLiteral("started"));
}

QString crashLogDirectory() { return g_directory; }

void noteActivity(const QString& what) {
  const QByteArray line =
      (QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + what).toUtf8().left(kLineBytes - 1);
  const unsigned i = g_next.fetch_add(1);
  char* slot = g_lines[i % kLines];
  std::memcpy(slot, line.constData(), static_cast<std::size_t>(line.size()));
  slot[line.size()] = '\0';
}

QStringList recentActivity() {
  QStringList out;
  const unsigned next = g_next.load();
  const unsigned first = next > kLines ? next - kLines : 0;
  for (unsigned i = first; i < next; ++i) out << QString::fromUtf8(g_lines[i % kLines]);
  return out;
}

QStringList takeNewCrashReports() {
  if (g_directory.isEmpty()) return {};
  QSettings settings;
  const QDateTime seen = settings.value("crash/seen").toDateTime();
  QStringList out;
  const QString current = QString::fromUtf8(g_path);
  for (const QFileInfo& f : QDir(g_directory).entryInfoList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Time)) {
    if (f.absoluteFilePath() == QFileInfo(current).absoluteFilePath()) continue;
    if (!seen.isValid() || f.lastModified() > seen) out << f.absoluteFilePath();
  }
  settings.setValue("crash/seen", QDateTime::currentDateTime());
  return out;
}

QString writeCrashReportForTest(const char* reason) {
  g_written = false;
  writeReport(reason, "test");
  g_written = false;
  return QString::fromUtf8(g_path);
}

}  // namespace sbr::app
