// sbr_mcp — MCP server (stdio) that lets an AI client operate the running
// SketchBRep app. The app must have "AI 連携 > 外部 AI からの操作を許可" on;
// requests then go through the same commands, validation and undo as the GUI.
//
//   sbr_mcp                         forward to the running app
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
#include <QLocalSocket>

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

// One request/response round trip over the app's local socket (one JSON per line).
class AppConnection {
 public:
  std::string send(const std::string& request) {
    if (socket_.state() != QLocalSocket::ConnectedState) {
      socket_.abort();
      socket_.connectToServer(QString::fromStdString(sbr::api::bridgeServerName()));
      if (!socket_.waitForConnected(2000)) {
        return failure("SketchBRep is not running, or AI access is off (menu: AI 連携 > 外部 AI からの操作を許可)");
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
  std::string openPath;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--headless") {
      headless = true;
    } else if (a == "--open" && i + 1 < argc) {
      openPath = argv[++i];
    } else {
      std::cerr << "usage: sbr_mcp [--headless [--open file.sbr]]\n";
      return 2;
    }
  }

  sbr::api::McpServer::Backend backend;
  std::unique_ptr<sbr::doc::Modeler> modeler;
  std::unique_ptr<sbr::api::CommandApi> api;
  std::unique_ptr<AppConnection> connection;
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
    connection = std::make_unique<AppConnection>();
    // The client usually lists tools once at start-up, possibly before the
    // app runs: answer "describe" from the shared schema in that case.
    static sbr::doc::Modeler schemaModel;
    static sbr::api::CommandApi offline = [] {
      sbr::api::CommandApi a(schemaModel, sbr::api::CommandApi::Options{false});
      for (auto& e : sbr::api::CommandApi::appCommandSpecs()) {
        e.run = [](const std::string&) -> std::string { throw std::runtime_error("SketchBRep is not running"); };
        a.addExtension(std::move(e));
      }
      return a;
    }();
    backend = [&connection](const std::string& r) {
      const std::string response = connection->send(r);
      const auto request = nlohmann::json::parse(r, nullptr, false);
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
