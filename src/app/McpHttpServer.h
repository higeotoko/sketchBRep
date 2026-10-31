#pragma once
#include <QObject>
#include <QTcpServer>
#include <QSet>
#include <QUrl>
#include <memory>
#include "sbr/api/McpServer.h"
class QTcpSocket;
namespace sbr::app {
// Stateless Streamable HTTP MCP adapter. Only loopback is listened on;
// an explicitly configured HTTPS tunnel supplies access from ChatGPT.
class McpHttpServer : public QObject {
 public:
  explicit McpHttpServer(api::McpServer::Backend backend,QObject* parent=nullptr);
  ~McpHttpServer() override;
  bool start();
  void stop();
  bool isListening() const { return listener_.isListening(); }
  QUrl localUrl() const;
  QUrl publicUrl() const;
  bool setPublicBase(const QUrl& base);
  quint16 port() const { return listener_.serverPort(); }
  quint64 requestsHandled() const { return requestsHandled_; }
  QString errorString() const { return listener_.errorString(); }
 private:
  void acceptConnection();
  void dispatch(QTcpSocket* socket,const QByteArray& body,quint64 generation);
  void reply(QTcpSocket* socket,int status,const QByteArray& body={});
  bool allowedHost(const QByteArray& host) const;
  bool allowedOrigin(const QByteArray& origin) const;
  QTcpServer listener_;
  api::McpServer protocol_;
  QSet<QTcpSocket*> clients_;
  QByteArray secretPath_;
  QUrl publicBase_;
  quint64 generation_=0,requestsHandled_=0;
  bool executing_=false;
};
}
