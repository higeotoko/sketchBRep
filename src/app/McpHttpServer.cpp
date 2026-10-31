#include "McpHttpServer.h"
#include "ProgressDialog.h"
#include <QHostAddress>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QMap>
#include <algorithm>
#include <QVariant>
#include <nlohmann/json.hpp>

namespace sbr::app {
namespace {
constexpr qsizetype maxHeaders=32768,maxBody=2*1024*1024;
struct Incoming { QByteArray data;bool complete=false,continued=false; };
// Decode an HTTP/1.1 chunked body without interpreting trailers as request
// headers. A second request after the terminating chunk is never accepted.
enum class Chunks { Incomplete, Complete, Invalid, TooLarge };
Chunks decodeChunks(const QByteArray& data,QByteArray& body,qsizetype& consumed) {
  qsizetype pos=0;
  while(true) {
    const auto end=data.indexOf("\r\n",pos);
    if(end<0)return data.size()-pos>1024?Chunks::Invalid:Chunks::Incomplete;
    if(end-pos>1024)return Chunks::Invalid;
    const auto sizeText=data.mid(pos,end-pos).split(';').front();
    if(sizeText.isEmpty()||std::any_of(sizeText.begin(),sizeText.end(),[](char c){return !((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'));}))return Chunks::Invalid;
    bool ok=false;const auto size=sizeText.toULongLong(&ok,16);if(!ok)return Chunks::Invalid;
    if(size>static_cast<qulonglong>(maxBody-body.size()))return Chunks::TooLarge;
    pos=end+2;
    if(size==0) {
      if(data.size()<pos+2)return Chunks::Incomplete;
      if(data.mid(pos,2)=="\r\n"){consumed=pos+2;return Chunks::Complete;}
      const auto trailersEnd=data.indexOf("\r\n\r\n",pos);
      if(trailersEnd<0)return data.size()-pos>8192?Chunks::Invalid:Chunks::Incomplete;
      if(trailersEnd-pos>8192)return Chunks::Invalid;
      for(auto line:data.mid(pos,trailersEnd-pos).split('\n')) {
        line=line.trimmed();const auto colon=line.indexOf(':');if(colon<=0)return Chunks::Invalid;
        const auto key=line.left(colon).toLower();
        if(key=="host"||key=="origin"||key=="content-length"||key=="transfer-encoding"||key=="content-type"||key=="authorization")return Chunks::Invalid;
      }
      consumed=trailersEnd+4;return Chunks::Complete;
    }
    const auto count=static_cast<qsizetype>(size);
    if(data.size()-pos<count+2)return Chunks::Incomplete;
    if(data.mid(pos+count,2)!="\r\n")return Chunks::Invalid;
    body+=data.mid(pos,count);pos+=count+2;
  }
}
bool samePath(const QByteArray& a,const QByteArray& b) {
  if(a.size()!=b.size())return false;
  unsigned mismatch=0;for(qsizetype i=0;i<a.size();++i)mismatch|=static_cast<unsigned char>(a[i]^b[i]);
  return mismatch==0;
}
}
McpHttpServer::McpHttpServer(api::McpServer::Backend backend,QObject* parent)
    :QObject(parent),protocol_(std::move(backend),"sketchbrep-chatgpt") {
  connect(&listener_,&QTcpServer::newConnection,this,[this]{acceptConnection();});
}
McpHttpServer::~McpHttpServer(){stop();}
bool McpHttpServer::start() {
  if(isListening())return true;
  if(!listener_.listen(QHostAddress::LocalHost,0))return false;
  QByteArray token;token.reserve(64);
  for(int i=0;i<8;++i)token+=QByteArray::number(QRandomGenerator::system()->generate(),16).rightJustified(8,'0');
  secretPath_="/mcp/"+token;publicBase_=QUrl();requestsHandled_=0;++generation_;return true;
}
void McpHttpServer::stop() {
  ++generation_;listener_.close();secretPath_.clear();publicBase_=QUrl();
  const auto clients=clients_.values();clients_.clear();for(auto* socket:clients){socket->abort();socket->deleteLater();}
}
QUrl McpHttpServer::localUrl() const {
  return isListening()?QUrl(QString("http://127.0.0.1:%1%2").arg(port()).arg(QString::fromLatin1(secretPath_))):QUrl();
}
QUrl McpHttpServer::publicUrl() const {
  if(!isListening()||publicBase_.isEmpty())return {};
  auto url=publicBase_;url.setPath(QString::fromLatin1(secretPath_));return url;
}
bool McpHttpServer::setPublicBase(const QUrl& base) {
  const QHostAddress address(base.host());
  if(base.host().compare("localhost",Qt::CaseInsensitive)==0||base.host().endsWith(".localhost",Qt::CaseInsensitive)||address.isLoopback())return false;
  if(!base.isValid()||base.scheme()!="https"||base.host().isEmpty()||!base.userInfo().isEmpty()||
     base.hasQuery()||base.hasFragment()||(!base.path().isEmpty()&&base.path()!="/")||base.port(443)!=443)return false;
  publicBase_=base;publicBase_.setPath(QString());return true;
}
bool McpHttpServer::allowedHost(const QByteArray& raw) const {
  const auto host=raw.trimmed().toLower();const auto suffix=":"+QByteArray::number(port());
  if(host=="127.0.0.1"+suffix||host=="localhost"+suffix)return true;
  if(publicBase_.isEmpty())return false;
  const auto allowed=QUrl::toAce(publicBase_.host()).toLower();return host==allowed||host==allowed+":443";
}
bool McpHttpServer::allowedOrigin(const QByteArray& raw) const {
  if(raw.isEmpty())return true; // server-to-server MCP requests normally omit Origin
  const auto origin=QUrl::fromEncoded(raw);
  if(!origin.isValid()||origin.hasQuery()||origin.hasFragment()||!origin.userInfo().isEmpty()||
     (!origin.path().isEmpty()&&origin.path()!="/"))return false;
  if(origin.scheme()=="http"&&origin.port()==port()&&(origin.host()=="127.0.0.1"||origin.host()=="localhost"))return true;
  if(origin.scheme()!="https"||origin.port(443)!=443)return false;
  return origin.host()=="chatgpt.com"||origin.host()=="chat.openai.com"||
         (!publicBase_.isEmpty()&&origin.host()==publicBase_.host());
}
void McpHttpServer::reply(QTcpSocket* socket,int status,const QByteArray& body) {
  if(!clients_.contains(socket)||socket->property("replied").toBool())return;
  if(body.size()>(64<<20)){reply(socket,500,"{\"error\":\"response too large\"}");return;}
  socket->setProperty("replied",true);
  QByteArray reason="Error";
  switch(status){case 200:reason="OK";break;case 202:reason="Accepted";break;case 400:reason="Bad Request";break;
    case 403:reason="Forbidden";break;case 404:reason="Not Found";break;case 405:reason="Method Not Allowed";break;
    case 408:reason="Request Timeout";break;case 411:reason="Length Required";break;case 413:reason="Content Too Large";break;
    case 415:reason="Unsupported Media Type";break;case 417:reason="Expectation Failed";break;case 500:reason="Internal Server Error";break;
    case 501:reason="Not Implemented";break;case 503:reason="Service Unavailable";break;case 504:reason="Gateway Timeout";break;}
  QByteArray response="HTTP/1.1 "+QByteArray::number(status)+" "+reason+"\r\nContent-Length: "+QByteArray::number(body.size())+
      "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
  if(status==405)response+="Allow: POST\r\n";
  socket->write(response+"\r\n"+body);socket->disconnectFromHost();
  QPointer<QTcpSocket> guard(socket);QTimer::singleShot(10000,this,[guard]{if(guard)guard->abort();});
}
void McpHttpServer::acceptConnection() {
  while(auto* socket=listener_.nextPendingConnection()) {
    if(!isListening()){socket->abort();socket->deleteLater();continue;}
    socket->setReadBufferSize(maxHeaders+maxBody+1);
    connect(socket,&QTcpSocket::disconnected,this,[this,socket]{clients_.remove(socket);socket->deleteLater();});
    if(clients_.size()>=16){clients_.insert(socket);reply(socket,503);continue;}
    clients_.insert(socket);const auto generation=generation_;
    auto input=std::make_shared<Incoming>();QPointer<QTcpSocket> guard(socket);
    QTimer::singleShot(10000,this,[this,guard,input,generation]{if(guard&&generation==generation_&&!input->complete)reply(guard,408);});
    auto receive=[this,socket,input,generation]{
      if(input->complete||!clients_.contains(socket)||generation!=generation_)return;
      input->data+=socket->readAll();
      if(input->data.size()>maxHeaders+maxBody){reply(socket,413);return;}
      const auto end=input->data.indexOf("\r\n\r\n");
      if(end<0){if(input->data.size()>maxHeaders)reply(socket,413);return;}
      if(end>maxHeaders){reply(socket,413);return;}
      const auto lines=input->data.left(end).split('\n');const auto first=lines.front().trimmed().split(' ');
      if(first.size()!=3||first[2]!="HTTP/1.1"){reply(socket,400);return;}
      QMap<QByteArray,QByteArray> headers;
      for(qsizetype i=1;i<lines.size();++i) {
        const auto line=lines[i].trimmed();const auto colon=line.indexOf(':');
        if(colon<=0){reply(socket,400);return;}
        const auto name=line.left(colon).toLower();
        if(headers.contains(name)){reply(socket,400);return;}
        headers.insert(name,line.mid(colon+1).trimmed());
      }
      if(!allowedHost(headers.value("host"))||!allowedOrigin(headers.value("origin"))){reply(socket,403);return;}
      if(!samePath(first[1],secretPath_)){reply(socket,404);return;}
      if(first[0]!="POST"){reply(socket,405);return;} // optional SSE GET is deliberately not offered
      const bool chunked=headers.contains("transfer-encoding");
      if(chunked&&(headers.value("transfer-encoding").toLower()!="chunked"||headers.contains("content-length"))){reply(socket,400);return;}
      if(!chunked&&!headers.contains("content-length")){reply(socket,411);return;}
      qlonglong length=0;
      if(!chunked) {
        bool ok=false;length=headers["content-length"].toLongLong(&ok);const auto digits=headers["content-length"];
        if(!ok||length<0||digits.isEmpty()||std::any_of(digits.begin(),digits.end(),[](char c){return c<'0'||c>'9';})){reply(socket,400);return;}
        if(length>maxBody){reply(socket,413);return;}
      }
      if(headers.value("content-type").split(';').front().trimmed().toLower()!="application/json"){reply(socket,415);return;}
      const auto version=headers.value("mcp-protocol-version");
      if(!version.isEmpty()&&version!="2025-06-18"&&version!="2025-03-26"&&version!="2024-11-05"){reply(socket,400);return;}
      const auto expect=headers.value("expect").toLower();
      if(!expect.isEmpty()&&expect!="100-continue"){reply(socket,417);return;}
      QByteArray body;bool complete=false;
      if(chunked) {
        qsizetype consumed=0;const auto encoded=input->data.mid(end+4);const auto state=decodeChunks(encoded,body,consumed);
        if(state==Chunks::Invalid){reply(socket,400);return;}if(state==Chunks::TooLarge){reply(socket,413);return;}
        complete=state==Chunks::Complete;
        if(complete&&consumed!=encoded.size()){reply(socket,400);return;}
      } else {
        const qsizetype expected=end+4+static_cast<qsizetype>(length);
        if(input->data.size()>expected){reply(socket,400);return;}
        complete=input->data.size()==expected;if(complete)body=input->data.mid(end+4);
      }
      if(!complete){
        if(expect=="100-continue"&&!input->continued){socket->write("HTTP/1.1 100 Continue\r\n\r\n");input->continued=true;}
        return;
      }
      input->complete=true;input->data.clear();
      QPointer<QTcpSocket> guard(socket);
      QTimer::singleShot(120000,this,[this,guard,generation]{if(guard&&generation==generation_)reply(guard,504);});
      dispatch(socket,body,generation);
    };
    connect(socket,&QTcpSocket::readyRead,this,receive);QTimer::singleShot(0,socket,receive);
  }
}
void McpHttpServer::dispatch(QTcpSocket* socket,const QByteArray& body,quint64 generation) {
  if(generation!=generation_||!isListening()||!clients_.contains(socket)||socket->property("replied").toBool())return;
  if(executing_||GuiProgress::busy()) {
    QPointer<QTcpSocket> guard(socket);QTimer::singleShot(100,this,[this,guard,body,generation]{if(guard)dispatch(guard,body,generation);});return;
  }
  const auto request=nlohmann::json::parse(body.constData(),body.constData()+body.size(),nullptr,false);
  if(!request.is_object()||request.value("jsonrpc",nlohmann::json())!="2.0"||
     !request.contains("method")||!request["method"].is_string()||
     (request.contains("params")&&!request["params"].is_object())||
     (request.contains("id")&&!request["id"].is_string()&&!request["id"].is_number_integer())){reply(socket,400);return;}
  ++requestsHandled_;executing_=true;
  struct Reset{bool& value;~Reset(){value=false;}}reset{executing_};
  try {
    auto response=protocol_.handle(body.toStdString());
    if(response&&request["method"]=="initialize") {
      auto initialized=nlohmann::json::parse(*response);
      initialized["result"]["instructions"]=initialized["result"].value("instructions",std::string())+
        " You operate the currently open app model. File save/open/import/export commands are disabled here; the user saves the project. "
        "If a write request times out, inspect the model before retrying: it may already have completed.";
      response=initialized.dump();
    }
    reply(socket,response?200:202,response?QByteArray::fromStdString(*response):QByteArray());
  }catch(const std::exception&){reply(socket,500);}
}
}
