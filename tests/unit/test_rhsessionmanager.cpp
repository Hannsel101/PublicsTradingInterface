#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include "rhbrowserlogin.h"
#include "securecredentiallocker.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
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
    bool failWrite = false;
    bool failRead = false;
    bool failRemove = false;
    int reads = 0;
    int writes = 0;
    std::function<void()> onWrite;
    void write(int index, const QString &token, Completion done) override
    {
        ++writes;
        if (onWrite) onWrite();
        QTimer::singleShot(0, this, [this, index, token, done = std::move(done)]() {
            if (!failWrite)
                values.insert(index, token);
            done(!failWrite, failWrite ? QStringLiteral("Keychain unavailable") : QString());
        });
    }
    void read(int index, ReadCompletion done) override
    {
        ++reads;
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
        if (existing) QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path));
        bool enteredCommit = false;
        f.store.onWrite = [&] {
            enteredCommit = true;
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

    void restartedWebSessionReadsTheKeychainInsteadOfCachingTheToken()
    {
        BrowserFixture f;
        QVERIFY(f.ready);
        f.http.responses = {R"({"id":"stable-id","email":"owner@example.test"})", R"({"results":[]})", R"({"results":[]})"};
        f.http.statusCodes = {200, 200, 200};
        QVERIFY(f.manager->beginBrowserLogin());
        f.manager->acceptBrowserToken("initial-fixture", {});
        QTRY_VERIFY(!f.manager->busy());
        f.manager.reset();
        f.store.values[0] = "keychain-fixture";
        const int reads = f.store.reads;
        RHSessionManager restarted(f.network, f.dir.filePath("sessions.json"), &f.store, nullptr, 180000, 2500, &f.locker);
        QCOMPARE(restarted.rowCount(), 1);
        QCOMPARE(restarted.data(restarted.index(0), RHSessionManager::AccountIdRole).toString(), QString("stable-id"));
        QVERIFY(restarted.startSession(0));
        QTRY_VERIFY(restarted.sessionActive());
        QCOMPARE(f.store.reads, reads + 1);
        const QByteArray request = f.http.requests.last();
        QVERIFY(request.startsWith("GET /accounts/"));
        QVERIFY(request.contains(QByteArray("Authorization: ") + QByteArray("Bea" "rer ") + QByteArray("keychain-fixture")));
        QVERIFY(!request.contains("initial-fixture"));
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
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path)); // Block the atomic metadata commit.
        QVERIFY(manager.deleteSavedSession(0));
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(deleted.size(), 0);
        QCOMPARE(manager.rowCount(), 1);
        QVERIFY(store.values.isEmpty());
        QVERIFY(!manager.error().isEmpty());
        QVERIFY(QDir().rmdir(path));
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
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path));
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
        QVERIFY(QDir().mkdir(path)); // Occupy the file path after initial load.
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
        QCOMPARE(accounts->data(accounts->index(0, 0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Individual Investing"));
        QCOMPARE(accounts->data(accounts->index(0, 0), RHAccountModel::AccountNumberRole).toString(), QStringLiteral("100"));
        QCOMPARE(accounts->data(accounts->index(0, 0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$10,245.50"));
        QCOMPARE(accounts->data(accounts->index(1, 0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Roth IRA"));
        QCOMPARE(accounts->data(accounts->index(1, 0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$501.23"));
        QCOMPARE(accounts->data(accounts->index(2, 0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Traditional IRA"));
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
        QCOMPARE(accounts.data(accounts.index(0), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Traditional IRA"));
        QCOMPARE(accounts.data(accounts.index(0), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("$12.35"));
        QCOMPARE(accounts.data(accounts.index(1), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Retirement Account"));
        QCOMPARE(accounts.data(accounts.index(1), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("Unavailable"));
        QCOMPARE(accounts.data(accounts.index(2), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Holiday account"));
        QCOMPARE(accounts.data(accounts.index(2), RHAccountModel::AccountNumberRole).toString(), QStringLiteral("C"));
        QCOMPARE(accounts.data(accounts.index(2), RHAccountModel::CurrentBalanceRole).toString(), QStringLiteral("Unavailable"));
        QCOMPARE(accounts.data(accounts.index(3), RHAccountModel::DisplayNameRole).toString(), QStringLiteral("Retirement Account"));
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
