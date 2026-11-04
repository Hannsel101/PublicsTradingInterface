#include "telegramrobinhoodauthservice.h"
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
class TelegramRobinhoodAuthServiceTest : public QObject {
    Q_OBJECT
private slots:
    void cancellationCannotReplaceAnAlreadyTerminalNativeOutcome() {
        Fixture f; TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        connect(f.manager.get(),&RHSessionManager::busyChanged,&service,[&] {
            if (!f.manager->busy()) service.cancel();
        });
        QVERIFY(service.submit("request","missing@example.test"));
        QTRY_COMPARE(result.size(),1);
        QCOMPARE(result[0][1].toStringList(),QStringList{"Account Session Not Stored in List of Sessions"});
    }
    void lateCancellationCannotReplaceAnElapsedDeadline_data() {
        QTest::addColumn<bool>("holdLookup");
        QTest::newRow("lookup") << true;
        QTest::newRow("progress") << false;
    }
    void lateCancellationCannotReplaceAnElapsedDeadline() {
        QFETCH(bool, holdLookup);
        Fixture f; f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        f.store.holdRead=holdLookup;
        TelegramRobinhoodAuthService service(f.manager.get(),nullptr,500);
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QSignalSpy progress(&service,&TelegramRobinhoodAuthService::reportReady);
        QVERIFY(service.submit("request","owner@example.test"));
        if (holdLookup) QTRY_VERIFY(!f.store.pendingReads.isEmpty());
        else QTRY_COMPARE(progress.size(),1);
        // Deliberately prevent timer delivery after the monotonic budget expires.
        QTest::qSleep(600);
        QCOMPARE(result.size(),0);
        service.cancel();
        QTRY_COMPARE(result.size(),1);
        QCOMPARE(result[0][1].toStringList(),QStringList{
            "Authentication Failed for owner@example.test, reason: 8 minute timeout reached"});
        if (holdLookup) {
            QVERIFY(service.busy());
            auto reads=std::exchange(f.store.pendingReads,{}); for(auto &read:reads)read();
        }
        QTRY_VERIFY(!service.busy());
        service.cancel(); service.continueAfterProgress("request");
        QTest::qWait(20); QCOMPARE(result.size(),1);
        QCOMPARE(f.store.writes,1); QCOMPARE(f.http.requests.size(),3);
    }
    void nativeTerminalDisownsBackendBeforeQueuedCompletion() {
        Fixture f;
        QPointer<TelegramRobinhoodAuthService> service=new TelegramRobinhoodAuthService(f.manager.get());
        bool replaced=false;
        connect(f.manager.get(),&RHSessionManager::busyChanged,f.manager.get(),[&] {
            if (f.manager->busy() || replaced) return;
            replaced=true;
            QVERIFY(f.manager->beginNativeLogin());
            delete service.data();
        });
        QVERIFY(service->submit("request","missing@example.test"));
        QTRY_VERIFY(!service); QVERIFY(replaced);
        QVERIFY(f.manager->busy()); QVERIFY(f.manager->nativeLoginPending());
        f.manager->cancelSignIn();
    }
    void constructionAndInvalidInputAreCredentialFree() {
        TelegramRobinhoodAuthService service;
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QVERIFY(!service.busy());
        QVERIFY(!service.submit({},"owner@example.test"));
        QVERIFY(!service.submit("bad","no email"));
        QVERIFY(!service.submit("bad",QString(513,'a')+"@example.test"));
        service.continueAfterProgress("bad"); service.cancel();
        QVERIFY(!service.busy()); QCOMPARE(result.size(),0);
    }
    void unrelatedNativeWorkIsNeverCancelledByServiceDestruction() {
        Fixture f; QVERIFY(f.manager->beginNativeLogin());
        auto service=std::make_unique<TelegramRobinhoodAuthService>(f.manager.get());
        QVERIFY(service->busy()); QVERIFY(!service->submit("request","owner@example.test"));
        service->cancel(); service.reset();
        QVERIFY(f.manager->busy()); QVERIFY(f.manager->nativeLoginPending());
        f.manager->cancelSignIn(); QVERIFY(!f.manager->busy());
    }
    void unavailableNativeCompletionIsDeferredAndExactlyOnce() {
        Fixture f; f.manager->setNativeAuthClient(nullptr);
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QVERIFY(service.submit("request","owner@example.test")); QCOMPARE(result.size(),0);
        QTRY_COMPARE(result.size(),1); QTRY_VERIFY(!service.busy());
        service.cancel(); service.continueAfterProgress("request"); QTest::qWait(20);
        QCOMPARE(result.size(),1); QCOMPARE(f.http.requests.size(),0);
    }
    void dependencyDeletionDuringLookupSettlesWithoutDestroyedSignal() {
        Fixture f; f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy()); f.store.holdRead=true;
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QVERIFY(service.submit("request","owner@example.test")); QTRY_VERIFY(!f.store.pendingReads.isEmpty());
        QTest::ignoreMessage(QtWarningMsg,"QObject::disconnect: wildcard call disconnects from destroyed signal of RHSessionManager::unnamed");
        f.manager.reset();
        QTRY_COMPARE_WITH_TIMEOUT(result.size(),1,1000); QVERIFY(!service.busy());
        auto reads=std::exchange(f.store.pendingReads,{}); for(auto &read:reads)read();
        QTest::qWait(20); QCOMPARE(result.size(),1);
    }
    void cancelDuringLookupRetainsLeaseAndRejectsStaleCallbacks() {
        Fixture f; f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy()); f.store.holdRead=true;
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QVERIFY(service.submit("request","owner@example.test")); QTRY_VERIFY(!f.store.pendingReads.isEmpty());
        service.cancel(); QTRY_COMPARE(result.size(),1); QVERIFY(service.busy());
        QCOMPARE(result[0][1].toStringList(),QStringList{"Authentication Failed: authentication cancelled"});
        QVERIFY(!service.submit("other","owner@example.test")); service.cancel();
        auto reads=std::exchange(f.store.pendingReads,{}); for(auto &read:reads){read();read();}
        QTRY_VERIFY(!service.busy()); service.continueAfterProgress("request");
        QTest::qWait(20); QCOMPARE(result.size(),1); QCOMPARE(f.http.requests.size(),3); QCOMPARE(f.store.writes,1);
    }
    void completionStateIsSettledBeforeReentrantAdmission() {
        Fixture f; TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        bool replacement=false;
        connect(&service,&TelegramRobinhoodAuthService::busyChanged,&service,[&] {
            if(!service.busy() && !replacement) { replacement=true; QVERIFY(service.submit("second","missing@example.test")); }
        });
        QVERIFY(service.submit("first","missing@example.test"));
        QTRY_COMPARE(result.size(),2); QVERIFY(replacement);
        QCOMPARE(result[0][0].toString(),QString("first")); QCOMPARE(result[1][0].toString(),QString("second"));
        QVERIFY(!service.busy());
    }
    void progressObserverCanDeleteService() {
        Fixture f; f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        QPointer<TelegramRobinhoodAuthService> service=new TelegramRobinhoodAuthService(f.manager.get());
        connect(service,&TelegramRobinhoodAuthService::reportReady,f.manager.get(),[&](QString,QStringList){delete service.data();});
        QVERIFY(service->submit("request","owner@example.test"));
        QTRY_VERIFY(!service); QTRY_VERIFY(!f.manager->busy()); QCOMPARE(f.http.requests.size(),3);
    }
    void emailsAreEscapedInProgressAndSuccess() {
        Fixture f; auto profile=Fixture::profile(); profile.replace("owner@example.test","owner<&>@example.test");
        f.http.responses={Fixture::grant(),profile,Fixture::accounts(),Fixture::grant(),profile,Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner<&>@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy progress(&service,&TelegramRobinhoodAuthService::reportReady);
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        connect(&service,&TelegramRobinhoodAuthService::reportReady,&service,[&](QString id,QStringList){service.continueAfterProgress(id);});
        QVERIFY(service.submit("request","owner<&>@example.test")); QTRY_COMPARE(result.size(),1);
        QCOMPARE(progress[0][1].toStringList(),QStringList{"Authentication for account: owner&lt;&amp;&gt;@example.test in progress..."});
        QCOMPARE(result[0][1].toStringList(),QStringList{"Authentication Successful for owner&lt;&amp;&gt;@example.test"});
    }
    void rejectionUsesFiniteSummaryNotProviderSecrets_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<int>("status"); QTest::addColumn<QString>("summary");
        QTest::newRow("rejected")<<QByteArray(R"({"error":"invalid_grant","detail":"fixture-password native-access <unsafe>"})")<<400
            <<QString("provider rejected authentication (HTTP 400; error=invalid_grant; reason=unclassified)");
        QTest::newRow("credentials")<<QByteArray(R"({"error":"invalid_credentials","detail":"Unable to log in with provided credentials."})")<<400
            <<QString("provider rejected authentication (HTTP 400; error=invalid_credentials; reason=credentials_rejected)");
        QTest::newRow("rate-limit")<<QByteArray(R"({"error":"rate_limited","detail":"Request was throttled. private detail"})")<<400
            <<QString("provider rejected authentication (HTTP 400; error=rate_limited; reason=rate_limited)");
        QTest::newRow("network-http-error")<<QByteArray(R"({"detail":"private detail"})")<<429<<QString("authentication network request failed");
        QTest::newRow("captcha")<<QByteArray(R"({"error":"captcha_required","detail":"native-access fixture-password"})")<<400
            <<QString("provider rejected authentication (HTTP 400; error=captcha_required; reason=unclassified)");
        QTest::newRow("malformed")<<QByteArray("invalid-json native-access fixture-password")<<200<<QString("malformed authentication response");
    }
    void rejectionUsesFiniteSummaryNotProviderSecrets() {
        QFETCH(QByteArray,body); QFETCH(int,status); QFETCH(QString,summary);
        Fixture f; f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        f.http.responses.append(body);
        f.http.statuses={200,200,200,status};
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        connect(&service,&TelegramRobinhoodAuthService::reportReady,&service,[&](QString id,QStringList){service.continueAfterProgress(id);});
        QVERIFY(service.submit("request","owner@example.test"));
        QTRY_COMPARE(result.size(),1); QTRY_VERIFY(!service.busy());
        QCOMPARE(result[0][1].toStringList(),QStringList{QString("Authentication Failed: ")+summary});
        QCOMPARE(f.store.writes,1);
    }
    void farValidTokenIsNotReauthenticated_data() {
        QTest::addColumn<bool>("exactHour");
        QTest::newRow("far-valid")<<false;
        QTest::newRow("exactly-one-hour")<<true;
    }
    void farValidTokenIsNotReauthenticated() {
        QFETCH(bool,exactHour);
        Fixture f;
        auto grant=Fixture::grant(); grant.replace("3600","36000");
        f.http.responses={grant,Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        const auto stored=f.store.values;
        const auto expiry=QJsonDocument::fromJson(stored.value(0).toUtf8()).object()["expires_at"].toString();
        QVERIFY(QDateTime::fromString(expiry,Qt::ISODateWithMs).isValid());
        if (exactHour) f.manager->setStoredAuthenticationClockForTests([expiry] {
            return QDateTime::fromString(expiry,Qt::ISODateWithMs).addSecs(-3600);
        });
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto before=metadata.readAll(); metadata.close();
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QSignalSpy progress(&service,&TelegramRobinhoodAuthService::reportReady);
        QVERIFY(service.submit("skip","OWNER@example.test"));
        QTRY_COMPARE(result.size(),1); QTRY_VERIFY(!service.busy());
        QCOMPARE(result[0][0].toString(),QString("skip"));
        QCOMPARE(result[0][1].toStringList(),QStringList{QString("Authentication not performed for owner@example.test, reason: token expires at ")+expiry});
        QCOMPARE(progress.size(),0); QCOMPARE(f.http.requests.size(),3); QCOMPARE(f.store.writes,1);
        QCOMPARE(f.store.values,stored);
        QVERIFY(metadata.open(QIODevice::ReadOnly)); QCOMPARE(metadata.readAll(),before);
        QTest::qWait(10); QCOMPARE(result.size(),1);
    }
    void timeoutUsesOneNativeBudgetAndRetainsCleanup_data() {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"lookup","progress","commit"}) QTest::newRow(phase)<<QString(phase);
    }
    void timeoutUsesOneNativeBudgetAndRetainsCleanup() {
        QFETCH(QString,phase); Fixture f;
        f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin()); QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        f.http.responses.append(Fixture::grant()); f.http.responses.append(Fixture::profile()); f.http.responses.append(Fixture::accounts());
        f.store.holdRead=phase=="lookup"; f.store.holdWrite=phase=="commit";
        TelegramRobinhoodAuthService service(f.manager.get(),nullptr,phase=="commit"?1200:120);
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QSignalSpy progress(&service,&TelegramRobinhoodAuthService::reportReady);
        QVERIFY(service.submit("request","OWNER@example.test"));
        if (phase=="lookup") QTRY_VERIFY(!f.store.pendingReads.isEmpty());
        else {
            QTRY_COMPARE(progress.size(),1);
            if (phase=="commit") { service.continueAfterProgress("request"); QTRY_VERIFY(bool(f.store.pending)); }
        }
        QTRY_COMPARE_WITH_TIMEOUT(result.size(),1,2500);
        const QString email=phase=="lookup"?"OWNER@example.test":"owner@example.test";
        QCOMPARE(result[0][1].toStringList(),QStringList{QString("Authentication Failed for %1, reason: 8 minute timeout reached").arg(email)});
        if (phase!="progress") {
            QVERIFY(service.busy()); QVERIFY(!service.submit("other","owner@example.test"));
            if (phase=="lookup") { auto reads=std::exchange(f.store.pendingReads,{}); for(auto &read:reads)read(); }
            else { auto write=std::move(f.store.pending); write(); }
        }
        service.continueAfterProgress("request");
        QTRY_VERIFY(!service.busy()); QTest::qWait(20); QCOMPARE(result.size(),1);
        if (phase!="commit") { QCOMPARE(f.store.writes,1); QCOMPARE(f.http.requests.size(),3); }
    }
    void progressIsCorrelatedConsumedOnceAndSuccessWaitsForCommit() {
        Fixture f;
        f.http.responses={Fixture::grant(),Fixture::profile(),Fixture::accounts()};
        QVERIFY(f.manager->beginNativeLogin());
        QVERIFY(f.manager->signIn("owner@example.test","fixture-password"));
        QTRY_VERIFY(!f.manager->busy());
        f.http.responses.append(Fixture::grant()); f.http.responses.append(Fixture::profile()); f.http.responses.append(Fixture::accounts());
        f.store.holdWrite=true;
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service,&TelegramRobinhoodAuthService::completed);
        QSignalSpy progress(&service,&TelegramRobinhoodAuthService::reportReady);
        QVERIFY(service.submit("request"," OWNER@example.test "));
        QTRY_COMPARE(progress.size(),1);
        QCOMPARE(progress[0][0].toString(),QString("request"));
        QCOMPARE(progress[0][1].toStringList(),QStringList{"Authentication for account: owner@example.test in progress..."});
        QCOMPARE(f.http.requests.size(),3);
        service.continueAfterProgress("other"); QTest::qWait(10); QCOMPARE(f.http.requests.size(),3);
        service.continueAfterProgress("request"); service.continueAfterProgress("request");
        QTRY_VERIFY(bool(f.store.pending));
        QCOMPARE(result.size(),0); QVERIFY(service.busy());
        auto write=std::move(f.store.pending); write();
        QTRY_COMPARE(result.size(),1);
        QCOMPARE(result[0][1].toStringList(),QStringList{"Authentication Successful for owner@example.test"});
        QTRY_VERIFY(!service.busy()); QCOMPARE(f.store.writes,2); QCOMPARE(f.http.requests.size(),6);
    }
    void notFoundHasExactMessage() {
        Fixture f;
        TelegramRobinhoodAuthService service(f.manager.get());
        QSignalSpy result(&service, &TelegramRobinhoodAuthService::completed);
        QSignalSpy progress(&service, &TelegramRobinhoodAuthService::reportReady);
        QVERIFY(service.submit("request", "missing@example.test"));
        QTRY_COMPARE(result.size(),1);
        QCOMPARE(result[0][0].toString(),QString("request"));
        QCOMPARE(result[0][1].toStringList(),QStringList{"Account Session Not Stored in List of Sessions"});
        QCOMPARE(progress.size(),0);
        QTRY_VERIFY(!service.busy());
        QCOMPARE(f.http.requests.size(),0);
    }
};
QTEST_GUILESS_MAIN(TelegramRobinhoodAuthServiceTest)
#include "test_telegramrobinhoodauthservice.moc"
