#include "telegrambotruntime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <functional>

// Synthetic provider-shaped fixtures, independently specified from Bot API
// User, Update and Message schemas: https://core.telegram.org/bots/api.
class BotServer final : public QTcpServer
{
public:
    struct Request { QByteArray method; QByteArray path; QJsonObject body; QPointer<QTcpSocket> socket; };
    QList<Request> requests;
    std::function<void(const Request &)> handler;
    BotServer()
    {
        if (!listen(QHostAddress::LocalHost)) qFatal("Cannot listen on loopback");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (auto line : buffer->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto first = buffer->left(buffer->indexOf('\r')).split(' ');
                    Request r{first.value(0), first.value(1),
                              QJsonDocument::fromJson(buffer->mid(end + 4, length)).object(), socket};
                    requests.append(r);
                    if (handler) handler(r);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); }
    static void raw(const Request &r, QByteArray body, int status = 200, QByteArray extra = {})
    {
        if (!r.socket || r.socket->state() != QAbstractSocket::ConnectedState) return;
        r.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n" + extra + "\r\n" + body);
        r.socket->disconnectFromHost();
    }
    static void answer(const Request &r, QJsonValue result)
    { raw(r, QJsonDocument(QJsonObject{{"ok", true}, {"result", result}}).toJson(QJsonDocument::Compact)); }
    static QJsonObject bot() { return {{"id", 123456}, {"is_bot", true}, {"first_name", "Test"}, {"username", "test_bot"}}; }
};

class TestTelegramBotRuntime final : public QObject
{
    Q_OBJECT
    static QJsonObject update(qint64 id, QString type = QStringLiteral("private"), bool bot = false,
                              QString field = QStringLiteral("message"))
    {
        return {{"update_id", id}, {field, QJsonObject{
            {"message_id", 77}, {"date", 1700000000}, {"text", "/start"},
            {"from", QJsonObject{{"id", 42}, {"is_bot", bot}, {"first_name", "Human"}}},
            {"chat", QJsonObject{{"id", 42}, {"type", type}, {"first_name", "Human"}}}}}};
    }
    static QJsonObject statusUpdate(qint64 id)
    {
        auto incoming = update(id);
        auto message = incoming["message"].toObject(); message["text"] = "/status";
        incoming["message"] = message;
        return incoming;
    }
    static void acknowledge(const BotServer::Request &r)
    {
        BotServer::answer(r, QJsonObject{{"message_id", 90}, {"date", 1700000001},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}});
    }
private slots:
    void healthSuccessUsesInjectedLocalEndpoint()
    {
        BotServer server;
        BotServer health;
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        QVERIFY(!runtime.healthy());
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        QVERIFY(runtime.healthy()); // Legacy activation, no startup health request.
        QCOMPARE(health.requests.size(), 0);
        auto incoming = update(10);
        auto message = incoming["message"].toObject(); message["text"] = "/status";
        incoming["message"] = message;
        BotServer::answer(server.requests[1], QJsonArray{incoming});
        QTRY_COMPARE_WITH_TIMEOUT(health.requests.size(), 1, 1000);
        QCOMPARE(health.requests[0].method, QByteArray("GET"));
        QCOMPARE(health.requests[0].path, QByteArray("/health"));
        QCOMPARE(server.requests.size(), 2); // No optimistic Telegram reply.
        BotServer::raw(health.requests[0], "ok", 204);
        QTRY_COMPARE(server.requests.size(), 3);
        QCOMPARE(server.requests[2].body.value("text").toString(), QStringLiteral("Active"));
        QVERIFY(runtime.healthy());
        runtime.stop();
        QVERIFY(!runtime.healthy());
    }
    void healthTimeoutSettlesExactlyOnce()
    {
        BotServer server;
        BotServer health;
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        auto incoming = update(10);
        auto message = incoming["message"].toObject(); message["text"] = "status";
        incoming["message"] = message;
        QElapsedTimer elapsed; elapsed.start();
        BotServer::answer(server.requests[1], QJsonArray{incoming});
        QTRY_COMPARE(health.requests.size(), 1);
        QSignalSpy changes(&runtime, &TelegramBotRuntime::stateChanged);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 3800);
        QVERIFY(elapsed.elapsed() >= 2900);
        QVERIFY(elapsed.elapsed() < 3800);
        QCOMPARE(server.requests[2].body.value("text").toString(), QStringLiteral("Inactive"));
        QVERIFY(runtime.active()); // Transport stays cancellable and recoverable.
        QVERIFY(!runtime.healthy());
        QCOMPARE(changes.size(), 1);
        BotServer::raw(health.requests[0], "late success");
        QTest::qWait(250);
        QCOMPARE(server.requests.size(), 3); // No duplicate timeout/error/finished reply.
        QCOMPARE(changes.size(), 1);
        QTRY_VERIFY(!health.requests[0].socket || health.requests[0].socket->state() == QAbstractSocket::UnconnectedState);
        runtime.stop();
    }
    void rejectsUnsafeHealthEndpoints_data()
    {
        QTest::addColumn<QString>("url");
        for (const auto *url : {"http://example.com/health", "https://localhost/health",
                 "http://127.0.0.1.example.com/health", "http://user:password@localhost/health",
                 "http://localhost/health?secret=1", "http://localhost/health#fragment",
                 "file:///health"})
            QTest::newRow(url) << QString::fromLatin1(url);
    }
    void rejectsUnsafeHealthEndpoints()
    {
        QFETCH(QString, url);
        BotServer server;
        TelegramBotRuntime runtime(server.root(), QUrl(url));
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:synthetic"));
        QVERIFY(!runtime.starting());
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.healthy());
        QCOMPARE(errors.size(), 1);
        QTest::qWait(50);
        QCOMPARE(server.requests.size(), 0);
    }
    void healthFailureRecoversWithoutRestart_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("http-error") << 0;
        QTest::newRow("early-network-error") << 1;
        QTest::newRow("redirect") << 2;
        QTest::newRow("oversized-body") << 3;
        QTest::newRow("truncated-2xx") << 4;
    }
    void healthFailureRecoversWithoutRestart()
    {
        QFETCH(int, mode);
        BotServer server;
        BotServer health;
        BotServer redirectTarget;
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        QSignalSpy changes(&runtime, &TelegramBotRuntime::stateChanged);
        BotServer::answer(server.requests[1], QJsonArray{statusUpdate(10)});
        QTRY_COMPARE(health.requests.size(), 1);
        QElapsedTimer elapsed; elapsed.start();
        const auto failed = health.requests[0];
        if (mode == 0) BotServer::raw(failed, "unavailable", 503);
        else if (mode == 1) {
            failed.socket->write("HTTP/1.1 500 Failure\r\nContent-Length: 20\r\n\r\nx");
            failed.socket->disconnectFromHost();
        } else if (mode == 2) BotServer::raw(failed, {}, 302,
            "Location: " + redirectTarget.root().toEncoded() + "/stolen\r\n");
        else if (mode == 3) BotServer::raw(failed, QByteArray(64 * 1024 + 1, 'x'));
        else {
            failed.socket->write("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nx");
            failed.socket->disconnectFromHost();
        }
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 1000);
        QVERIFY(elapsed.elapsed() < 1500); // Errors settle before the deadline.
        QCOMPARE(server.requests[2].body.value("text").toString(), QStringLiteral("Inactive"));
        QVERIFY(runtime.active());
        QVERIFY(!runtime.healthy());
        QCOMPARE(changes.size(), 1);
        QCOMPARE(errors.size(), 0); // Local health is not a Telegram transport failure.
        acknowledge(server.requests[2]);
        QTRY_COMPARE(server.requests.size(), 4);
        BotServer::answer(server.requests[3], QJsonArray{statusUpdate(11)});
        QTRY_COMPARE(health.requests.size(), 2);
        BotServer::raw(health.requests[1], "healthy", 201);
        QTRY_COMPARE(server.requests.size(), 5);
        QCOMPARE(server.requests[4].body.value("text").toString(), QStringLiteral("Active"));
        QVERIFY(runtime.healthy());
        QCOMPARE(changes.size(), 2);
        acknowledge(server.requests[4]);
        QTRY_COMPARE(server.requests.size(), 6);
        QTest::qWait(3100); // Retired error timer cannot overwrite recovered health.
        QCOMPARE(changes.size(), 2);
        QCOMPARE(server.requests.size(), 6);
        QCOMPARE(redirectTarget.requests.size(), 0);
        runtime.stop();
    }
    void healthDrippingBodyStillHasAbsoluteDeadline()
    {
        BotServer server;
        BotServer health;
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        BotServer::answer(server.requests[1], QJsonArray{statusUpdate(10)});
        QTRY_COMPARE(health.requests.size(), 1);
        const auto socket = health.requests[0].socket;
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 10000\r\n\r\nx");
        QTimer drip;
        connect(&drip, &QTimer::timeout, &health, [socket] {
            if (socket && socket->state() == QAbstractSocket::ConnectedState) socket->write("x");
        });
        drip.start(50);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 3800);
        QCOMPARE(server.requests[2].body.value("text").toString(), QStringLiteral("Inactive"));
        QVERIFY(!runtime.healthy());
        runtime.stop();
    }
    void healthDeadlineRejectsLateCompletionAfterEventLoopStall()
    {
        BotServer server;
        BotServer health;
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        BotServer::answer(server.requests[1], QJsonArray{statusUpdate(10)});
        QTRY_COMPARE(health.requests.size(), 1);
        BotServer::raw(health.requests[0], "ok");
        QThread::msleep(3100); // Both network completion and timer are ready together.
        QTRY_COMPARE(server.requests.size(), 3);
        QCOMPARE(server.requests[2].body.value("text").toString(), QStringLiteral("Inactive"));
        QVERIFY(!runtime.healthy());
        QTest::qWait(150);
        QCOMPARE(server.requests.size(), 3);
        runtime.stop();
    }
    void healthPendingCancellation_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("stop") << 0;
        QTest::newRow("restart") << 1;
        QTest::newRow("destroy") << 2;
    }
    void healthPendingCancellation()
    {
        QFETCH(int, action);
        BotServer server;
        BotServer health;
        auto runtime = std::make_unique<TelegramBotRuntime>(server.root(), health.root().resolved(QUrl("/health")));
        runtime->start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        BotServer::answer(server.requests[1], QJsonArray{statusUpdate(10)});
        QTRY_COMPARE(health.requests.size(), 1);
        const auto stale = health.requests[0];
        if (action == 0) {
            runtime->stop();
            QVERIFY(!runtime->healthy());
            QVERIFY(!runtime->active());
        } else if (action == 1) {
            runtime->start(QStringLiteral("123456:replacement"));
            QTRY_COMPARE(server.requests.size(), 3);
            BotServer::answer(server.requests[2], BotServer::bot());
            QTRY_COMPARE(server.requests.size(), 4);
            QVERIFY(runtime->healthy());
        } else runtime.reset();
        QTRY_VERIFY(!stale.socket || stale.socket->state() == QAbstractSocket::UnconnectedState);
        BotServer::raw(stale, "late");
        QTest::qWait(3200);
        QCOMPARE(server.requests.size(), action == 1 ? 4 : 2);
        if (action == 1) QVERIFY(runtime->healthy());
    }
    void healthSignalReentrancy_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("stop") << 0;
        QTest::newRow("restart") << 1;
        QTest::newRow("delete") << 2;
    }
    void healthSignalReentrancy()
    {
        QFETCH(int, action);
        BotServer server;
        BotServer health;
        QPointer<TelegramBotRuntime> runtime = new TelegramBotRuntime(server.root(), health.root().resolved(QUrl("/health")));
        runtime->start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        BotServer::answer(server.requests[1], QJsonArray{statusUpdate(10)});
        QTRY_COMPARE(health.requests.size(), 1);
        bool acted = false;
        connect(runtime, &TelegramBotRuntime::stateChanged, &server, [&] {
            if (acted || !runtime || runtime->healthy()) return;
            acted = true;
            if (action == 0) runtime->stop();
            else if (action == 1) runtime->start(QStringLiteral("123456:replacement"));
            else delete runtime.data();
        });
        BotServer::raw(health.requests[0], "down", 503);
        QTRY_VERIFY(acted);
        QTest::qWait(200);
        QCOMPARE(server.requests.size(), action == 1 ? 3 : 2);
        if (action == 1) QCOMPARE(server.requests[2].path, QByteArray("/bot123456:replacement/getMe"));
        delete runtime.data();
    }
    void statusCommandReplies_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("expectedText");
        QTest::newRow("status") << "status" << "Active";
        QTest::newRow("slash-status") << "/status" << "Active";
        QTest::newRow("surrounding-whitespace") << " \tstatus\n" << "Active";
        QTest::newRow("unknown-command") << "help" << "Message received.";
        QTest::newRow("not-a-command-prefix") << "status now" << "Message received.";
        QTest::newRow("not-a-substring") << "mystatus" << "Message received.";
        QTest::newRow("other-bot") << "/status@other_bot" << "Message received.";
    }
    void statusCommandReplies()
    {
        QFETCH(QString, command);
        QFETCH(QString, expectedText);
        BotServer server;
        BotServer health;
        health.handler = [](const BotServer::Request &r) { BotServer::raw(r, "ok"); };
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 1000);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 1000);
        auto incoming = update(10);
        auto message = incoming.value("message").toObject();
        message["text"] = command;
        incoming["message"] = message;
        BotServer::answer(server.requests[1], QJsonArray{incoming, incoming});
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 1000);
        const auto sent = server.requests[2];
        QCOMPARE(sent.method, QByteArray("POST"));
        QCOMPARE(sent.path, QByteArray("/bot123456:synthetic/sendMessage"));
        QCOMPARE(sent.body, (QJsonObject{{"chat_id", 42}, {"text", expectedText}}));
        // Provider-shaped receipt: text echo is optional, not copied from the request.
        BotServer::answer(sent, QJsonObject{{"message_id", 90}, {"date", 1700000001},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 4, 1000);
        QVERIFY(server.requests[3].path.endsWith("getUpdates"));
        QCOMPARE(server.requests[3].body.value("offset").toInteger(), 11);
        QVERIFY(runtime.active());
        QCOMPARE(errors.size(), 0);
        runtime.stop();
        QTest::qWait(300);
        QCOMPARE(server.requests.size(), 4);
    }
    void statusReplyReceiptMatchesActualReply_data()
    {
        QTest::addColumn<QString>("receiptText");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("status-echo") << "Active" << true;
        QTest::newRow("stale-acknowledgement-echo") << "Message received." << false;
    }
    void statusReplyReceiptMatchesActualReply()
    {
        QFETCH(QString, receiptText);
        QFETCH(bool, accepted);
        BotServer server;
        BotServer health;
        health.handler = [](const BotServer::Request &r) { BotServer::raw(r, "ok"); };
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        auto incoming = update(10);
        auto message = incoming.value("message").toObject();
        message["text"] = "status";
        incoming["message"] = message;
        BotServer::answer(server.requests[1], QJsonArray{incoming});
        QTRY_COMPARE(server.requests.size(), 3);
        QCOMPARE(server.requests[2].body.value("text").toString(), QStringLiteral("Active"));
        BotServer::answer(server.requests[2], QJsonObject{{"message_id", 90}, {"date", 1700000001},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}, {"text", receiptText}});
        if (accepted) {
            QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 4, 1000);
            QVERIFY(server.requests[3].path.endsWith("getUpdates"));
            QVERIFY(runtime.active());
            QCOMPARE(errors.size(), 0);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 1000);
            QVERIFY(errors[0][0].toString().contains("uncertain", Qt::CaseInsensitive));
            QVERIFY(!runtime.active());
            QTest::qWait(300);
            QCOMPARE(server.requests.size(), 3);
        }
        runtime.stop();
    }
    void queuedStatusRepliesKeepTheirOriginAndText()
    {
        BotServer server;
        BotServer health;
        health.handler = [](const BotServer::Request &r) { BotServer::raw(r, "ok"); };
        TelegramBotRuntime runtime(server.root(), health.root().resolved(QUrl("/health")));
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        const auto command = [](qint64 id, qint64 chat, const QString &text) {
            auto incoming = update(id);
            auto message = incoming.value("message").toObject();
            message["text"] = text;
            auto sender = message.value("from").toObject(); sender["id"] = chat;
            auto origin = message.value("chat").toObject(); origin["id"] = chat;
            message["from"] = sender;
            message["chat"] = origin;
            incoming["message"] = message;
            return incoming;
        };
        const auto first = command(10, 42, QStringLiteral("status"));
        BotServer::answer(server.requests[1], QJsonArray{
            command(12, 84, QStringLiteral("/status")), first,
            command(11, 42, QStringLiteral("hello")), first});
        const QList<QPair<qint64, QString>> expected{
            {42, QStringLiteral("Active")}, {42, QStringLiteral("Message received.")},
            {84, QStringLiteral("Active")}};
        for (int i = 0; i < expected.size(); ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), i + 3, 1000);
            const auto sent = server.requests[i + 2];
            QVERIFY(sent.path.endsWith("sendMessage"));
            QCOMPARE(sent.body, (QJsonObject{{"chat_id", expected[i].first}, {"text", expected[i].second}}));
            // Explicit independent provider receipt, including the matching expected text.
            BotServer::answer(sent, QJsonObject{{"message_id", 90 + i}, {"date", 1700000001},
                {"chat", QJsonObject{{"id", expected[i].first}, {"type", "private"}}},
                {"text", expected[i].second}});
        }
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 6, 1000);
        QVERIFY(server.requests[5].path.endsWith("getUpdates"));
        QCOMPARE(server.requests[5].body.value("offset").toInteger(), 13);
        QCOMPARE(errors.size(), 0);
        runtime.stop();
    }
    void rejectsUnsafeEndpointsAndTokens_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("token");
        QTest::newRow("remote-http") << "http://example.com" << "123456:secret";
        QTest::newRow("remote-https") << "https://example.com" << "123456:secret";
        QTest::newRow("production-http") << "http://api.telegram.org" << "123456:secret";
        QTest::newRow("credentials") << "https://user:pass@api.telegram.org" << "123456:secret";
        QTest::newRow("query") << "https://api.telegram.org?x=1" << "123456:secret";
        QTest::newRow("path") << "https://api.telegram.org/untrusted" << "123456:secret";
        QTest::newRow("port") << "https://api.telegram.org:444" << "123456:secret";
        QTest::newRow("fragment") << "https://api.telegram.org#x" << "123456:secret";
        QTest::newRow("loopback-lookalike") << "http://127.0.0.1.example.com" << "123456:secret";
        QTest::newRow("loopback-path-token") << "loopback" << "123456:secret/../../getMe";
        QTest::newRow("empty-token") << "https://api.telegram.org" << "";
        QTest::newRow("path-token") << "https://api.telegram.org" << "123456:secret/../../getMe";
        QTest::newRow("newline-token") << "https://api.telegram.org" << "123456:secret\n";
    }
    void rejectsUnsafeEndpointsAndTokens()
    {
        QFETCH(QString, url);
        QFETCH(QString, token);
        BotServer server;
        TelegramBotRuntime runtime{url == QStringLiteral("loopback") ? server.root() : QUrl(url)};
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(token);
        QVERIFY(!runtime.starting());
        QVERIFY(!runtime.active());
        QCOMPARE(errors.size(), 1);
        QVERIFY(!errors[0][0].toString().contains("secret"));
    }
    void rejectsInvalidBotIdentity_data()
    {
        QTest::addColumn<QJsonObject>("identity");
        auto bot = BotServer::bot();
        bot["is_bot"] = false;
        QTest::newRow("human") << bot;
        bot = BotServer::bot(); bot["id"] = 999;
        QTest::newRow("wrong-bot") << bot;
        bot = BotServer::bot(); bot["id"] = "123456";
        QTest::newRow("string-id") << bot;
        bot = BotServer::bot(); bot.remove("first_name");
        QTest::newRow("missing-required-name") << bot;
    }
    void rejectsInvalidBotIdentity()
    {
        QFETCH(QJsonObject, identity);
        BotServer server;
        server.handler = [identity](const BotServer::Request &r) { BotServer::answer(r, identity); };
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 1000);
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.starting());
        QCOMPARE(server.requests.size(), 1);
    }
    void transportFailuresAreSanitized_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("expected");
        for (const auto status : {401, 403, 409, 429})
            QTest::newRow(qPrintable(QString::number(status))) << status
                << QByteArray("{\"ok\":false,\"error_code\":") + QByteArray::number(status)
                    + ",\"description\":\"123456:secret\",\"parameters\":{\"retry_after\":1}}"
                << QString::number(status);
        QTest::newRow("html") << 200 << QByteArray("123456:secret") << "invalid";
        QTest::newRow("redirect") << 302 << QByteArray{} << "redirect";
        QTest::newRow("large") << 200 << QByteArray(1024 * 1024 + 1, 'x') << "limit";
        QTest::newRow("network") << 0 << QByteArray{} << "network";
    }
    void transportFailuresAreSanitized()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QString, expected);
        BotServer server;
        BotServer redirectTarget;
        server.handler = [&](const BotServer::Request &r) {
            if (status == 0) r.socket->abort();
            else BotServer::raw(r, body, status,
                status == 302 ? "Location: " + redirectTarget.root().toEncoded() + "/stolen\r\n" : QByteArray{});
        };
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 2000);
        const auto message = errors[0][0].toString();
        QVERIFY2(message.contains(expected, Qt::CaseInsensitive), qPrintable(message));
        QVERIFY(!message.contains("secret"));
        QVERIFY(!message.contains("http"));
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.starting());
        QTest::qWait(400);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(redirectTarget.requests.size(), 0);
    }
    void possiblyCommittedSendIsNeverRetried_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("lost") << 0;
        QTest::newRow("malformed") << 1;
        QTest::newRow("wrong-chat") << 2;
        QTest::newRow("missing-message-id") << 3;
        QTest::newRow("wrong-text-echo") << 4;
    }
    void possiblyCommittedSendIsNeverRetried()
    {
        QFETCH(int, mode);
        BotServer server;
        server.handler = [mode](const BotServer::Request &r) {
            if (r.path.endsWith("getMe")) BotServer::answer(r, BotServer::bot());
            else if (r.path.endsWith("getUpdates")) BotServer::answer(r, QJsonArray{update(10), update(11)});
            else if (mode == 0) r.socket->abort(); // remote mutation committed, receipt lost
            else if (mode == 1) BotServer::raw(r, "invalid receipt 123456:secret");
            else {
                QJsonObject receipt{{"date", 1700000001}, {"chat", QJsonObject{{"id", mode == 2 ? 99 : 42}, {"type", "private"}}}};
                if (mode != 3) receipt["message_id"] = 90;
                if (mode == 4) receipt["text"] = "wrong";
                BotServer::answer(r, receipt);
            }
        };
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 2000);
        QVERIFY(errors[0][0].toString().contains("uncertain", Qt::CaseInsensitive));
        QVERIFY(!errors[0][0].toString().contains("secret"));
        QVERIFY(!runtime.active());
        QTest::qWait(500);
        QCOMPARE(server.requests.size(), 3); // no mutation retry, no dependent send or next poll
    }
    void requestHasAbsoluteDeadline()
    {
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE(server.requests.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 12000);
        QVERIFY(errors[0][0].toString().contains("timed out"));
        QVERIFY(!runtime.starting());
        QVERIFY(!runtime.active());
        QTRY_VERIFY(!server.requests[0].socket || server.requests[0].socket->state() == QAbstractSocket::UnconnectedState);
    }
    void stopAbortsAndRestartIsolated_data()
    {
        QTest::addColumn<int>("phase");
        QTest::newRow("getMe") << 0;
        QTest::newRow("getUpdates") << 1;
        QTest::newRow("sendMessage") << 2;
    }
    void stopAbortsAndRestartIsolated()
    {
        QFETCH(int, phase);
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:old_secret"));
        QTRY_COMPARE(server.requests.size(), 1);
        if (phase >= 1) {
            BotServer::answer(server.requests[0], BotServer::bot());
            QTRY_COMPARE(server.requests.size(), 2);
        }
        if (phase >= 2) {
            BotServer::answer(server.requests[1], QJsonArray{update(10), update(11)});
            QTRY_COMPARE(server.requests.size(), 3);
        }
        const auto stale = server.requests.last();
        runtime.stop();
        runtime.stop();
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.starting());
        QTRY_VERIFY(!stale.socket || stale.socket->state() == QAbstractSocket::UnconnectedState);
        const auto count = server.requests.size();
        runtime.start(QStringLiteral("654321:new_secret"));
        QTRY_COMPARE(server.requests.size(), count + 1);
        auto identity = BotServer::bot(); identity["id"] = 654321;
        BotServer::answer(stale, BotServer::bot()); // intentionally late, must be harmless
        BotServer::answer(server.requests.last(), identity);
        QTRY_VERIFY(runtime.active());
        QTRY_COMPARE(server.requests.size(), count + 2);
        QVERIFY(server.requests.last().path.startsWith("/bot654321:new_secret/"));
        QCOMPARE(server.requests.last().body.value("offset").toInteger(), 0);
        runtime.stop();
        QTest::qWait(400);
        QCOMPARE(server.requests.size(), count + 2);
        QCOMPARE(errors.size(), 0);
    }
    void stoppingInsideStartingSignalDoesNotDispatch()
    {
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        connect(&runtime, &TelegramBotRuntime::stateChanged, &runtime, [&] {
            if (runtime.starting()) runtime.stop();
        });
        runtime.start(QStringLiteral("123456:secret"));
        QVERIFY(!runtime.starting());
        QTest::qWait(100);
        QCOMPARE(server.requests.size(), 0);
    }
    void restartingInsideStartingSignalDoesNotDispatchOldRun()
    {
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        bool restart = true;
        connect(&runtime, &TelegramBotRuntime::stateChanged, &runtime, [&] {
            if (runtime.starting() && restart) {
                restart = false;
                runtime.start(QStringLiteral("654321:new_secret"));
            }
        });
        runtime.start(QStringLiteral("123456:old_secret"));
        QTRY_COMPARE(server.requests.size(), 1);
        QTest::qWait(100);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests[0].path, QByteArray("/bot654321:new_secret/getMe"));
        runtime.stop();
    }
    void explicitRestartDoesNotBlindlyRepeatUncertainSend()
    {
        BotServer server;
        int polls = 0;
        server.handler = [&polls](const BotServer::Request &r) {
            if (r.path.endsWith("getMe")) BotServer::answer(r, BotServer::bot());
            else if (r.path.endsWith("getUpdates")) {
                if (++polls <= 2) BotServer::answer(r, QJsonArray{update(10)});
            } else r.socket->abort();
        };
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 1000);
        QCOMPARE(server.requests.size(), 3);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 6, 1500);
        QVERIFY(runtime.active());
        QCOMPARE(errors.size(), 1);
        QVERIFY(server.requests[5].path.endsWith("getUpdates"));
        runtime.stop();
    }
    void invalidReplacementNotifiesStopped()
    {
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        runtime.start(QStringLiteral("123456:secret"));
        QSignalSpy changes(&runtime, &TelegramBotRuntime::stateChanged);
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("bad-token"));
        QVERIFY(!runtime.starting());
        QCOMPARE(changes.size(), 1);
        QCOMPARE(errors.size(), 1);
    }
    void invalidUpdateBatchNeverSends_data()
    {
        QTest::addColumn<QJsonValue>("batch");
        QTest::newRow("not-array") << QJsonValue(QJsonObject{});
        QTest::newRow("missing-id") << QJsonValue(QJsonArray{update(10), QJsonObject{{"message", QJsonObject{}}}});
        auto bad = update(11); bad["update_id"] = 11.5;
        QTest::newRow("fractional-id") << QJsonValue(QJsonArray{update(10), bad});
        bad["update_id"] = 0;
        QTest::newRow("zero-id") << QJsonValue(QJsonArray{update(10), bad});
        QJsonArray oversized;
        for (int i = 0; i < 101; ++i) oversized.append(update(i + 1));
        QTest::newRow("too-many") << QJsonValue(oversized);
    }
    void invalidUpdateBatchNeverSends()
    {
        QFETCH(QJsonValue, batch);
        BotServer server;
        server.handler = [batch](const BotServer::Request &r) {
            if (r.path.endsWith("getMe")) BotServer::answer(r, BotServer::bot());
            else if (r.path.endsWith("getUpdates")) BotServer::answer(r, batch);
        };
        TelegramBotRuntime runtime(server.root());
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 1000);
        QVERIFY(!runtime.active());
        QCOMPARE(server.requests.size(), 2);
    }
    void messagesSentOnBehalfOfHumanAreNotHumanInput()
    {
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE(server.requests.size(), 1);
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_COMPARE(server.requests.size(), 2);
        auto botMessage = update(1);
        auto message = botMessage["message"].toObject();
        message["sender_business_bot"] = BotServer::bot();
        botMessage["message"] = message;
        auto anonymous = update(2);
        message = anonymous["message"].toObject();
        message["sender_chat"] = QJsonObject{{"id", -100}, {"type", "channel"}};
        anonymous["message"] = message;
        BotServer::answer(server.requests[1], QJsonArray{botMessage, anonymous});
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 1000);
        QVERIFY(server.requests[2].path.endsWith("getUpdates"));
        runtime.stop();
    }
    void privateHumansOnlyNoDuplicatesAndPostJson()
    {
        BotServer server;
        server.handler = [](const BotServer::Request &r) {
            if (r.path.endsWith("getMe")) BotServer::answer(r, BotServer::bot());
        };
        TelegramBotRuntime runtime(server.root());
        runtime.start(QStringLiteral("123456:secret"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 1000);
        QCOMPARE(server.requests[1].path, QByteArray("/bot123456:secret/getUpdates"));
        QCOMPARE(server.requests[1].method, QByteArray("POST"));
        QCOMPARE(server.requests[1].body.value("timeout").toInt(), 25);
        QCOMPARE(server.requests[1].body.value("allowed_updates").toArray(), QJsonArray{"message"});
        BotServer::answer(server.requests[1], QJsonArray{update(1, "group"), update(2, "private", true),
            update(3, "private", false, "edited_message"), update(4, "channel", false, "channel_post"),
            update(5), update(5)});
        QTRY_COMPARE(server.requests.size(), 3);
        QCOMPARE(server.requests[2].path, QByteArray("/bot123456:secret/sendMessage"));
        QCOMPARE(server.requests[2].body, (QJsonObject{{"chat_id", 42}, {"text", "Message received."}}));
        // Legitimate sendMessage result does not need a text echo.
        BotServer::answer(server.requests[2], QJsonObject{{"message_id", 90}, {"date", 1700000001},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 4, 1500);
        QCOMPARE(server.requests[3].body.value("offset").toInteger(), 6);
        BotServer::answer(server.requests[3], QJsonArray{update(5)});
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 5, 1500);
        QVERIFY(server.requests[4].path.endsWith("getUpdates"));
        BotServer::answer(server.requests[4], QJsonArray{});
        QTest::qWait(80);
        QCOMPARE(server.requests.size(), 5); // immediate empty polls never spin
        runtime.stop();
        QTest::qWait(400);
        QCOMPARE(server.requests.size(), 5);
    }
    void credentialStateIsNotExposedAsProperties()
    {
        TelegramBotRuntime runtime;
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.starting());
        QCOMPARE(runtime.metaObject()->indexOfProperty("token"), -1);
        QCOMPARE(runtime.metaObject()->indexOfProperty("apiRoot"), -1);
    }
    void validatesBeforeActiveAndStops()
    {
        BotServer server;
        TelegramBotRuntime runtime(server.root());
        QSignalSpy changes(&runtime, &TelegramBotRuntime::stateChanged);
        QSignalSpy errors(&runtime, &TelegramBotRuntime::errorOccurred);
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.starting());
        runtime.start(QStringLiteral("123456:secret"));
        QVERIFY(runtime.starting());
        QVERIFY(!runtime.active());
        QTRY_COMPARE(server.requests.size(), 1);
        QCOMPARE(server.requests[0].method, QByteArray("POST"));
        QCOMPARE(server.requests[0].path, QByteArray("/bot123456:secret/getMe"));
        QVERIFY(server.requests[0].body.isEmpty());
        BotServer::answer(server.requests[0], BotServer::bot());
        QTRY_VERIFY(runtime.active());
        QVERIFY(!runtime.starting());
        runtime.stop();
        QVERIFY(!runtime.active());
        QVERIFY(!runtime.starting());
        QVERIFY(changes.size() >= 3);
        QCOMPARE(errors.size(), 0);
    }
};
QTEST_GUILESS_MAIN(TestTelegramBotRuntime)
#include "test_telegrambotruntime.moc"
