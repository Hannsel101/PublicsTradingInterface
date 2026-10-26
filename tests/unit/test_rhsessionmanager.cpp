#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

class FakeCredentialStore final : public RHCredentialStore
{
public:
    QHash<int, QString> values;
    bool failWrite = false;
    void write(int index, const QString &token, Completion done) override
    {
        QTimer::singleShot(0, this, [this, index, token, done = std::move(done)]() {
            if (!failWrite)
                values.insert(index, token);
            done(!failWrite, failWrite ? QStringLiteral("Keychain unavailable") : QString());
        });
    }
    void read(int index, ReadCompletion done) override
    {
        QTimer::singleShot(0, this, [this, index, done = std::move(done)]() {
            done(values.contains(index), values.value(index), QStringLiteral("Session token unavailable"));
        });
    }
    void remove(int index, Completion done) override
    {
        QTimer::singleShot(0, this, [this, index, done = std::move(done)]() {
            values.remove(index);
            done(true, {});
        });
    }
};

static bool writeApprovalConfig(const QString &path, quint16 port)
{
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);
    const auto route = [&base](const QString &path, const QString &method, const QString &encoding) {
        const QString headerSet = path == QStringLiteral("/accounts/") ? QStringLiteral("Authorized")
                                : encoding == QStringLiteral("form") ? QStringLiteral("Form")
                                : encoding == QStringLiteral("json") ? QStringLiteral("Json") : QString();
        return QJsonObject{{QStringLiteral("URL"), base + path},
                           {QStringLiteral("Method"), method},
                           {QStringLiteral("BodyEncoding"), encoding},
                           {QStringLiteral("HeaderSets"), headerSet.isEmpty() ? QJsonArray{} : QJsonArray{headerSet}}};
    };
    const QJsonObject endpoints{
        {QStringLiteral("Login"), route(QStringLiteral("/login/"), QStringLiteral("POST"), QStringLiteral("form"))},
        {QStringLiteral("BeginVerification"), route(QStringLiteral("/machine/"), QStringLiteral("POST"), QStringLiteral("json"))},
        {QStringLiteral("ReadVerificationInquiry"), route(QStringLiteral("/inquiry/{machine_id}/"), QStringLiteral("GET"), QStringLiteral("none"))},
        {QStringLiteral("CheckPrompt"), route(QStringLiteral("/push/{challenge_id}/"), QStringLiteral("GET"), QStringLiteral("none"))},
        {QStringLiteral("ContinueVerification"), route(QStringLiteral("/inquiry/{machine_id}/"), QStringLiteral("POST"), QStringLiteral("json"))},
        {QStringLiteral("Accounts"), route(QStringLiteral("/accounts/"), QStringLiteral("GET"), QStringLiteral("none"))}
    };
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(QJsonDocument(QJsonObject{{QStringLiteral("Headers"), QJsonObject{
                                                 {QStringLiteral("Form"), QJsonObject{{QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded")}}},
                                                 {QStringLiteral("Json"), QJsonObject{{QStringLiteral("Content-Type"), QStringLiteral("application/json")}}},
                                                 {QStringLiteral("Authorized"), QJsonObject{{QStringLiteral("Authorization"), QStringLiteral("{token_type} {access_token}")}}}}},
                                             {QStringLiteral("Endpoints"), endpoints}}).toJson()) > 0;
}

struct ApprovalServer {
    QTcpServer server;
    QVector<QByteArray> requests;
    QVector<QByteArray> responses;

    ApprovalServer()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            auto *peer = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(peer, &QTcpSocket::readyRead, peer, [this, peer, buffer]() {
                buffer->append(peer->readAll());
                const qsizetype headersEnd = buffer->indexOf("\r\n\r\n");
                if (headersEnd < 0)
                    return;
                const auto match = QRegularExpression(QStringLiteral("content-length: (\\d+)"))
                                       .match(QString::fromLatin1(buffer->first(headersEnd).toLower()));
                const int bodyLength = match.hasMatch() ? match.captured(1).toInt() : 0;
                if (buffer->size() < headersEnd + 4 + bodyLength)
                    return;
                requests.append(*buffer);
                const int ordinal = requests.size() - 1;
                if (ordinal < responses.size()) {
                    const QByteArray &body = responses.at(ordinal);
                    const QByteArray status = ordinal == 0 ? "403 Forbidden" : "200 OK";
                    peer->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                                + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    peer->flush();
                    peer->disconnectFromHost();
                }
            });
        });
    }
};

class RHSessionManagerTest : public QObject
{
    Q_OBJECT
private slots:
    void saveSeparatesSecretFromMetadataAndBoundsLifetime()
    {
        QTemporaryDir dir;
        RHNetworkEngine network;
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        RHSessionManager manager(network, path, &store);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("person@example.test"),
                                                 QStringLiteral("private-test-token"), 40 * 86400,
                                                 QStringLiteral("Bearer")));
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(manager.data(manager.index(0), RHSessionManager::AccountNameRole).toString(),
                 QStringLiteral("person@example.test"));
        QCOMPARE(manager.data(manager.index(0), RHSessionManager::SessionIndexRole).toInt(), 0);
        QVERIFY(!manager.data(manager.index(0), RHSessionManager::IsExpiredRole).toBool());
        QCOMPARE(store.values.value(0), QStringLiteral("private-test-token"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray json = file.readAll();
        QVERIFY(!json.contains("private-test-token"));
        const QJsonObject entry = QJsonDocument::fromJson(json).object().value("sessions").toArray().first().toObject();
        QCOMPARE(entry.value("index").toInt(), 0);
        QCOMPARE(entry.value("account_name").toString(), QStringLiteral("person@example.test"));
        const QDateTime expires = QDateTime::fromString(entry.value("expires_at").toString(), Qt::ISODateWithMs);
        const qint64 remaining = QDateTime::currentDateTimeUtc().secsTo(expires);
        QVERIFY(remaining >= 30LL * 86400 - 5 && remaining <= 30LL * 86400);
    }

    void expiredRemovalAndGapReuse()
    {
        QTemporaryDir dir;
        RHNetworkEngine network;
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("first@test.invalid"), QStringLiteral("one"), 1));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("second@test.invalid"), QStringLiteral("two"), 1));
        QTRY_COMPARE(saved.size(), 2);
        QTest::qWait(1200);
        QVERIFY(manager.data(manager.index(0), RHSessionManager::IsExpiredRole).toBool());
        QVERIFY(!manager.startSession(0));
        QSignalSpy reauth(&manager, &RHSessionManager::reauthenticationRequested);
        QVERIFY(manager.reauthenticate(0));
        QTRY_COMPARE(reauth.size(), 1);
        QCOMPARE(reauth.first().first().toString(), QStringLiteral("first@test.invalid"));
        QCOMPARE(manager.rowCount(), 1);
        QVERIFY(!store.values.contains(0));
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("third@test.invalid"), QStringLiteral("three"), 3600));
        QTRY_COMPARE(saved.size(), 3);
        QCOMPARE(manager.data(manager.index(1), RHSessionManager::SessionIndexRole).toInt(), 0);
    }

    void malformedMetadataFailsClosed()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{not-json");
        file.close();
        RHNetworkEngine network;
        FakeCredentialStore store;
        RHSessionManager manager(network, path, &store);
        QVERIFY(!manager.load());
        QVERIFY(!manager.saveAuthenticatedSession(QStringLiteral("person@test.invalid"), QStringLiteral("secret"), 3600));
        QVERIFY(store.values.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{not-json"));
    }

    void keychainFailureDoesNotCreateMetadata()
    {
        QTemporaryDir dir;
        RHNetworkEngine network;
        FakeCredentialStore store;
        store.failWrite = true;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        RHSessionManager manager(network, path, &store);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("temporary"), 3600));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(!QFile::exists(path));
        QVERIFY(!manager.error().isEmpty());
    }

    void metadataFailureRollsBackNewKeychainEntry()
    {
        QTemporaryDir dir;
        RHNetworkEngine network;
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        RHSessionManager manager(network, path, &store);
        QVERIFY(QDir().mkdir(path)); // Occupy the file path after initial load.
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("temporary"), 3600));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(store.values.isEmpty());
        QVERIFY(!manager.error().isEmpty());
    }

    void loginHonorsServerGrantAndNeverSavesChallengeResponse()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir;
        QFile config(dir.filePath(QStringLiteral("api.json")));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QStringLiteral(R"json({"Headers":{"Form":{"Content-Type":"application/x-www-form-urlencoded"}},"Endpoints":{"Login":{"URL":"http:\/\/127.0.0.1:%1/login/","Method":"POST","BodyEncoding":"form","HeaderSets":["Form"],"BodyDefaults":{"client_id":"public-client","grant_type":"password"}}}})json")
                         .arg(server.serverPort()).toUtf8());
        config.close();
        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(config.fileName(), &error), qPrintable(error));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        QVERIFY(manager.signIn(QStringLiteral("owner@test.invalid"), QStringLiteral("dummy-password")));
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable() > 0);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("POST /login/ HTTP/1.1"));
        QVERIFY(request.contains("client_id=public-client"));
        QVERIFY(request.contains("expires_in=2592000"));
        const QByteArray response = R"({"access_token":"loopback-session","token_type":"Bearer","expires_in":3600})";
        peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
        peer->flush();
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(store.values.value(0), QStringLiteral("loopback-session"));
        QFile metadata(dir.filePath(QStringLiteral("sessions.json")));
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto entry = QJsonDocument::fromJson(metadata.readAll()).object().value("sessions").toArray().first().toObject();
        const QDateTime expires = QDateTime::fromString(entry.value("expires_at").toString(), Qt::ISODateWithMs);
        const qint64 remaining = QDateTime::currentDateTimeUtc().secsTo(expires);
        QVERIFY(remaining >= 3595 && remaining <= 3600);

        QVERIFY(manager.signIn(QStringLiteral("other@test.invalid"), QStringLiteral("dummy-password")));
        QTRY_VERIFY(server.hasPendingConnections());
        peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable() > 0);
        peer->readAll();
        const QByteArray challenge = R"({"verification_workflow":{"id":"offline"}})";
        peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(challenge.size()) + "\r\nConnection: close\r\n\r\n" + challenge);
        peer->flush();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 1);
        QVERIFY(!manager.error().isEmpty());
        QCOMPARE(store.values.size(), 1);
    }

    void deviceApprovalPollsThenRetriesOAuthBeforeSaving()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString path = dir.filePath(QStringLiteral("api.json"));
        QVERIFY(writeApprovalConfig(path, http.server.serverPort()));
        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(path, &error), qPrintable(error));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        http.responses = {
            R"({"verification_workflow":{"id":"workflow"}})",
            R"({"id":"machine"})",
            R"({"context":{"sheriff_challenge":{"id":"push-challenge","type":"prompt","status":"issued"}}})",
            R"({"challenge_status":"pending"})",
            R"({"challenge_status":"validated"})",
            R"({"type_context":{"result":"workflow_status_approved"}})",
            R"({"access_token":"approved-token","token_type":"Bearer","expires_in":3600})",
            R"({"results":[]})"
        };
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.signIn(QStringLiteral("owner@test.invalid"), QStringLiteral("test-only-password")));
        QTRY_VERIFY_WITH_TIMEOUT(manager.sessionActive(), 20000);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(store.values.value(0), QStringLiteral("approved-token"));
        QCOMPARE(http.requests.size(), http.responses.size());
        QVERIFY(http.requests.at(0).startsWith("POST /login/"));
        QVERIFY(http.requests.at(1).contains("\"workflow_id\":\"workflow\""));
        QVERIFY(http.requests.at(2).startsWith("GET /inquiry/machine/"));
        QVERIFY(http.requests.at(3).startsWith("GET /push/push-challenge/"));
        QVERIFY(http.requests.at(5).startsWith("POST /inquiry/machine/"));
        QVERIFY(http.requests.at(6).startsWith("POST /login/"));
        QVERIFY(http.requests.at(7).startsWith("GET /accounts/"));
        QVERIFY(http.requests.at(7).contains("Authorization: Bearer approved-token"));
    }

    void directPushChallengeCanYieldTokensAfterApproval()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString path = dir.filePath(QStringLiteral("api.json"));
        QVERIFY(writeApprovalConfig(path, http.server.serverPort()));
        RHNetworkEngine network;
        QVERIFY(network.loadConfiguration(path));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        http.responses = {R"({"challenge_id":"direct-push","type":"device_approval"})",
                          R"({"challenge_status":"validated"})",
                          R"({"access_token":"direct-token","token_type":"Bearer","expires_in":1800})",
                          R"({"results":[]})"};
        QVERIFY(manager.signIn(QStringLiteral("owner@test.invalid"), QStringLiteral("test-only-password")));
        QTRY_VERIFY_WITH_TIMEOUT(manager.sessionActive(), 6000);
        QCOMPARE(store.values.value(0), QStringLiteral("direct-token"));
        QCOMPARE(http.requests.size(), 4);
        QVERIFY(http.requests.at(1).startsWith("GET /push/direct-push/"));
        QVERIFY(!manager.approvalPending());
        QCOMPARE(manager.approvalSecondsRemaining(), 0);
    }

    void approvalTimeoutAbortsOutstandingRequestWithoutSaving()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString path = dir.filePath(QStringLiteral("api.json"));
        QVERIFY(writeApprovalConfig(path, http.server.serverPort()));
        RHNetworkEngine network;
        QVERIFY(network.loadConfiguration(path));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store,
                                 nullptr, 180, 30);
        http.responses = {R"({"challenge":{"id":"pending-push","type":"prompt"}})"};
        QVERIFY(manager.signIn(QStringLiteral("owner@test.invalid"), QStringLiteral("test-only-password")));
        QTRY_VERIFY(manager.approvalPending());
        QCOMPARE(manager.approvalSecondsRemaining(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
        QCOMPARE(manager.error(), QStringLiteral("Device approval timed out."));
        QVERIFY(!manager.approvalPending());
        QCOMPARE(manager.approvalSecondsRemaining(), 0);
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(store.values.isEmpty());
        QVERIFY(!manager.sessionActive());
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("sessions.json"))));
    }

    void cancelledApprovalCannotSaveLateTokens()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString path = dir.filePath(QStringLiteral("api.json"));
        QVERIFY(writeApprovalConfig(path, http.server.serverPort()));
        RHNetworkEngine network;
        QVERIFY(network.loadConfiguration(path));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        http.responses = {R"({"challenge_id":"pending-push","mfa_type":"push"})"};
        QVERIFY(manager.signIn(QStringLiteral("owner@test.invalid"), QStringLiteral("test-only-password")));
        QTRY_VERIFY(manager.approvalPending());
        manager.cancelSignIn();
        QVERIFY(!manager.busy());
        QVERIFY(!manager.approvalPending());
        QCOMPARE(manager.approvalSecondsRemaining(), 0);
        QTest::qWait(100);
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(store.values.isEmpty());
    }

    void declinedPushFailsClosed()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString path = dir.filePath(QStringLiteral("api.json"));
        QVERIFY(writeApprovalConfig(path, http.server.serverPort()));
        RHNetworkEngine network;
        QVERIFY(network.loadConfiguration(path));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        http.responses = {R"({"challenge_id":"declined-push","type":"device_approval"})",
                          R"({"challenge_status":"denied"})"};
        QVERIFY(manager.signIn(QStringLiteral("owner@test.invalid"), QStringLiteral("test-only-password")));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.error().contains(QStringLiteral("declined")));
        QVERIFY(!manager.approvalPending());
        QVERIFY(store.values.isEmpty());
    }

    void verifiedStartUsesOnlyReadSecretAndConfiguredEndpoint()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir;
        QFile config(dir.filePath(QStringLiteral("api.json")));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QStringLiteral(R"json({"Headers":{"Authorized":{"Authorization":"{token_type} {access_token}"}},"Endpoints":{"Accounts":{"URL":"http:\/\/127.0.0.1:%1/accounts/","Method":"GET","BodyEncoding":"none","HeaderSets":["Authorized"]}}})json")
                         .arg(server.serverPort()).toUtf8());
        config.close();
        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(config.fileName(), &error), qPrintable(error));
        FakeCredentialStore store;
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("mock-token"), 3600));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(manager.startSession(0));
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable() > 0);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("GET /accounts/ HTTP/1.1"));
        QVERIFY(request.contains(QByteArray("Authorization: Bearer ") + store.values.value(0).toUtf8()));
        QVERIFY(!manager.sessionActive());
        const QByteArray response = R"({"results":[{"account_number":"1234","type":"cash"}]})";
        peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
        peer->flush();
        QTRY_VERIFY(manager.sessionActive());
        QCOMPARE(manager.accounts().size(), 1);
        manager.clearActiveSession();
        QVERIFY(!manager.sessionActive());
        QVERIFY(!store.values.value(0).isEmpty());
    }
};

QTEST_MAIN(RHSessionManagerTest)
#include "test_rhsessionmanager.moc"
