#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include "rhbrowserlogin.h"
#include "rhoauthgrant.h"
#include "securecredentiallocker.h"

#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkReply>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest/QtTest>

class TestLocker final : public SecureCredentialLocker
{
public:
    explicit TestLocker(const QTemporaryDir &dir)
        : SecureCredentialLocker(dir.filePath(QStringLiteral("profiles.dat")),
              [](bool, KeyCompletion done) { done(true, QByteArray(32, 'k'), {}); }) {}
};

class FakeCredentialStore final : public RHCredentialStore
{
public:
    QHash<int, QString> values;
    bool synchronousWrite = false;
    bool failWrite = false;
    bool failRead = false;
    bool failRemove = false;
    bool holdReads = false;
    bool holdWrites = false;
    QVector<std::function<void()>> pendingReads;
    QVector<std::function<void()>> pendingWrites;
    int reads = 0;
    int writes = 0;
    std::function<void()> onWrite;
    void write(int index, const QString &token, Completion done) override
    {
        ++writes;
        if (onWrite) onWrite();
        if (holdWrites) {
            pendingWrites.append([this, index, token, done = std::move(done)] {
                values.insert(index, token); done(true, {});
            });
            return;
        }
        if (synchronousWrite) {
            if (!failWrite) values.insert(index, token);
            done(!failWrite, failWrite ? QStringLiteral("Keychain unavailable") : QString());
            return;
        }
        QTimer::singleShot(0, this, [this, index, token, done = std::move(done)]() {
            if (!failWrite)
                values.insert(index, token);
            done(!failWrite, failWrite ? QStringLiteral("Keychain unavailable") : QString());
        });
    }
    void read(int index, ReadCompletion done) override
    {
        ++reads;
        if (holdReads) {
            const QString snapshot = values.value(index);
            pendingReads.append([done = std::move(done), snapshot] { done(true, snapshot, {}); });
            return;
        }
        QTimer::singleShot(0, this, [this, index, done = std::move(done)]() {
            done(!failRead, failRead ? QString() : values.value(index),
                 failRead ? QStringLiteral("Keychain unavailable") : QString());
        });
    }
    void remove(int index, Completion done) override
    {
        QTimer::singleShot(0, this, [this, index, done = std::move(done)]() {
            if (!failRemove)
                values.remove(index);
            done(!failRemove, failRemove ? QStringLiteral("Keychain unavailable") : QString());
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
        {QStringLiteral("UserProfile"), route(QStringLiteral("/user/"), QStringLiteral("GET"), QStringLiteral("none"))},
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
    QVector<int> statusCodes;
    QPointer<QTcpSocket> lastPeer;

    ApprovalServer()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            auto *peer = server.nextPendingConnection();
            lastPeer = peer;
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
                    const QByteArray status = ordinal < statusCodes.size()
                        ? QByteArray::number(statusCodes.at(ordinal)) + " Test Response"
                        : QByteArray(ordinal == 0 ? "403 Forbidden" : "200 OK");
                    peer->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                                + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    peer->flush();
                    peer->disconnectFromHost();
                }
            });
        });
    }
};

struct BrowserFixture {
    QTemporaryDir dir;
    ApprovalServer http;
    RHNetworkEngine network{nullptr, true};
    FakeCredentialStore store;
    TestLocker locker{dir};
    std::unique_ptr<RHSessionManager> manager;
    RHBrowserLogin browser{{}, nullptr, RHBrowserLogin::BackendMode::Test};
    bool ready = false;
    bool seedOAuth(int lifetime = -60, bool refreshHint = true) {
        const auto now = QDateTime::currentDateTimeUtc();
        RHOAuthGrant grant;
        if (!RHOAuthGrant::parseResponse(QJsonObject{{"access_token", "old-access"}, {"refresh_token", "old-refresh"},
                {"scope", "internal"}, {"token_type", "Bearer"}, {"expires_in", 3600}},
                "client", "device", now.addSecs(lifetime - 3600), &grant)) return false;
        grant.accountId = "owner-id";
        store.values[0] = grant.serialize();
        QFile metadata(dir.filePath("sessions.json"));
        if (!metadata.open(QIODevice::WriteOnly)) return false;
        metadata.write(QJsonDocument(QJsonObject{{"sessions", QJsonArray{QJsonObject{
            {"index", 0}, {"account_name", "owner"}, {"account_id", "owner-id"},
            {"created_at", grant.obtainedAt.toString(Qt::ISODateWithMs)},
            {"expires_at", grant.expiresAt.toString(Qt::ISODateWithMs)}, {"token_type", "Bearer"},
            {"can_refresh", refreshHint}}}}}).toJson());
        metadata.close();
        QFile config(dir.filePath("api.json"));
        if (!config.open(QIODevice::ReadOnly)) return false;
        auto root = QJsonDocument::fromJson(config.readAll()).object(); config.close();
        auto endpoints = root["Endpoints"].toObject();
        endpoints["TokenRefresh"] = QJsonObject{{"URL", QString("http://127.0.0.1:%1/oauth2/token/").arg(http.server.serverPort())},
            {"Method", "POST"}, {"BodyEncoding", "form"}, {"HeaderSets", QJsonArray{"Form"}}};
        root["Endpoints"] = endpoints;
        if (!config.open(QIODevice::WriteOnly)) return false;
        config.write(QJsonDocument(root).toJson()); config.close();
        return network.loadConfiguration(config.fileName()) && manager->load();
    }
    bool enableHoldings() {
        QFile config(dir.filePath("api.json"));
        if (!config.open(QIODevice::ReadOnly)) return false;
        auto root = QJsonDocument::fromJson(config.readAll()).object(); config.close();
        auto endpoints = root["Endpoints"].toObject();
        for (const auto &pair : {qMakePair(QString("CachedPositions"), QString("/positions/?nonzero=true")),
                                qMakePair(QString("Instruments"), QString("/instruments/"))})
            endpoints[pair.first] = QJsonObject{{"URL", QString("http://127.0.0.1:%1").arg(http.server.serverPort()) + pair.second},
                {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        root["Endpoints"] = endpoints;
        if (!config.open(QIODevice::WriteOnly)) return false;
        config.write(QJsonDocument(root).toJson()); config.close(); return network.loadConfiguration(config.fileName());
    }
    BrowserFixture() {
        if (!http.server.listen(QHostAddress::LocalHost)) return;
        const QString config = dir.filePath("api.json");
        if (!writeApprovalConfig(config, http.server.serverPort()) || !network.loadConfiguration(config)) return;
        manager = std::make_unique<RHSessionManager>(network, dir.filePath("sessions.json"),
            &store, nullptr, 180000, 2500, &locker);
        manager->setBrowserLogin(&browser);
        ready = true;
    }
};

class RHSessionManagerTest : public QObject
{
    Q_OBJECT
private slots:
    void cancelledReadCannotEraseSynchronousReplacementSaveFailure()
    {
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("old", "old-token", 3600));
        QTRY_VERIFY(!f.manager->busy());
        f.store.holdReads = true;
        QVERIFY(f.manager->initializeSession(0));
        QCOMPARE(f.store.pendingReads.size(), 1);
        bool replaced = false;
        connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
            if (f.manager->busy() || replaced) return;
            replaced = true;
            f.store.pendingReads.takeFirst()();
            f.store.synchronousWrite = true; f.store.failWrite = true;
            QVERIFY(f.manager->saveAuthenticatedSession("replacement", "replacement-token", 3600));
        });
        f.manager->cancelSignIn();
        QVERIFY(replaced); QCOMPARE(f.manager->error(), QString("Keychain unavailable"));
    }

    void legacySaveAdmissionRejectsReentrantCredentialOperations()
    {
        BrowserFixture f; QVERIFY(f.ready);
        RHSessionManager other(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        f.store.holdWrites = true;
        QVERIFY(!f.manager->signIn({}, {}));
        QVERIFY(!f.manager->error().isEmpty());
        bool observed = false;
        bool innerAccepted = true;
        bool deleteAccepted = true;
        bool loadAccepted = true;
        bool initializeAccepted = true;
        QString errorAfterReentry;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (observed || !f.manager->error().isEmpty()) return;
            observed = true;
            innerAccepted = f.manager->saveAuthenticatedSession("owner", "inner-token", 3600);
            deleteAccepted = f.manager->deleteSavedSession(0);
            loadAccepted = f.manager->load();
            initializeAccepted = f.manager->initializeSession(0);
            errorAfterReentry = f.manager->error();
        });
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "outer-token", 3600));
        QVERIFY(observed);
        QVERIFY(!innerAccepted);
        QVERIFY(!deleteAccepted);
        QVERIFY(!loadAccepted);
        QVERIFY(!initializeAccepted);
        QVERIFY(errorAfterReentry.isEmpty());
        QCOMPARE(f.store.pendingWrites.size(), 1);
        QCOMPARE(f.store.writes, 1);
        QVERIFY(!other.saveAuthenticatedSession("intruder", "intruder-token", 3600));
        QVERIFY(!other.deleteSavedSession(0));
        QCOMPARE(f.store.pendingWrites.size(), 1);
        auto complete = f.store.pendingWrites.takeFirst(); complete(); complete = {};
        QVERIFY(!f.manager->busy());
        QCOMPARE(f.manager->rowCount(), 1);
        QCOMPARE(f.store.values[0], QString("outer-token"));
        QVERIFY(other.load()); QCOMPARE(other.rowCount(), 1);
        QVERIFY(other.deleteSavedSession(0)); QTRY_VERIFY(!other.busy());
        f.store.holdWrites = false;
        QVERIFY(other.saveAuthenticatedSession("replacement", "replacement-token", 3600));
        QTRY_VERIFY(!other.busy());
        QCOMPARE(f.store.values[0], QString("replacement-token"));
        QVERIFY(f.manager->load()); QCOMPARE(f.manager->rowCount(), 1);
        QCOMPARE(f.manager->data(f.manager->index(0), RHSessionManager::AccountNameRole).toString(), QString("replacement"));
    }
    void heldReadCancellationIdleObserverCannotDestroyOrClobberReplacement_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("replacement") << false;
        QTest::newRow("destroy") << true;
    }
    void heldReadCancellationIdleObserverCannotDestroyOrClobberReplacement()
    {
        QFETCH(bool, destroy);
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "owner-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->saveAuthenticatedSession("other", "other-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        f.store.holdReads = true;
        QVERIFY(f.manager->initializeSession(0));
        QCOMPARE(f.store.pendingReads.size(), 1);
        bool observed = false;
        QString replacementError;
        connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
            if (observed || f.manager->busy()) return;
            observed = true;
            if (destroy) { f.manager.reset(); return; }
            auto complete = f.store.pendingReads.takeFirst(); complete(); complete = {};
            QVERIFY(f.manager->initializeSession(1));
            QVERIFY(!f.manager->signIn({}, {}));
            replacementError = f.manager->error();
            QVERIFY(!replacementError.isEmpty());
        });
        f.manager->cancelSignIn();
        QVERIFY(observed);
        if (destroy) {
            QVERIFY(!f.manager);
            auto complete = f.store.pendingReads.takeFirst(); complete(); complete = {};
        } else {
            QVERIFY(f.manager->busy());
            QCOMPARE(f.store.pendingReads.size(), 1);
            QCOMPARE(f.manager->error(), replacementError);
            f.manager->cancelSignIn();
            auto complete = f.store.pendingReads.takeFirst(); complete(); complete = {};
        }
    }
    void sessionReadAdmissionPrecedesClearingError()
    {
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "owner-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        f.store.holdReads = true;
        f.store.holdWrites = true;
        QVERIFY(!f.manager->signIn({}, {}));
        bool observed = false;
        bool saveAccepted = true;
        bool deleteAccepted = true;
        bool loadAccepted = true;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (observed || !f.manager->error().isEmpty()) return;
            observed = true;
            saveAccepted = f.manager->saveAuthenticatedSession("owner", "intruder-token", 3600);
            deleteAccepted = f.manager->deleteSavedSession(0);
            loadAccepted = f.manager->load();
        });
        QVERIFY(f.manager->initializeSession(0));
        QVERIFY(observed);
        QVERIFY(!saveAccepted);
        QVERIFY(!deleteAccepted);
        QVERIFY(!loadAccepted);
        QVERIFY(f.manager->error().isEmpty());
        QCOMPARE(f.store.pendingReads.size(), 1);
        QCOMPARE(f.store.pendingWrites.size(), 0);
        f.manager->cancelSignIn();
        auto complete = f.store.pendingReads.takeFirst(); complete(); complete = {};
    }
    void oauthUnserializableGrantNeverReachesCredentialStore_data()
    {
        QTest::addColumn<bool>("renewal");
        QTest::newRow("oversized-identity") << false;
        QTest::newRow("bounded-response-with-inherited-fields") << true;
    }
    void oauthUnserializableGrantNeverReachesCredentialStore()
    {
        QFETCH(bool, renewal);
        BrowserFixture f; QVERIFY(f.ready);
        const QJsonObject initial{{"access_token", "initial-access"}, {"refresh_token", "initial-refresh"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 3600}};
        QString original;
        if (renewal) {
            QVERIFY(f.seedOAuth()); original = f.store.values[0];
            QJsonObject response{{"access_token", "renewed-access"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"extension", ""}};
            const qsizetype padding = 512 * 1024 - 1 - QJsonDocument(response).toJson(QJsonDocument::Compact).size();
            response["extension"] = QString(padding, 'p');
            RHOAuthGrant previous, parsed;
            QVERIFY(RHOAuthGrant::deserialize(original, &previous));
            QVERIFY(RHOAuthGrant::parseResponse(response, "client", "device", QDateTime::currentDateTimeUtc(), &parsed, nullptr, &previous));
            QVERIFY(parsed.serialize().isEmpty());
            f.http.responses.append(QJsonDocument(response).toJson(QJsonDocument::Compact));
        }
        const QString identity = renewal ? QString("owner-id") : QString(1024 * 1024, 'i');
        f.http.responses.append(QJsonDocument(QJsonObject{{"id", identity}, {"username", "owner"}}).toJson(QJsonDocument::Compact));
        f.http.responses.append(R"({"results":[]})");
        if (renewal) f.http.responses.append(R"({"results":[]})");
        f.http.statusCodes.fill(200, f.http.responses.size());
        if (renewal) QVERIFY(f.manager->initializeSession(0));
        else QVERIFY(f.manager->acceptOAuthGrant(initial, "client", "device"));
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.writes, 0);
        QVERIFY(!f.manager->error().isEmpty());
        QVERIFY(!f.manager->sessionActive());
        if (renewal) QCOMPARE(f.store.values[0], original);
        else { QVERIFY(f.store.values.isEmpty()); QCOMPARE(f.manager->rowCount(), 0); }
    }
    void credentialReadReservationExcludesConcurrentRotationAndMutators()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        RHSessionManager other(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        f.store.holdReads = true;
        QVERIFY(f.manager->initializeSession(0));
        QCOMPARE(f.store.pendingReads.size(), 1);
        const int reads = f.store.reads;
        other.initializeSession(0);
        QCOMPARE(f.store.reads, reads); // No second OLD snapshot may escape the reservation.
        QVERIFY(!other.deleteSavedSession(0));
        QVERIFY(!other.saveAuthenticatedSession("intruder", "other-token", 3600));
        QCOMPARE(f.store.writes, 0);
        f.manager->cancelSignIn();
        // A held store callback remains an owner even after cancellation/destruction.
        f.manager.reset();
        QVERIFY(!other.deleteSavedSession(0));
        auto complete = f.store.pendingReads.takeFirst(); complete(); complete = {};
        QVERIFY(other.deleteSavedSession(0)); QTRY_VERIFY(!other.busy());
    }
    void oauthCompletedRotationRejectsStaleModelAndOldSecret()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        RHSessionManager stale(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        const QString predecessor = f.store.values[0];
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes.fill(200, 4);
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->sessionActive());
        const int reads = f.store.reads;
        stale.initializeSession(0); QTRY_VERIFY(!stale.busy());
        QCOMPARE(f.store.reads, reads); QCOMPARE(f.http.requests.size(), 4);
        // Even a restored old secure result must not match the committed revision binding.
        QVERIFY(stale.load()); f.store.values[0] = predecessor;
        stale.initializeSession(0); QTRY_VERIFY(!stale.busy());
        QCOMPARE(f.http.requests.size(), 4);
    }
    void oauthHeldWriteReservationSurvivesOwnerDestruction()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        RHSessionManager other(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        f.store.holdWrites = true;
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes.fill(200, 3);
        QVERIFY(f.manager->initializeSession(0)); QTRY_COMPARE(f.store.pendingWrites.size(), 1);
        f.manager.reset();
        QVERIFY(!other.deleteSavedSession(0));
        QVERIFY(!other.saveAuthenticatedSession("replacement", "replacement-token", 3600));
        auto complete = f.store.pendingWrites.takeFirst(); complete(); complete = {};
        f.store.holdWrites = false;
        QVERIFY(other.load()); QVERIFY(other.deleteSavedSession(0)); QTRY_VERIFY(!other.busy());
        QVERIFY(other.saveAuthenticatedSession("replacement", "replacement-token", 3600)); QTRY_VERIFY(!other.busy());
        QCOMPARE(f.store.values[0], QString("replacement-token"));
        QVERIFY(other.load()); QCOMPARE(other.rowCount(), 1);
        QCOMPARE(other.data(other.index(0), RHSessionManager::AccountNameRole).toString(), QString("replacement"));
    }
    void oauthActivationFailureObserverCannotDestroyOrIdleReplacement_data()
    {
        QTest::addColumn<QString>("branch"); QTest::addColumn<bool>("destroy");
        for (const QString &branch : {QString("send"), QString("http"), QString("identity")}) {
            QTest::newRow(qPrintable(branch + "-replace")) << branch << false;
            QTest::newRow(qPrintable(branch + "-destroy")) << branch << true;
        }
    }
    void oauthActivationFailureObserverCannotDestroyOrIdleReplacement()
    {
        QFETCH(QString, branch); QFETCH(bool, destroy);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        QVERIFY(f.manager->saveAuthenticatedSession("other", "other-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})",
            branch == "identity" ? QByteArray(R"({"results":[{}]})") : QByteArray("{}")};
        f.http.statusCodes = {200, 200, 200, 401};
        if (branch == "identity") f.http.statusCodes[3] = 200;
        if (branch == "send") connect(f.manager.get(), &RHSessionManager::sessionSaved, f.manager.get(), [&](int slot) {
            if (slot != 0) return;
            QFile file(f.dir.filePath("api.json")); QVERIFY(file.open(QIODevice::ReadOnly));
            auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
            auto endpoints = root["Endpoints"].toObject(); endpoints.remove("Accounts"); root["Endpoints"] = endpoints;
            QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(root).toJson()); file.close();
            QVERIFY(f.network.loadConfiguration(file.fileName()));
        });
        bool observed = false;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (observed || f.manager->error().isEmpty()) return;
            observed = true;
            if (destroy) { f.manager.reset(); return; }
            f.manager->clearActiveSession(); f.store.holdReads = true;
            QVERIFY(f.manager->initializeSession(1));
        });
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(observed);
        if (!destroy) { QVERIFY(f.manager->busy()); QCOMPARE(f.store.pendingReads.size(), 1); }
        RHOAuthGrant saved; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &saved));
        QCOMPARE(saved.refreshToken(), QByteArray("new-refresh"));
        QCOMPARE(f.http.requests.size(), branch == "send" ? 3 : 4);
    }
    void legacyRollbackRetainsCredentialReservationAcrossErrorObservers()
    {
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "old-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        RHSessionManager other(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        QLockFile metadataLock(f.dir.filePath("sessions.json.lock")); QVERIFY(metadataLock.tryLock());
        bool observed = false;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (observed || f.manager->error().isEmpty()) return;
            observed = true; metadataLock.unlock(); f.manager->clearActiveSession();
            QVERIFY(!other.saveAuthenticatedSession("owner", "competing-token", 3600));
        });
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "temporary-token", 3600));
        QTRY_VERIFY(observed); QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.values[0], QString("old-token"));
    }
    void staleBrowserCommitCannotOverwriteReusedSlotOrResurrectDeletedProfile_data()
    {
        QTest::addColumn<bool>("reuse"); QTest::newRow("deleted") << false; QTest::newRow("reused") << true;
    }
    void staleBrowserCommitCannotOverwriteReusedSlotOrResurrectDeletedProfile()
    {
        QFETCH(bool, reuse);
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "old-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        RHSessionManager stale(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        QVERIFY(f.manager->deleteSavedSession(0)); QTRY_VERIFY(!f.manager->busy());
        if (reuse) {
            QVERIFY(f.manager->saveAuthenticatedSession("replacement", "replacement-token", 3600));
            QTRY_VERIFY(!f.manager->busy());
        }
        const int writes = f.store.writes;
        const int reads = f.store.reads;
        QVERIFY(!stale.deleteSavedSession(0));
        QVERIFY(!stale.saveAuthenticatedSession("owner", "stale-legacy-token", 3600));
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        QVERIFY(stale.acceptOAuthGrant(QJsonObject{{"access_token", "stale-access"}, {"refresh_token", "stale-refresh"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 3600}}, "client", "device"));
        QTRY_VERIFY(!stale.busy());
        QCOMPARE(f.store.writes, writes); QCOMPARE(f.store.reads, reads);
        QCOMPARE(f.store.values.contains(0), reuse);
        if (reuse) QCOMPARE(f.store.values[0], QString("replacement-token"));
        QVERIFY(f.manager->load()); QCOMPARE(f.manager->rowCount(), reuse ? 1 : 0);
    }
    void credentialReservationRejectsSeparateProcessMutators()
    {
        const QString childPath = qEnvironmentVariable("RH_TEST_RESERVED_METADATA");
        if (!childPath.isEmpty()) {
            QTemporaryDir dir; FakeCredentialStore store; TestLocker locker(dir); RHNetworkEngine network(nullptr, true);
            RHSessionManager manager(network, childPath, &store, nullptr, 180000, 2500, &locker);
            QVERIFY(!manager.saveAuthenticatedSession("owner", "child-token", 3600));
            QVERIFY(!manager.deleteSavedSession(0)); QCOMPARE(store.reads, 0); QCOMPARE(store.writes, 0);
            return;
        }
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        f.store.holdWrites = true;
        // An outstanding legacy write must also retain the process reservation.
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "held-token", 3600));
        QTRY_COMPARE(f.store.pendingWrites.size(), 1); f.manager.reset();
        QProcess child; auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("RH_TEST_RESERVED_METADATA", f.dir.filePath("sessions.json"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {"credentialReservationRejectsSeparateProcessMutators"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardOutput().constData());
        auto complete = f.store.pendingWrites.takeFirst(); complete(); complete = {};
        QLockFile probe(f.dir.filePath("sessions.json.credentials.lock")); QVERIFY(probe.tryLock(0));
    }
    void tradingReadCannotCrossReusedProfileSlot_data()
    {
        QTest::addColumn<bool>("heldRead"); QTest::newRow("before-read") << false; QTest::newRow("during-read") << true;
    }
    void tradingReadCannotCrossReusedProfileSlot()
    {
        QFETCH(bool, heldRead);
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})"}; f.http.statusCodes = {200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "owner-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy()); QVERIFY(f.manager->sessionActive());
        RHSessionManager other(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        bool completed = false;
        const auto acquired = [&](bool ok, RHSessionManager::TradingSession session, QString) {
            QVERIFY(!ok); QVERIFY(session.token.isEmpty()); completed = true;
            f.manager->releaseTradingSession();
        };
        if (heldRead) {
            f.store.holdReads = true; f.manager->requestTradingSession(acquired);
            QCOMPARE(f.store.pendingReads.size(), 1); f.store.holdReads = false;
        }
        QVERIFY(other.deleteSavedSession(0)); QTRY_VERIFY(!other.busy());
        QVERIFY(other.saveAuthenticatedSession("replacement", "replacement-token", 3600)); QTRY_VERIFY(!other.busy());
        if (heldRead) { auto complete = f.store.pendingReads.takeFirst(); complete(); }
        else f.manager->requestTradingSession(acquired);
        QTRY_VERIFY(completed); QVERIFY(!f.manager->busy());
    }
    void oauthExpiredInitializationRenewsSelectedProfileWithoutBrowser()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200, 200};
        QVERIFY(f.manager->initializeSession(0));
        QVERIFY(!f.manager->initializeSession(0));
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY2(f.manager->sessionActive(), qPrintable(f.manager->error()));
        QCOMPARE(f.http.requests.size(), 4);
        QVERIFY(f.http.requests[0].startsWith("POST /oauth2/token/"));
        QVERIFY(f.http.requests[0].contains("grant_type=refresh_token"));
        QVERIFY(f.http.requests[0].contains("refresh_token=old-refresh"));
        QVERIFY(!f.http.requests[0].contains("password"));
        for (int i = 1; i < f.http.requests.size(); ++i)
            QVERIFY(f.http.requests[i].contains("Authorization: Bearer new-access"));
        RHOAuthGrant saved; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &saved));
        QCOMPARE(saved.refreshToken(), QByteArray("new-refresh"));
        QCOMPARE(saved.accountId, QString("owner-id"));
        QVERIFY(!f.browser.active());
        QCOMPARE(f.manager->data(f.manager->index(0), RHSessionManager::CanRefreshRole).toBool(), true);
    }
    void oauthRotatedGrantCommitFailureNeverRestoresOrRetriesOldRefresh_data()
    {
        QTest::addColumn<bool>("storeFails");
        QTest::newRow("metadata-fails") << false;
        QTest::newRow("store-fails") << true;
    }
    void oauthRotatedGrantCommitFailureNeverRestoresOrRetriesOldRefresh()
    {
        QFETCH(bool, storeFails);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        const auto oldSecret = f.store.values[0];
        QLockFile lock(f.dir.filePath("sessions.json.lock"));
        f.store.failWrite = storeFails;
        if (!storeFails) f.store.onWrite = [&] { if (!lock.isLocked()) QVERIFY(lock.tryLock()); };
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"rotated-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->initializeSession(0));
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.writes, 1);
        QVERIFY(!f.manager->sessionActive());
        QVERIFY(!f.manager->error().isEmpty());
        if (storeFails) QCOMPARE(f.store.values[0], oldSecret);
        else { RHOAuthGrant saved; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &saved));
               QCOMPARE(saved.refreshToken(), QByteArray("rotated-refresh")); }
        lock.unlock();
        QVERIFY(!f.manager->initializeSession(0));
        QVERIFY(!f.manager->error().contains("rollback", Qt::CaseInsensitive));
        QCOMPARE(f.http.requests.size(), 3);
    }
    void oauthStoreFailureQuarantineSurvivesRestart()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        f.store.failWrite = true;
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"rotated-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        f.manager.reset();
        f.manager = std::make_unique<RHSessionManager>(f.network, f.dir.filePath("sessions.json"),
            &f.store, nullptr, 180000, 2500, &f.locker);
        QVERIFY(f.manager->initializeSession(0));
        QTRY_VERIFY(!f.manager->busy() || f.http.requests.size() > 3);
        QCOMPARE(f.http.requests.size(), 3);
        QVERIFY(!f.manager->sessionActive());
        QVERIFY(!f.manager->error().isEmpty());
    }
    void oauthTradingLeaseExtractsBearerAndForbidsAuthMutation()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        const QByteArray accounts = R"({"results":[{"id":"brokerage","account_number":"123","type":"cash","cash":"1.00"}]})";
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", accounts, accounts};
        f.http.statusCodes = {200, 200, 200, 200};
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->sessionActive());
        bool acquired = false;
        f.manager->requestTradingSession([&](bool ok, RHSessionManager::TradingSession session, QString error) {
            QVERIFY2(ok, qPrintable(error)); acquired = true;
            QCOMPARE(session.token, QByteArray("new-access"));
            QVERIFY(f.manager->isTradingSessionCurrent(session));
            QVERIFY(!f.manager->initializeSession(0));
            QVERIFY(!f.manager->acceptOAuthGrant(QJsonObject{}, "client", "device"));
            f.manager->clearActiveSession();
            QVERIFY(f.manager->isTradingSessionCurrent(session));
        });
        QTRY_VERIFY(acquired);
        f.manager->releaseTradingSession();
        QVERIFY(!f.manager->busy());
    }
    void oauthIdleActiveProfileRenewsNearExpiryOnly()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":90,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})",
            R"({"access_token":"idle-access","refresh_token":"idle-refresh","token_type":"Bearer","expires_in":3600})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes.fill(200, 8);
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->sessionActive());
        const auto credentialRecord = [&] {
            QFile file(f.dir.filePath("sessions.json"));
            if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
            return QJsonDocument::fromJson(file.readAll()).object()["sessions"].toArray().first().toObject();
        };
        const auto firstRecord = credentialRecord();
        const auto firstRevision = firstRecord["credential_revision"].toString().toULongLong();
        QVERIFY(firstRevision > 0);
        QVERIFY(!firstRecord["renewal_attempted_at"].toString().isEmpty());
        f.manager->checkSessionRenewal();
        f.manager->checkSessionRenewal();
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.http.requests.size(), 8);
        QVERIFY(f.http.requests[4].contains("refresh_token=new-refresh"));
        QVERIFY(f.http.requests[7].contains("Authorization: Bearer idle-access"));
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &grant));
        QCOMPARE(grant.refreshToken(), QByteArray("idle-refresh"));
        QCOMPARE(grant.effectiveScope, QString("internal"));
        f.manager->checkSessionRenewal();
        QCOMPARE(f.http.requests.size(), 8);
        const auto secondRecord = credentialRecord();
        const auto secondRevision = secondRecord["credential_revision"].toString().toULongLong();
        QVERIFY(secondRevision > firstRevision);
        QVERIFY(!secondRecord["renewal_attempted_at"].toString().isEmpty());
        QVERIFY(!secondRecord["renewal_pending"].toBool());
        QCOMPARE(secondRecord["credential_digest"].toString(), QString::fromLatin1(
            QCryptographicHash::hash(f.store.values[0].toUtf8(), QCryptographicHash::Sha256).toHex()));
        QVERIFY(f.manager->deleteSavedSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->saveAuthenticatedSession("replacement", "replacement-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(credentialRecord()["credential_revision"].toString().toULongLong() > secondRevision);
    }
    void oauthFailureObserverCannotIdleReplacementSession_data()
    {
        QTest::addColumn<bool>("readFails"); QTest::newRow("renewal-error") << false; QTest::newRow("store-read-error") << true;
    }
    void oauthFailureObserverCannotIdleReplacementSession()
    {
        QFETCH(bool, readFails);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        QVERIFY(f.manager->saveAuthenticatedSession("other", "other-token", 3600));
        QTRY_VERIFY(!f.manager->busy());
        f.http.responses = {R"({"error":"invalid_grant"})"}; f.http.statusCodes = {400};
        if (readFails) { f.http.responses.clear(); f.store.failRead = true; }
        bool replaced = false;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (replaced || f.manager->error().isEmpty()) return;
            replaced = true;
            f.store.failRead = false;
            f.manager->clearActiveSession();
            QVERIFY(f.manager->initializeSession(1));
        });
        QVERIFY(f.manager->initializeSession(0));
        QTRY_VERIFY(replaced);
        QTRY_COMPARE(f.http.requests.size(), readFails ? 1 : 2);
        QVERIFY(f.manager->busy());
        f.manager->cancelSignIn();
        QVERIFY(!f.manager->busy());
    }
    void oauthSavedObserverCannotActivateRetiredGeneration()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        QVERIFY(f.manager->saveAuthenticatedSession("other", "other-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes.fill(200, 4);
        connect(f.manager.get(), &RHSessionManager::sessionSaved, f.manager.get(), [&](int slot) {
            if (slot != 0) return;
            f.manager->clearActiveSession();
            QVERIFY(f.manager->initializeSession(1));
        });
        QVERIFY(f.manager->initializeSession(0));
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.http.requests.size(), 4);
        QVERIFY(f.http.requests[3].contains("Authorization: Bearer other-token"));
        QCOMPARE(f.manager->activeAccount(), QString("other"));
    }
    void oauthRenewalRejectsUntrustedResponses_data()
    {
        QTest::addColumn<QByteArray>("response"); QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("profile");
        const QByteArray owner = R"({"id":"owner-id","username":"owner"})";
        QTest::newRow("invalid-grant") << QByteArray(R"({"error":"invalid_grant","error_description":"sensitive-server-text"})") << 400 << owner;
        QTest::newRow("unauthorized") << QByteArray("{}") << 401 << owner;
        QTest::newRow("malformed") << QByteArray("not-json") << 200 << owner;
        QTest::newRow("reduced-scope") << QByteArray(R"({"access_token":"bad-access","refresh_token":"bad-refresh","token_type":"Bearer","expires_in":3600,"scope":"read"})") << 200 << owner;
        QTest::newRow("wrong-principal") << QByteArray(R"({"access_token":"bad-access","refresh_token":"bad-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})") << 200
            << QByteArray(R"({"id":"impostor","username":"owner"})");
    }
    void oauthRenewalRejectsUntrustedResponses()
    {
        QFETCH(QByteArray, response); QFETCH(int, status); QFETCH(QByteArray, profile);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth()); const auto before = f.store.values[0];
        f.http.responses = {response, profile, R"({"results":[]})"}; f.http.statusCodes = {status, 200, 200};
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(!f.manager->sessionActive()); QCOMPARE(f.store.writes, 0); QCOMPARE(f.store.values[0], before);
        QVERIFY(!f.manager->error().isEmpty()); QVERIFY(!f.manager->error().contains("sensitive-server-text"));
        QCOMPARE(f.http.requests.size(), profile.contains("impostor") ? 2 : 1);
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.http.requests.size(), profile.contains("impostor") ? 2 : 1);
    }
    void oauthOmittedRotationAndScopeRetainRealPreviousContext()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth(-60, false));
        f.http.responses = {R"({"access_token":"new-access","token_type":"Bearer","expires_in":3600})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes.fill(200, 4);
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy()); QVERIFY(f.manager->sessionActive());
        RHOAuthGrant saved; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &saved));
        QCOMPARE(saved.refreshToken(), QByteArray("old-refresh")); QCOMPARE(saved.effectiveScope, QString("internal"));
        QVERIFY(!saved.response.contains("refresh_token")); QVERIFY(!saved.response.contains("scope"));
        QCOMPARE(f.manager->roleNames().value(RHSessionManager::CanRefreshRole), QByteArray("canRefresh"));
    }
    void oauthCancelledRenewalCannotWriteOrActivateLateResponse()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth()); const auto before = f.store.values[0];
        QVERIFY(f.manager->initializeSession(0)); QTRY_COMPARE(f.http.requests.size(), 1);
        const auto peer = f.http.lastPeer;
        f.manager->cancelSignIn(); QVERIFY(!f.manager->busy());
        if (peer) {
            const QByteArray body = R"({"access_token":"late-access","refresh_token":"late-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})";
            peer->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body); peer->flush();
        }
        QTest::qWait(50); QVERIFY(!f.manager->sessionActive()); QCOMPARE(f.store.writes, 0); QCOMPARE(f.store.values[0], before);
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy()); QCOMPARE(f.http.requests.size(), 1);
    }
    void oauthCompleteGrantPreservesActualLongExpiryAcrossRestart()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"}; f.http.statusCodes = {200, 200};
        QVERIFY(f.manager->acceptOAuthGrant(QJsonObject{{"access_token", "access"}, {"refresh_token", "refresh"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 5184000}}, "client", "device"));
        QTRY_VERIFY(!f.manager->busy()); QCOMPARE(f.store.writes, 1);
        QVERIFY(f.manager->load()); QCOMPARE(f.manager->rowCount(), 1);
        RHOAuthGrant saved; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &saved));
        QCOMPARE(f.manager->data(f.manager->index(0), RHSessionManager::ExpiresAtRole).toString(), saved.expiresAt.toString(Qt::ISODateWithMs));
    }
    void oauthStaleManagerCannotDispatchConsumedRefreshToken()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        RHSessionManager stale(f.network, f.dir.filePath("sessions.json"), &f.store,
            nullptr, 180000, 2500, &f.locker);
        QVERIFY(f.manager->initializeSession(0)); QTRY_COMPARE(f.http.requests.size(), 1);
        QVERIFY(!stale.initializeSession(0));
        QTRY_VERIFY(!stale.busy() || f.http.requests.size() > 1);
        QCOMPARE(f.http.requests.size(), 1);
        QVERIFY(!stale.error().isEmpty());
        f.manager->cancelSignIn();
    }
    void initializationObserverCannotReplaceThenResumeOldStart()
    {
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "owner-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->saveAuthenticatedSession("other", "other-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        f.http.responses = {R"({"results":[]})", R"({"results":[]})"}; f.http.statusCodes = {200, 200};
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy()); QVERIFY(f.manager->sessionActive());
        bool replaced = false;
        connect(f.manager.get(), &RHSessionManager::sessionActiveChanged, f.manager.get(), [&] {
            if (f.manager->sessionActive() || replaced) return;
            replaced = true; QVERIFY(f.manager->initializeSession(1));
        });
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.http.requests.size(), 2); QCOMPARE(f.manager->activeAccount(), QString("other"));
    }
    void oauthMalformedRenewalMetadataFailsClosed_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        QTest::newRow("bad-attempt-date") << QString("renewal_attempted_at") << QJsonValue("invalid-date");
        QTest::newRow("nonboolean-refresh-hint") << QString("can_refresh") << QJsonValue("true");
        QTest::newRow("revision-without-secret-binding") << QString("credential_revision") << QJsonValue("1");
        QTest::newRow("negative-revision") << QString("credential_revision") << QJsonValue("-1");
        QTest::newRow("numeric-revision") << QString("credential_revision") << QJsonValue(1);
        QTest::newRow("unbound-digest") << QString("credential_digest") << QJsonValue(QString(64, 'a'));
        QTest::newRow("malformed-digest") << QString("credential_digest") << QJsonValue("invalid");
        QTest::newRow("unbound-pending-renewal") << QString("renewal_pending") << QJsonValue(true);
        QTest::newRow("corrupt-high-water") << QString("credential_revision_high_water") << QJsonValue("corrupt");
        QTest::newRow("negative-high-water") << QString("credential_revision_high_water") << QJsonValue("-1");
        QTest::newRow("numeric-high-water") << QString("credential_revision_high_water") << QJsonValue(1);
    }
    void oauthMalformedRenewalMetadataFailsClosed()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        QFile file(f.dir.filePath("sessions.json")); QVERIFY(file.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        if (field == "credential_revision_high_water") root[field] = value;
        else {
            auto rows = root["sessions"].toArray(); auto row = rows[0].toObject(); row[field] = value; rows[0] = row; root["sessions"] = rows;
        }
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(root).toJson()); file.close();
        QVERIFY(!f.manager->load()); QVERIFY(!f.manager->initializeSession(0)); QCOMPARE(f.http.requests.size(), 0);
    }
    void oauthRefreshHintCannotAuthenticateMalformedSecretOrSecondaryProfile_data()
    {
        QTest::addColumn<QString>("secret"); QTest::addColumn<bool>("secondary");
        QTest::newRow("malformed-envelope") << QString("{broken-json}") << false;
        QTest::newRow("bare-expired-token") << QString("old-access") << false;
        QTest::newRow("secondary-profile-bundle") << QString() << true;
    }
    void oauthRefreshHintCannotAuthenticateMalformedSecretOrSecondaryProfile()
    {
        QFETCH(QString, secret); QFETCH(bool, secondary);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        if (!secret.isEmpty()) f.store.values[0] = secret;
        if (secondary) {
            QFile file(f.dir.filePath("sessions.json")); QVERIFY(file.open(QIODevice::ReadOnly));
            auto root = QJsonDocument::fromJson(file.readAll()).object(); file.close();
            auto rows = root["sessions"].toArray(); auto row = rows[0].toObject(); row["token_type"] = "WebSecondaryBearer"; rows[0] = row; root["sessions"] = rows;
            QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(root).toJson()); file.close(); QVERIFY(f.manager->load());
        }
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(!f.manager->sessionActive()); QCOMPARE(f.http.requests.size(), 0); QCOMPARE(f.store.writes, 0);
    }
    void oauthCommitPublicationGateBlocksReplacementAndSupportsDestruction_data()
    {
        QTest::addColumn<bool>("destroy"); QTest::newRow("replacement") << false; QTest::newRow("destroy") << true;
    }
    void oauthCommitPublicationGateBlocksReplacementAndSupportsDestruction()
    {
        QFETCH(bool, destroy);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        f.store.synchronousWrite = true;
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes.fill(200, 4);
        bool observed = false;
        connect(f.manager.get(), &RHSessionManager::dataChanged, f.manager.get(), [&] {
            if (observed) return; observed = true;
            if (destroy) { f.manager.reset(); return; }
            f.manager->cancelSignIn(); f.manager->clearActiveSession();
            QVERIFY(f.manager->busy()); QVERIFY(!f.manager->initializeSession(0));
            QVERIFY(!f.manager->acceptOAuthGrant(QJsonObject{}, "client", "device"));
        });
        QVERIFY(f.manager->initializeSession(0));
        QTRY_VERIFY(observed); QTRY_VERIFY(!f.manager || !f.manager->busy());
        if (!destroy) QVERIFY(f.manager->sessionActive());
        RHOAuthGrant saved; QVERIFY(RHOAuthGrant::deserialize(f.store.values[0], &saved));
        QCOMPARE(saved.refreshToken(), QByteArray("new-refresh"));
    }
    void oauthQuarantineErrorObserverCannotIdleReplacementSession()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth());
        f.http.responses = {R"({"error":"invalid_grant"})"}; f.http.statusCodes = {400};
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->saveAuthenticatedSession("other", "other-token", 3600)); QTRY_VERIFY(!f.manager->busy());
        bool replaced = false;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (replaced || f.manager->error().isEmpty()) return;
            replaced = true; f.manager->clearActiveSession(); QVERIFY(f.manager->initializeSession(1));
        });
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(replaced); QTRY_COMPARE(f.http.requests.size(), 2);
        QVERIFY(f.manager->busy()); f.manager->cancelSignIn();
    }
    void oauthDuplicateAccountIdentityCannotPersistRotatedGrant()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.seedOAuth()); const auto before = f.store.values[0];
        f.http.responses = {R"({"access_token":"new-access","refresh_token":"new-refresh","token_type":"Bearer","expires_in":3600,"scope":"internal"})",
            R"({"id":"owner-id","username":"owner"})", R"({"results":[{"id":"duplicate"},{"id":"duplicate"}]})"};
        f.http.statusCodes.fill(200, 3);
        QVERIFY(f.manager->initializeSession(0)); QTRY_VERIFY(!f.manager->busy() || f.store.writes > 0);
        QCOMPARE(f.store.writes, 0); QCOMPARE(f.store.values[0], before); QVERIFY(!f.manager->sessionActive());
    }
    void oauthRejectsIncompleteGrantWithoutPersistence()
    {
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(!f.manager->acceptOAuthGrant(QJsonObject{{"access_token", "not-a-full-grant"}}, "client", "device"));
        QCOMPARE(f.store.writes, 0);
        QCOMPARE(f.manager->rowCount(), 0);
        QVERIFY(!f.manager->busy());
        QVERIFY(f.manager->metaObject()->indexOfMethod("acceptOAuthGrant(QJsonObject,QString,QString)") < 0);
    }
    void oauthGrantValidatesIdentityBeforeStoringCompleteBundle()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        const QJsonObject response{{"access_token", "fresh-access"}, {"refresh_token", "fresh-refresh"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 3600}, {"provider_extra", "preserved"}};
        QVERIFY(f.manager->acceptOAuthGrant(response, "client", "device"));
        QCOMPARE(f.store.writes, 0);
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.writes, 1);
        QCOMPARE(f.manager->rowCount(), 1);
        QVERIFY(f.store.values.value(0).startsWith('{'));
        QVERIFY(f.store.values.value(0).contains("fresh-refresh"));
        QVERIFY(f.store.values.value(0).contains("provider_extra"));
        QCOMPARE(f.http.requests.size(), 2);
        for (const auto &request : f.http.requests) QVERIFY(request.contains("Authorization: Bearer fresh-access"));
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto bytes = metadata.readAll();
        QVERIFY(!bytes.contains("fresh-access")); QVERIFY(!bytes.contains("fresh-refresh"));
        QCOMPARE(QJsonDocument::fromJson(bytes).object()["sessions"].toArray()[0].toObject()["can_refresh"].toBool(), true);
    }
    void metadataFailureObserverCannotReplaceBeforeRollback_data()
    {
        QTest::addColumn<bool>("rollbackFails");
        QTest::newRow("restored") << false;
        QTest::newRow("rollback-fails") << true;
    }
    void metadataFailureObserverCannotReplaceBeforeRollback()
    {
        QFETCH(bool, rollbackFails);
        BrowserFixture f; QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "old-token", 3600));
        QTRY_VERIFY(!f.manager->busy());
        QFile metadata(f.dir.filePath("sessions.json"));
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto before = metadata.readAll(); metadata.close();
        const auto oldExpiry = f.manager->data(f.manager->index(0), RHSessionManager::ExpiresAtRole);
        QLockFile lock(metadata.fileName() + ".lock"); QVERIFY(lock.tryLock());
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        bool observed = false;
        bool reconciledAtError = false;
        bool replacementAccepted = false;
        connect(f.manager.get(), &RHSessionManager::errorChanged, f.manager.get(), [&] {
            if (f.manager->error().isEmpty() || observed) return;
            observed = true;
            reconciledAtError = f.store.writes == 3
                && f.store.values.value(0) == (rollbackFails ? "new-token" : "old-token");
            f.manager->clearActiveSession();
            f.manager->cancelSignIn();
            replacementAccepted = f.manager->beginBrowserLogin();
        });
        f.store.onWrite = [&] { if (f.store.writes == 3) f.store.failWrite = rollbackFails; };
        QSignalSpy saved(f.manager.get(), &RHSessionManager::sessionSaved);
        QVERIFY(f.manager->beginBrowserLogin("owner"));
        f.manager->acceptPrimaryBrowserToken("new-token");
        QTRY_VERIFY(observed);
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(reconciledAtError);
        QVERIFY(!replacementAccepted);
        QCOMPARE(f.store.writes, 3);
        QCOMPARE(f.store.values.value(0), rollbackFails ? QString("new-token") : QString("old-token"));
        QCOMPARE(saved.size(), 0);
        QCOMPARE(f.manager->data(f.manager->index(0), RHSessionManager::ExpiresAtRole), oldExpiry);
        QVERIFY(metadata.open(QIODevice::ReadOnly)); QCOMPARE(metadata.readAll(), before);
        if (rollbackFails) {
            QVERIFY(f.manager->error().contains("rollback"));
            const int reads = f.store.reads;
            QVERIFY(!f.manager->startSession(0));
            QCOMPARE(f.store.reads, reads);
            QVERIFY(f.manager->error().contains("Reauthenticate"));
        }
        lock.unlock(); metadata.close();
        // Failure is actionable: successful reauthentication restores usability.
        f.store.failWrite = false;
        f.http.responses.append(R"({"id":"owner-id","username":"owner"})");
        f.http.responses.append(R"({"results":[]})");
        f.http.responses.append(R"({"results":[]})");
        f.http.statusCodes = {200, 200, 200, 200, 200};
        QVERIFY(f.manager->beginBrowserLogin("owner"));
        f.manager->acceptPrimaryBrowserToken("recovered-token");
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.values.value(0), QString("recovered-token"));
        QCOMPARE(saved.size(), 1);
        QVERIFY(f.manager->startSession(0));
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->sessionActive());
    }

    void rowPublicationCannotBeReplacedFromAboutToInsert()
    {
        BrowserFixture f; QVERIFY(f.ready);
        QAbstractItemModelTester tester(f.manager.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy inserted(f.manager.get(), &QAbstractItemModel::rowsInserted);
        QSignalSpy saved(f.manager.get(), &RHSessionManager::sessionSaved);
        bool attempted = false;
        bool replacementAccepted = false;
        bool busyAfterClear = false;
        const auto reentrant = connect(f.manager.get(), &QAbstractItemModel::rowsAboutToBeInserted, f.manager.get(), [&] {
            attempted = true;
            f.manager->clearActiveSession();
            busyAfterClear = f.manager->busy();
            f.manager->cancelSignIn();
            replacementAccepted = f.manager->beginBrowserLogin();
        });
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("row-token", {});
        QTRY_VERIFY(attempted);
        QCOMPARE(inserted.size(), 1);
        QVERIFY(busyAfterClear);
        QVERIFY(!replacementAccepted);
        QVERIFY(!f.manager->busy());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(f.manager->rowCount(), 1);
        disconnect(reentrant);
        // Exercise a subsequent structural operation with the same tester attached.
        QVERIFY(f.manager->saveAuthenticatedSession("second", "second-token", 3600));
        QTRY_COMPARE(inserted.size(), 2);
        QCOMPARE(f.manager->rowCount(), 2);
    }

    void browserIdleReplacementPreservesSelectedIdentity()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.store.synchronousWrite = true;
        f.http.responses = {R"({"id":"original-owner","username":"owner"})", R"({"results":[]})",
                            R"({"id":"intruder","username":"intruder"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200, 200};
        bool replaced = false;
        connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
            if (!f.manager->busy() && !replaced) {
                replaced = true;
                QVERIFY(f.manager->beginBrowserLogin("original-owner"));
                f.manager->acceptPrimaryBrowserToken("replacement-token");
            }
        });
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("original-token", {});
        QTRY_VERIFY(replaced && !f.manager->busy());
        QCOMPARE(f.manager->rowCount(), 1);
        QCOMPARE(f.store.writes, 1);
        QCOMPARE(f.store.values.value(0), QString("original-token"));
        QVERIFY(f.manager->error().contains("does not match"));
    }

    void browserSavedNotificationCanStartSession()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200};
        bool started = false;
        connect(f.manager.get(), &RHSessionManager::sessionSaved, f.manager.get(), [&](int index) {
            QVERIFY(!f.manager->busy());
            started = f.manager->startSession(index);
        });
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptPrimaryBrowserToken("start-fixture");
        QTRY_VERIFY(started);
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->sessionActive());
    }

    void browserCommitNotificationsRejectCompetingOperations_data()
    {
        QTest::addColumn<bool>("fromBrowserFinish");
        QTest::addColumn<bool>("competingSave");
        QTest::newRow("pending-false-start") << false << false;
        QTest::newRow("pending-false-save") << false << true;
        QTest::newRow("browser-finish-start") << true << false;
        QTest::newRow("browser-finish-save") << true << true;
    }

    void browserCommitNotificationsRejectCompetingOperations()
    {
        QFETCH(bool, fromBrowserFinish);
        QFETCH(bool, competingSave);
        BrowserFixture f; QVERIFY(f.ready);
        if (!competingSave) {
            QVERIFY(f.manager->saveAuthenticatedSession("existing", "existing-token", 3600));
            QTRY_VERIFY(!f.manager->busy());
        }
        const int initialRows = f.manager->rowCount();
        const int initialWrites = f.store.writes;
        const int initialReads = f.store.reads;
        QAbstractItemModelTester tester(f.manager.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy aboutToInsert(f.manager.get(), &QAbstractItemModel::rowsAboutToBeInserted);
        QSignalSpy inserted(f.manager.get(), &QAbstractItemModel::rowsInserted);
        QSignalSpy saved(f.manager.get(), &RHSessionManager::sessionSaved);
        QStringList events;
        connect(f.manager.get(), &QAbstractItemModel::rowsInserted, f.manager.get(), [&] {
            QVERIFY(f.manager->busy());
            events.append("published");
        });
        connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
            if (!f.manager->busy()) events.append("idle");
        });
        bool observed = false;
        bool busyAfterClear = false;
        bool accepted = false;
        int observerWrites = -1;
        int observerReads = -1;
        const auto compete = [&] {
            if (observed) return;
            observed = true;
            events.append("observer");
            f.manager->clearActiveSession();
            busyAfterClear = f.manager->busy();
            accepted = competingSave
                ? f.manager->saveAuthenticatedSession("competitor", "competing-token", 3600)
                : f.manager->startSession(0);
            observerWrites = f.store.writes;
            observerReads = f.store.reads;
        };
        if (fromBrowserFinish)
            connect(&f.browser, &RHBrowserLogin::activeChanged, f.manager.get(), [&] {
                if (!f.browser.active()) compete();
            });
        else
            connect(f.manager.get(), &RHSessionManager::browserLoginPendingChanged, f.manager.get(), [&] {
                if (!f.manager->browserLoginPending()) compete();
            });
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("verified-token", {});
        QTRY_VERIFY(observed && !f.manager->busy());
        QTest::qWait(20); // Drain any incorrectly started credential/network work.
        QVERIFY(busyAfterClear);
        QVERIFY(!accepted);
        QCOMPARE(observerWrites, initialWrites);
        QCOMPARE(observerReads, initialReads);
        QCOMPARE(f.http.requests.size(), 2);
        QCOMPARE(f.store.reads, initialReads);
        QCOMPARE(f.store.writes, initialWrites + 1);
        QCOMPARE(f.store.values.value(initialRows), QString("verified-token"));
        if (!competingSave) QCOMPARE(f.store.values.value(0), QString("existing-token"));
        QCOMPARE(aboutToInsert.size(), 1);
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.at(0).at(0).toInt(), initialRows);
        QCOMPARE(f.manager->rowCount(), initialRows + 1);
        QCOMPARE(events, QStringList({"observer", "published", "idle"}));
        QVERIFY(!f.manager->sessionActive());
        QCOMPARE(f.manager->data(f.manager->index(initialRows), RHSessionManager::AccountNameRole).toString(), QString("owner"));
        QCOMPARE(f.manager->data(f.manager->index(initialRows), RHSessionManager::AccountIdRole).toString(), QString("owner-id"));
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto records = QJsonDocument::fromJson(metadata.readAll()).object()["sessions"].toArray();
        QCOMPARE(records.size(), initialRows + 1);
        const auto record = records.at(initialRows).toObject();
        QCOMPARE(record["index"].toInt(), initialRows);
        QCOMPARE(record["account_name"].toString(), QString("owner"));
        QCOMPARE(record["account_id"].toString(), QString("owner-id"));
        QCOMPARE(record["token_type"].toString(), QString("WebSecondaryBearer"));
        QCOMPARE(record["expires_at"].toString(), f.manager->data(f.manager->index(initialRows), RHSessionManager::ExpiresAtRole).toString());
    }

    void browserPendingObserverCannotReplaceCommit()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"id":"old-owner","username":"old"})", R"({"results":[]})",
                            R"({"id":"new-owner","username":"new"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200, 200};
        bool attempted = false;
        bool accepted = false;
        connect(f.manager.get(), &RHSessionManager::browserLoginPendingChanged, f.manager.get(), [&] {
            if (!f.manager->browserLoginPending() && !attempted) {
                attempted = true;
                f.manager->clearActiveSession();
                accepted = f.manager->beginBrowserLogin();
                f.manager->acceptPrimaryBrowserToken("new-primary-token");
            }
        });
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("old-secondary-token", {});
        QTRY_VERIFY(attempted && !f.manager->busy());
        QVERIFY(!accepted);
        QCOMPARE(f.http.requests.size(), 2);
        QCOMPARE(f.store.writes, 1);
        QCOMPARE(f.manager->rowCount(), 1);
        QCOMPARE(f.store.values.value(0), QString("old-secondary-token"));
        QCOMPARE(f.manager->data(f.manager->index(0), RHSessionManager::AccountIdRole).toString(), QString("old-owner"));
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto record = QJsonDocument::fromJson(metadata.readAll()).object()["sessions"].toArray().at(0).toObject();
        QCOMPARE(record["token_type"].toString(), QString("WebSecondaryBearer"));
    }

    void browserCompletionObserverMayDestroyManager_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const char *boundary : {"browser-finished", "pending-false", "store-write", "rows-about-to-insert-deferred", "rows-inserted",
                                     "data-changed", "reauth-false", "idle", "saved"})
            QTest::newRow(boundary) << QString::fromLatin1(boundary);
    }

    void browserCompletionObserverMayDestroyManager()
    {
        QFETCH(QString, boundary);
        BrowserFixture f; QVERIFY(f.ready);
        const bool replacing = boundary == "data-changed" || boundary == "reauth-false";
        if (replacing) {
            QVERIFY(f.manager->saveAuthenticatedSession("owner", "previous-token", 3600));
            QTRY_VERIFY(!f.manager->busy());
        }
        f.http.responses = {R"({"id":"owner-id","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        bool destroyed = false;
        const auto destroy = [&] { destroyed = true; f.manager.reset(); };
        if (boundary == "browser-finished")
            connect(&f.browser, &RHBrowserLogin::activeChanged, f.manager.get(), [&] {
                if (!f.browser.active()) destroy();
            });
        else if (boundary == "pending-false")
            connect(f.manager.get(), &RHSessionManager::browserLoginPendingChanged, f.manager.get(), [&] {
                if (!f.manager->browserLoginPending()) destroy();
            });
        else if (boundary == "store-write") f.store.onWrite = destroy;
        else if (boundary == "rows-about-to-insert-deferred")
            // Qt itself still uses model-private state after emitting about-to-insert.
            // Deletion at this boundary must use QObject::deleteLater().
            connect(f.manager.get(), &QAbstractItemModel::rowsAboutToBeInserted, f.manager.get(), [&] {
                destroyed = true;
                f.manager.release()->deleteLater();
            });
        else if (boundary == "rows-inserted")
            connect(f.manager.get(), &QAbstractItemModel::rowsInserted, f.manager.get(), destroy);
        else if (boundary == "data-changed")
            connect(f.manager.get(), &QAbstractItemModel::dataChanged, f.manager.get(), destroy);
        else if (boundary == "reauth-false")
            connect(f.manager.get(), &RHSessionManager::reauthenticatingChanged, f.manager.get(), [&] {
                if (!f.manager->reauthenticating()) destroy();
            });
        else if (boundary == "idle")
            connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
                if (!f.manager->busy()) destroy();
            });
        else connect(f.manager.get(), &RHSessionManager::sessionSaved, f.manager.get(), destroy);
        QVERIFY(f.manager->beginBrowserLogin(replacing ? QString("owner") : QString{}));
        f.manager->acceptBrowserToken("completion-token", {});
        QTRY_VERIFY(destroyed);
        // Drain the store's asynchronous completion after the manager is gone.
        QTest::qWait(20);
        QVERIFY(!f.manager);
        if (boundary == "browser-finished" || boundary == "pending-false") QCOMPARE(f.store.writes, 0);
    }

    void primaryBrowserTokenIsVerifiedAndStoredSeparately()
    {
        BrowserFixture f; QVERIFY(f.ready);
        const QByteArray accounts = R"({"results":[{"account_number":"100","url":"https:\/\/api.robinhood.com/accounts/100/","type":"cash"}]})";
        f.http.responses = {R"({"id":"primary-owner","username":"owner"})", accounts, accounts};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptPrimaryBrowserToken("primary-token-fixture");
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.values.value(0), QString("primary-token-fixture"));
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto record = QJsonDocument::fromJson(metadata.readAll()).object()["sessions"].toArray().at(0).toObject();
        QCOMPARE(record["token_type"].toString(), QString("Bearer"));
        QCOMPARE(record["account_id"].toString(), QString("primary-owner"));
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->tradingUnavailableReason().isEmpty());
        QCOMPARE(f.http.requests.size(), 3);
        for (const auto &request : f.http.requests) {
            QVERIFY(request.startsWith("GET "));
            QVERIFY(request.contains("Authorization: Bearer primary-token-fixture\r\n"));
        }
    }

    void tradingContextLocksTheActiveProfileAndReleasesBeforeIdle()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"results":[{"account_number":"100","url":"https:\/\/api.robinhood.com/accounts/100/","type":"cash"}]})"};
        f.http.statusCodes = {200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "order-context-fixture", 3600));
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        RHSessionManager::TradingSession acquired;
        bool completed = false;
        f.manager->requestTradingSession([&](bool ok, RHSessionManager::TradingSession session, QString error) {
            QVERIFY2(ok, qPrintable(error)); acquired = std::move(session); completed = true;
        });
        QVERIFY(f.manager->busy());
        f.manager->clearActiveSession();
        QVERIFY(f.manager->sessionActive());
        QVERIFY(!f.manager->startSession(0));
        QVERIFY(!f.manager->load());
        QVERIFY(!f.manager->deleteSavedSession(0));
        QTRY_VERIFY(completed);
        QVERIFY(f.manager->isTradingSessionCurrent(acquired));
        QCOMPARE(acquired.token, QByteArray("order-context-fixture"));
        QCOMPARE(acquired.accounts.size(), 1);
        QCOMPARE(acquired.accounts.at(0).toMap()["accountNumber"].toString(), QString("100"));
        bool restarted = false;
        connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
            if (!f.manager->busy() && !restarted) { restarted = true; f.manager->clearActiveSession(); }
        });
        f.manager->releaseTradingSession();
        QVERIFY(restarted); QVERIFY(!f.manager->busy()); QVERIFY(!f.manager->sessionActive());
        QVERIFY(!f.manager->isTradingSessionCurrent(acquired));
    }

    void secondaryBrowserSessionCannotAcquireTradingToken()
    {
        BrowserFixture f; QVERIFY(f.ready);
        const QByteArray accounts = R"({"results":[{"account_number":"100","type":"cash"}]})";
        f.http.responses = {R"({"id":"readonly-owner","username":"owner"})", accounts, accounts};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->beginBrowserLogin()); f.manager->acceptBrowserToken("readonly-fixture", {});
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const int reads = f.store.reads;
        bool rejected = false;
        f.manager->requestTradingSession([&](bool ok, RHSessionManager::TradingSession session, QString error) {
            QVERIFY(!ok); QVERIFY(session.token.isEmpty()); QVERIFY(error.contains("read-only")); rejected = true;
        });
        QVERIFY(rejected); QCOMPARE(f.store.reads, reads); QVERIFY(!f.manager->busy());
        QVERIFY(f.manager->tradingUnavailableReason().contains("read-only"));
    }

    void staleTradingAcquisitionCannotPublishAnotherLease()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})"};
        f.http.statusCodes = {200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "lease-token-fixture", 3600));
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        bool oldRejected = false;
        bool currentReady = false;
        RHSessionManager::TradingSession current;
        f.manager->requestTradingSession([&](bool ok, RHSessionManager::TradingSession session, QString) {
            QVERIFY(!ok); QVERIFY(session.token.isEmpty()); oldRejected = true;
        });
        f.manager->releaseTradingSession();
        f.manager->requestTradingSession([&](bool ok, RHSessionManager::TradingSession session, QString error) {
            QVERIFY2(ok, qPrintable(error)); current = std::move(session); currentReady = true;
        });
        QTRY_VERIFY(oldRejected && currentReady);
        QVERIFY(f.manager->busy()); QVERIFY(f.manager->isTradingSessionCurrent(current));
        f.manager->releaseTradingSession(); QVERIFY(!f.manager->busy());
    }

    void failedTradingStoreReadIsSanitizedAndReleasable()
    {
        BrowserFixture f; QVERIFY(f.ready);
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})"};
        f.http.statusCodes = {200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "lease-token-fixture", 3600));
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        f.store.failRead = true;
        bool rejected = false;
        f.manager->requestTradingSession([&](bool ok, RHSessionManager::TradingSession session, QString error) {
            QVERIFY(!ok); QVERIFY(session.token.isEmpty()); QVERIFY(error.contains("secure store"));
            QVERIFY(!error.contains("lease-token-fixture")); rejected = true;
            f.manager->releaseTradingSession();
        });
        QTRY_VERIFY(rejected); QVERIFY(!f.manager->busy()); QVERIFY(f.manager->sessionActive());
    }

    void clearActiveSessionCancelsPendingInstrument_data()
    {
        QTest::addColumn<bool>("restart");
        QTest::newRow("cancel") << false;
        QTest::newRow("restart-from-idle-signal") << true;
    }
    void clearActiveSessionCancelsPendingInstrument()
    {
        QFETCH(bool, restart);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        const QByteArray first = R"({"results":[{"account_number":"100","type":"cash"}]})";
        const QByteArray second = R"({"results":[{"account_number":"200","brokerage_account_type":"ira_roth"}]})";
        const QByteArray oldHoldings = R"({"results":[{"account_number":"100","symbol":"OLD","quantity":"1.25"}],"next":null})";
        // The last instrument response is deliberately held by omitting its ordinal.
        f.http.responses = {second, R"({"results":[],"next":null})", first, oldHoldings, first,
            R"({"results":[{"account_number":"100","instrument":"/instruments/one/","quantity":"2"},{"account_number":"100","instrument":"/instruments/two/","quantity":"3"}],"next":null})"};
        f.http.statusCodes = {200,403,200,200,200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("first", "fixture-one", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->saveAuthenticatedSession("second", "fixture-two", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(1)); QTRY_VERIFY(!f.manager->busy());
        const auto secondCache = f.manager->cachedAccounts(1);
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto oldSnapshot = f.manager->cachedAccounts(0)[0].toMap();
        QVERIFY(f.manager->startSession(0)); QTRY_COMPARE(f.http.requests.size(), 7);
        QVERIFY(f.http.requests.last().startsWith("GET /instruments/one/"));
        QVERIFY(f.manager->sessionActive()); QVERIFY(f.manager->busy());
        const auto firstCache = f.manager->cachedAccounts(0);
        QCOMPARE(firstCache[0].toMap()["holdings"], oldSnapshot["holdings"]);
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto diskBefore = metadata.readAll(); metadata.close();
        const QPointer<QTcpSocket> held = f.http.lastPeer; QVERIFY(held);
        const QByteArray lateBody = R"({"symbol":"LATE"})";
        held->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(lateBody.size()) + "\r\nConnection: close\r\n\r\n");
        held->flush();
        QTest::qWait(20); // Headers arrived, but the instrument body remains pending.
        QSignalSpy disconnected(held, &QTcpSocket::disconnected);
        bool restarted = false;
        bool inactiveWhenIdle = false;
        connect(f.manager.get(), &RHSessionManager::busyChanged, f.manager.get(), [&] {
            if (f.manager->busy() || restarted) return;
            inactiveWhenIdle = !f.manager->sessionActive() && f.manager->accounts()->rowCount() == 0;
            if (restart) { restarted = true; QVERIFY(f.manager->startSession(1)); }
        });
        f.manager->clearActiveSession();
        QCOMPARE(f.manager->busy(), restart);
        QVERIFY(!f.manager->sessionActive());
        QCOMPARE(f.manager->accounts()->rowCount(), restart ? 1 : 0);
        QVERIFY(inactiveWhenIdle);
        // Attempt the held old-generation response before processing socket closure.
        if (held && held->state() == QAbstractSocket::ConnectedState) {
            held->write(lateBody);
            held->flush();
        }
        QTRY_COMPARE(disconnected.count(), 1);
        if (restart) {
            QTRY_COMPARE(f.http.requests.size(), 8);
            QVERIFY(f.http.requests.last().startsWith("GET /accounts/"));
            QVERIFY(f.manager->busy()); // The replacement Accounts request remains pending.
            QCOMPARE(f.manager->accounts()->data(f.manager->accounts()->index(0, 0), RHAccountModel::AccountNumberRole).toString(), QString("200"));
        }
        QTest::qWait(50);
        QCOMPARE(f.http.requests.size(), restart ? 8 : 7); // No second instrument dispatch.
        QCOMPARE(f.manager->cachedAccounts(0), firstCache);
        QCOMPARE(f.manager->cachedAccounts(1), secondCache);
        QVERIFY(metadata.open(QIODevice::ReadOnly)); QCOMPARE(metadata.readAll(), diskBefore); metadata.close();
        if (restart) {
            const QByteArray body = second;
            QVERIFY(f.http.lastPeer);
            f.http.lastPeer->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            f.http.lastPeer->flush(); f.http.lastPeer->disconnectFromHost();
            // The next (optional positions) request is also held; new selection survives.
            QTRY_VERIFY(f.manager->sessionActive()); QTRY_COMPARE(f.http.requests.size(), 9);
            QCOMPARE(f.manager->accounts()->rowCount(), 1);
            f.manager->clearActiveSession();
        }
    }

    void duplicatePositionsCannotInflateSnapshot()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})",
            R"({"results":[{"account_number":"100","symbol":"DUP","instrument":"/instruments/abc/","quantity":"1.25"},{"account_number":"100","symbol":"DUP","instrument":"/instruments/abc/","quantity":"1.25"}],"next":null})"}; f.http.statusCodes = {200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto row = f.manager->cachedAccounts(0)[0].toMap();
        QVERIFY(row["holdings"].toList().isEmpty()); QCOMPARE(row["holdingsStatus"].toString(), QString("unavailable"));
        QVERIFY(row["holdingsUpdatedAt"].toString().isEmpty());
    }

    void cacheNeverPersistsUnvalidatedAccountUrlsOrRawFields()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash","url":"https:\/\/api.robinhood.com/accounts/100/?access_token=raw-secret","access_token":"raw-token","description":"raw-description"}]})",
            R"({"results":[],"next":null})"}; f.http.statusCodes = {200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto bytes = metadata.readAll(); QVERIFY(!bytes.contains("raw-secret")); QVERIFY(!bytes.contains("raw-token")); QVERIFY(!bytes.contains("raw-description"));
        const auto rows = f.manager->cachedAccounts(0); QCOMPARE(rows.size(), 1);
        QVERIFY(!rows[0].toMap().contains("_url")); QVERIFY(!rows[0].toMap().contains("_key"));
    }

    void cancellationFromCacheNotificationCannotReactivateSession()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})"}; f.http.statusCodes = {200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        bool cancelled = false;
        connect(f.manager.get(), &RHSessionManager::accountCacheChanged, f.manager.get(), [&] {
            if (!cancelled && !f.manager->cachedAccounts(0).isEmpty()) { cancelled = true; f.manager->cancelSignIn(); }
        });
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(cancelled); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(!f.manager->sessionActive()); QCOMPARE(f.http.requests.size(), 1);
        QCOMPARE(f.manager->cachedAccounts(0).size(), 1);
    }

    void failedVerificationLeavesOfflineSnapshotStale()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})",
            R"({"results":[{"account_number":"100","symbol":"OLD","quantity":"1.25"}],"next":null})", R"({"detail":"unauthorized"})"};
        f.http.statusCodes = {200,200,401};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto before = f.manager->cachedAccounts(0)[0].toMap();
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto after = f.manager->cachedAccounts(0)[0].toMap();
        QCOMPARE(after["holdings"], before["holdings"]); QCOMPARE(after["holdingsUpdatedAt"], before["holdingsUpdatedAt"]);
        QCOMPARE(after["holdingsStatus"].toString(), QString("cached"));
        QCOMPARE(f.manager->accounts()->rowCount(), 1); QVERIFY(!f.manager->sessionActive());
    }

    void staleManagerCannotClobberNewerSnapshot()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        RHSessionManager stale(f.network, f.dir.filePath("sessions.json"), &f.store, nullptr, 180000, 2500, &f.locker);
        const QByteArray accounts = R"({"results":[{"account_number":"100","type":"cash"}]})";
        f.http.responses = {accounts, R"({"results":[{"account_number":"100","symbol":"LATEST","quantity":"7.5"}],"next":null})", accounts, R"({"results":[],"next":null})"};
        f.http.statusCodes = {200,200,200,403};
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto latest = f.manager->cachedAccounts(0)[0].toMap()["holdings"];
        QVERIFY(stale.startSession(0)); QTRY_VERIFY(!stale.busy());
        RHSessionManager restored(f.network, f.dir.filePath("sessions.json"), &f.store, nullptr, 180000, 2500, &f.locker);
        const auto rows = restored.cachedAccounts(0); QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap()["holdings"], latest);
        QCOMPARE(f.http.requests.size(), 3);
    }

    void failedCacheWriteRetainsSnapshotAndUnknownMetadata()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        const QByteArray accounts = R"({"results":[{"account_number":"100","type":"cash"}]})";
        f.http.responses = {accounts, R"({"results":[{"account_number":"100","symbol":"OLD","quantity":"1.25"}],"next":null})", accounts};
        f.http.statusCodes = {200,200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto before = f.manager->cachedAccounts(0)[0].toMap();
        const QString path = f.dir.filePath("sessions.json"); QFile metadata(path);
        QVERIFY(metadata.open(QIODevice::ReadOnly)); auto root = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
        root["extension"] = "preserve-root"; auto records = root["sessions"].toArray(); auto record = records[0].toObject();
        record["extension"] = "preserve-profile"; records[0] = record; root["sessions"] = records;
        QVERIFY(metadata.open(QIODevice::WriteOnly)); metadata.write(QJsonDocument(root).toJson()); metadata.close();
        QVERIFY(f.manager->startSession(0)); QTRY_COMPARE(f.http.requests.size(), 4);
        QVERIFY(f.manager->sessionActive());
        QLockFile lock(path + ".lock"); QVERIFY(lock.tryLock());
        const QByteArray body = R"({"results":[],"next":null})";
        QVERIFY(f.http.lastPeer);
        f.http.lastPeer->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        f.http.lastPeer->flush(); f.http.lastPeer->disconnectFromHost();
        QTRY_VERIFY(!f.manager->busy()); lock.unlock();
        const auto after = f.manager->cachedAccounts(0)[0].toMap();
        QCOMPARE(after["holdings"], before["holdings"]); QCOMPARE(after["holdingsUpdatedAt"], before["holdingsUpdatedAt"]);
        QCOMPARE(after["holdingsStatus"].toString(), QString("cached")); QVERIFY(f.manager->sessionActive());
        QVERIFY(metadata.open(QIODevice::ReadOnly)); const auto bytes = metadata.readAll(); metadata.close();
        QVERIFY(!bytes.contains("fixture")); const auto saved = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(saved["extension"].toString(), QString("preserve-root"));
        QCOMPARE(saved["sessions"].toArray()[0].toObject()["extension"].toString(), QString("preserve-profile"));
        QVERIFY(!(metadata.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
        RHSessionManager restored(f.network, path, &f.store, nullptr, 180000, 2500, &f.locker);
        QCOMPARE(restored.cachedAccounts(0)[0].toMap()["holdings"], before["holdings"]);
    }

    void paginatedSnapshotsCommitOnlyAfterLastPage_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("cycle");
        QTest::newRow("complete") << 200 << false;
        QTest::newRow("failed-second-page") << 403 << false;
        QTest::newRow("cycle") << 200 << true;
    }
    void paginatedSnapshotsCommitOnlyAfterLastPage()
    {
        QFETCH(int, status); QFETCH(bool, cycle);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        const QByteArray accounts = R"({"results":[{"account_number":"100","type":"cash"}]})";
        f.http.responses = {accounts, R"({"results":[{"account_number":"100","symbol":"OLD","quantity":"0.25"}],"next":null})", accounts,
            R"({"results":[{"account_number":"100","symbol":"ONE","quantity":"1.125"}],"next":"/positions/?cursor=two"})",
            cycle ? QByteArray(R"({"results":[],"next":"/positions/?cursor=two"})")
                  : QByteArray(R"({"results":[{"account_number":"100","symbol":"TWO","quantity":"0.00000000000000000000001"}],"next":null})")};
        f.http.statusCodes = {200,200,200,200,status};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto before = f.manager->cachedAccounts(0)[0].toMap();
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto after = f.manager->cachedAccounts(0)[0].toMap();
        if (status == 200 && !cycle) {
            QCOMPARE(after["holdings"].toList().size(), 2);
            QCOMPARE(after["holdings"].toList()[1].toMap()["quantity"].toString(), QString("0.00000000000000000000001"));
            QCOMPARE(after["holdingsStatus"].toString(), QString("fresh"));
        } else { QCOMPARE(after["holdings"], before["holdings"]); QCOMPARE(after["holdingsUpdatedAt"], before["holdingsUpdatedAt"]); }
        QCOMPARE(f.http.requests.size(), 5);
        QVERIFY(f.http.requests[4].contains("account_number=100"));
        QVERIFY(f.http.requests[4].contains("nonzero=true"));
    }

    void brokerageRowsHaveIndependentSnapshotsAndFilters()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        f.http.responses = {R"({"results":[{"account_number":"100","brokerage_account_type":"individual"},{"account_number":"200","brokerage_account_type":"ira_roth"},{"id":"no-number","type":"cash"}]})",
            R"({"results":[{"account_number":"100","symbol":"STANDARD","quantity":"1.23"}],"next":null})",
            R"({"results":[{"account_number":"200","symbol":"RETIREMENT","quantity":"4.56"}],"next":null})"};
        f.http.statusCodes = {200,200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto rows = f.manager->cachedAccounts(0); QCOMPARE(rows.size(), 3);
        QCOMPARE(rows[0].toMap()["holdings"].toList()[0].toMap()["symbol"].toString(), QString("STANDARD"));
        QCOMPARE(rows[1].toMap()["holdings"].toList()[0].toMap()["symbol"].toString(), QString("RETIREMENT"));
        QCOMPARE(rows[2].toMap()["holdingsStatus"].toString(), QString("unavailable"));
        QVERIFY(rows[2].toMap()["holdingsUpdatedAt"].toString().isEmpty());
        QCOMPARE(f.http.requests.size(), 3);
        QVERIFY(f.http.requests[1].contains("account_number=100")); QVERIFY(f.http.requests[2].contains("account_number=200"));
    }

    void cancelledRefreshCannotPopulateAnotherProfile()
    {
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash"}]})"}; f.http.statusCodes = {200};
        QVERIFY(f.manager->saveAuthenticatedSession("first", "fixture-one", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_COMPARE(f.http.requests.size(), 2);
        QVERIFY(f.manager->busy()); f.manager->cancelSignIn();
        QVERIFY(!f.manager->busy());
        const auto previous = f.manager->cachedAccounts(0);
        QCOMPARE(previous.size(), 1); QVERIFY(previous[0].toMap()["holdingsUpdatedAt"].toString().isEmpty());
        f.http.responses.append(QByteArray{});
        f.http.responses.append(R"({"results":[{"account_number":"100","brokerage_account_type":"ira_roth"}]})");
        f.http.responses.append(R"({"results":[{"account_number":"100","quantity":"2.5","symbol":"SECOND"}],"next":null})");
        f.http.statusCodes = {200,200,200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("second", "fixture-two", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(1)); QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.manager->cachedAccounts(0), previous);
        const auto second = f.manager->cachedAccounts(1)[0].toMap();
        QCOMPARE(second["displayName"].toString(), QString("Roth IRA"));
        QCOMPARE(second["holdings"].toList()[0].toMap()["symbol"].toString(), QString("SECOND"));
    }

    void incompleteRefreshNeverClearsSnapshot_data()
    {
        QTest::addColumn<QByteArray>("response"); QTest::addColumn<int>("status");
        QTest::newRow("missing-pagination-completion") << QByteArray(R"({"results":[]})") << 200;
        QTest::newRow("forbidden") << QByteArray(R"({"results":[],"next":null})") << 403;
        QTest::newRow("unauthorized") << QByteArray(R"({"results":[],"next":null})") << 401;
        QTest::newRow("malformed") << QByteArray("{broken") << 200;
        QTest::newRow("bad-quantity") << QByteArray(R"({"results":[{"account_number":"100","symbol":"BAD","quantity":"NaN"}],"next":null})") << 200;
        QTest::newRow("other-account") << QByteArray(R"({"results":[{"account_number":"200","symbol":"BAD","quantity":"1"}],"next":null})") << 200;
        QTest::newRow("foreign-next") << QByteArray(R"({"results":[],"next":"https:\/\/evil.example/positions/"})") << 200;
        QTest::newRow("foreign-instrument") << QByteArray(R"({"results":[{"account_number":"100","instrument":"https:\/\/evil.example/instruments/abc/","quantity":"1"}],"next":null})") << 200;
    }
    void incompleteRefreshNeverClearsSnapshot()
    {
        QFETCH(QByteArray, response); QFETCH(int, status);
        BrowserFixture f; QVERIFY(f.ready); QVERIFY(f.enableHoldings());
        const QByteArray accounts = R"({"results":[{"account_number":"100","type":"cash"}]})";
        f.http.responses = {accounts, R"({"results":[{"account_number":"100","symbol":"AAPL","quantity":"1.25"}],"next":null})", accounts, response,
            accounts, R"({"results":[],"next":null})"};
        f.http.statusCodes = {200,200,200,status,200,200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture", 3600)); QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto before = f.manager->cachedAccounts(0)[0].toMap();
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto failed = f.manager->cachedAccounts(0)[0].toMap();
        QCOMPARE(failed["holdings"], before["holdings"]);
        QCOMPARE(failed["holdingsUpdatedAt"], before["holdingsUpdatedAt"]);
        QCOMPARE(failed["holdingsStatus"].toString(), QString("cached"));
        QVERIFY(!failed["holdingsError"].toString().isEmpty()); QVERIFY(f.manager->sessionActive());
        QCOMPARE(f.http.requests.size(), 4);
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        const auto empty = f.manager->cachedAccounts(0)[0].toMap();
        QVERIFY(empty["holdings"].toList().isEmpty()); QCOMPARE(empty["holdingsStatus"].toString(), QString("fresh"));
        QVERIFY(!empty["holdingsUpdatedAt"].toString().isEmpty());
    }

    void fractionalSnapshotSurvivesRestartAndDeletesWithProfile()
    {
        BrowserFixture f;
        QVERIFY(f.ready);
        QFile config(f.dir.filePath("api.json"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(config.readAll()).object(); config.close();
        auto endpoints = root["Endpoints"].toObject();
        const auto endpoint = [&f](const QString &path) { return QJsonObject{{"URL", QString("http://127.0.0.1:%1").arg(f.http.server.serverPort()) + path}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}}; };
        endpoints["CachedPositions"] = endpoint("/positions/?nonzero=true");
        endpoints["Instruments"] = endpoint("/instruments/"); root["Endpoints"] = endpoints;
        QVERIFY(config.open(QIODevice::WriteOnly)); config.write(QJsonDocument(root).toJson()); config.close();
        QVERIFY(f.network.loadConfiguration(config.fileName()));
        f.http.responses = {R"({"results":[{"account_number":"100","type":"cash","url":"https:\/\/api.robinhood.com/accounts/100/"}]})",
            R"({"results":[{"account_number":"100","quantity":"0.12345678901234567890","instrument":"/instruments/abc/"},{"account_number":"100","quantity":"0.000","symbol":"ZERO"}],"next":null})",
            R"({"symbol":"AAPL"})"};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->saveAuthenticatedSession("owner", "fixture-token", 3600));
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(f.manager->startSession(0)); QTRY_VERIFY(!f.manager->busy());
        auto rows = f.manager->cachedAccounts(0);
        QCOMPARE(rows.size(), 1);
        auto row = rows[0].toMap();
        QCOMPARE(row["holdingsStatus"].toString(), QString("fresh"));
        auto holdings = row["holdings"].toList(); QCOMPARE(holdings.size(), 1);
        QCOMPARE(holdings[0].toMap()["symbol"].toString(), QString("AAPL"));
        QCOMPARE(holdings[0].toMap()["quantity"].toString(), QString("0.12345678901234567890"));
        QVERIFY(!row["holdingsUpdatedAt"].toString().isEmpty());
        QCOMPARE(f.http.requests.size(), 3);
        QVERIFY(f.http.requests[1].contains("account_number=100"));
        f.manager.reset();
        RHSessionManager restored(f.network, f.dir.filePath("sessions.json"), &f.store, nullptr, 180000, 2500, &f.locker);
        rows = restored.cachedAccounts(0); QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].toMap()["holdings"].toList(), holdings);
        QCOMPARE(rows[0].toMap()["holdingsStatus"].toString(), QString("cached"));
        QCOMPARE(f.http.requests.size(), 3);
        QVERIFY(restored.deleteSavedSession(0)); QTRY_VERIFY(!restored.busy());
        QVERIFY(restored.cachedAccounts(0).isEmpty());
        RHSessionManager deleted(f.network, f.dir.filePath("sessions.json"), &f.store, nullptr, 180000, 2500, &f.locker);
        QVERIFY(deleted.cachedAccounts(0).isEmpty());
    }

    void genericRetirementNeedsAnUnambiguousStructuredSubtype()
    {
        RHAccountModel model;
        model.setAccounts(QJsonArray{
            QJsonObject{{"type", "cash"}, {"rh_account_type", "retirement"}, {"retirement", QJsonObject{{"subtype", "roth"}}}},
            QJsonObject{{"type", "cash"}, {"account_type", "ira"}},
            QJsonObject{{"type", "cash"}, {"account_type", "ira"}, {"retirement_type", "traditional"}}});
        QCOMPARE(model.data(model.index(0), RHAccountModel::DisplayNameRole).toString(), QString("Roth IRA"));
        QCOMPARE(model.data(model.index(1), RHAccountModel::DisplayNameRole).toString(), QString("Account type unavailable"));
        QCOMPARE(model.data(model.index(2), RHAccountModel::DisplayNameRole).toString(), QString("IRA"));
    }

    void brokerageAccountTypeTakesPrecedence()
    {
        RHAccountModel model;
        model.setAccounts(QJsonArray{QJsonObject{{"type", "cash"}, {"brokerage_account_type", "ira_roth"}},
            QJsonObject{{"type", "cash"}, {"brokerage_account_type", "ira_traditional"}},
            QJsonObject{{"type", "cash"}, {"brokerage_account_type", "ira_sep"}},
            QJsonObject{{"type", "cash"}, {"brokerage_account_type", "individual"}, {"nickname", "IRA"}}});
        QCOMPARE(model.data(model.index(0), RHAccountModel::DisplayNameRole).toString(), QString("Roth IRA"));
        QCOMPARE(model.data(model.index(1), RHAccountModel::DisplayNameRole).toString(), QString("IRA"));
        QCOMPARE(model.data(model.index(2), RHAccountModel::DisplayNameRole).toString(), QString("Account type unavailable"));
        QCOMPARE(model.data(model.index(3), RHAccountModel::DisplayNameRole).toString(), QString("Standard Brokerage"));
    }

    void structuredAccountClassification()
    {
        RHAccountModel model;
        model.setAccounts(QJsonArray{
            QJsonObject{{"type", "margin"}, {"nickname", "Roth IRA"}},
            QJsonObject{{"type", "cash"}, {"retirement", QJsonObject{{"type", "traditional"}}}},
            QJsonObject{{"type", "cash"}, {"retirement", QJsonObject{{"type", "roth"}}}},
            QJsonObject{{"type", "cash"}, {"retirement_type", "sep_ira"}}});
        QCOMPARE(model.data(model.index(0), RHAccountModel::DisplayNameRole).toString(), QString("Standard Brokerage"));
        QCOMPARE(model.data(model.index(1), RHAccountModel::DisplayNameRole).toString(), QString("IRA"));
        QCOMPARE(model.data(model.index(2), RHAccountModel::DisplayNameRole).toString(), QString("Roth IRA"));
        QCOMPARE(model.data(model.index(3), RHAccountModel::DisplayNameRole).toString(), QString("Account type unavailable"));
    }

    void browserFailureAndNativeCancellationResetPendingState()
    {
        BrowserFixture f;
        QVERIFY(f.ready);
        QVERIFY(f.manager->beginBrowserLogin());
        f.browser.cancel();
        QVERIFY(!f.manager->busy());
        QVERIFY(!f.manager->browserLoginPending());
        QVERIFY(f.manager->beginBrowserLogin());
        f.browser.pageFailed();
        QVERIFY(!f.manager->busy());
        QVERIFY(!f.manager->browserLoginPending());
        QVERIFY(!f.browser.active());
        QVERIFY(f.store.values.isEmpty());
        f.manager->setBrowserLogin(nullptr);
        QVERIFY(!f.manager->beginBrowserLogin());
        QVERIFY(f.manager->error().contains("Qt"));
    }

    void capturedTokenRejectsUnsafeBytesAndExpiredCookies_data()
    {
        QTest::addColumn<QByteArray>("token");
        QTest::addColumn<bool>("expired");
        QTest::newRow("empty") << QByteArray{} << false;
        QTest::newRow("newline") << QByteArray("private\r\nheader") << false;
        QTest::newRow("space") << QByteArray("private header") << false;
        QTest::newRow("nul") << QByteArray("private\0value", 13) << false;
        QTest::newRow("non-ascii") << QByteArray::fromHex("ff") << false;
        QTest::newRow("expired") << QByteArray("expired-fixture") << true;
    }
    void capturedTokenRejectsUnsafeBytesAndExpiredCookies()
    {
        QFETCH(QByteArray, token);
        QFETCH(bool, expired);
        BrowserFixture f;
        QVERIFY(f.ready);
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken(token, expired ? QDateTime::currentDateTimeUtc().addSecs(-1) : QDateTime{});
        QVERIFY(!f.manager->busy());
        QVERIFY(!f.manager->browserLoginPending());
        QVERIFY(!f.browser.active());
        QVERIFY(f.store.values.isEmpty());
        QVERIFY(f.http.requests.isEmpty());
        QVERIFY(!f.manager->error().contains("private"));
    }

    void secondaryVerificationFailureNeverPublishes_data()
    {
        QTest::addColumn<QByteArray>("profile");
        QTest::addColumn<QByteArray>("accounts");
        QTest::addColumn<int>("profileStatus");
        QTest::addColumn<int>("accountStatus");
        const QByteArray profile = R"({"id":"stable-user","email":"owner@example.test"})";
        QTest::newRow("unauthorized-profile") << QByteArray(R"({"detail":"private-marker"})") << QByteArray{} << 401 << 200;
        QTest::newRow("forbidden-profile") << QByteArray(R"({"access_token":"private-marker"})") << QByteArray{} << 403 << 200;
        QTest::newRow("profile-not-json") << QByteArray("private-marker") << QByteArray{} << 200 << 200;
        QTest::newRow("profile-no-id") << QByteArray(R"({"email":"owner@example.test"})") << QByteArray{} << 200 << 200;
        QTest::newRow("profile-no-label") << QByteArray(R"({"id":"stable-user"})") << QByteArray{} << 200 << 200;
        QTest::newRow("accounts-forbidden") << profile << QByteArray(R"({"detail":"private-marker"})") << 200 << 403;
        QTest::newRow("accounts-malformed") << profile << QByteArray(R"({"results":{}})") << 200 << 200;
    }
    void secondaryVerificationFailureNeverPublishes()
    {
        QFETCH(QByteArray, profile);
        QFETCH(QByteArray, accounts);
        QFETCH(int, profileStatus);
        QFETCH(int, accountStatus);
        BrowserFixture f;
        QVERIFY(f.ready);
        f.http.responses = {profile, accounts};
        f.http.statusCodes = {profileStatus, accountStatus};
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("private-token-fixture", {});
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(!f.manager->browserLoginPending());
        QVERIFY(!f.browser.active());
        QCOMPARE(f.store.writes, 0);
        QCOMPARE(f.manager->rowCount(), 0);
        QVERIFY(!f.manager->error().isEmpty());
        if (profileStatus == 401 || profileStatus == 403 || accountStatus == 403) {
            QVERIFY(f.manager->error().contains("secondary", Qt::CaseInsensitive));
            QVERIFY(f.manager->error().contains("not saved", Qt::CaseInsensitive));
        }
        QVERIFY(!f.manager->error().contains("private-marker"));
        QVERIFY(!f.manager->error().contains("private-token-fixture"));
        QVERIFY(!QFile::exists(f.dir.filePath("sessions.json")));
        for (const auto &request : f.http.requests) QVERIFY(request.startsWith("GET "));
    }

    void unavailableBrowserExplainsPlatformSecurityBoundary()
    {
        BrowserFixture f;
        QVERIFY(f.ready);
        f.manager->setBrowserLogin(nullptr);
        QVERIFY(!f.manager->beginBrowserLogin());
        QVERIFY(f.manager->error().contains("pre-navigation"));
        QCOMPARE(f.store.writes, 0);
        QCOMPARE(f.http.requests.size(), 0);
    }

    void cookieExpiryIsAnUpperBound_data()
    {
        QTest::addColumn<int>("cookieSeconds");
        QTest::addColumn<int>("expected");
        QTest::newRow("session-cookie") << 0 << 3600;
        QTest::newRow("short-cookie") << 120 << 120;
        QTest::newRow("persistent-cookie-cap") << 2592000 << 86400;
    }
    void cookieExpiryIsAnUpperBound()
    {
        QFETCH(int, cookieSeconds);
        QFETCH(int, expected);
        BrowserFixture f;
        QVERIFY(f.ready);
        f.http.responses = {R"({"id":"stable-user","username":"owner"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        const QDateTime before = QDateTime::currentDateTimeUtc();
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("expiry-fixture", cookieSeconds ? before.addSecs(cookieSeconds) : QDateTime{});
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.manager->rowCount(), 1);
        const QDateTime expiry = QDateTime::fromString(f.manager->data(f.manager->index(0), RHSessionManager::ExpiresAtRole).toString(), Qt::ISODateWithMs);
        QVERIFY(before.secsTo(expiry) <= expected);
        QVERIFY(before.secsTo(expiry) >= expected - 3);
    }

    void identityMismatchCannotReplaceLegacyOrStableProfile_data()
    {
        QTest::addColumn<bool>("stable");
        QTest::newRow("legacy-email") << false;
        QTest::newRow("stable-id-same-email") << true;
    }
    void identityMismatchCannotReplaceLegacyOrStableProfile()
    {
        QFETCH(bool, stable);
        BrowserFixture f;
        QVERIFY(f.ready);
        if (stable) {
            f.http.responses = {R"({"id":"selected-id","email":"owner@example.test"})", R"({"results":[]})"};
            f.http.statusCodes = {200, 200};
            QVERIFY(f.manager->beginBrowserLogin());
            f.manager->acceptBrowserToken("old-fixture", {});
        } else {
            QVERIFY(f.manager->saveAuthenticatedSession("owner@example.test", "old-fixture", 3600));
        }
        QTRY_VERIFY(!f.manager->busy());
        const int offset = f.http.requests.size();
        f.http.responses.resize(offset + 1);
        f.http.statusCodes.resize(offset + 1);
        f.http.responses[offset] = stable ? R"({"id":"wrong-id","email":"owner@example.test"})"
                                         : R"({"id":"wrong-id","email":"other@example.test"})";
        f.http.statusCodes[offset] = 200;
        QVERIFY(f.manager->reauthenticate(0));
        f.manager->acceptBrowserToken("wrong-fixture", {});
        QTRY_VERIFY(!f.manager->busy());
        QCOMPARE(f.store.values.value(0), QString("old-fixture"));
        QCOMPARE(f.manager->rowCount(), 1);
        QVERIFY(f.manager->error().contains("match"));
        QCOMPARE(f.http.requests.size(), offset + 1);
    }

    void browserCommitIsNonCancellableAndRollsBackMetadataFailure_data()
    {
        QTest::addColumn<bool>("existing");
        QTest::newRow("new-profile") << false;
        QTest::newRow("renewed-legacy-profile") << true;
    }
    void browserCommitIsNonCancellableAndRollsBackMetadataFailure()
    {
        QFETCH(bool, existing);
        BrowserFixture f;
        QVERIFY(f.ready);
        if (existing) {
            QVERIFY(f.manager->saveAuthenticatedSession("owner@example.test", "old-fixture", 3600));
            QTRY_VERIFY(!f.manager->busy());
        }
        const QString path = f.dir.filePath("sessions.json");
        f.http.responses = {R"({"id":"stable-id","email":"owner@example.test"})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200};
        QSignalSpy busy(f.manager.get(), &RHSessionManager::busyChanged);
        QVERIFY(f.manager->beginBrowserLogin(existing ? QString("owner@example.test") : QString{}));
        bool enteredCommit = false;
        f.store.onWrite = [&] {
            if (enteredCommit) return;
            enteredCommit = true;
            if (existing) QVERIFY(QFile::remove(path));
            QVERIFY(QDir().mkdir(path)); // Fail after ownership validation, not before it.
            QVERIFY(f.manager->busy());
            QVERIFY(!f.manager->browserLoginPending());
            f.manager->cancelSignIn();
            QVERIFY(f.manager->busy());
        };
        f.manager->acceptBrowserToken("replacement-fixture", {});
        QTRY_VERIFY(!f.manager->busy());
        QVERIFY(enteredCommit);
        QCOMPARE(busy.size(), 2);
        QCOMPARE(f.manager->rowCount(), existing ? 1 : 0);
        QCOMPARE(f.store.values.contains(0), existing);
        if (existing) QCOMPARE(f.store.values.value(0), QString("old-fixture"));
        QVERIFY(!f.manager->error().isEmpty());
    }

    void restartedWebSessionRejectsUnboundKeychainReplacement()
    {
        BrowserFixture f;
        QVERIFY(f.ready);
        f.http.responses = {R"({"id":"stable-id","email":"owner@example.test"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("initial-fixture", {});
        QTRY_VERIFY(!f.manager->busy());
        f.manager.reset();
        // A different secret at the same slot is not the committed revision.
        f.store.values[0] = "keychain-fixture";
        const int reads = f.store.reads;
        RHSessionManager restarted(f.network, f.dir.filePath("sessions.json"), &f.store, nullptr, 180000, 2500, &f.locker);
        QCOMPARE(restarted.rowCount(), 1);
        QCOMPARE(restarted.data(restarted.index(0), RHSessionManager::AccountIdRole).toString(), QString("stable-id"));
        QVERIFY(restarted.startSession(0));
        QTRY_VERIFY(!restarted.busy());
        QVERIFY(!restarted.sessionActive());
        QCOMPARE(f.store.reads, reads + 1);
        QCOMPARE(f.http.requests.size(), 2);
        QVERIFY(restarted.error().contains("revision"));
        const auto roles = restarted.roleNames();
        for (const auto &name : roles) QVERIFY(!name.toLower().contains("token"));
        QVERIFY(restarted.metaObject()->indexOfMethod("acceptBrowserToken(QByteArray,QDateTime)") == -1);
    }

    void browserLoginLeavesTheActiveSessionView()
    {
        BrowserFixture f;
        QVERIFY(f.ready);
        QVERIFY(f.manager->saveAuthenticatedSession("owner@example.test", "legacy-fixture", 3600));
        QTRY_VERIFY(!f.manager->busy());
        f.http.responses = {R"({"results":[]})"};
        f.http.statusCodes = {200};
        QVERIFY(f.manager->startSession(0));
        QTRY_VERIFY(f.manager->sessionActive());
        QVERIFY(f.manager->beginBrowserLogin());
        QVERIFY(!f.manager->sessionActive());
        QVERIFY(f.manager->browserLoginPending());
    }

    void legacyUsernameReauthenticationKeepsTheSelectedKeychainSlot()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString config = dir.filePath("api.json");
        QVERIFY(writeApprovalConfig(config, http.server.serverPort()));
        RHNetworkEngine network(nullptr, true);
        QVERIFY(network.loadConfiguration(config));
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath("sessions.json"), &store, nullptr, 180000, 2500, &locker);
        QVERIFY(manager.saveAuthenticatedSession("legacy-owner", "old-fixture", 3600));
        QTRY_VERIFY(!manager.busy());
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        manager.setBrowserLogin(&browser);
        http.responses = {R"({"id":"stable-user","username":"legacy-owner","email":"new@example.test"})", R"({"results":[]})"};
        http.statusCodes = {200, 200};
        QVERIFY(manager.reauthenticate(0));
        manager.acceptBrowserToken("renewed-fixture", {});
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(store.values.value(0), QString("renewed-fixture"));
        QCOMPARE(manager.data(manager.index(0), RHSessionManager::AccountIdRole).toString(), QString("stable-user"));
    }

    void browserCaptureVerifiesIdentityAndAccountsBeforePublishing()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString config = dir.filePath("api.json");
        QVERIFY(writeApprovalConfig(config, http.server.serverPort()));
        RHNetworkEngine network(nullptr, true);
        QVERIFY(network.loadConfiguration(config));
        FakeCredentialStore store;
        TestLocker locker(dir);
        const QString path = dir.filePath("sessions.json");
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        manager.setBrowserLogin(&browser);
        QVERIFY(manager.beginBrowserLogin());
        QVERIFY(manager.browserLoginPending());
        QVERIFY(browser.active());
        manager.acceptBrowserToken("captured-fixture", QDateTime::currentDateTimeUtc().addSecs(7200));
        QTRY_COMPARE(http.requests.size(), 1);
        QVERIFY(http.requests.first().startsWith("GET /user/"));
        QVERIFY(store.values.isEmpty());
        QVERIFY(!QFile::exists(path));
        manager.cancelSignIn();
        QVERIFY(!manager.busy());
        QVERIFY(!manager.browserLoginPending());
        QVERIFY(!browser.active());
        manager.acceptBrowserToken("stale-fixture", {});
        QTest::qWait(30);
        QVERIFY(store.values.isEmpty());
        http.responses = {QByteArray{}, R"({"id":"stable-user","email":"owner@example.test"})", R"({"results":[]})"};
        http.statusCodes = {200, 200, 200};
        QVERIFY(manager.beginBrowserLogin());
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        manager.acceptBrowserToken("captured-fixture", QDateTime::currentDateTimeUtc().addSecs(7200));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(!manager.busy());
        QVERIFY(!manager.browserLoginPending());
        QVERIFY(!browser.active());
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(manager.data(manager.index(0), RHSessionManager::AccountIdRole).toString(), QString("stable-user"));
        QCOMPARE(store.values.value(0), QString("captured-fixture"));
        QFile metadata(path);
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        const QByteArray raw = metadata.readAll();
        QVERIFY(!raw.contains("captured-fixture"));
        QCOMPARE(QJsonDocument::fromJson(raw).object().value("sessions").toArray().first().toObject().value("token_type").toString(), QString("WebSecondaryBearer"));
        QVERIFY(!QFile::exists(dir.filePath("profiles.dat")));
    }

    void headlessPasswordLoginIsDisabled()
    {
        QTemporaryDir dir;
        ApprovalServer http;
        QVERIFY(http.server.listen(QHostAddress::LocalHost));
        const QString config = dir.filePath("api.json");
        QVERIFY(writeApprovalConfig(config, http.server.serverPort()));
        RHNetworkEngine network(nullptr, true);
        QVERIFY(network.loadConfiguration(config));
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath("sessions.json"), &store,
                                 nullptr, 180000, 2500, &locker);
        QVERIFY(!manager.signIn("owner@example.test", "private-password"));
        QVERIFY(!manager.busy());
        QTest::qWait(30);
        QVERIFY(http.requests.isEmpty());
        QVERIFY(store.values.isEmpty());
        QVERIFY(!QFile::exists(dir.filePath("profiles.dat")));
        QVERIFY(!manager.error().contains("private-password"));
    }

    void saveSeparatesSecretFromMetadataAndBoundsLifetime()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
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

    void renewedProfileReplacesTokenWithoutDuplicatingAccount()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("old"), 1));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("renewed"), 3600));
        QTRY_COMPARE(saved.size(), 2);
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(saved.last().first().toInt(), 0);
        QCOMPARE(store.values.value(0), QStringLiteral("renewed"));
        QVERIFY(!manager.data(manager.index(0), RHSessionManager::IsExpiredRole).toBool());
    }

    void expiredRemovalAndGapReuse()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("first@test.invalid"), QStringLiteral("one"), 1));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("second@test.invalid"), QStringLiteral("two"), 1));
        QTRY_COMPARE(saved.size(), 2);
        QTest::qWait(1200);
        QVERIFY(manager.data(manager.index(0), RHSessionManager::IsExpiredRole).toBool());
        QVERIFY(!manager.startSession(0));
        // Missing network configuration must not destroy the expired profile/token.
        QVERIFY(!manager.reauthenticate(0));
        QCOMPARE(manager.rowCount(), 2);
        QCOMPARE(store.values.value(0), QStringLiteral("one"));
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("third@test.invalid"), QStringLiteral("three"), 3600));
        QTRY_COMPARE(saved.size(), 3);
        QCOMPARE(manager.data(manager.index(1), RHSessionManager::SessionIndexRole).toInt(), 0);
    }

    void deletingSavedLoginRemovesOnlyItsTokenAndMetadata()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QSignalSpy deleted(&manager, &RHSessionManager::sessionDeleted);
        QSignalSpy reauth(&manager, &RHSessionManager::reauthenticationRequested);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("first@test.invalid"), QStringLiteral("one"), 3600));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("second@test.invalid"), QStringLiteral("two"), 3600));
        QTRY_COMPARE(saved.size(), 2);
        QVERIFY(manager.deleteSavedSession(0));
        QVERIFY(manager.busy());
        QVERIFY(!manager.deleteSavedSession(1));
        QTRY_COMPARE(deleted.size(), 1);
        QCOMPARE(deleted.first().first().toInt(), 0);
        QCOMPARE(reauth.size(), 0);
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(manager.data(manager.index(0), RHSessionManager::SessionIndexRole).toInt(), 1);
        QVERIFY(!store.values.contains(0));
        QCOMPARE(store.values.value(1), QStringLiteral("two"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray entries = QJsonDocument::fromJson(file.readAll()).object().value("sessions").toArray();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().toObject().value("index").toInt(), 1);
        RHSessionManager reloaded(network, path, &store);
        QCOMPARE(reloaded.rowCount(), 1);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("third@test.invalid"), QStringLiteral("three"), 3600));
        QTRY_COMPARE(saved.size(), 3);
        QCOMPARE(manager.data(manager.index(1), RHSessionManager::SessionIndexRole).toInt(), 0);
    }

    void deletingProfileAlsoRemovesEncryptedPassword()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store,
                                 nullptr, 180000, 2500, &locker);
        bool stored = false;
        locker.save(QStringLiteral("owner@test.invalid"), QStringLiteral("owner@test.invalid"),
                    QStringLiteral("password"), [&](bool ok, QString) { stored = ok; });
        QTRY_VERIFY(stored);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("token"), 3600));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 0);
        bool completed = false;
        bool found = true;
        locker.read(QStringLiteral("owner@test.invalid"), [&](bool ok, QString, QString, QString) {
            found = ok;
            completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(!found);
    }

    void failedDeleteKeepsSavedLoginAvailableForRetry()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QSignalSpy deleted(&manager, &RHSessionManager::sessionDeleted);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("one"), 3600));
        QTRY_COMPARE(saved.size(), 1);
        store.failRemove = true;
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(deleted.size(), 0);
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(store.values.value(0), QStringLiteral("one"));
        QVERIFY(!manager.error().isEmpty());
        QVERIFY(QFile::exists(path));
        store.failRemove = false;
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_COMPARE(deleted.size(), 1);
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(store.values.isEmpty());
    }

    void metadataFailureAfterKeychainRemovalCanBeRetried()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QSignalSpy deleted(&manager, &RHSessionManager::sessionDeleted);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("one"), 3600));
        QTRY_COMPARE(saved.size(), 1);
        QLockFile lock(path + ".lock"); QVERIFY(lock.tryLock()); // Block only the commit, not ownership validation.
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(deleted.size(), 0);
        QCOMPARE(manager.rowCount(), 1);
        QVERIFY(store.values.isEmpty());
        QVERIFY(!manager.error().isEmpty());
        lock.unlock();
        QVERIFY(manager.deleteSavedSession(0)); // Missing keychain entry is idempotent.
        QTRY_COMPARE(deleted.size(), 1);
        QCOMPARE(manager.rowCount(), 0);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("sessions").toArray().size(), 0);
    }

    void malformedMetadataFailsClosed()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{not-json");
        file.close();
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QVERIFY(!manager.load());
        QVERIFY(!manager.saveAuthenticatedSession(QStringLiteral("person@test.invalid"), QStringLiteral("secret"), 3600));
        QVERIFY(store.values.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("{not-json"));
    }

    void keychainFailureDoesNotCreateMetadata()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        store.failWrite = true;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("temporary"), 3600));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(!QFile::exists(path));
        QVERIFY(!manager.error().isEmpty());
    }

    void renewalMetadataFailureRestoresOriginalToken_data()
    {
        QTest::addColumn<bool>("missingToken");
        QTest::newRow("existing-token") << false;
        QTest::newRow("deleted-token") << true;
    }
    void renewalMetadataFailureRestoresOriginalToken()
    {
        QFETCH(bool, missingToken);
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("original"), 3600));
        QTRY_VERIFY(!manager.busy());
        if (missingToken)
            store.values.remove(0);
        bool sabotaged = false;
        store.onWrite = [&] {
            if (sabotaged) return;
            sabotaged = true; QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path));
        };
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("replacement"), 3600));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(store.values.contains(0), !missingToken);
        QCOMPARE(store.values.value(0), missingToken ? QString() : QStringLiteral("original"));
        QVERIFY(!manager.error().isEmpty());
    }

    void renewalBackendReadFailurePreservesTokenAndMetadata()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("original"), 3600));
        QTRY_COMPARE(saved.size(), 1);
        QFile metadata(path);
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        const QByteArray before = metadata.readAll();
        metadata.close();
        store.failRead = true;
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("replacement"), 7200));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(manager.rowCount(), 1);
        QCOMPARE(store.values.value(0), QStringLiteral("original"));
        QCOMPARE(manager.error(), QStringLiteral("Keychain unavailable"));
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        QCOMPARE(metadata.readAll(), before);
    }

    void missingTokenCannotStartSession()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store,
                                 nullptr, 180000, 2500, &locker);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("original"), 3600));
        QTRY_VERIFY(!manager.busy());
        store.values.remove(0);
        QVERIFY(manager.startSession(0));
        QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.sessionActive());
        QCOMPARE(manager.error(), QStringLiteral("Saved session token is unavailable."));
    }

    void deletingLegacyTokenOnlyProfileDoesNotFetchMasterKey()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        int fetched = 0;
        const QString path = dir.filePath(QStringLiteral("profiles.dat"));
        SecureCredentialLocker locker(path, [&](bool, SecureCredentialLocker::KeyCompletion done) {
            ++fetched;
            done(false, {}, QStringLiteral("Master key missing"));
        });
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store,
                                 nullptr, 180000, 2500, &locker);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("legacy@test.invalid"), QStringLiteral("token"), 3600));
        QTRY_VERIFY(!manager.busy());
        QSignalSpy deleted(&manager, &RHSessionManager::sessionDeleted);
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_COMPARE(deleted.size(), 1);
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(store.values.isEmpty());
        QCOMPARE(fetched, 0);
        QVERIFY(!QFile::exists(path));
        QVERIFY(!QFile::exists(path + QStringLiteral(".lock")));
    }

    void metadataFailureRollsBackNewKeychainEntry()
    {
        QTemporaryDir dir;
        RHNetworkEngine network(nullptr, true);
        FakeCredentialStore store;
        const QString path = dir.filePath(QStringLiteral("sessions.json"));
        TestLocker locker(dir);
        RHSessionManager manager(network, path, &store, nullptr, 180000, 2500, &locker);
        store.onWrite = [&] { QVERIFY(QDir().mkdir(path)); }; // Fail after pre-write ownership validation.
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("temporary"), 3600));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.rowCount(), 0);
        QVERIFY(store.values.isEmpty());
        QVERIFY(!manager.error().isEmpty());
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
        RHNetworkEngine network(nullptr, true);
        QString error;
        QVERIFY2(network.loadConfiguration(config.fileName(), &error), qPrintable(error));
        FakeCredentialStore store;
        TestLocker locker(dir);
        RHSessionManager manager(network, dir.filePath(QStringLiteral("sessions.json")), &store, nullptr, 180000, 2500, &locker);
        QSignalSpy saved(&manager, &RHSessionManager::sessionSaved);
        QVERIFY(manager.saveAuthenticatedSession(QStringLiteral("owner@test.invalid"), QStringLiteral("mock-token"), 3600));
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(manager.startSession(0));
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable() > 0);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("GET /accounts/ HTTP/1.1"));
        QVERIFY(request.contains(QByteArray("Authorization: ") + QByteArray("Bea" "rer ") + store.values.value(0).toUtf8()));
        QVERIFY(!manager.sessionActive());
        const QByteArray response = R"({"results":[{"account_number":"100","type":"margin","rh_account_type":"individual","total_equity":"10245.50"},{"account_number":"200","type":"cash","rh_account_type":"roth_ira","equity":"501.23"},{"account_number":"300","type":"cash","description":"Traditional IRA","cash_balances":{"cash":"7000.00"}}]})";
        peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
        peer->flush();
        QTRY_VERIFY(manager.sessionActive());
        QAbstractItemModel *accounts = manager.accounts();
        QCOMPARE(accounts->rowCount(), 3);
        QCOMPARE(accounts->data(accounts->index(0, 0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Standard Brokerage"));
        QCOMPARE(accounts->data(accounts->index(0, 0), RHAccountModel::AccountNumberRole).toString(), QStringLiteral("100"));
        QCOMPARE(accounts->data(accounts->index(0, 0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$10,245.50"));
        QCOMPARE(accounts->data(accounts->index(1, 0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Roth IRA"));
        QCOMPARE(accounts->data(accounts->index(1, 0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$501.23"));
        QCOMPARE(accounts->data(accounts->index(2, 0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Standard Brokerage"));
        QCOMPARE(accounts->data(accounts->index(2, 0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$7,000.00"));
        QCOMPARE(accounts->roleNames().value(RHAccountModel::CurrentBalanceRole), QByteArray("currentBalance"));
        manager.clearActiveSession();
        QVERIFY(!manager.sessionActive());
        QCOMPARE(accounts->rowCount(), 0);
        QVERIFY(!store.values.value(0).isEmpty());
    }

    void accountModelPreservesUnknownBalancesAndNestedIraType()
    {
        RHAccountModel accounts;
        QSignalSpy resets(&accounts, &QAbstractItemModel::modelReset);
        accounts.setAccounts(QJsonArray{
            QJsonObject{{"account_number", "A"}, {"type", "cash"},
                        {"retirement", QJsonObject{{"account_type", "traditional_ira"}}},
                        {"total_equity", 12.345}},
            QJsonObject{{"account_number", "B"}, {"type", "margin"},
                        {"is_retirement", true}, {"cash_balances", QJsonObject{{"cash", "800.00"}}}},
            QJsonObject{{"rhs_account_number", "C"}, {"type", "cash"},
                        {"nickname", "Holiday account"}, {"equity", "not-a-number"}},
            QJsonObject{{"account_number", "E"}, {"type", "cash"},
                        {"rh_account_type", "retirement"}}
        });
        QCOMPARE(accounts.rowCount(), 4);
        QCOMPARE(accounts.data(accounts.index(0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("IRA"));
        QCOMPARE(accounts.data(accounts.index(0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$12.35"));
        QCOMPARE(accounts.data(accounts.index(1), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Account type unavailable"));
        QCOMPARE(accounts.data(accounts.index(1), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("Unavailable"));
        QCOMPARE(accounts.data(accounts.index(2), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Standard Brokerage"));
        QCOMPARE(accounts.data(accounts.index(2), RHAccountModel::AccountNumberRole).toString(), QStringLiteral("C"));
        QCOMPARE(accounts.data(accounts.index(2), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("Unavailable"));
        QCOMPARE(accounts.data(accounts.index(3), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Account type unavailable"));
        accounts.setAccounts(QJsonArray{QJsonObject{{"account_number", "D"}, {"type", "cash"},
                                                    {"equity", "0.00"}}});
        QCOMPARE(accounts.rowCount(), 1);
        QCOMPARE(accounts.data(accounts.index(0), RHAccountModel::AccountNumberRole).toString(), QStringLiteral("D"));
        QCOMPARE(accounts.data(accounts.index(0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$0.00"));
        QCOMPARE(resets.size(), 2);
    }
};

QTEST_MAIN(RHSessionManagerTest)
#include "test_rhsessionmanager.moc"
