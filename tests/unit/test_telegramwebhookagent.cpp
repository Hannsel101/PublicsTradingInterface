#include "telegramagent.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QTimer>
#include <QtTest>
#include <functional>

// Synthetic, independently enumerated Bot API User/Message/WebhookInfo fixtures.
// No request-echo receipts; traffic is loopback and credentials are synthetic.
class WebhookTestServer : public QTcpServer
{
public:
    struct Request {
        QByteArray method;
        QUrl url;
        QMap<QByteArray, QByteArray> headers;
        QJsonObject body;
        QPointer<QTcpSocket> socket;
    };
    QList<Request> requests;
    std::function<void(const Request &)> handler;
    explicit WebhookTestServer(quint16 port = 0)
    {
        if (!listen(QHostAddress::LocalHost, port)) qFatal("Offline listener unavailable");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    *bytes += socket->readAll();
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    QMap<QByteArray, QByteArray> headers;
                    for (const auto &line : bytes->left(end).split('\n')) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0) headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
                    }
                    const auto length = headers.value("content-length").toInt();
                    if (bytes->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto first = bytes->left(bytes->indexOf('\r')).split(' ');
                    Request r{first.value(0), QUrl::fromEncoded(first.value(1)), headers,
                        QJsonDocument::fromJson(bytes->mid(end + 4, length)).object(), socket};
                    requests.append(r);
                    if (handler) handler(r);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); }
    static void raw(const Request &r, QByteArray body, int status = 200)
    {
        if (!r.socket || r.socket->state() != QAbstractSocket::ConnectedState) return;
        r.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        r.socket->disconnectFromHost();
    }
    static void answer(const Request &r, QJsonValue result)
    { raw(r, QJsonDocument(QJsonObject{{"ok", true}, {"result", result}}).toJson(QJsonDocument::Compact)); }
    int count(const QString &method) const
    {
        int total = 0;
        for (const auto &r : requests) if (r.url.path().endsWith('/' + method)) ++total;
        return total;
    }
    Request last(const QString &method) const
    {
        for (auto it = requests.crbegin(); it != requests.crend(); ++it)
            if (it->url.path().endsWith('/' + method)) return *it;
        return {};
    }
};

static quint16 freePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) qFatal("No offline port");
    return server.serverPort();
}

struct WebhookFixture
{
    QTemporaryDir ownership;
    WebhookTestServer provider;
    WebhookTestServer health;
    TelegramAgent::WebhookOptions options;
    QMap<QString, QString> urls;
    QMap<QString, QString> secrets;
    bool rejectRegistration = false;
    bool holdRegistration = false;
    bool holdSend = false;
    bool holdInfo = false;
    bool holdDelete = false;
    bool malformedInfo = false;
    int healthStatus = 200;
    WebhookFixture()
    {
        if (!ownership.isValid()) qFatal("Offline ownership directory unavailable");
        options.tunnel.ownershipDirectory = ownership.filePath(QStringLiteral("tunnel-ownership"));
        options.tunnel.port = freePort();
        const auto inspector = freePort();
        options.tunnel.executable = QCoreApplication::applicationFilePath();
        options.tunnel.argumentPrefix = {QStringLiteral("--fake-ngrok"), QString::number(inspector), QString::number(options.tunnel.port)};
        options.tunnel.inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/tunnels").arg(inspector));
        options.tunnel.discoveryDelayMs = 10;
        options.tunnel.startupTimeoutMs = 3000;
        health.handler = [this](const auto &r) { WebhookTestServer::raw(r, "ok", healthStatus); };
        provider.handler = [this](const WebhookTestServer::Request &r) {
            const auto parts = r.url.path().split('/');
            const auto bot = parts.value(1);
            const auto method = parts.value(2);
            const QUrlQuery query(r.url);
            if (method == "getMe") {
                WebhookTestServer::answer(r, QJsonObject{{"id", bot.mid(3).section(':', 0, 0).toLongLong()},
                    {"is_bot", true}, {"first_name", "Offline"}, {"username", "offline_bot"}});
            } else if (method == "setWebhook") {
                if (holdRegistration) return;
                if (rejectRegistration) { WebhookTestServer::answer(r, false); return; }
                urls[bot] = r.body.value("url").toString();
                secrets[bot] = r.body.value("secret_token").toString();
                WebhookTestServer::answer(r, true);
            } else if (method == "getWebhookInfo") {
                if (holdInfo) return;
                WebhookTestServer::answer(r, malformedInfo ? QJsonObject{} : QJsonObject{
                    {"url", urls.value(bot)}, {"has_custom_certificate", false}, {"pending_update_count", 0}});
            } else if (method == "deleteWebhook") {
                urls[bot].clear();
                if (holdDelete) return;
                WebhookTestServer::answer(r, true);
            } else if (method == "sendMessage") {
                if (!holdSend) acknowledge(r);
            } else {
                WebhookTestServer::raw(r, "{\"ok\":false,\"error_code\":409}", 409);
            }
        };
    }
    static void acknowledge(const WebhookTestServer::Request &r)
    {
        WebhookTestServer::answer(r, QJsonObject{{"message_id", 90}, {"date", 1700000001},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}});
    }
    QUrl healthUrl() const { return health.root().resolved(QUrl("/health")); }
    QUrl receiver(const QString &bot = QStringLiteral("bot123456:synthetic")) const
    {
        QUrl url(urls.value(bot));
        url.setScheme("http"); url.setHost("127.0.0.1"); url.setPort(options.tunnel.port);
        return url;
    }
    static QJsonObject update(qint64 id, QString text = QStringLiteral("hello"))
    {
        return {{"update_id", id}, {"message", QJsonObject{
            {"message_id", 77}, {"date", 1700000000}, {"text", text},
            {"from", QJsonObject{{"id", 42}, {"is_bot", false}, {"first_name", "Human"}}},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}, {"first_name", "Human"}}}}}};
    }
    int post(const QJsonObject &update, const QString &bot = QStringLiteral("bot123456:synthetic"), QByteArray overrideSecret = {})
    {
        QNetworkAccessManager network;
        QNetworkRequest request(receiver(bot));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("X-Telegram-Bot-Api-Secret-Token", overrideSecret.isNull() ? secrets.value(bot).toUtf8() : overrideSecret);
        auto *reply = network.post(request, QJsonDocument(update).toJson(QJsonDocument::Compact));
        if (!QTest::qWaitFor([&] { return reply->isFinished(); }, 3000)) return -1;
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }
};

class TestTelegramWebhookAgent : public QObject
{
    Q_OBJECT
private slots:
    void statusAndUnknownAreDeniedWithoutAllowlist()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(1, "status")), 200);
        QCOMPARE(f.post(f.update(2, "unknown")), 200);
        QTest::qWait(100);
        QCOMPARE(f.health.requests.size(), 0);
        QCOMPARE(f.provider.count("sendMessage"), 0);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void saturatedCommandQueueIsRetryableNotAcknowledged()
    {
        QTemporaryDir ledger;
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setCommandLedgerPathForTests(ledger.path());
        agent.setAllowedChatIds({"42"});
        QSignalSpy commands(&agent, &TelegramAgent::tradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QTRY_COMPARE(commands.size(), 1);
        for (int i = 2; i <= 101; ++i)
            QCOMPARE(f.post(f.update(i, "publics_auto_buy MSFT")), 200);
        QCOMPARE(f.post(f.update(102, "publics_auto_buy NVDA")), 503);
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200); // duplicate, no work
        QCOMPARE(commands.size(), 1);
        QFile watermark(ledger.filePath("123456.watermark"));
        QVERIFY(watermark.open(QIODevice::ReadOnly));
        QCOMPARE(watermark.readAll(), QByteArray("PTI-TRADE-1\n123456\n101\n"));
        connect(&agent, &TelegramAgent::tradeCommandReceived, &agent,
            [&agent](const QString &id, const auto &, const auto &) { agent.completeTradeCommand(id, {}); });
        agent.completeTradeCommand(commands[0][0].toString(), {});
        QTRY_COMPARE(commands.size(), 101);
        QCOMPARE(f.post(f.update(102, "publics_auto_buy NVDA")), 200);
        QTRY_COMPARE(commands.size(), 102);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void durableAdmissionFailureReturnsRetryable503()
    {
        QTemporaryDir ledger;
        QFile corrupt(ledger.filePath("123456.watermark"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("not a valid ledger"); corrupt.close();
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setCommandLedgerPathForTests(ledger.path());
        agent.setAllowedChatIds({"42"});
        QSignalSpy commands(&agent, &TelegramAgent::tradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 503);
        QCOMPARE(commands.size(), 0);
        QVERIFY(agent.active()); // transient ledger failure does not disable the retry route
        QVERIFY(corrupt.remove());
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QTRY_COMPARE(commands.size(), 1);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void webhookCommandsShareAuthorizationAndDurableBotScopedAdmission()
    {
        QTemporaryDir ledger;
        WebhookFixture f;
        TelegramAgent first(f.provider.root(), f.healthUrl(), f.options);
        first.setAllowedChatIds({"42"});
        TelegramAgent second(f.provider.root(), f.healthUrl(), f.options);
        second.setAllowedChatIds({"42"});
        for (auto *agent : {&first, &second}) {
            agent->setCommandLedgerPathForTests(ledger.path()); agent->setAllowedChatIds({"42"});
        }
        QSignalSpy one(&first, &TelegramAgent::tradeCommandReceived), two(&second, &TelegramAgent::tradeCommandReceived);
        first.start("123456:synthetic", "persisted_secret_123456"); second.start("654321:synthetic", "persisted_secret_654321");
        QTRY_VERIFY_WITH_TIMEOUT(first.active() && second.active(), 5000);
        QVERIFY(f.post(f.update(100, "publics_auto_buy AAPL"), "bot123456:synthetic", "wrong") >= 400);
        auto denied = f.update(100, "publics_auto_buy AAPL"); auto message = denied["message"].toObject();
        auto chat = message["chat"].toObject(); chat["id"] = 84; message["chat"] = chat; denied["message"] = message;
        QCOMPARE(f.post(denied), 200); QCOMPARE(one.size(), 0);
        QCOMPARE(f.post(f.update(100, "publics_auto_buy aapl, msft,aapl")), 200);
        QTRY_COMPARE(one.size(), 1);
        QCOMPARE(one[0][2].toStringList(), (QStringList{"AAPL", "MSFT"}));
        QCOMPARE(f.post(f.update(100, "publics_auto_buy AAPL")), 200);
        QCOMPARE(f.post(f.update(99, "publics_auto_buy NVDA")), 200); // monotonic old-ID rejection
        QCOMPARE(f.post(f.update(100, "publics_auto_sell AAPL"), "bot654321:synthetic"), 200);
        QTRY_COMPARE(two.size(), 1); QCOMPARE(two[0][1].toString(), QString("sell"));
        QCOMPARE(one.size(), 1);
        const auto stale = one[0][0].toString();
        first.stop(); QTRY_VERIFY(!first.stopping());
        first.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(first.active(), 5000);
        first.completeTradeCommand(stale, {"STALE"});
        QCOMPARE(f.post(f.update(100, "publics_auto_buy AAPL")), 200);
        QTest::qWait(100); QCOMPARE(one.size(), 1); QCOMPARE(f.provider.count("sendMessage"), 0);
        first.stop(); second.stop(); QTRY_VERIFY(!first.stopping() && !second.stopping());
    }
    void explicitWebhookModeIsInitiallyIdle()
    {
        TelegramAgent::WebhookOptions options;
        TelegramAgent agent(QUrl("http://127.0.0.1:1"), QUrl("http://127.0.0.1:2/health"), options);
        agent.setAllowedChatIds({"42"});
        QVERIFY(!agent.active()); QVERIFY(!agent.starting()); QVERIFY(!agent.stopping());
    }
    void registrationReadbackAndOwnedCleanup()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QVERIFY(agent.healthy());
        const auto registration = f.provider.last("setWebhook");
        QCOMPARE(registration.method, QByteArray("POST"));
        QVERIFY(!registration.url.hasQuery());
        QCOMPARE(registration.body.value("allowed_updates").toArray(), QJsonArray{"message"});
        QCOMPARE(registration.body.value("max_connections").toInt(), 1);
        QCOMPARE(registration.body.value("drop_pending_updates"), QJsonValue(false));
        QCOMPARE(registration.body.value("secret_token").toString(), QString("persisted_secret_123456"));
        QCOMPARE(f.provider.count("getWebhookInfo"), 1);
        QCOMPARE(f.provider.count("getUpdates"), 0);
        agent.stop();
        QVERIFY(agent.stopping()); QVERIFY(!agent.active()); QVERIFY(!agent.healthy()); QVERIFY(!agent.starting());
        agent.start("654321:synthetic", "persisted_secret_654321");
        agent.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!agent.stopping(), 5000);
        QCOMPARE(f.provider.count("getMe"), 1);
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
        QCOMPARE(f.provider.last("deleteWebhook").method, QByteArray("GET"));
        QCOMPARE(QUrlQuery(f.provider.last("deleteWebhook").url).queryItemValue("drop_pending_updates"), QString("false"));
        QCOMPARE(f.provider.count("getWebhookInfo"), 3);
        QVERIFY(f.urls.value("bot123456:synthetic").isEmpty());
        QCOMPARE(f.provider.count("getUpdates"), 0);
        QTcpServer receiver;
        QVERIFY(receiver.listen(QHostAddress::LocalHost, f.options.tunnel.port));
    }
    void updatesDuringRegistrationWaitForVerifiedActivation()
    {
        QTemporaryDir ledger;
        WebhookFixture f;
        f.holdInfo = true;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy commands(&agent, &TelegramAgent::tradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("getWebhookInfo"), 1, 5000);
        QVERIFY(agent.starting());
        QVERIFY(!agent.active());
        QCOMPARE(f.post(f.update(1, "/status")), 200);
        QCOMPARE(f.post(f.update(2, "publics_auto_buy AAPL")), 200);
        QCOMPARE(commands.size(), 0);
        QVERIFY(QFile::exists(ledger.filePath("123456.watermark")));
        QCOMPARE(f.provider.count("sendMessage"), 0);
        QCOMPARE(f.health.requests.size(), 0);
        f.holdInfo = false;
        WebhookTestServer::answer(f.provider.last("getWebhookInfo"), QJsonObject{
            {"url", f.urls.value("bot123456:synthetic")}, {"has_custom_certificate", false}, {"pending_update_count", 0}});
        QTRY_VERIFY(agent.active());
        QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("sendMessage"), 1, 1500);
        QCOMPARE(f.provider.last("sendMessage").body.value("text").toString(), QStringLiteral("🟢 Active"));
        QTRY_COMPARE(commands.size(), 1);
        agent.stop();
        QTRY_VERIFY(!agent.stopping());
    }
    void stopWaitsForRegistrationThatCommitsLate()
    {
        WebhookFixture f;
        f.holdRegistration = true;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("setWebhook"), 1, 5000);
        const auto registration = f.provider.last("setWebhook");
        agent.stop();
        QTest::qWait(250);
        QVERIFY(agent.stopping());
        const QUrlQuery query(registration.url);
        f.urls["bot123456:synthetic"] = registration.body.value("url").toString();
        WebhookTestServer::answer(registration, true);
        QTRY_VERIFY_WITH_TIMEOUT(!agent.stopping(), 5000);
        QVERIFY(f.urls.value("bot123456:synthetic").isEmpty());
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
    }
    void duplicateWhileHealthBusyAndOutOfOrder()
    {
        WebhookFixture f;
        f.health.handler = {};
        f.holdSend = true;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(200, "/status")), 200);
        QTRY_COMPARE(f.health.requests.size(), 1);
        QCOMPARE(f.post(f.update(199)), 200);
        QCOMPARE(f.post(f.update(199)), 200);
        QCOMPARE(f.post(f.update(200, "/status")), 200);
        QCOMPARE(f.provider.count("sendMessage"), 0);
        QVERIFY(!f.health.requests.first().headers.contains("x-pti-health-secret"));
        WebhookTestServer::raw(f.health.requests.first(), "ok", 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 1);
        QCOMPARE(f.provider.last("sendMessage").body.value("text").toString(), QString("🟢 Active"));
        QCOMPARE(f.post(f.update(199)), 200);
        QCOMPARE(f.provider.count("sendMessage"), 1);
        f.acknowledge(f.provider.last("sendMessage"));
        QTRY_COMPARE(f.provider.count("sendMessage"), 2);
        QCOMPARE(f.provider.last("sendMessage").body.value("text").toString(), QString("Message received."));
        f.acknowledge(f.provider.last("sendMessage"));
        agent.stop(); QTRY_VERIFY(!agent.stopping());
        QCOMPARE(f.provider.count("sendMessage"), 2);
    }
    void healthInactiveThenRecovery()
    {
        WebhookFixture f;
        f.healthStatus = 503;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(1, "status")), 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 1);
        QCOMPARE(f.provider.last("sendMessage").body.value("text").toString(), QString("Inactive"));
        QVERIFY(!agent.healthy()); QVERIFY(agent.active());
        f.healthStatus = 200;
        QCOMPARE(f.post(f.update(2, "/status")), 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 2);
        QCOMPARE(f.provider.last("sendMessage").body.value("text").toString(), QString("🟢 Active"));
        QVERIFY(agent.healthy());
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void twoBotsIsolatedAndPostAuthenticated()
    {
        WebhookFixture f;
        TelegramAgent first(f.provider.root(), f.healthUrl(), f.options);
        first.setAllowedChatIds({"42"});
        TelegramAgent second(f.provider.root(), f.healthUrl(), f.options);
        second.setAllowedChatIds({"42"});
        first.start("123456:synthetic", "persisted_secret_123456"); second.start("654321:synthetic", "persisted_secret_654321");
        QTRY_VERIFY_WITH_TIMEOUT(first.active() && second.active(), 5000);
        QVERIFY(f.urls.value("bot123456:synthetic") != f.urls.value("bot654321:synthetic"));
        QVERIFY(f.post(f.update(1), "bot123456:synthetic", "wrong") >= 400);
        QCOMPARE(f.provider.count("sendMessage"), 0);
        QVERIFY(f.post(f.update(1), "bot123456:synthetic", f.secrets.value("bot654321:synthetic").toUtf8()) >= 400);
        QCOMPARE(f.post(f.update(1), "bot654321:synthetic"), 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 1);
        QVERIFY(f.provider.last("sendMessage").url.path().startsWith("/bot654321:"));
        auto group = f.update(2); auto message = group["message"].toObject();
        auto chat = message["chat"].toObject(); chat["type"] = "group"; message["chat"] = chat; group["message"] = message;
        QCOMPARE(f.post(group), 200);
        QCOMPARE(f.provider.count("sendMessage"), 1);
        first.stop(); QTRY_VERIFY(!first.stopping());
        QVERIFY(second.active());
        QCOMPARE(f.post(f.update(2), "bot654321:synthetic"), 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 2);
        second.stop(); QTRY_VERIFY(!second.stopping());
        QCOMPARE(f.provider.count("getUpdates"), 0);
    }
    void stopNeverDeletesReplacementWebhook()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        f.urls["bot123456:synthetic"] = "https://someone-else.invalid/new";
        agent.stop(); QTRY_VERIFY(!agent.stopping());
        QCOMPARE(f.provider.count("deleteWebhook"), 0);
        QCOMPARE(f.urls.value("bot123456:synthetic"), QString("https://someone-else.invalid/new"));
    }
    void failingRegistrationDoesNotActivateOrPoll()
    {
        WebhookFixture f; f.rejectRegistration = true;
        QTest::ignoreMessage(QtDebugMsg, "Telegram agent error: Telegram webhook registration was not confirmed.");
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 5000);
        QTRY_VERIFY(!agent.stopping());
        QVERIFY(!agent.active()); QVERIFY(!agent.starting()); QVERIFY(!agent.healthy());
        QCOMPARE(f.provider.count("setWebhook"), 1);
        QCOMPARE(f.provider.count("getUpdates"), 0);
        QCOMPARE(f.provider.count("deleteWebhook"), 0);
    }
    void lostRegistrationReceiptCleansOnlyReadbackOwnedUrl()
    {
        WebhookFixture f;
        auto standard = f.provider.handler;
        f.provider.handler = [standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook")) {
                auto held = r; held.socket = nullptr; standard(held);
                WebhookTestServer::raw(r, "not-json");
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 5000);
        QTRY_VERIFY(!agent.stopping());
        QVERIFY(!agent.active());
        QCOMPARE(f.provider.count("setWebhook"), 1);
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
    }
    void stopHoldsGateUntilRemoteCleanupCompletes()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        f.holdInfo = true;
        agent.stop();
        QTRY_COMPARE(f.provider.count("getWebhookInfo"), 2);
        QVERIFY(agent.stopping());
        agent.start("654321:synthetic", "persisted_secret_654321");
        QCOMPARE(f.provider.count("getMe"), 1);
        f.holdInfo = false;
        WebhookTestServer::answer(f.provider.last("getWebhookInfo"), QJsonObject{
            {"url", f.urls.value("bot123456:synthetic")}, {"has_custom_certificate", false}, {"pending_update_count", 0}});
        QTRY_VERIFY(!agent.stopping());
        agent.start("654321:synthetic", "persisted_secret_654321"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void cleanupMalformedReadbackReportsUnknownWithoutDeletion()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        f.malformedInfo = true;
        agent.stop(); QTRY_VERIFY(!agent.stopping());
        QCOMPARE(f.provider.count("deleteWebhook"), 0);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("unknown"));
    }
    void privateNativeHealthRouteWorksWithoutPublicHealthAccess()
    {
        WebhookFixture f;
        const auto health = QUrl(QStringLiteral("http://127.0.0.1:%1/health").arg(f.options.tunnel.port));
        TelegramAgent agent(f.provider.root(), health, f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QNetworkAccessManager network;
        auto *unauthorized = network.get(QNetworkRequest(health));
        QTRY_VERIFY(unauthorized->isFinished());
        QVERIFY(unauthorized->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400);
        QCOMPARE(f.post(f.update(1, "status")), 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 1);
        QCOMPARE(f.provider.last("sendMessage").body.value("text").toString(), QString("🟢 Active"));
        QVERIFY(agent.healthy());
        QCOMPARE(f.health.requests.size(), 0);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void unexpectedTunnelExitRetiresTransportAndCleansOwnedWebhook()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        QNetworkAccessManager network;
        auto exit = f.options.tunnel.inspectorUrl; exit.setPath("/exit");
        network.get(QNetworkRequest(exit));
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 5000);
        QTRY_VERIFY(!agent.stopping());
        QVERIFY(!agent.active()); QVERIFY(!agent.healthy());
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
        QCOMPARE(f.provider.count("getUpdates"), 0);
    }
    void cleanupErrorObserverCanDestroyOwner()
    {
        WebhookFixture f;
        auto *agent = new TelegramAgent(f.provider.root(), f.healthUrl(), f.options);
        agent->setAllowedChatIds({"42"});
        QPointer<TelegramAgent> guard(agent);
        agent->start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent->active(), 5000);
        f.malformedInfo = true;
        connect(agent, &TelegramAgent::errorOccurred, agent, [agent] { delete agent; });
        agent->stop();
        QTRY_VERIFY(guard.isNull());
        QTcpServer receiver;
        QTRY_VERIFY(receiver.isListening() || receiver.listen(QHostAddress::LocalHost, f.options.tunnel.port));
    }
    void synchronousHostFailureCannotPublishIntoReplacementStart()
    {
        WebhookFixture f;
        f.options.tunnel.inspectorUrl = QUrl("https://unsafe.invalid/api/tunnels");
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        bool restarted = false;
        bool staleError = false;
        connect(&agent, &TelegramAgent::stateChanged, &agent, [&] {
            if (!restarted && !agent.active() && !agent.starting() && !agent.stopping()) {
                restarted = true;
                agent.start("654321:synthetic", "persisted_secret_654321");
            }
        });
        connect(&agent, &TelegramAgent::errorOccurred, &agent, [&](const auto &) {
            if (restarted && agent.starting()) staleError = true;
        });
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_COMPARE(f.provider.count("getMe"), 2);
        QTRY_VERIFY(!agent.starting() && !agent.stopping());
        QVERIFY(!staleError);
    }
    void wholeCleanupDeadlineBoundsMultiRequestChain()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        f.holdInfo = f.holdDelete = true;
        QElapsedTimer elapsed; elapsed.start();
        agent.stop();
        QTRY_COMPARE(f.provider.count("getWebhookInfo"), 2);
        const auto held = f.provider.last("getWebhookInfo");
        const auto owned = f.urls.value("bot123456:synthetic");
        QTimer::singleShot(8000, &agent, [held, owned] {
            WebhookTestServer::answer(held, QJsonObject{{"url", owned},
                {"has_custom_certificate", false}, {"pending_update_count", 0}});
        });
        QTRY_VERIFY_WITH_TIMEOUT(!agent.stopping(), 17500);
        QVERIFY(elapsed.elapsed() >= 14000);
        QVERIFY(elapsed.elapsed() < 17500);
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("unknown"));
    }
    void cleanupCompletionObserverCanRestart()
    {
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        bool restarted = false;
        connect(&agent, &TelegramAgent::stateChanged, &agent, [&] {
            if (!restarted && !agent.active() && !agent.starting() && !agent.stopping()) {
                restarted = true;
                agent.start("654321:synthetic", "persisted_secret_654321");
            }
        });
        agent.stop();
        QTRY_VERIFY_WITH_TIMEOUT(restarted && agent.active(), 5000);
        QCOMPARE(f.provider.count("getMe"), 2);
        QCOMPARE(f.provider.count("setWebhook"), 2);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    // Stable fake child prefix for controller/UI tests, followed by real ngrok args.
    if (arguments.size() >= 4 && arguments[1] == QStringLiteral("--fake-ngrok")) {
        WebhookTestServer inspector(arguments[2].toUShort());
        const auto localPort = arguments[3].toUShort();
        inspector.handler = [localPort, &app](const auto &r) {
            if (r.url.path() == QStringLiteral("/exit")) {
                WebhookTestServer::raw(r, "{}");
                QTimer::singleShot(0, &app, &QCoreApplication::quit);
                return;
            }
            WebhookTestServer::raw(r, QJsonDocument(QJsonObject{{"tunnels", QJsonArray{QJsonObject{
                {"name", "offline"}, {"proto", "https"}, {"public_url", "https://offline-ngrok.invalid"},
                {"config", QJsonObject{{"addr", QStringLiteral("http://127.0.0.1:%1").arg(localPort)}}}}}}}).toJson(QJsonDocument::Compact));
        };
        return app.exec();
    }
    TestTelegramWebhookAgent test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_telegramwebhookagent.moc"
