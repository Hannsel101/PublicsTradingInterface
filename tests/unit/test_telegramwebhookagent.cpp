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
#include <csignal>

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
    void authenticationWebhookRequiresSameSecretAndPrivateHumanPolicy()
    {
        QTemporaryDir ledger; WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy auth(&agent, &TelegramAgent::robinhoodAuthenticationCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        const QString text = "/authenticate_robinhood_account Trader+tag@Example.com";
        QCOMPARE(f.post(f.update(1, text), "bot123456:synthetic", "wrong_secret"), 403);
        qint64 id = 2;
        for (const auto &field : QStringList{"unknown-chat", "sender-mismatch", "group", "bot", "edited_message",
                "forward_origin", "forward_from", "via_bot", "edit_date", "sender_chat", "business_connection_id"}) {
            auto update = f.update(id++, text); auto message = update["message"].toObject();
            if (field == "unknown-chat" || field == "sender-mismatch") {
                auto from = message["from"].toObject(); from["id"] = 84; message["from"] = from;
                if (field == "unknown-chat") { auto chat = message["chat"].toObject(); chat["id"] = 84; message["chat"] = chat; }
            } else if (field == "bot") {
                auto from = message["from"].toObject(); from["is_bot"] = true; message["from"] = from;
            } else if (field == "group") {
                auto chat = message["chat"].toObject(); chat["type"] = "group"; message["chat"] = chat;
            } else if (field != "edited_message") message[field] = QJsonObject{};
            update["message"] = message;
            if (field == "edited_message") { update.remove("message"); update[field] = message; }
            QCOMPARE(f.post(update), 200);
        }
        for (const auto &empty : QStringList{"authenticate_robinhood_account", "/authenticate_robinhood_account \t\r\n"})
            QCOMPARE(f.post(f.update(id++, empty)), 200);
        QCOMPARE(auth.size(), 0); QCOMPARE(f.provider.count("sendMessage"), 0);
        QVERIFY(!QFile::exists(ledger.filePath("123456.watermark")));
        QCOMPARE(f.post(f.update(100, text)), 200);
        QTRY_COMPARE(auth.size(), 1);
        QCOMPARE(auth[0][1].toString(), QString("Trader+tag@Example.com"));
        QFile file(ledger.filePath("123456.watermark")); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("PTI-TRADE-1\n123456\n100\n")); file.close();
        // Another admitted auth job cannot replace the first job's origin/report ticket.
        QCOMPARE(f.post(f.update(101, "authenticate_robinhood_account next@example.test")), 200);
        QCOMPARE(auth.size(), 1);
        QSignalSpy reports(&agent, &TelegramAgent::tradeReportFinished);
        QVERIFY(agent.reportTradeCommand(auth[0][0].toString(), {"Stage starting"}));
        QTRY_COMPARE(reports.size(), 1);
        QCOMPARE(reports[0][0].toString(), auth[0][0].toString());
        QCOMPARE(auth.size(), 1);
        agent.completeTradeCommand(auth[0][0].toString(), {"Stage finished"});
        QTRY_COMPARE(auth.size(), 2);
        QCOMPARE(auth[1][1].toString(), QString("next@example.test"));
        QCOMPARE(f.provider.count("sendMessage"), 2);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void authenticationAndTradesShareDurableWatermarkAcrossInstances_data()
    {
        QTest::addColumn<QString>("trade"); QTest::addColumn<bool>("authFirst");
        for (const auto &trade : QStringList{"publics_auto_buy AAPL", "publics_auto_sell AAPL",
                "robinhood_auto_buy AAPL", "robinhood_auto_sell AAPL", "auto_buy AAPL", "auto_sell AAPL"}) {
            QTest::newRow(qPrintable("auth-to-" + trade)) << trade << true;
            QTest::newRow(qPrintable(trade + "-to-auth")) << trade << false;
        }
    }
    void authenticationAndTradesShareDurableWatermarkAcrossInstances()
    {
        QFETCH(QString, trade); QFETCH(bool, authFirst);
        QTemporaryDir ledger; WebhookFixture f;
        const QString authText = "authenticate_robinhood_account a@example.test";
        {
            TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
            agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
            QSignalSpy auth(&agent, &TelegramAgent::robinhoodAuthenticationCommandReceived);
            QSignalSpy publics(&agent, &TelegramAgent::tradeCommandReceived);
            QSignalSpy rh(&agent, &TelegramAgent::robinhoodTradeCommandReceived);
            QSignalSpy combined(&agent, &TelegramAgent::combinedTradeCommandReceived);
            agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
            QCOMPARE(f.post(f.update(100, authFirst ? authText : trade)), 200);
            QTRY_COMPARE(auth.size() + publics.size() + rh.size() + combined.size(), 1);
            agent.stop(); QTRY_VERIFY(!agent.stopping());
        }
        {
            TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
            agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
            QSignalSpy auth(&agent, &TelegramAgent::robinhoodAuthenticationCommandReceived);
            QSignalSpy publics(&agent, &TelegramAgent::tradeCommandReceived);
            QSignalSpy rh(&agent, &TelegramAgent::robinhoodTradeCommandReceived);
            QSignalSpy combined(&agent, &TelegramAgent::combinedTradeCommandReceived);
            agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
            QCOMPARE(f.post(f.update(100, authFirst ? trade : authText)), 200);
            QCOMPARE(f.post(f.update(99, authFirst ? trade : authText)), 200);
            QTest::qWait(50);
            QCOMPARE(auth.size() + publics.size() + rh.size() + combined.size(), 0);
            QCOMPARE(f.provider.count("sendMessage"), 0);
            QCOMPARE(f.post(f.update(101, authFirst ? trade : authText)), 200);
            QTRY_COMPARE(auth.size() + publics.size() + rh.size() + combined.size(), 1);
            agent.stop(); QTRY_VERIFY(!agent.stopping());
        }
    }

    void authenticationDurableFailureIsRetryable()
    {
        QTemporaryDir ledger; WebhookFixture f;
        QFile corrupt(ledger.filePath("123456.watermark")); QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("corrupt"); corrupt.close();
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy auth(&agent, &TelegramAgent::robinhoodAuthenticationCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        const auto update = f.update(1, "authenticate_robinhood_account a@example.test");
        QCOMPARE(f.post(update), 503); QCOMPARE(auth.size(), 0);
        QCOMPARE(f.provider.count("sendMessage"), 0); QVERIFY(corrupt.remove());
        QCOMPARE(f.post(update), 200); QTRY_COMPARE(auth.size(), 1);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void emptyTradeCommandsAreSilentOnWebhook()
    {
        QTemporaryDir ledger;
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy publics(&agent, &TelegramAgent::tradeCommandReceived);
        QSignalSpy robinhood(&agent, &TelegramAgent::robinhoodTradeCommandReceived);
        QSignalSpy combined(&agent, &TelegramAgent::combinedTradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        qint64 id = 1;
        for (const auto &command : QStringList{"publics_auto_buy", "publics_auto_sell",
                "robinhood_auto_buy", "robinhood_auto_sell", "auto_buy", "auto_sell"}) {
            for (const auto &text : QStringList{command, "/" + command,
                    " \t" + command + " \t\r\n", "/" + command + " \t",
                    command + ",,,", "/" + command + QString::fromUtf8("\u00a0,,\t,\n\u2003,")})
                QCOMPARE(f.post(f.update(id++, text)), 200);
        }
        QTest::qWait(100);
        QCOMPARE(f.provider.count("sendMessage"), 0);
        QCOMPARE(publics.size(), 0); QCOMPARE(robinhood.size(), 0); QCOMPARE(combined.size(), 0);
        QVERIFY(!QFile::exists(ledger.filePath("123456.watermark")));
        QCOMPARE(f.post(f.update(id, "unrelated message")), 200);
        QTRY_COMPARE(f.provider.count("sendMessage"), 1);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void tickerSeparatorsOnAuthenticatedWebhook_data()
    {
        QTest::addColumn<QString>("command");
        for (const auto &command : QStringList{"publics_auto_buy", "publics_auto_sell",
                "robinhood_auto_buy", "robinhood_auto_sell", "auto_buy", "auto_sell"})
            QTest::newRow(qPrintable(command)) << command;
    }
    void tickerSeparatorsOnAuthenticatedWebhook()
    {
        QFETCH(QString, command);
        QTemporaryDir ledger; WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy publics(&agent, &TelegramAgent::tradeCommandReceived);
        QSignalSpy rh(&agent, &TelegramAgent::robinhoodTradeCommandReceived);
        QSignalSpy combined(&agent, &TelegramAgent::combinedTradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        qint64 id = 1;
        for (const auto &tail : QStringList{" aapl mSfT    goog AaPl", " aapl, mSfT  ,goog AaPl",
                " aapl mSfT,goog AaPl", QString::fromUtf8("\u00a0,,aapl\t\r\nmSfT,\u2003goog\u2028AaPl,,\u3000")}) {
            const auto update = f.update(id, " \t/" + command + tail);
            QCOMPARE(f.post(update, "bot123456:synthetic", "wrong_secret"), 403);
            QCOMPARE(f.post(update), 200);
            const auto &calls = command.startsWith("publics_") ? publics : command.startsWith("robinhood_") ? rh : combined;
            QTRY_COMPARE(calls.size(), id);
            QCOMPARE(publics.size() + rh.size() + combined.size(), id);
            QCOMPARE(calls.last()[1].toString(), command.endsWith("sell") ? QString("sell") : QString("buy"));
            QCOMPARE(calls.last()[2].toStringList(), (QStringList{"AAPL", "MSFT", "GOOG"}));
            QCOMPARE(f.post(update), 200); // Duplicate intake must not replay.
            QCOMPARE(calls.size(), id);
            agent.completeTradeCommand(calls.last()[0].toString(), {});
            ++id;
        }
        QCOMPARE(f.provider.count("sendMessage"), 0);
        QFile file(ledger.filePath("123456.watermark")); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("PTI-TRADE-1\n123456\n4\n"));
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void tickerWhitespaceBoundsOnAuthenticatedWebhook_data() { tickerSeparatorsOnAuthenticatedWebhook_data(); }
    void tickerWhitespaceBoundsOnAuthenticatedWebhook()
    {
        QFETCH(QString, command);
        QTemporaryDir ledger; WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy publics(&agent, &TelegramAgent::tradeCommandReceived);
        QSignalSpy rh(&agent, &TelegramAgent::robinhoodTradeCommandReceived);
        QSignalSpy combined(&agent, &TelegramAgent::combinedTradeCommandReceived);
        const auto &calls = command.startsWith("publics_") ? publics : command.startsWith("robinhood_") ? rh : combined;
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        const int bound = command.startsWith("publics_") ? 1024 : 4096;
        const QString exact = command + QString(bound - command.size() - 1, '\t') + "A";
        QCOMPARE(f.post(f.update(1, exact)), 200);
        QTRY_COMPARE(calls.size(), 1);
        QCOMPARE(calls.last()[2].toStringList(), (QStringList{"A"}));
        agent.completeTradeCommand(calls.last()[0].toString(), {});
        QCOMPARE(f.post(f.update(2, exact + " ")), 200);
        // The envelope's existing 4096-unit ceiling silently denies oversized
        // RH/composite text; Publics' narrower parser bound retains its reply.
        QTRY_COMPARE(f.provider.count("sendMessage"), bound == 1024 ? 1 : 0);
        QCOMPARE(calls.size(), 1);
        QStringList symbols;
        const int count = command.startsWith("publics_") ? 50 : 300;
        for (int i = 0; i < count; ++i)
            symbols.append(QString(1, QChar('A' + i / 26)) + QChar('A' + i % 26));
        QCOMPARE(f.post(f.update(3, command + " ,," + symbols.join("\t\n") + ",,")), 200);
        QTRY_COMPARE(calls.size(), 2);
        QCOMPARE(calls.last()[2].toStringList(), symbols);
        agent.completeTradeCommand(calls.last()[0].toString(), {});
        if (command.startsWith("publics_")) {
            QCOMPARE(f.post(f.update(4, command + " " + QString("a \t").repeated(51))), 200);
            QTRY_COMPARE(f.provider.count("sendMessage"), 2);
            QCOMPARE(calls.size(), 2);
        }
        QFile file(ledger.filePath("123456.watermark")); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("PTI-TRADE-1\n123456\n3\n"));
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

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
    void saturatedCommandQueueIsRetryableNotAcknowledged_data()
    {
        QTest::addColumn<int>("route");
        QTest::newRow("publics") << 0; QTest::newRow("robinhood") << 1; QTest::newRow("combined") << 2;
    }
    void saturatedCommandQueueIsRetryableNotAcknowledged()
    {
        QFETCH(int, route);
        const bool robinhood = route == 1;
        const QString prefix = route == 2 ? "auto_" : robinhood ? "robinhood_auto_" : "publics_auto_";
        const auto signal = route == 2 ? &TelegramAgent::combinedTradeCommandReceived : robinhood ? &TelegramAgent::robinhoodTradeCommandReceived : &TelegramAgent::tradeCommandReceived;
        QTemporaryDir ledger;
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setCommandLedgerPathForTests(ledger.path());
        agent.setAllowedChatIds({"42"});
        QSignalSpy commands(&agent, signal);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(1, prefix + "buy AAPL")), 200);
        QTRY_COMPARE(commands.size(), 1);
        for (int i = 2; i <= 101; ++i)
            QCOMPARE(f.post(f.update(i, prefix + "buy MSFT")), 200);
        QCOMPARE(f.post(f.update(102, prefix + "buy NVDA")), 503);
        QCOMPARE(f.post(f.update(1, prefix + "buy AAPL")), 200); // duplicate, no work
        QCOMPARE(commands.size(), 1);
        QFile watermark(ledger.filePath("123456.watermark"));
        QVERIFY(watermark.open(QIODevice::ReadOnly));
        QCOMPARE(watermark.readAll(), QByteArray("PTI-TRADE-1\n123456\n101\n"));
        connect(&agent, signal, &agent,
            [&agent](const QString &id, const auto &, const auto &) { agent.completeTradeCommand(id, {}); });
        agent.completeTradeCommand(commands[0][0].toString(), {});
        QTRY_COMPARE(commands.size(), 101);
        QCOMPARE(f.post(f.update(102, prefix + "buy NVDA")), 200);
        QTRY_COMPARE(commands.size(), 102);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void durableAdmissionFailureReturnsRetryable503_data()
    {
        QTest::addColumn<int>("route");
        QTest::newRow("publics") << 0; QTest::newRow("robinhood") << 1; QTest::newRow("combined") << 2;
    }
    void durableAdmissionFailureReturnsRetryable503()
    {
        QFETCH(int, route);
        const bool robinhood = route == 1;
        const QString prefix = route == 2 ? "auto_" : robinhood ? "robinhood_auto_" : "publics_auto_";
        const auto signal = route == 2 ? &TelegramAgent::combinedTradeCommandReceived : robinhood ? &TelegramAgent::robinhoodTradeCommandReceived : &TelegramAgent::tradeCommandReceived;
        QTemporaryDir ledger;
        QFile corrupt(ledger.filePath("123456.watermark"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("not a valid ledger"); corrupt.close();
        WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setCommandLedgerPathForTests(ledger.path());
        agent.setAllowedChatIds({"42"});
        QSignalSpy commands(&agent, signal);
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QCOMPARE(f.post(f.update(1, prefix + "buy AAPL")), 503);
        QCOMPARE(commands.size(), 0);
        QVERIFY(agent.active()); // transient ledger failure does not disable the retry route
        QVERIFY(corrupt.remove());
        QCOMPARE(f.post(f.update(1, prefix + "buy AAPL")), 200);
        QTRY_COMPARE(commands.size(), 1);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }
    void webhookCommandsShareAuthorizationAndDurableBotScopedAdmission_data()
    {
        QTest::addColumn<int>("route");
        QTest::newRow("publics") << 0; QTest::newRow("robinhood") << 1; QTest::newRow("combined") << 2;
    }
    void webhookCommandsShareAuthorizationAndDurableBotScopedAdmission()
    {
        QFETCH(int, route);
        const bool robinhood = route == 1;
        const QString prefix = route == 2 ? "auto_" : robinhood ? "robinhood_auto_" : "publics_auto_";
        const auto signal = route == 2 ? &TelegramAgent::combinedTradeCommandReceived : robinhood ? &TelegramAgent::robinhoodTradeCommandReceived : &TelegramAgent::tradeCommandReceived;
        QTemporaryDir ledger;
        WebhookFixture f;
        TelegramAgent first(f.provider.root(), f.healthUrl(), f.options);
        first.setAllowedChatIds({"42"});
        TelegramAgent second(f.provider.root(), f.healthUrl(), f.options);
        second.setAllowedChatIds({"42"});
        for (auto *agent : {&first, &second}) {
            agent->setCommandLedgerPathForTests(ledger.path()); agent->setAllowedChatIds({"42"});
        }
        QSignalSpy one(&first, signal), two(&second, signal);
        QSignalSpy opposite(&first, robinhood ? &TelegramAgent::tradeCommandReceived : &TelegramAgent::robinhoodTradeCommandReceived);
        first.start("123456:synthetic", "persisted_secret_123456"); second.start("654321:synthetic", "persisted_secret_654321");
        QTRY_VERIFY_WITH_TIMEOUT(first.active() && second.active(), 5000);
        QVERIFY(f.post(f.update(100, prefix + "buy AAPL"), "bot123456:synthetic", "wrong") >= 400);
        auto denied = f.update(100, prefix + "buy AAPL"); auto message = denied["message"].toObject();
        auto chat = message["chat"].toObject(); chat["id"] = 84; message["chat"] = chat; denied["message"] = message;
        QCOMPARE(f.post(denied), 200); QCOMPARE(one.size(), 0);
        QCOMPARE(f.post(f.update(100, prefix + "buy aapl, msft,aapl")), 200);
        QTRY_COMPARE(one.size(), 1);
        QCOMPARE(one[0][2].toStringList(), (QStringList{"AAPL", "MSFT"}));
        QCOMPARE(f.post(f.update(100, prefix + "buy AAPL")), 200);
        QCOMPARE(f.post(f.update(99, prefix + "buy NVDA")), 200); // monotonic old-ID rejection
        QCOMPARE(f.post(f.update(100, prefix + "sell AAPL"), "bot654321:synthetic"), 200);
        QTRY_COMPARE(two.size(), 1); QCOMPARE(two[0][1].toString(), QString("sell"));
        QCOMPARE(one.size(), 1);
        const auto stale = one[0][0].toString();
        first.stop(); QTRY_VERIFY(!first.stopping());
        first.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(first.active(), 5000);
        first.completeTradeCommand(stale, {"STALE"});
        QCOMPARE(f.post(f.update(100, prefix + "buy AAPL")), 200);
        QCOMPARE(f.post(f.update(100, robinhood ? "publics_auto_sell AAPL" : "robinhood_auto_sell AAPL")), 200);
        QTest::qWait(100); QCOMPARE(one.size(), 1); QCOMPARE(opposite.size(), 0); QCOMPARE(f.provider.count("sendMessage"), 0);
        first.stop(); second.stop(); QTRY_VERIFY(!first.stopping() && !second.stopping());
    }
    void robinhoodWebhookPreservesEveryTickerBeyondPublicsLimit()
    {
        QTemporaryDir ledger; WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy commands(&agent, &TelegramAgent::robinhoodTradeCommandReceived);
        QSignalSpy publics(&agent, &TelegramAgent::tradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        QStringList symbols;
        for (int i = 0; i < 700; ++i) symbols.append(QStringLiteral("T%1").arg(i));
        QCOMPARE(f.post(f.update(1, "/robinhood_auto_sell " + symbols.join(','))), 200);
        QTRY_COMPARE(commands.size(), 1);
        QCOMPARE(commands[0][1].toString(), QString("sell"));
        QCOMPARE(commands[0][2].toStringList(), symbols);
        QCOMPARE(publics.size(), 0);
        agent.completeTradeCommand(commands[0][0].toString(), {});
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QTest::qWait(100); QCOMPARE(commands.size(), 1); QCOMPARE(publics.size(), 0);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
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
    void updatesDuringRegistrationWaitForVerifiedActivation_data()
    {
        QTest::addColumn<int>("route");
        QTest::newRow("publics") << 0; QTest::newRow("robinhood") << 1; QTest::newRow("combined") << 2;
    }
    void updatesDuringRegistrationWaitForVerifiedActivation()
    {
        QFETCH(int, route);
        const bool robinhood = route == 1;
        const QString prefix = route == 2 ? "auto_" : robinhood ? "robinhood_auto_" : "publics_auto_";
        const auto signal = route == 2 ? &TelegramAgent::combinedTradeCommandReceived : robinhood ? &TelegramAgent::robinhoodTradeCommandReceived : &TelegramAgent::tradeCommandReceived;
        QTemporaryDir ledger;
        WebhookFixture f;
        f.holdInfo = true;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy commands(&agent, signal);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("getWebhookInfo"), 1, 5000);
        QVERIFY(agent.starting());
        QVERIFY(!agent.active());
        QCOMPARE(f.post(f.update(1, "/status")), 200);
        QCOMPARE(f.post(f.update(2, prefix + "buy AAPL")), 200);
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
    void transientRegistrationRecovery_data()
    {
        QTest::addColumn<int>("failures");
        QTest::newRow("first-attempt") << 0;
        QTest::newRow("second-attempt") << 1;
        QTest::newRow("third-attempt") << 2;
        QTest::newRow("exhausted") << 3;
    }
    void transientRegistrationRecovery()
    {
        QFETCH(int, failures);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook") && f.provider.count("setWebhook") <= failures) {
                // Commit first, then lose the acknowledgement: cleanup must reconcile.
                auto committed = r; committed.socket = nullptr; standard(committed);
                WebhookTestServer::raw(r, "{\"ok\":false,\"error_code\":503}", 503);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        bool idleGap = false;
        connect(&agent, &TelegramAgent::stateChanged, &agent, [&] {
            if (!agent.active() && !agent.starting() && !agent.stopping()
                && f.provider.count("setWebhook") < qMin(failures + 1, 3)) idleGap = true;
        });
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return agent.active() || (!errors.isEmpty() && !agent.stopping()); }, 10000));
        QCOMPARE(f.provider.count("setWebhook"), qMin(failures + 1, 3));
        QCOMPARE(agent.active(), failures < 3);
        QCOMPARE(errors.size(), failures < 3 ? 0 : 1);
        QVERIFY(!idleGap);
        QCOMPARE(f.provider.count("getMe"), 1);
        QCOMPARE(f.provider.count("deleteWebhook"), failures);
        QCOMPARE(f.provider.count("getUpdates"), 0);
        for (const auto &r : f.provider.requests) if (r.url.path().endsWith("/setWebhook")) {
            QCOMPARE(r.url.path(), QString("/bot123456:synthetic/setWebhook"));
            QCOMPARE(r.body.value("secret_token").toString(), QString("persisted_secret_123456"));
        }
        QTest::qWait(350);
        QCOMPARE(f.provider.count("setWebhook"), qMin(failures + 1, 3));
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void revokedPolicyCancelsRegistrationRecovery()
    {
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook") && f.provider.count("setWebhook") == 1) {
                auto committed = r; committed.socket = nullptr; standard(committed);
                f.holdDelete = true;
                WebhookTestServer::raw(r, "{}", 503);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("deleteWebhook"), 1, 5000);
        agent.setAllowedChatIds({});
        agent.setAllowedChatIds({"42"}); // Cannot regain authority in this Start.
        f.holdDelete = false;
        WebhookTestServer::answer(f.provider.last("deleteWebhook"), true);
        QVERIFY(QTest::qWaitFor([&] { return !agent.stopping(); }, 5000));
        QTest::qWait(400);
        QVERIFY(!agent.active()); QVERIFY(!agent.starting());
        QCOMPARE(f.provider.count("setWebhook"), 1);
    }

    void registrationDeadlineRecoversAfterAuthoritativeCleanup()
    {
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook") && f.provider.count("setWebhook") == 1) {
                auto committed = r; committed.socket = nullptr; standard(committed);
                // The manager and agent both bound this real loopback request.
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return agent.active() || (!errors.isEmpty() && !agent.stopping()); }, 15000));
        QVERIFY(agent.active()); QCOMPARE(errors.size(), 0);
        QCOMPARE(f.provider.count("setWebhook"), 2);
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void recoveryWaitsForRemoteConfirmationAndLastHostRelease()
    {
        WebhookFixture f;
#ifdef Q_OS_UNIX
        f.options.tunnel.argumentPrefix.append("--hold-release");
#endif
        const auto standard = f.provider.handler;
        QSignalSpy released(TelegramWebhookHost::instance(), &TelegramWebhookHost::released);
        bool cleanupConfirmed = false;
        bool prematureRegistration = false;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook")) {
                if (f.provider.count("setWebhook") == 1) {
                    auto committed = r; committed.socket = nullptr; standard(committed);
                    f.holdInfo = f.holdDelete = true;
                    WebhookTestServer::raw(r, "{}", 503);
                    return;
                }
                prematureRegistration = !cleanupConfirmed || released.isEmpty();
            }
            standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return f.provider.count("getWebhookInfo") == 1; }, 5000));
        QVERIFY(agent.stopping()); QCOMPARE(errors.size(), 0);
        QCOMPARE(f.provider.count("setWebhook"), 1);
        WebhookTestServer::answer(f.provider.last("getWebhookInfo"), QJsonObject{{"url", f.urls.value("bot123456:synthetic")}});
        QVERIFY(QTest::qWaitFor([&] { return f.provider.count("deleteWebhook") == 1; }, 3000));
        QCOMPARE(f.provider.count("setWebhook"), 1);
        f.holdDelete = false;
        WebhookTestServer::answer(f.provider.last("deleteWebhook"), true);
        QVERIFY(QTest::qWaitFor([&] { return f.provider.count("getWebhookInfo") == 2; }, 3000));
        QCOMPARE(f.provider.count("setWebhook"), 1);
        f.holdInfo = false;
        cleanupConfirmed = true;
        WebhookTestServer::answer(f.provider.last("getWebhookInfo"), QJsonObject{{"url", ""}});
#ifdef Q_OS_UNIX
        QTest::qWait(100);
        QVERIFY(released.isEmpty()); // Remote completion alone cannot bypass the child.
        QCOMPARE(f.provider.count("setWebhook"), 1);
#endif
        QVERIFY(QTest::qWaitFor([&] { return agent.active(); }, 5000));
        QVERIFY(!prematureRegistration); QCOMPARE(errors.size(), 0);
        QCOMPARE(f.provider.count("setWebhook"), 2);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void explicitStopCancelsRecovery_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("registration-pending") << QString("registration");
        QTest::newRow("cleanup-pending") << QString("cleanup");
        QTest::newRow("backoff-pending") << QString("backoff");
    }
    void explicitStopCancelsRecovery()
    {
        QFETCH(QString, phase);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook")) {
                auto committed = r; committed.socket = nullptr; standard(committed);
                if (phase == "registration") return;
                f.holdDelete = phase == "cleanup";
                WebhookTestServer::raw(r, "{}", 503);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        if (phase == "backoff") {
            connect(TelegramWebhookHost::instance(), &TelegramWebhookHost::released, &agent, [&] {
                // The host queues finishStop before this timer; Stop lands in backoff.
                QTimer::singleShot(0, &agent, [&] { agent.stop(); });
            });
        }
        agent.start("123456:synthetic", "persisted_secret_123456");
        if (phase == "registration") {
            QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("setWebhook"), 1, 5000);
            agent.stop();
            WebhookTestServer::answer(f.provider.last("setWebhook"), true);
        } else if (phase == "cleanup") {
            QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("deleteWebhook"), 1, 5000);
            agent.stop(); // Must cancel even though internal stopping is already true.
            f.holdDelete = false;
            WebhookTestServer::answer(f.provider.last("deleteWebhook"), true);
        }
        QVERIFY(QTest::qWaitFor([&] { return !agent.starting() && !agent.stopping(); }, 5000));
        QTest::qWait(400);
        QVERIFY(!agent.active()); QCOMPARE(errors.size(), 0);
        QCOMPARE(f.provider.count("setWebhook"), 1);
        QCOMPARE(f.provider.count("deleteWebhook"), 1);
    }

    void unknownCleanupNeverRetries_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("malformed") << QString("malformed");
        QTest::newRow("uncertain-empty") << QString("empty");
        QTest::newRow("uncertain-replacement") << QString("replacement");
        QTest::newRow("delete-unconfirmed") << QString("delete");
    }
    void unknownCleanupNeverRetries()
    {
        QFETCH(QString, kind);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook")) {
                auto committed = r; committed.socket = nullptr; standard(committed);
                if (kind == "empty") f.urls["bot123456:synthetic"].clear();
                if (kind == "replacement") f.urls["bot123456:synthetic"] = "https://other.invalid/owner";
                f.malformedInfo = kind == "malformed";
                WebhookTestServer::raw(r, "{}", 503);
            } else if (kind == "delete" && r.url.path().endsWith("/deleteWebhook")) {
                WebhookTestServer::answer(r, false);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return errors.size() == 1 && !agent.stopping(); }, 5000));
        QVERIFY(errors[0][0].toString().contains("unknown"));
        QTest::qWait(400);
        QCOMPARE(f.provider.count("setWebhook"), 1);
        QCOMPARE(f.provider.count("deleteWebhook"), kind == "delete" ? 1 : 0);
        QVERIFY(!agent.active()); QVERIFY(!agent.starting());
    }

    void transientRegistrationTransportCategories_data()
    {
        QTest::addColumn<QString>("phase"); QTest::addColumn<QString>("failure");
        QTest::newRow("set-network-close") << QString("setWebhook") << QString("network");
        QTest::newRow("set-api-503") << QString("setWebhook") << QString("api");
        QTest::newRow("readback-network-close") << QString("getWebhookInfo") << QString("network");
        QTest::newRow("readback-service-503") << QString("getWebhookInfo") << QString("service");
    }
    void transientRegistrationTransportCategories()
    {
        QFETCH(QString, phase); QFETCH(QString, failure);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        bool injected = false;
        f.provider.handler = [&, standard](const auto &r) {
            if (!injected && r.url.path().endsWith('/' + phase)) {
                injected = true;
                if (phase == "setWebhook") { auto committed = r; committed.socket = nullptr; standard(committed); }
                if (failure == "network") {
                    // A complete request can commit before its truncated reply closes.
                    r.socket->write("HTTP/1.1 200 Test\r\nContent-Length: 100\r\nConnection: close\r\n\r\n{");
                    r.socket->disconnectFromHost();
                } else WebhookTestServer::raw(r, "{\"ok\":false,\"error_code\":503}", failure == "api" ? 200 : 503);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return agent.active() || (!errors.isEmpty() && !agent.stopping()); }, 7000));
        QVERIFY(agent.active()); QCOMPARE(errors.size(), 0);
        QCOMPARE(f.provider.count("setWebhook"), 2); QCOMPARE(f.provider.count("deleteWebhook"), 1);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void freshManualStartResetsBudgetAndPreservesRecoveredIdentity()
    {
        QTemporaryDir ledger; WebhookFixture f;
        int failuresRemaining = 0;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook") && failuresRemaining > 0) {
                --failuresRemaining;
                auto committed = r; committed.socket = nullptr; standard(committed);
                WebhookTestServer::raw(r, "{}", 503);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        QSignalSpy commands(&agent, &TelegramAgent::tradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
        // The reported active -> Stop -> one new Start path recovers automatically.
        failuresRemaining = 1;
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return agent.active(); }, 5000));
        QCOMPARE(errors.size(), 0); QCOMPARE(f.provider.count("setWebhook"), 3);
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QTRY_COMPARE(commands.size(), 1);
        agent.completeTradeCommand(commands[0][0].toString(), {});
        agent.stop(); QTRY_VERIFY(!agent.stopping());
        failuresRemaining = 3;
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return errors.size() == 1 && !agent.stopping(); }, 8000));
        QCOMPARE(f.provider.count("setWebhook"), 6); QVERIFY(!agent.active());
        failuresRemaining = 2;
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return agent.active(); }, 8000));
        QCOMPARE(f.provider.count("setWebhook"), 9); QCOMPARE(errors.size(), 1);
        QCOMPARE(f.provider.count("getMe"), 4);
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200); // Durable claim survives all Starts.
        QCOMPARE(commands.size(), 1);
        QCOMPARE(f.post(f.update(2, "publics_auto_buy MSFT")), 200);
        QTRY_COMPARE(commands.size(), 2);
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void preReadbackAdmissionNeverReplaysAcrossRecovery()
    {
        QTemporaryDir ledger; WebhookFixture f; f.holdInfo = true;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy commands(&agent, &TelegramAgent::tradeCommandReceived);
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QTRY_COMPARE_WITH_TIMEOUT(f.provider.count("getWebhookInfo"), 1, 5000);
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QCOMPARE(commands.size(), 0);
        QVERIFY(QFile::exists(ledger.filePath("123456.watermark")));
        f.holdInfo = false;
        WebhookTestServer::raw(f.provider.last("getWebhookInfo"), "{}", 503);
        QVERIFY(QTest::qWaitFor([&] { return agent.active(); }, 5000));
        QCOMPARE(errors.size(), 0); QCOMPARE(f.provider.count("setWebhook"), 2);
        QCOMPARE(commands.size(), 0); // Never execute the retired startup queue.
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QCOMPARE(commands.size(), 0);
        QCOMPARE(f.post(f.update(2, "publics_auto_buy MSFT")), 200);
        QTRY_COMPARE(commands.size(), 1);
        QCOMPARE(commands[0][2].toStringList(), QStringList{"MSFT"});
        agent.stop(); QTRY_VERIFY(!agent.stopping());
    }

    void permanentRegistrationFailuresNeverRetry_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<int>("apiCode"); QTest::addColumn<QString>("kind");
        for (const int code : {400, 401, 403, 409, 429}) {
            QTest::newRow(qPrintable(QString("http-%1").arg(code))) << code << 0 << QString("error");
            QTest::newRow(qPrintable(QString("api-%1").arg(code))) << 200 << code << QString("error");
        }
        QTest::newRow("hard-api-over-http-service") << 503 << 401 << QString("error");
        QTest::newRow("malformed") << 200 << 0 << QString("malformed");
        QTest::newRow("missing-result") << 200 << 0 << QString("missing");
        QTest::newRow("rejected-result") << 200 << 0 << QString("false");
        QTest::newRow("contradictory-success-service") << 200 << 503 << QString("contradictory");
        QTest::newRow("redirect") << 302 << 0 << QString("error");
        QTest::newRow("oversized") << 200 << 0 << QString("large");
    }
    void permanentRegistrationFailuresNeverRetry()
    {
        QFETCH(int, status); QFETCH(int, apiCode); QFETCH(QString, kind);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [=](const auto &r) {
            if (r.url.path().endsWith("/setWebhook")) {
                // A terminal receipt must not retry even when cleanup can prove ownership.
                auto committed = r; committed.socket = nullptr; standard(committed);
                QByteArray body = QJsonDocument(QJsonObject{{"ok", false}, {"error_code", apiCode}}).toJson();
                if (kind == "malformed") body = "not-json";
                if (kind == "missing") body = "{\"ok\":true}";
                if (kind == "false") body = "{\"ok\":true,\"result\":false}";
                if (kind == "contradictory") body = "{\"ok\":true,\"result\":false,\"error_code\":503}";
                if (kind == "large") body = QByteArray(1024 * 1024 + 1, 'x');
                WebhookTestServer::raw(r, body, status);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return !errors.isEmpty() && !agent.stopping(); }, 5000));
        QTest::qWait(350);
        QVERIFY(!agent.active()); QVERIFY(!agent.starting());
        QCOMPARE(f.provider.count("setWebhook"), 1); QCOMPARE(f.provider.count("getUpdates"), 0);
    }

    void activeMessageServiceFailureDoesNotRetryOrReplay()
    {
        QTemporaryDir ledger; WebhookFixture f;
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        QSignalSpy commands(&agent, &TelegramAgent::tradeCommandReceived);
        agent.start("123456:synthetic", "persisted_secret_123456"); QTRY_VERIFY_WITH_TIMEOUT(agent.active(), 5000);
        const auto standard = f.provider.handler;
        f.provider.handler = [standard](const auto &r) {
            if (r.url.path().endsWith("/sendMessage")) WebhookTestServer::raw(r, "{}", 503);
            else standard(r);
        };
        QCOMPARE(f.post(f.update(1, "publics_auto_buy AAPL")), 200);
        QTRY_COMPARE(commands.size(), 1);
        agent.completeTradeCommand(commands[0][0].toString(), {"Completed once"});
        QVERIFY(QTest::qWaitFor([&] { return !errors.isEmpty() && !agent.stopping(); }, 5000));
        QTest::qWait(350);
        QCOMPARE(f.provider.count("setWebhook"), 1); QCOMPARE(f.provider.count("sendMessage"), 1);
        QCOMPARE(commands.size(), 1); QVERIFY(!agent.active());
    }

    void recoveryStateObserverCanDeleteOrReplaceOwner_data()
    {
        QTest::addColumn<bool>("replace");
        QTest::newRow("delete") << false; QTest::newRow("replace") << true;
    }
    void recoveryStateObserverCanDeleteOrReplaceOwner()
    {
        QFETCH(bool, replace);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        f.provider.handler = [&, standard](const auto &r) {
            if (r.url.path().endsWith("/setWebhook") && f.provider.count("setWebhook") == 1) {
                auto committed = r; committed.socket = nullptr; standard(committed);
                WebhookTestServer::raw(r, "{}", 503);
            } else standard(r);
        };
        auto *agent = new TelegramAgent(f.provider.root(), f.healthUrl(), f.options);
        QPointer<TelegramAgent> owner(agent);
        agent->setAllowedChatIds({"42"});
        bool observed = false;
        connect(agent, &TelegramAgent::stateChanged, agent, [&] {
            if (observed || !owner->starting() || f.provider.count("setWebhook") != 1) return;
            observed = true;
            if (!replace) { delete owner.data(); return; }
            owner->stop();
            owner->start("654321:synthetic", "persisted_secret_654321");
        });
        agent->start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return observed && (!owner || owner->active()); }, 7000));
        QCOMPARE(f.provider.count("setWebhook"), replace ? 2 : 1);
        if (owner) {
            QCOMPARE(f.provider.last("setWebhook").url.path(), QString("/bot654321:synthetic/setWebhook"));
            owner->stop(); QTRY_VERIFY(!owner->stopping()); delete owner.data();
        }
    }

    void recoveryNeverTakesOverReplacementAfterConfirmedRegistration_data()
    {
        QTest::addColumn<bool>("afterDelete");
        QTest::newRow("ownership-check") << false;
        QTest::newRow("deletion-readback") << true;
    }
    void recoveryNeverTakesOverReplacementAfterConfirmedRegistration()
    {
        QFETCH(bool, afterDelete);
        WebhookFixture f;
        const auto standard = f.provider.handler;
        bool failedReadback = false;
        f.provider.handler = [&, standard](const auto &r) {
            if (!failedReadback && r.url.path().endsWith("/getWebhookInfo")) {
                failedReadback = true;
                if (!afterDelete) f.urls["bot123456:synthetic"] = "https://replacement.invalid/owner";
                WebhookTestServer::raw(r, "{}", 503);
            } else if (afterDelete && r.url.path().endsWith("/getWebhookInfo") && f.provider.count("getWebhookInfo") == 3) {
                f.urls["bot123456:synthetic"] = "https://replacement.invalid/owner";
                standard(r);
            } else standard(r);
        };
        TelegramAgent agent(f.provider.root(), f.healthUrl(), f.options);
        agent.setAllowedChatIds({"42"}); QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic", "persisted_secret_123456");
        QVERIFY(QTest::qWaitFor([&] { return agent.active() || (!errors.isEmpty() && !agent.stopping()); }, 7000));
        QVERIFY(!agent.active()); QCOMPARE(errors.size(), 1);
        QCOMPARE(f.provider.count("setWebhook"), 1); QCOMPARE(f.provider.count("deleteWebhook"), afterDelete ? 1 : 0);
        QCOMPARE(f.urls.value("bot123456:synthetic"), QString("https://replacement.invalid/owner"));
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
#ifdef Q_OS_UNIX
        // Exercise last-lease teardown through the real host's bounded kill path.
        if (arguments.contains(QStringLiteral("--hold-release"))) std::signal(SIGTERM, SIG_IGN);
#endif
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
