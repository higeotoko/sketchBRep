#pragma once
// Bridge for external AI agents (local sbr_mcp and optional ChatGPT HTTP).
//
// Off by default. When enabled, a QLocalServer (named pipe on Windows,
// user-only access) accepts one CommandApi request per line and executes it
// on the GUI thread through the same doc::Modeler as the tools, so every AI
// edit is validated, shown immediately and undoable with Ctrl+Z.

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QSet>
#include <QString>

#include "sbr/api/CommandApi.h"

class QLocalServer;
class QLocalSocket;

namespace sbr::app {

class McpHttpServer;

class AiBridge : public QObject {
  Q_OBJECT
 public:
  AiBridge(doc::Modeler& modeler, std::vector<api::CommandApi::Extension> extensions,
           QObject* parent = nullptr);
  ~AiBridge() override;

  // Called around each request that writes the model or files (e.g. to
  // cancel an unfinished drawing tool); reads and view changes skip them.
  void setHooks(std::function<void()> before, std::function<void()> after);

  bool setEnabled(bool enabled);  // false if the server could not listen
  bool isEnabled() const;
  QString serverName() const;
  QString lastError() const { return lastError_; }
  bool startChatGpt();
  void stopChatGpt();
  McpHttpServer* chatGptServer() const { return http_.get(); }

  // Same entry point as a socket request (used by tests).
  std::string execute(const std::string& requestJson);

 signals:
  void commandExecuted(const QString& command, bool ok, const QString& error);
  void clientCountChanged(int count);

 private:
  void onConnection();
  void onReadyRead(QLocalSocket* socket);

  api::CommandApi api_;
  std::unique_ptr<McpHttpServer> http_;
  std::unique_ptr<QLocalServer> server_;
  std::function<void()> before_, after_;
  static constexpr int kBusyRetryMs = 100;
  QSet<QLocalSocket*> retryPending_;  // sockets waiting for a long operation
  int clients_ = 0;
  QString lastError_;
};

}  // namespace sbr::app
