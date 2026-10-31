#include "AiBridge.h"
#include "McpHttpServer.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>

#include <nlohmann/json.hpp>

#include "ProgressDialog.h"
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
  if(!enabled)stopChatGpt();
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

bool AiBridge::startChatGpt() {
  if(!isEnabled()){lastError_=QStringLiteral("Enable AI access first");return false;}
  if(!http_)http_=std::make_unique<McpHttpServer>([this](const std::string& request){return execute(request);});
  if(!http_->start()){lastError_=http_->errorString();return false;}
  return true;
}
void AiBridge::stopChatGpt(){if(http_)http_->stop();}

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
  if (GuiProgress::busy()) {
    // A long operation is running and handing events out: requests wait
    // in the socket, in order, until it is over (P0-04).
    if (!retryPending_.contains(socket)) {
      retryPending_.insert(socket);
      QPointer<QLocalSocket> guard(socket);
      QTimer::singleShot(kBusyRetryMs, this, [this, guard] {
        if (!guard) return;
        retryPending_.remove(guard.data());
        onReadyRead(guard.data());
      });
    }
    return;
  }
  // One request per turn of the event loop: a client sending many at once
  // does not hold the window (the user's Esc, painting) until all are done.
  // A request without end (no newline in 64 MB) is refused and the client
  // let go; a client not reading its answers waits until it does.
  constexpr qint64 kMaxRequestBytes = 64 << 20;
  constexpr qint64 kMaxUnreadAnswerBytes = 64 << 20;
  auto again = [this, socket](int ms) {
    if (retryPending_.contains(socket)) return;
    retryPending_.insert(socket);
    QPointer<QLocalSocket> guard(socket);
    QTimer::singleShot(ms, this, [this, guard] {
      if (!guard) return;
      retryPending_.remove(guard.data());
      onReadyRead(guard.data());
    });
  };
  if (!socket->canReadLine()) {
    if (socket->bytesAvailable() > kMaxRequestBytes) {
      socket->write(nlohmann::json{{"ok", false}, {"error", "request too large (no end of line in 64 MB)"}}.dump().c_str());
      socket->write("\n");
      socket->flush();
      socket->disconnectFromServer();
    }
    return;
  }
  if (socket->bytesToWrite() > kMaxUnreadAnswerBytes) {
    again(50);
    return;
  }
  const QByteArray line = socket->readLine().trimmed();
  if (!line.isEmpty()) socket->write(QByteArray::fromStdString(execute(line.toStdString())) + '\n');
  socket->flush();
  if (socket->canReadLine()) again(0);
}

std::string AiBridge::execute(const std::string& requestJson) {
  // Only requests that change the model (or files) interrupt what the user
  // is drawing; reads, preview and view changes leave the tool's state alone
  // (review C).
  using Effect = api::CommandApi::Effect;
  const Effect effect = api_.effectOf(requestJson);
  const bool interrupts = effect == Effect::ModelWrite || effect == Effect::ExternalIo;
  if (interrupts && GuiProgress::busy()) {
    // Never inside another operation (the socket path waits instead).
    const std::string busy = nlohmann::json{
        {"ok", false},
        {"error", "The model is busy with a long operation; send the request again when it is over"}}.dump();
    emit commandExecuted(QString(), false, QStringLiteral("busy"));
    return busy;
  }
  if (interrupts && before_) before_();
  const std::string response = api_.execute(requestJson);
  if (interrupts && after_) after_();
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
