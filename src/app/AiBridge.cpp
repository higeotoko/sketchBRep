#include "AiBridge.h"

#include <QLocalServer>
#include <QLocalSocket>

#include <nlohmann/json.hpp>

#include "sbr/api/McpServer.h"

namespace sbr::app {

AiBridge::AiBridge(doc::Modeler& modeler, std::vector<api::CommandApi::Extension> extensions, QObject* parent)
    : QObject(parent), api_(modeler, api::CommandApi::Options{/*allowFileCommands=*/false}) {
  for (auto& e : extensions) api_.addExtension(std::move(e));
}

AiBridge::~AiBridge() { setEnabled(false); }

void AiBridge::setHooks(std::function<void()> before, std::function<void()> after) {
  before_ = std::move(before);
  after_ = std::move(after);
}

QString AiBridge::serverName() const { return QString::fromStdString(api::bridgeServerName()); }

bool AiBridge::isEnabled() const { return server_ && server_->isListening(); }

bool AiBridge::setEnabled(bool enabled) {
  if (enabled == isEnabled()) return true;
  if (!enabled) {
    server_.reset();  // closes the server and its connections
    clients_ = 0;
    emit clientCountChanged(0);
    return true;
  }
  server_ = std::make_unique<QLocalServer>();
  server_->setSocketOptions(QLocalServer::UserAccessOption);
  QLocalServer::removeServer(serverName());  // stale socket file after a crash (Unix)
  if (!server_->listen(serverName())) {
    lastError_ = server_->errorString();
    server_.reset();
    return false;
  }
  connect(server_.get(), &QLocalServer::newConnection, this, &AiBridge::onConnection);
  return true;
}

void AiBridge::onConnection() {
  while (QLocalSocket* socket = server_->nextPendingConnection()) {
    emit clientCountChanged(++clients_);
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
      emit clientCountChanged(--clients_);
      socket->deleteLater();
    });
  }
}

void AiBridge::onReadyRead(QLocalSocket* socket) {
  while (socket->canReadLine()) {
    const QByteArray line = socket->readLine().trimmed();
    if (line.isEmpty()) continue;
    socket->write(QByteArray::fromStdString(execute(line.toStdString())) + '\n');
  }
  socket->flush();
}

std::string AiBridge::execute(const std::string& requestJson) {
  if (before_) before_();
  const std::string response = api_.execute(requestJson);
  if (after_) after_();
  const auto request = nlohmann::json::parse(requestJson, nullptr, false);
  const auto result = nlohmann::json::parse(response, nullptr, false);
  const QString command =
      request.is_object() ? QString::fromStdString(request.value("command", std::string())) : QString();
  const bool ok = result.is_object() && result.value("ok", false);
  const QString error = ok || !result.is_object() ? QString()
                                                  : QString::fromStdString(result.value("error", std::string()));
  emit commandExecuted(command, ok, error);
  return response;
}

}  // namespace sbr::app
