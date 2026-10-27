#include "rhnativeauthclient.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QRegularExpression>

namespace {
struct Store : RHCredentialStore {
    QHash<int,QString> values;
    int writes=0;
    bool failWrite=false, holdWrite=false;
    bool holdRead=false, failRead=false, holdRemove=false;
    QList<std::function<void()>> pendingReads;
    std::function<void()> pending;
    std::function<void()> pendingRemove;
    void read(int slot, ReadCompletion done) override {
        const QString snapshot=values.value(slot);
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
    QPointer<QTcpSocket> peer;
    Server() {
        QObject::connect(&server,&QTcpServer::newConnection,&server,[this]{
            auto *socket=server.nextPendingConnection();peer=socket;
            auto buffer=std::make_shared<QByteArray>();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[this,socket,buffer]{
                *buffer+=socket->readAll();auto end=buffer->indexOf("\r\n\r\n");if(end<0)return;
                auto m=QRegularExpression("content-length: (\\d+)").match(QString::fromLatin1(buffer->first(end).toLower()));
                if(buffer->size()<end+4+(m.hasMatch()?m.captured(1).toInt():0))return;
                requests.append(*buffer);const int i=requests.size()-1;
                if(i<responses.size())respond(socket,responses[i],i<statuses.size()?statuses[i]:200);
            });
        });
        server.listen(QHostAddress::LocalHost);
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
    RHNativeAuthClient client{RHNativeAuthClient::TestTransport{http.origin(),1000,2000,1}};
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
    static QByteArray grant(){return R"({"access_token":"native-access","refresh_token":"native-refresh","scope":"internal","token_type":"Bearer","expires_in":3600})";}
    static QByteArray profile(){return R"({"id":"owner-id","email":"owner@example.test","username":"not-the-label"})";}
    static QByteArray accounts(){return R"({"results":[{"account_number":"account-one"}]})";}
    QString label()const{return manager->data(manager->index(0),RHSessionManager::AccountNameRole).toString();}
};
}
class RHSessionManagerNativeTest : public QObject {
    Q_OBJECT
private slots:
    void realNativeFormCredentialAndCodeContract() {
        Fixture f;
        f.http.responses = {R"({"verification_workflow":{"id":"workflow"}})",
            R"({"id":"machine"})",
            R"({"context":{"sheriff_challenge":{"type":"sms","status":"issued","id":"challenge"}}})"};
        f.http.statuses = {400,200,200};
        QVERIFY(f.manager->beginNativeLogin());
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("RobinSessions"),f.manager.get());
        const auto source=QStringLiteral(PUBLICS_RH_NATIVE_FORM_SOURCE);
        QVERIFY(!source.isEmpty());
        QQmlComponent component(&engine,QUrl::fromLocalFile(source));
        QScopedPointer<QObject> form(component.create());
        QVERIFY2(form,qPrintable(component.errorString()));
        auto *email=form->findChild<QObject *>("robinNativeEmail");
        auto *password=form->findChild<QObject *>("robinNativePassword");
        auto *signIn=form->findChild<QObject *>("signInRobinNative");
        auto *code=form->findChild<QObject *>("robinNativeVerificationCode");
        auto *submit=form->findChild<QObject *>("submitRobinNativeCode");
        QVERIFY(email&&password&&signIn&&code&&submit);
        QVERIFY(f.manager->busy());
        QVERIFY(email->property("enabled").toBool());
        QVERIFY(password->property("enabled").toBool());
        email->setProperty("text","owner@example.test");
        password->setProperty("text","fixture-password");
        QVERIFY(signIn->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(signIn,"clicked",Qt::DirectConnection));
        QVERIFY(password->property("text").toString().isEmpty());
        QVERIFY(!email->property("enabled").toBool());
        QTRY_VERIFY(f.manager->nativeCodeRequired());
        QVERIFY(f.manager->busy());
        QVERIFY(code->property("enabled").toBool());
        QCOMPARE(f.store.writes,0);
        f.http.responses.append(R"({"status":"validated","challenge_status":"validated"})");
        f.http.responses.append(R"({"type_context":{"result":"workflow_status_approved"}})");
        f.http.responses.append(Fixture::grant());
        f.http.responses.append(Fixture::profile());
        f.http.responses.append(Fixture::accounts());
        code->setProperty("text","654321");
        QVERIFY(submit->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(submit,"clicked",Qt::DirectConnection));
        QVERIFY(code->property("text").toString().isEmpty());
        QTRY_COMPARE(f.store.writes,1);
        QTRY_VERIFY(!f.manager->nativeLoginPending());
        QCOMPARE(f.label(),QString("owner@example.test"));
    }
    void clearingActiveSessionCancelsNativeFormBeforeIdle(){
        Fixture f; QVERIFY(f.manager->beginNativeLogin());
        bool replaced=false;
        connect(f.manager.get(),&RHSessionManager::busyChanged,f.manager.get(),[&]{
            if(f.manager->busy()||replaced)return;replaced=true;
            QVERIFY(!f.manager->nativeLoginPending());
            QVERIFY(f.manager->beginNativeLogin());
        });
        f.manager->clearActiveSession();
        QVERIFY(replaced);QVERIFY(f.manager->busy());QVERIFY(f.manager->nativeLoginPending());
        QCOMPARE(f.store.writes,0);
    }
    void verificationFailureNeverPersists_data(){
        QTest::addColumn<QByteArray>("profile");QTest::addColumn<QByteArray>("accounts");
        QTest::newRow("missing-email")<<QByteArray(R"({"id":"owner-id","username":"fallback"})")<<Fixture::accounts();
        QTest::newRow("wrong-email")<<QByteArray(R"({"id":"owner-id","email":"other@example.test"})")<<Fixture::accounts();
        QTest::newRow("missing-id")<<QByteArray(R"({"email":"owner@example.test"})")<<Fixture::accounts();
        QTest::newRow("malformed-accounts")<<Fixture::profile()<<QByteArray(R"({"results":null})");
        QTest::newRow("duplicate-accounts")<<Fixture::profile()<<QByteArray(R"({"results":[{"id":"same"},{"id":"same"}]})");
    }
    void verificationFailureNeverPersists(){
        QFETCH(QByteArray,profile);QFETCH(QByteArray,accounts);
        Fixture f;f.http.responses={Fixture::grant(),profile,accounts};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());QVERIFY(!f.manager->nativeLoginPending());QVERIFY(!f.manager->error().isEmpty());
        QCOMPARE(f.store.writes,0);QCOMPARE(f.manager->rowCount(),0);QVERIFY(f.store.values.isEmpty());
        QVERIFY(!f.manager->error().contains("fixture-password"));QVERIFY(!f.manager->error().contains("native-access"));
    }
    void commitIsSingleFlightAndNotCancellable(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};f.store.holdWrite=true;
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(bool(f.store.pending));QCOMPARE(f.store.writes,1);QVERIFY(f.manager->nativeLoginPending());
        QVERIFY(!f.manager->nativeLoginCancellable());f.manager->cancelSignIn();
        QVERIFY(!f.manager->beginNativeLogin());QVERIFY(!f.manager->load());QVERIFY(!f.manager->deleteSavedSession(0));
        QVERIFY(!f.manager->signIn("other","other"));
        RHSessionManager other(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QVERIFY(!other.saveAuthenticatedSession("other","other-token",3600));
        f.store.pending();f.store.pending={};QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.store.writes,1);
        QCOMPARE(f.label(),QString("owner@example.test"));
    }
    void cancellationBeforeGrantNeverPersists(){
        Fixture f;QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.http.requests.size(),1);f.manager->cancelSignIn();
        QVERIFY(!f.manager->busy());QVERIFY(!f.manager->nativeLoginPending());
        Server::respond(f.http.peer,Fixture::grant());QTest::qWait(20);QCOMPARE(f.store.writes,0);QCOMPARE(f.manager->rowCount(),0);
    }
    void storeFailureKeepsNoProfileAndRetryWorks(){
        Fixture f;f.store.failWrite=true;
        f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts(),Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());QVERIFY(!f.manager->nativeLoginPending());QCOMPARE(f.manager->rowCount(),0);
        QCOMPARE(f.manager->error(),QString("Store unavailable"));f.store.failWrite=false;
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QVERIFY(!f.manager->busy());
    }
    void rotationKeepsCredentialsAndOpaqueLabelAcrossRestart(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts(),
            R"({"access_token":"rotated-access","refresh_token":"rotated-refresh","token_type":"Bearer","expires_in":3600})",
            Fixture::profile(),Fixture::accounts(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());
        const auto original=QJsonDocument::fromJson(f.store.values[0].toUtf8()).object().value("response").toObject();
        QCOMPARE(original.value("access_token"),QJsonValue("native-access"));
        f.manager=std::make_unique<RHSessionManager>(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QTRY_COMPARE(f.label(),QString("owner@example.test"));
        QVERIFY(f.manager->initializeSession(0));QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.writes,2);QVERIFY(f.manager->sessionActive());
        RHOAuthGrant grant;QVERIFY(RHOAuthGrant::deserialize(f.store.values[0],&grant));
        QCOMPARE(grant.nativeEmail,QString("owner@example.test"));QCOMPARE(grant.nativePassword,QString("fixture-password"));
        QCOMPARE(grant.refreshToken(),QByteArray("rotated-refresh"));
        QVERIFY(!f.http.requests[3].contains("fixture-password"));QVERIFY(!f.http.requests[3].contains("owner%40"));
        QVERIFY(f.http.requests[3].contains("grant_type=refresh_token"));QVERIFY(f.http.requests[3].contains("refresh_token=native-refresh"));
        QFile metadata(f.dir.filePath("sessions.json"));QVERIFY(metadata.open(QIODevice::ReadOnly));const auto disk=metadata.readAll();
        QVERIFY(!disk.contains("owner@example.test"));QVERIFY(!disk.contains("fixture-password"));QVERIFY(!disk.contains("rotated-access"));
        QVERIFY(f.manager->deleteSavedSession(0));QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.manager->rowCount(),0);QVERIFY(f.store.values.isEmpty());
        QVERIFY(f.manager->load());QCOMPARE(f.manager->rowCount(),0);
    }
    void codeAndPushApprovalStayPendingUntilComplete_data(){
        QTest::addColumn<bool>("push");QTest::newRow("sms")<<false;QTest::newRow("push")<<true;
    }
    void codeAndPushApprovalStayPendingUntilComplete(){
        QFETCH(bool,push);Fixture f;
        f.http.responses={R"({"verification_workflow":{"id":"workflow"}})",R"({"id":"machine"})",
            push?QByteArray(R"({"context":{"sheriff_challenge":{"type":"prompt","status":"issued","id":"challenge"}}})"):
                QByteArray(R"({"context":{"sheriff_challenge":{"type":"sms","status":"issued","id":"challenge"}}})")};
        f.http.statuses={400,200,200};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password","123456"));
        if(push){QTRY_COMPARE(f.http.requests.size(),4);QVERIFY(f.manager->nativeLoginStatus().contains("Approve"));}
        else {QTRY_VERIFY(f.manager->nativeCodeRequired());QVERIFY(f.manager->busy());QVERIFY(!f.manager->nativeLoginWorking());QCOMPARE(f.http.requests.size(),3);}
        if(push) QVERIFY(f.manager->nativeLoginWorking());
        QVERIFY(f.manager->nativeLoginPending());QVERIFY(f.manager->nativeLoginCancellable());QCOMPARE(f.store.writes,0);
        f.http.responses.append(R"({"status":"validated","challenge_status":"validated"})");
        f.http.responses.append(R"({"type_context":{"result":"workflow_status_approved"}})");
        f.http.responses.append(Fixture::grant());f.http.responses.append(Fixture::profile());f.http.responses.append(Fixture::accounts());
        if(push)Server::respond(f.http.peer,f.http.responses[3]);else f.manager->submitNativeVerificationCode("123456");
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.store.writes,1);
        QCOMPARE(f.http.requests[5].mid(f.http.requests[5].indexOf("\r\n\r\n")+4),
                 f.http.requests[0].mid(f.http.requests[0].indexOf("\r\n\r\n")+4));
        QVERIFY(f.http.requests[5].contains("mfa_code=123456"));
    }
    void reauthenticationPreservesStableSlotAndRejectsWrongPrincipal(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts(),Fixture::grant(),
            R"({"id":"intruder-id","email":"owner@example.test"})",Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());const QString original=f.store.values[0];
        QVERIFY(f.manager->reauthenticate(0));QVERIFY(f.manager->nativeLoginPending());QVERIFY(f.manager->reauthenticating());
        QVERIFY(f.manager->signIn("owner@example.test","new-password"));QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.values[0],original);QCOMPARE(f.store.writes,1);QCOMPARE(f.manager->rowCount(),1);
        QVERIFY(f.manager->reauthenticate(0));QVERIFY(f.manager->signIn("owner@example.test","new-password"));
        QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.store.writes,2);QCOMPARE(f.manager->rowCount(),1);
        QCOMPARE(f.manager->data(f.manager->index(0),RHSessionManager::SessionIndexRole).toInt(),0);
        RHOAuthGrant grant;QVERIFY(RHOAuthGrant::deserialize(f.store.values[0],&grant));QCOMPARE(grant.nativePassword,QString("new-password"));
    }
    void notificationsAllowDestruction_data(){
        QTest::addColumn<QString>("event");
        for(const auto &event:{"form","request","verification","commit","finished","idle","error","saved"})QTest::newRow(event)<<QString(event);
    }
    void notificationsAllowDestruction(){
        QFETCH(QString,event);Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        if(event=="error")f.store.failWrite=true;
        bool destroyed=false;
        const auto destroy=[&]{destroyed=true;f.manager.reset();};
        connect(f.manager.get(),&RHSessionManager::nativeLoginChanged,f.manager.get(),[&]{
            const auto status=f.manager->nativeLoginStatus();
            if((event=="form"&&status.contains("Enter email"))||(event=="request"&&status.contains("Requesting"))
                ||(event=="verification"&&status.contains("Verifying"))||(event=="commit"&&status.contains("Saving"))
                ||(event=="finished"&&!f.manager->nativeLoginPending()))destroy();
        });
        connect(f.manager.get(),&RHSessionManager::busyChanged,f.manager.get(),[&]{if(event=="idle"&&!f.manager->busy())destroy();});
        connect(f.manager.get(),&RHSessionManager::errorChanged,f.manager.get(),[&]{if(event=="error"&&!f.manager->error().isEmpty())destroy();});
        connect(f.manager.get(),&RHSessionManager::sessionSaved,f.manager.get(),[&]{if(event=="saved")destroy();});
        QVERIFY(f.manager->beginNativeLogin());
        if(f.manager)f.manager->signIn("owner@example.test","fixture-password");
        QTRY_VERIFY(destroyed);QVERIFY(!f.manager);
    }
    void cancelledNativePendingObserverCannotIdleReplacementRead(){
        Fixture f;QVERIFY(f.manager->saveAuthenticatedSession("saved","saved-token",3600));QTRY_VERIFY(!f.manager->busy());
        f.store.holdRead=true;QVERIFY(f.manager->beginNativeLogin());bool replaced=false;QString replacementError;
        connect(f.manager.get(),&RHSessionManager::nativeLoginChanged,f.manager.get(),[&]{
            if(f.manager->nativeLoginPending()||replaced)return;replaced=true;
            f.manager->clearActiveSession();QVERIFY(f.manager->initializeSession(0));QVERIFY(!f.manager->reauthenticate(-1));
            replacementError=f.manager->error();QVERIFY(!replacementError.isEmpty());
        });
        f.manager->cancelSignIn();QVERIFY(replaced);QVERIFY(f.manager->busy());
        QCOMPARE(f.manager->error(),replacementError);QCOMPARE(f.store.pendingReads.size(),1);
        f.manager->cancelSignIn();auto complete=f.store.pendingReads.takeFirst();complete();
    }
    void nativeIdleCannotPublishSavedAfterSynchronousReplacementFailure(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        int saved=0;bool replaced=false;
        connect(f.manager.get(),&RHSessionManager::sessionSaved,f.manager.get(),[&]{++saved;});
        connect(f.manager.get(),&RHSessionManager::busyChanged,f.manager.get(),[&]{
            if(f.manager->busy()||replaced)return;replaced=true;f.store.failWrite=true;
            QVERIFY(f.manager->saveAuthenticatedSession("replacement","replacement-token",3600));
        });
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(replaced);QVERIFY(!f.manager->busy());QCOMPARE(f.manager->error(),QString("Store unavailable"));
        QCOMPARE(saved,0);
    }
    void loadingNativeLabelsSurvivesDeletionAndStaleRead(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());
        f.store.holdRead=true;
        f.manager=std::make_unique<RHSessionManager>(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QVERIFY(f.label().startsWith("Saved Robinhood"));QCOMPARE(f.store.pendingReads.size(),1);
        QVERIFY(f.manager->deleteSavedSession(0));QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.manager->rowCount(),0);
        auto stale=f.store.pendingReads.takeFirst();stale();QCOMPARE(f.manager->rowCount(),0);QVERIFY(f.store.values.isEmpty());
        f.store.holdRead=false;QVERIFY(f.manager->load());QCOMPARE(f.manager->rowCount(),0);
    }
    void lateLabelCannotOverwriteNewOperationAndReadFailureUsesPlaceholder(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());f.store.holdRead=true;
        f.manager=std::make_unique<RHSessionManager>(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        f.manager->setNativeAuthClient(&f.client);QCOMPARE(f.store.pendingReads.size(),1);
        QVERIFY(f.manager->beginNativeLogin("owner-id"));auto stale=f.store.pendingReads.takeFirst();stale();
        QVERIFY(f.label().startsWith("Saved Robinhood"));QVERIFY(f.manager->nativeLoginEmail().isEmpty());f.manager->cancelSignIn();
        f.store.holdRead=false;f.store.failRead=true;QVERIFY(f.manager->load());QTest::qWait(10);
        QVERIFY(f.label().startsWith("Saved Robinhood"));QCOMPARE(f.store.writes,1);
    }
    void heldNativeWriteReservationSurvivesOwnerDestruction(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};f.store.holdWrite=true;
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(bool(f.store.pending));f.manager.reset();
        RHSessionManager other(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QVERIFY(!other.saveAuthenticatedSession("replacement","replacement-token",3600));
        auto complete=std::move(f.store.pending);complete();complete={};f.store.holdWrite=false;
        QVERIFY(other.saveAuthenticatedSession("replacement","replacement-token",3600));QTRY_VERIFY(!other.busy());
        QCOMPARE(f.store.values[0],QString("replacement-token"));
    }
    void delayedNativeReauthenticationCannotResurrectDeletedSlot_data(){
        QTest::addColumn<bool>("reuse");QTest::newRow("deleted")<<false;QTest::newRow("reused")<<true;
    }
    void delayedNativeReauthenticationCannotResurrectDeletedSlot(){
        QFETCH(bool,reuse);Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts(),Fixture::grant(),Fixture::profile()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->reauthenticate(0));QVERIFY(f.manager->signIn("owner@example.test","new-password"));
        QTRY_COMPARE(f.http.requests.size(),6);auto held=f.http.peer;
        RHSessionManager other(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QVERIFY(other.deleteSavedSession(0));QTRY_VERIFY(!other.busy());
        if(reuse){QVERIFY(other.saveAuthenticatedSession("replacement","replacement-token",3600));QTRY_VERIFY(!other.busy());}
        const auto before=f.store.values;const int writes=f.store.writes;
        Server::respond(held,Fixture::accounts());QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.values,before);QCOMPARE(f.store.writes,writes);QVERIFY(!f.manager->error().isEmpty());
        QVERIFY(f.manager->load());QCOMPARE(f.manager->rowCount(),reuse?1:0);
        if(reuse)QCOMPARE(f.label(),QString("replacement"));
    }
    void metadataResetRejectsNestedLoad(){
        Fixture f;QVERIFY(f.manager->saveAuthenticatedSession("legacy","legacy-token",3600));QTRY_VERIFY(!f.manager->busy());
        bool attempted=false;
        connect(f.manager.get(),&QAbstractItemModel::modelAboutToBeReset,f.manager.get(),[&]{
            if(attempted)return;attempted=true;QVERIFY(!f.manager->load());
        });
        QVERIFY(f.manager->load());QVERIFY(attempted);QCOMPARE(f.manager->rowCount(),1);
    }
    void loadingNativeLabelMetadataCanDestroyManagerFromNotification(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());
        connect(f.manager.get(),&RHSessionManager::accountCacheChanged,f.manager.get(),[&]{f.manager.reset();});
        auto *manager=f.manager.get();QVERIFY(manager->load());QVERIFY(!f.manager);
    }
    void legitimateNativeReauthenticationMigratesLegacyLabelInSameSlot(){
        Fixture f;QVERIFY(f.manager->saveAuthenticatedSession("not-the-label","legacy-token",3600));QTRY_VERIFY(!f.manager->busy());
        f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->reauthenticate(0));QVERIFY(f.manager->nativeLoginPending());
        QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.manager->rowCount(),1);QCOMPARE(f.label(),QString("owner@example.test"));
        QCOMPARE(f.manager->data(f.manager->index(0),RHSessionManager::SessionIndexRole).toInt(),0);
        QFile file(f.dir.filePath("sessions.json"));QVERIFY(file.open(QIODevice::ReadOnly));const auto disk=file.readAll();
        QVERIFY(!disk.contains("owner@example.test"));QVERIFY(!disk.contains("not-the-label"));
        RHOAuthGrant grant;QVERIFY(RHOAuthGrant::deserialize(f.store.values[0],&grant));QCOMPARE(grant.nativePassword,QString("fixture-password"));
    }
    void rollbackKeepsNativeCommitAndReservationUntilReconciled(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};f.store.holdWrite=true;f.store.holdRemove=true;
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(bool(f.store.pending));
        QLockFile blocked(f.dir.filePath("sessions.json.lock"));QVERIFY(blocked.tryLock());
        auto write=std::move(f.store.pending);write();write={};QVERIFY(bool(f.store.pendingRemove));
        QVERIFY(f.manager->nativeLoginPending());QVERIFY(f.manager->busy());QVERIFY(!f.manager->nativeLoginCancellable());
        f.manager->cancelSignIn();QVERIFY(f.manager->busy());QVERIFY(!f.manager->beginNativeLogin());
        RHSessionManager other(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QVERIFY(!other.saveAuthenticatedSession("replacement","replacement-token",3600));
        blocked.unlock();auto remove=std::move(f.store.pendingRemove);remove();remove={};
        QVERIFY(!f.manager->busy());QVERIFY(!f.manager->nativeLoginPending());QCOMPARE(f.manager->rowCount(),0);QVERIFY(f.store.values.isEmpty());
        f.store.holdWrite=false;f.store.holdRemove=false;
        QVERIFY(other.saveAuthenticatedSession("replacement","replacement-token",3600));QTRY_VERIFY(!other.busy());
    }
    void nativeClientReplacementCancelsOldGrant(){
        Fixture f;QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.http.requests.size(),1);
        RHNativeAuthClient replacement(RHNativeAuthClient::TestTransport{f.http.origin(),1000,2000,1});
        f.manager->setNativeAuthClient(&replacement);QVERIFY(!f.manager->busy());QVERIFY(!f.manager->nativeLoginPending());
        Server::respond(f.http.peer,Fixture::grant());QTest::qWait(10);QCOMPARE(f.store.writes,0);
        f.http.responses={QByteArray(),Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.store.writes,1);
    }
    void rejectionAndCodeTimeoutNeverWrite_data(){
        QTest::addColumn<bool>("timeout");QTest::newRow("rejected")<<false;QTest::newRow("waiting-code-timeout")<<true;
    }
    void rejectionAndCodeTimeoutNeverWrite(){
        QFETCH(bool,timeout);Fixture f;
        if(timeout){f.http.responses={R"({"verification_workflow":{"id":"workflow"}})",R"({"id":"machine"})",R"({"context":{"sheriff_challenge":{"type":"sms","status":"issued","id":"challenge"}}})"};f.http.statuses={400,200,200};}
        else {f.http.responses={R"({"error":"invalid_grant","detail":"fixture-password native-access"})"};f.http.statuses={400};}
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY_WITH_TIMEOUT(!f.manager->busy(),4000);QVERIFY(!f.manager->nativeLoginPending());QCOMPARE(f.store.writes,0);
        QVERIFY(!f.manager->error().contains("fixture-password"));QVERIFY(!f.manager->error().contains("native-access"));
    }
    void malformedNativeLabelMetadataFailsClosed(){
        Fixture f;f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.manager->rowCount(),1);QTRY_VERIFY(!f.manager->busy());
        QFile file(f.dir.filePath("sessions.json"));QVERIFY(file.open(QIODevice::ReadOnly));
        auto root=QJsonDocument::fromJson(file.readAll()).object();file.close();auto rows=root["sessions"].toArray();
        auto row=rows[0].toObject();row["native_label"]="true";rows[0]=row;root["sessions"]=rows;
        QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(root).toJson());file.close();
        QVERIFY(!f.manager->load());QCOMPARE(f.store.writes,1);
    }
    void nativeSuccessCommitsOnlyAfterIdentityAndAccounts(){
        Fixture f;QVERIFY(f.network.isConfigured());
        QVERIFY(f.manager->beginNativeLogin());QVERIFY(f.manager->nativeLoginPending());
        f.http.responses={Fixture::grant(),Fixture::profile()};
        QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_COMPARE(f.http.requests.size(),3);QCOMPARE(f.store.writes,0);QVERIFY(f.manager->nativeLoginPending());
        QVERIFY(f.manager->nativeLoginCancellable());
        Server::respond(f.http.peer,Fixture::accounts());
        QTRY_VERIFY(!f.manager->busy());QCOMPARE(f.store.writes,1);QVERIFY(!f.manager->nativeLoginPending());
        QCOMPARE(f.label(),QString("owner@example.test"));
        RHOAuthGrant grant;QVERIFY(RHOAuthGrant::deserialize(f.store.values[0],&grant));
        QCOMPARE(grant.nativeEmail,QString("owner@example.test"));QCOMPARE(grant.nativePassword,QString("fixture-password"));
        QCOMPARE(grant.accountId,QString("owner-id"));
        QFile metadata(f.dir.filePath("sessions.json"));QVERIFY(metadata.open(QIODevice::ReadOnly));const auto disk=metadata.readAll();
        QVERIFY(!disk.contains("owner@example.test"));QVERIFY(!disk.contains("fixture-password"));QVERIFY(!disk.contains("native-access"));
        f.manager=std::make_unique<RHSessionManager>(f.network,f.dir.filePath("sessions.json"),&f.store,nullptr,2000,1,&f.locker);
        QTRY_COMPARE(f.label(),QString("owner@example.test"));
    }
};
QTEST_MAIN(RHSessionManagerNativeTest)
#include "test_rhsessionmanager_native.moc"
