// sbr_mcp — MCP server (stdio) that lets an AI client operate the running
// SketchBRep app. The app must have "AI 連携 > 外部 AI からの操作を許可" on;
// requests then go through the same commands, validation and undo as the GUI.
//
//   sbr_mcp [--no-launch]           forward to the running app (when it is not running and a
//                                   tool is called, it is started with AI access on; docs/244)
//   sbr_mcp --headless [--open f]   no app: work on an in-memory model

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QThread>

#include <nlohmann/json.hpp>

#include "sbr/api/CommandApi.h"
#include "sbr/api/McpServer.h"
#include "sbr/doc/Modeler.h"

namespace {

// docs/243: the server ends when the client that started it ends, even when the end of its input
// never arrives (on Windows a handle to the pipe can stay open in another process, and the server
// then held its files: the folder could not be updated until it was killed by hand).
void endWithParent() {
#ifdef _WIN32
  DWORD parent = 0;
  const DWORD self = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    if (entry.th32ProcessID == self) parent = entry.th32ParentProcessID;
  CloseHandle(snapshot);
  if (parent == 0) return;
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent);
  if (!process) return;  // already gone, or not ours to watch: the end of the input still ends us
  std::thread([process] {
    WaitForSingleObject(process, INFINITE);
    std::_Exit(0);
  }).detach();
#else
  const pid_t parent = getppid();
  if (parent <= 1) return;
  std::thread([parent] {
    while (getppid() == parent) std::this_thread::sleep_for(std::chrono::seconds(2));
    std::_Exit(0);
  }).detach();
#endif
}

std::string failure(const std::string& message) {
  return nlohmann::json{{"ok", false}, {"error", message}}.dump();
}

// docs/244: whether a SketchBRep app is running (then it is not started again).
bool appRunning() {
#ifdef _WIN32
  bool found = false;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
    found = _wcsicmp(entry.szExeFile, L"SketchBRep.exe") == 0;
  CloseHandle(snapshot);
  return found;
#else
  return QProcess::execute(QStringLiteral("pgrep"), {QStringLiteral("-x"), QStringLiteral("SketchBRep")}) == 0;
#endif
}

// One request/response round trip over the app's local socket (one JSON per line).
class AppConnection {
 public:
  explicit AppConnection(bool launch) : launch_(launch) {}

  // `mayLaunch`: when the app is not running, start it (next to this program) with AI access on
  // and wait for it (docs/244). Not for the tool list the client asks for when it starts.
  std::string send(const std::string& request, bool mayLaunch) {
    if (socket_.state() != QLocalSocket::ConnectedState) {
      socket_.abort();
      socket_.connectToServer(QString::fromStdString(sbr::api::bridgeServerName()));
      if (!socket_.waitForConnected(2000)) {
        if (!mayLaunch || !launch_) {
          return failure("SketchBRep is not running, or AI access is off (menu: AI 連携 > 外部 AI からの操作を許可)");
        }
        if (appRunning()) {
          return failure("SketchBRep is running with AI access off: turn on AI 連携 > 外部 AI からの操作を許可 "
                         "(or close it, and the next request starts it with AI access on)");
        }
#ifdef _WIN32
        const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("SketchBRep.exe"));
#else
        const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("SketchBRep"));
#endif
        if (!QFileInfo::exists(exe)) return failure("SketchBRep is not running, and it is not next to sbr_mcp to start it");
        if (!QProcess::startDetached(exe, {QStringLiteral("--ai")}, QCoreApplication::applicationDirPath())) {
          return failure("SketchBRep could not be started");
        }
        QElapsedTimer waited;
        waited.start();
        bool connected = false;
        while (!connected && waited.elapsed() < 90000) {
          QThread::msleep(500);
          socket_.abort();
          socket_.connectToServer(QString::fromStdString(sbr::api::bridgeServerName()));
          connected = socket_.waitForConnected(1000);
        }
        if (!connected) return failure("SketchBRep was started but did not answer within 90 s");
      }
    }
    socket_.write(QByteArray::fromStdString(request) + '\n');
    socket_.flush();
    QByteArray line;
    while (!line.endsWith('\n')) {
      if (socket_.bytesAvailable() == 0 && !socket_.waitForReadyRead(120000)) {
        socket_.abort();
        return failure("no response from SketchBRep");
      }
      line += socket_.readLine();
    }
    return line.trimmed().toStdString();
  }

 private:
  QLocalSocket socket_;
  bool launch_ = true;
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  QCoreApplication app(argc, argv);
  endWithParent();
  bool headless = false;
  bool noLaunch = false;  // docs/244: do not start the app when it is not running
  std::string openPath;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--headless") {
      headless = true;
    } else if (a == "--open" && i + 1 < argc) {
      openPath = argv[++i];
    } else if (a == "--no-launch") {
      noLaunch = true;
    } else {
      std::cerr << "usage: sbr_mcp [--no-launch] | sbr_mcp --headless [--open file.sbr]\n";
      return 2;
    }
  }

  sbr::api::McpServer::Backend backend;
  std::unique_ptr<sbr::doc::Modeler> modeler;
  std::unique_ptr<sbr::api::CommandApi> api;
  std::unique_ptr<AppConnection> connection;
  if (noLaunch && headless) noLaunch = false;  // nothing to start
  if (headless) {
    modeler = std::make_unique<sbr::doc::Modeler>();
    if (!openPath.empty()) {
      if (const auto status = modeler->load(openPath); !status) {
        std::cerr << "cannot open " << openPath << ": " << status.message() << "\n";
        return 1;
      }
    }
    api = std::make_unique<sbr::api::CommandApi>(*modeler);
    backend = [&api](const std::string& r) { return api->execute(r); };
  } else {
    connection = std::make_unique<AppConnection>(!noLaunch);
    // The client usually lists tools once at start-up, possibly before the
    // app runs: answer "describe" from the shared schema in that case.
    static sbr::doc::Modeler schemaModel;
    static sbr::api::CommandApi offline = [] {
      sbr::api::CommandApi a(schemaModel);  // docs/242: the app allows the file commands to the AI
      for (auto& e : sbr::api::CommandApi::appCommandSpecs()) {
        e.run = [](const std::string&) -> std::string { throw std::runtime_error("SketchBRep is not running"); };
        a.addExtension(std::move(e));
      }
      return a;
    }();
    backend = [&connection](const std::string& r) {
      const auto request = nlohmann::json::parse(r, nullptr, false);
      const bool listing = request.value("command", std::string()) == "describe";
      const std::string response = connection->send(r, !listing);
      const auto parsed = nlohmann::json::parse(response, nullptr, false);
      if (request.value("command", std::string()) == "describe" && !parsed.value("ok", false)) {
        return offline.execute(r);
      }
      return response;
    };
  }

  sbr::api::McpServer server(backend, headless ? "sketchbrep-headless" : "sketchbrep");
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    if (const auto response = server.handle(line)) std::cout << *response << "\n" << std::flush;
  }
  return 0;
}
