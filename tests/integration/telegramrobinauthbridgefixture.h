#pragma once
// Synthetic loopback provider and external credential/key storage only.
#include "rhnativeauthclient.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include <QCryptographicHash>
#include <QtTest>



#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QRegularExpression>

namespace TelegramRobinAuthBridgeFixture {
struct Store : RHCredentialStore {
    QHash<int,QString> values;
    int writes=0, reads=0;
    bool failWrite=false, holdWrite=false;
    bool holdRead=false, failRead=false, holdRemove=false;
    QList<std::function<void()>> pendingReads;
    std::function<void()> pending;
    std::function<void()> pendingRemove;
    void read(int slot, ReadCompletion done) override {
        ++reads; const QString snapshot=values.value(slot);
        if(holdRead){pendingReads.append([done,snapshot]{done(true,snapshot,{});});return;}
        QTimer::singleShot(0,this,[=,this]{done(!failRead,failRead?QString():snapshot,{});});
    }
    void write(int slot,const QString &secret,Completion done) override {
        ++writes;
        auto action=[=,this]{if(!failWrite)values[slot]=secret;done(!failWrite,failWrite?QString("Store unavailable"):QString());};
        if(holdWrite)pending=action;else action();
    }
    void remove(int slot,Completion done) override {
        auto action=[=,this]{values.remove(slot);done(true,{});};
        if(holdRemove)pendingRemove=action;else action();
    }
};
struct Server {
    QTcpServer server;
    QList<QByteArray> responses, requests;
    QList<int> statuses;
    std::function<void()> onRequest;
    QPointer<QTcpSocket> peer;
    Server() {
        QObject::connect(&server,&QTcpServer::newConnection,&server,[this]{
            auto *socket=server.nextPendingConnection();peer=socket;
            auto buffer=std::make_shared<QByteArray>();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[this,socket,buffer]{
                *buffer+=socket->readAll();auto end=buffer->indexOf("\r\n\r\n");if(end<0 || socket->property("handled").toBool())return;
                auto m=QRegularExpression("content-length: (\\d+)").match(QString::fromLatin1(buffer->first(end).toLower()));
                if(buffer->size()<end+4+(m.hasMatch()?m.captured(1).toInt():0))return;
                socket->setProperty("handled",true); requests.append(*buffer); if(onRequest)onRequest(); const int i=requests.size()-1;
                if(i<responses.size())respond(socket,responses[i],i<statuses.size()?statuses[i]:200);
            });
        });
        if(!server.listen(QHostAddress::LocalHost))qFatal("Cannot bind offline Robinhood fixture");
    }
    static void respond(QTcpSocket *socket,const QByteArray &body,int status=200){
        socket->write("HTTP/1.1 "+QByteArray::number(status)+" Test\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
        socket->disconnectFromHost();
    }
    QUrl origin() const {return QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort()));}
};
struct Fixture {
    QTemporaryDir dir;
    Store store;
    Server http;
    SecureCredentialLocker locker{dir.filePath("locker"),[](bool,SecureCredentialLocker::KeyCompletion done){done(true,QByteArray(32,'k'),{});}};
    RHNetworkEngine network{nullptr,true};
    RHNativeAuthClient client{RHNativeAuthClient::TestTransport{http.origin(),5000,5000,1}};
    std::unique_ptr<RHSessionManager> manager;
    Fixture(){
        QJsonObject endpoints;
        for(const auto &name:{QString("UserProfile"),QString("Accounts"),QString("TokenRefresh")})
            endpoints[name]=QJsonObject{{"URL",http.origin().toString()+(name=="UserProfile"?"/user/":name=="Accounts"?"/accounts/":"/oauth2/token/")},
                {"Method",name=="TokenRefresh"?"POST":"GET"},{"HeaderSets",QJsonArray{}},{"BodyEncoding",name=="TokenRefresh"?"form":"none"}};
        QFile file(dir.filePath("api.json"));if(!file.open(QIODevice::WriteOnly))return;
        file.write(QJsonDocument(QJsonObject{{"Headers",QJsonObject{}},{"Endpoints",endpoints}}).toJson());file.close();
        network.loadConfiguration(file.fileName());
        manager=std::make_unique<RHSessionManager>(network,dir.filePath("sessions.json"),&store,nullptr,2000,1,&locker);
        manager->setNativeAuthClient(&client);
    }
    static QByteArray grant(int seconds=1800){return QJsonDocument(QJsonObject{{"access_token","native-access"},{"refresh_token","native-refresh"},{"scope","internal"},{"token_type","Bearer"},{"expires_in",seconds}}).toJson(QJsonDocument::Compact);}
    static QByteArray profile(){return R"({"id":"owner-id","email":"owner@example.test","username":"not-the-label"})";}
    static QByteArray accounts(){return R"({"results":[{"account_number":"account-one"}]})";}
    QString label()const{return manager->data(manager->index(0),RHSessionManager::AccountNameRole).toString();}
};
}
