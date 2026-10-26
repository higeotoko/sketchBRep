#include <QtTest/QtTest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QTimer>
#include <iostream>
#include "AiBridge.h"
#include "McpHttpServer.h"
using namespace sbr;
using json=nlohmann::json;
class ChatGptTransport : public QObject {
 Q_OBJECT
 QNetworkAccessManager network_;
 struct Response{int status=0;QByteArray body;};
 Response post(const QUrl& url,const json& body,const QList<QPair<QByteArray,QByteArray>>& headers={}) {
   QNetworkRequest request(url);request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
   request.setRawHeader("Accept","application/json, text/event-stream");
   for(const auto& h:headers)request.setRawHeader(h.first,h.second);
   auto* reply=network_.post(request,QByteArray::fromStdString(body.dump()));QSignalSpy finished(reply,&QNetworkReply::finished);
   if(!reply->isFinished()&&!finished.wait(10000)){reply->abort();reply->deleteLater();return {};}
   Response response{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),reply->readAll()};reply->deleteLater();return response;
 }
 json call(const QString& name,json args=json::object()) {
   return {{"jsonrpc","2.0"},{"id",1},{"method","tools/call"},{"params",{{"name",name.toStdString()},{"arguments",args}}}};
 }
 private slots:
 void realAppModelUsesMcpAndFileAccessRemainsBlocked(){
   doc::Modeler model;app::AiBridge bridge(model,{});QVERIFY(!bridge.startChatGpt());QVERIFY(bridge.setEnabled(true));QVERIFY(bridge.startChatGpt());
   auto* server=bridge.chatGptServer();const auto url=server->localUrl();QCOMPARE(url.host(),QString("127.0.0.1"));QVERIFY(url.path().size()>=69);
   auto initialized=post(url,{{"jsonrpc","2.0"},{"id",1},{"method","initialize"},{"params",{{"protocolVersion","2025-06-18"}}}});
   QCOMPARE(initialized.status,200);QCOMPARE(QString::fromStdString(json::parse(initialized.body.toStdString())["result"]["protocolVersion"]),QString("2025-06-18"));
   auto notification=post(url,{{"jsonrpc","2.0"},{"method","notifications/initialized"}});QCOMPARE(notification.status,202);QVERIFY(notification.body.isEmpty());
   auto list=post(url,{{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}});QCOMPARE(list.status,200);
   const auto tools=json::parse(list.body.toStdString())["result"]["tools"];QVERIFY(tools.size()>20);
   bool box=false;for(const auto& tool:tools){QVERIFY(tool["annotations"].contains("readOnlyHint"));QVERIFY(tool["annotations"].contains("destructiveHint"));QVERIFY(tool["annotations"].contains("openWorldHint"));box|=tool["name"]=="create_box";}QVERIFY(box);
   auto created=post(url,call("create_box",{{"origin",{0,0,0}},{"width",10},{"depth",20},{"height",30}}));
   QCOMPARE(created.status,200);QVERIFY(!json::parse(created.body.toStdString())["result"]["isError"].get<bool>());QCOMPARE(model.listObjects().size(),std::size_t(1));
   auto undone=post(url,call("undo"));QCOMPARE(undone.status,200);QVERIFY(model.listObjects().empty());
   QTemporaryDir output;const auto forbiddenPath=output.filePath("forbidden.step");
   auto forbidden=post(url,call("export_step",{{"path",forbiddenPath.toUtf8().toStdString()}}));QCOMPARE(forbidden.status,200);QVERIFY(json::parse(forbidden.body.toStdString())["result"]["isError"].get<bool>());
   QVERIFY(!QFile::exists(forbiddenPath));
   bridge.setEnabled(false);QVERIFY(!server->isListening());QVERIFY(server->localUrl().isEmpty());
 }
 void secretUrlHostOriginAndMethodsAreChecked(){
   doc::Modeler model;app::AiBridge bridge(model,{});QVERIFY(bridge.setEnabled(true));QVERIFY(bridge.startChatGpt());auto* server=bridge.chatGptServer();
   const auto url=server->localUrl();const json ping={{"jsonrpc","2.0"},{"id",1},{"method","ping"}};
   auto wrong=url;wrong.setPath("/mcp/not-the-secret");QCOMPARE(post(wrong,ping).status,404);
   QCOMPARE(post(url,ping,{{"Origin","https://untrusted.example"}}).status,403);
   QCOMPARE(post(url,ping,{{"Host","untrusted.example"}}).status,403);
   QVERIFY(!server->setPublicBase(QUrl("http://example.test")));QVERIFY(!server->setPublicBase(QUrl("https://example.test/path")));
   QVERIFY(!server->setPublicBase(QUrl("https://user:password@example.test")));QVERIFY(!server->setPublicBase(QUrl("https://example.test?token=x")));
   QVERIFY(server->setPublicBase(QUrl("https://tunnel.example")));QCOMPARE(server->publicUrl().path(),url.path());
   QCOMPARE(post(url,ping,{{"Host","tunnel.example"},{"Origin","https://tunnel.example"}}).status,200);
   QNetworkRequest request(url);auto* reply=network_.get(request);QSignalSpy done(reply,&QNetworkReply::finished);if(!reply->isFinished()){QVERIFY(done.wait(10000));}
   QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),405);QVERIFY(!reply->hasRawHeader("Access-Control-Allow-Origin"));reply->deleteLater();
   const auto path=url.path();bridge.stopChatGpt();QVERIFY(bridge.startChatGpt());QVERIFY(bridge.chatGptServer()->localUrl().path()!=path);QVERIFY(bridge.chatGptServer()->publicUrl().isEmpty());
   auto old=bridge.chatGptServer()->localUrl();old.setPath(path);QCOMPARE(post(old,ping).status,404);QVERIFY(model.listObjects().empty());
 }
 void malformedAndOversizedRequestsNeverRunTheBackend(){
   int calls=0;app::McpHttpServer server([&](const std::string&){++calls;return std::string("{}");});QVERIFY(server.start());
   QCOMPARE(post(server.localUrl(),json::array()).status,400);
   QCOMPARE(post(server.localUrl(),{{"jsonrpc","2.0"},{"id",1},{"method","tools/call"},{"params",json::array()}}).status,400);
   QCOMPARE(post(server.localUrl(),{{"jsonrpc","2.0"},{"id",1},{"method","ping"}},{{"MCP-Protocol-Version","invalid"}}).status,400);
   auto raw=[&](const QByteArray& headers){
     QTcpSocket socket;socket.connectToHost(QHostAddress::LocalHost,server.port());QSignalSpy connected(&socket,&QTcpSocket::connected);
     if(socket.state()!=QAbstractSocket::ConnectedState&&!connected.wait(5000))return QByteArray();
     const QByteArray request="POST "+server.localUrl().path().toLatin1()+" HTTP/1.1\r\nHost: 127.0.0.1:"+QByteArray::number(server.port())+"\r\n"+headers+"\r\n";
     socket.write(request);QByteArray result;QSignalSpy ready(&socket,&QTcpSocket::readyRead);
     if(socket.bytesAvailable()==0){ready.wait(5000);}result+=socket.readAll();return result;
   };
   QVERIFY(raw("Content-Length: 999999999\r\nContent-Type: application/json\r\n").startsWith("HTTP/1.1 413"));
   QVERIFY(raw("Content-Length: 2\r\nContent-Length: 2\r\nContent-Type: application/json\r\n").startsWith("HTTP/1.1 400"));
   QVERIFY(raw("Content-Length: 2\r\nContent-Type: text/plain\r\n").startsWith("HTTP/1.1 415"));
   QVERIFY(raw("Transfer-Encoding: chunked\r\nContent-Length: 2\r\nContent-Type: application/json\r\n").startsWith("HTTP/1.1 400"));
   QCOMPARE(calls,0);
 }
 void fragmentedChunkedJsonAndContinueWork(){
   doc::Modeler model;app::AiBridge bridge(model,{});QVERIFY(bridge.setEnabled(true));QVERIFY(bridge.startChatGpt());auto* server=bridge.chatGptServer();
   const auto body=QByteArray::fromStdString(call("create_box",{{"origin",{0,0,0}},{"width",10},{"depth",20},{"height",30}}).dump());
   QTcpSocket socket;socket.connectToHost(QHostAddress::LocalHost,server->port());QSignalSpy connected(&socket,&QTcpSocket::connected);if(socket.state()!=QAbstractSocket::ConnectedState){QVERIFY(connected.wait(5000));}
   socket.write("POST "+server->localUrl().path().toLatin1()+" HTTP/1.1\r\nHost: 127.0.0.1:"+QByteArray::number(server->port())+"\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n");
   QSignalSpy ready(&socket,&QTcpSocket::readyRead);if(socket.bytesAvailable()==0){QVERIFY(ready.wait(5000));}QVERIFY(socket.readAll().startsWith("HTTP/1.1 100 Continue"));
   const auto first=body.left(17),second=body.mid(17);
   socket.write(QByteArray::number(first.size(),16)+"\r\n"+first+"\r\n");QTest::qWait(10);QVERIFY(model.listObjects().empty());
   socket.write(QByteArray::number(second.size(),16)+"\r\n"+second+"\r\n0\r\n\r\n");
   if(socket.bytesAvailable()==0){QVERIFY(ready.wait(5000));}QVERIFY(socket.readAll().startsWith("HTTP/1.1 200"));QCOMPARE(model.listObjects().size(),std::size_t(1));
 }
 void capturedImageUsesInlineContentWithUnicodeTemporaryPath(){
   QTemporaryDir dir;const auto path=dir.filePath(QString::fromUtf8("画像.png"));QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("PNGfake");file.close();
   api::CommandApi::Extension capture{"capture_view","Test view",R"({"type":"object"})",[&](const std::string&){return json{{"image_path",path.toUtf8().toStdString()}}.dump();},api::CommandApi::Effect::Read};
   doc::Modeler model;app::AiBridge bridge(model,{capture});QVERIFY(bridge.setEnabled(true));QVERIFY(bridge.startChatGpt());
   const auto response=post(bridge.chatGptServer()->localUrl(),call("capture_view"));QCOMPARE(response.status,200);
   const auto content=json::parse(response.body.toStdString())["result"]["content"];QCOMPARE(content.size(),std::size_t(2));
   QVERIFY(content[1]["type"]=="image");QVERIFY(content[1]["mimeType"]=="image/png");QVERIFY(content[1]["data"]=="UE5HZmFrZQ==");
 }
 void incompleteRequestCannotExecuteAfterDisable(){
   int calls=0;app::McpHttpServer server([&](const std::string&){++calls;return std::string("{}");});QVERIFY(server.start());
   QTcpSocket socket;socket.connectToHost(QHostAddress::LocalHost,server.port());QSignalSpy connected(&socket,&QTcpSocket::connected);if(socket.state()!=QAbstractSocket::ConnectedState){QVERIFY(connected.wait(5000));}
   socket.write("POST "+server.localUrl().path().toLatin1()+" HTTP/1.1\r\nHost: 127.0.0.1:"+QByteArray::number(server.port())+"\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{");
   QTest::qWait(20);server.stop();QTest::qWait(20);QCOMPARE(calls,0);
 }
};
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv);
  // Optional harness for an independent official MCP SDK client. It is a
  // temporary in-memory model, never the user's application or files.
  if(app.arguments().contains("--sdk-smoke")) {
    doc::Modeler model;api::CommandApi api(model,api::CommandApi::Options{false});
    sbr::app::McpHttpServer server([&](const std::string& request){return api.execute(request);});
    if(!server.start())return 1;
    std::cout<<server.localUrl().toString().toStdString()<<std::endl;
    QTimer::singleShot(60000,&app,&QCoreApplication::quit);return app.exec();
  }
  ChatGptTransport test;return QTest::qExec(&test,argc,argv);
}
#include "test_chatgpt_transport.moc"
