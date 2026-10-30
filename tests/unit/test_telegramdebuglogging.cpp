#include "telegramagent.h"
#include "telegramwebhookmanager.h"
#include "telegramcontrollertestserver.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>
#include <utility>

namespace {
QStringList *capturedMessages = nullptr;
QtMessageHandler previousHandler = nullptr;
std::function<void()> onCapturedMessage;
void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (capturedMessages && QByteArray(context.category) == "publics.telegram.incoming") {
        capturedMessages->append(message);
        auto callback = std::exchange(onCapturedMessage, std::function<void()>{});
        if (callback) callback();
        return;
    }
    if (previousHandler) previousHandler(type, context, message);
}
struct Capture {
    QStringList messages;
    Capture() { capturedMessages = &messages; previousHandler = qInstallMessageHandler(captureMessage); }
    ~Capture() { onCapturedMessage = {}; qInstallMessageHandler(previousHandler); capturedMessages = nullptr; previousHandler = nullptr; }
};
QJsonObject incoming(const QString &text)
{
    return {{"update_id", 10}, {"message", QJsonObject{
        {"message_id", 7}, {"date", 1700000000}, {"text", text},
        {"from", QJsonObject{{"id", 42}, {"is_bot", false}, {"first_name", "synthetic-private-name"}}},
        {"chat", QJsonObject{{"id", 42}, {"type", "private"}, {"username", "synthetic-private-user"}}},
        {"password", "synthetic-private-password"}, {"bot_token", "123456:synthetic-private-token"},
        {"secret_token", "synthetic-private-verifier"},
        {"synthetic-private-field-name", "synthetic-private-extra"},
        {"reply_to_message", QJsonObject{{"text", "synthetic-private-reply"},
            {"from", QJsonObject{{"id", 84}, {"first_name", "synthetic-private-reply-name"}}}}}}}};
}
void verifyLog(const QStringList &messages, const QString &transport, const QString &text, bool recognized)
{
#if PUBLICS_EXPECT_INCOMING_LOGS
    QCOMPARE(messages.size(), 1);
    const QString output = messages.first();
    QVERIFY(!output.contains(QStringLiteral("synthetic-private")));
    QVERIFY(!output.contains(QStringLiteral("123456:")));
    const auto document = QJsonDocument::fromJson(output.toUtf8());
    QVERIFY(document.isObject());
    const auto record = document.object();
    QCOMPARE(record.value("format").toString(), QStringLiteral("Telegram Update JSON"));
    QCOMPARE(record.value("transport").toString(), transport);
    const auto payload = record.value("payload").toObject();
    QCOMPARE(payload.value("update_id").toInteger(), qint64(10));
    const auto message = payload.value("message").toObject();
    QCOMPARE(message.value("chat").toObject().value("id").toString(), QStringLiteral("[redacted]"));
    QCOMPARE(message.value("from").toObject().value("id").toString(), QStringLiteral("[redacted]"));
    QCOMPARE(message.value("text").toString(), recognized ? text : QStringLiteral("[redacted unrecognized command]"));
    QVERIFY(output.toUtf8().size() <= 8192);
#else
    Q_UNUSED(transport); Q_UNUSED(text); Q_UNUSED(recognized);
    QVERIFY(messages.isEmpty());
#endif
}
}

class TelegramDebugLoggingTest final : public QObject {
    Q_OBJECT
private slots:
    void pollingLogsAuthorizedCommandOnlyInDebug_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<bool>("recognized");
        QTest::newRow("status") << QStringLiteral("/status") << true;
        QTest::newRow("buy") << QStringLiteral("publics_auto_buy AAPL, MSFT") << true;
        QTest::newRow("sell") << QStringLiteral("publics_auto_sell AAPL") << true;
        QTest::newRow("rh-buy") << QStringLiteral("robinhood_auto_buy aapl, MSFT") << true;
        QTest::newRow("rh-sell") << QStringLiteral("/robinhood_auto_sell BRK.B,BRK-B") << true;
        QStringList many;
        for (int i = 0; i < 700; ++i) many.append(QStringLiteral("T%1").arg(i));
        QTest::newRow("rh-all-700") << "robinhood_auto_buy " + many.join(',') << true;
        QTest::newRow("rh-exact-4096") << "robinhood_auto_buy " + QString(4096 - QString("robinhood_auto_buy A").size(), ' ') + "A" << true;
        QTest::newRow("publics-over-50") << "publics_auto_buy " + QString("A,").repeated(50) + "A" << false;
        QTest::newRow("publics-over-1024") << "publics_auto_buy " + QString(1024, ' ') + "A" << false;
        QTest::newRow("combined-buy") << QStringLiteral("auto_buy aapl, MSFT") << true;
        QTest::newRow("combined-sell") << QStringLiteral("/auto_sell AAPL,MSFT") << true;
        QStringList combined;
        for (int i = 0; i < 300; ++i) combined.append(QString(1, QChar('A' + i / 26)) + QChar('A' + i % 26));
        QTest::newRow("combined-300") << "auto_buy " + combined.join(',') << true;
        QTest::newRow("combined-punctuation") << QStringLiteral("auto_buy BRK.B") << false;
        QTest::newRow("combined-digits") << QStringLiteral("auto_buy T1") << false;
        QTest::newRow("combined-case") << QStringLiteral("Auto_buy AAPL") << false;
        QTest::newRow("rh-old-buy") << QStringLiteral("robinhoodAutoBuy AAPL") << false;
        QTest::newRow("rh-wrong-case") << QStringLiteral("Robinhood_auto_buy AAPL") << false;
        QTest::newRow("rh-unicode") << QStringLiteral("robinhood_auto_buy K") << false;
        QTest::newRow("rh-freeform") << QStringLiteral("robinhood_auto_buy synthetic-private-free-form-text") << false;
        QTest::newRow("old-buy") << QStringLiteral("publicsAutoBuy AAPL") << false;
        QTest::newRow("old-sell") << QStringLiteral("publicsAutoSell AAPL") << false;
        QTest::newRow("unknown-private-text") << QStringLiteral("synthetic-private-free-form-text") << false;
    }
    void pollingLogsAuthorizedCommandOnlyInDebug()
    {
        QFETCH(QString, command); QFETCH(bool, recognized);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        ControllerBotServer provider, health;
        TelegramAgent agent(provider.root(), health.healthUrl());
        agent.setAllowedChatIds({QStringLiteral("42")});
        agent.setCommandLedgerPathForTests(directory.path());
        connect(&agent, &TelegramAgent::tradeCommandReceived, &agent,
            [&](QString ticket, QString, QStringList) { agent.completeTradeCommand(ticket, {QStringLiteral("Fixture result")}); });
        connect(&agent, &TelegramAgent::robinhoodTradeCommandReceived, &agent,
            [&](QString ticket, QString, QStringList) { agent.completeTradeCommand(ticket, {QStringLiteral("Fixture result")}); });
        connect(&agent, &TelegramAgent::combinedTradeCommandReceived, &agent,
            [&](QString ticket, QString, QStringList) { agent.completeTradeCommand(ticket, {QStringLiteral("Fixture result")}); });
        agent.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(provider.polls.size(), 1);
        Capture capture;
        const auto socket = provider.polls.takeFirst();
        QVERIFY(socket);
        ControllerBotServer::answer(socket, QJsonArray{incoming(command)});
        QTRY_COMPARE(provider.messages.size(), 1);
        verifyLog(capture.messages, QStringLiteral("polling"), command, recognized);
        QTRY_COMPARE(provider.polls.size(), 1);
        ControllerBotServer::answer(provider.polls.takeFirst(), QJsonArray{incoming(command)});
        QTRY_COMPARE(provider.polls.size(), 1);
        QCOMPARE(provider.messages.size(), 1); // Duplicate diagnostics never replay the command.
        QCOMPARE(capture.messages.size(), PUBLICS_EXPECT_INCOMING_LOGS ? 2 : 0);
        verifyLog(capture.messages.mid(1), QStringLiteral("polling"), command, recognized);
        agent.stop();
    }
    void messageHandlerMayStopOrDestroyRuntime_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::addColumn<QString>("command");
        for (const auto &command : QStringList{"status", "publics_auto_buy AAPL", "robinhood_auto_sell AAPL"}) {
            QTest::newRow(qPrintable("stop-" + command)) << false << command;
            QTest::newRow(qPrintable("destroy-" + command)) << true << command;
        }
    }
    void messageHandlerMayStopOrDestroyRuntime()
    {
#if PUBLICS_EXPECT_INCOMING_LOGS
        QFETCH(bool, destroy); QFETCH(QString, command);
        ControllerBotServer provider, health;
        auto agent = std::make_unique<TelegramAgent>(provider.root(), health.healthUrl());
        agent->setAllowedChatIds({QStringLiteral("42")});
        agent->start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(provider.polls.size(), 1);
        Capture capture;
        onCapturedMessage = [&] { if (destroy) agent.reset(); else agent->stop(); };
        ControllerBotServer::answer(provider.polls.takeFirst(), QJsonArray{incoming(command)});
        QTRY_COMPARE(capture.messages.size(), 1);
        QVERIFY(!agent || !agent->active());
        QCOMPARE(provider.messages.size(), 0);
#else
        QSKIP("Incoming payload logger is compiled out outside Debug.");
#endif
    }
    void messageHandlerRestartPreservesReplacementSession()
    {
#if PUBLICS_EXPECT_INCOMING_LOGS
        ControllerBotServer provider, health;
        TelegramAgent agent(provider.root(), health.healthUrl());
        agent.setAllowedChatIds({QStringLiteral("42")});
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start(QStringLiteral("123456:synthetic"));
        QTRY_COMPARE(provider.polls.size(), 1);
        Capture capture;
        onCapturedMessage = [&] {
            agent.stop();
            agent.start(QStringLiteral("123456:synthetic"));
        };
        ControllerBotServer::answer(provider.polls.takeFirst(), QJsonArray{incoming(QStringLiteral("status"))});
        QTRY_COMPARE(capture.messages.size(), 1);
        QTRY_VERIFY(agent.active());
        QTRY_COMPARE(provider.polls.size(), 1);
        QCOMPARE(provider.messages.size(), 0);
        auto next = incoming(QStringLiteral("status"));
        next.insert("update_id", 11);
        ControllerBotServer::answer(provider.polls.takeFirst(), QJsonArray{next});
        QTRY_COMPARE(provider.messages.size(), 1);
        QCOMPARE(capture.messages.size(), 2);
        QCOMPARE(errors.size(), 0);
        QVERIFY(agent.active());
        agent.stop();
#else
        QSKIP("Incoming payload logger is compiled out outside Debug.");
#endif
    }
    void standaloneWebhookLogsAfterAuthenticationOnly_data()
    {
        QTest::addColumn<QString>("command");
        QTest::newRow("status") << QString("status");
        QTest::newRow("publics") << QString("publics_auto_buy AAPL");
        QTest::newRow("rh") << QString("/robinhood_auto_sell aapl,MSFT");
        QStringList many;
        for (int i = 0; i < 700; ++i) many.append(QStringLiteral("T%1").arg(i));
        QTest::newRow("rh-700") << "robinhood_auto_buy " + many.join(',');
    }
    void standaloneWebhookLogsAfterAuthenticationOnly()
    {
        QFETCH(QString, command);
        TelegramWebhookManager::Options options; options.port = 0;
        TelegramWebhookManager manager(QStringLiteral("123456:synthetic"), QStringLiteral("fixture_verifier"),
            QStringLiteral("https://example.test/telegram"), 42, options);
        QVERIFY(manager.listeningPort());
        QSignalSpy commands(&manager, &TelegramWebhookManager::commandReceived);
        Capture capture;
        const auto post = [&](const QByteArray &secret, const QJsonObject &update) {
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, manager.listeningPort());
            if (!socket.waitForConnected(1000)) return -1;
            const auto body = QJsonDocument(update).toJson(QJsonDocument::Compact);
            socket.write("POST /telegram HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\n"
                "X-Telegram-Bot-Api-Secret-Token: " + secret + "\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            QByteArray response;
            if (!QTest::qWaitFor([&] { response += socket.readAll(); return response.contains("\r\n"); }, 3000)) return -1;
            return response.split(' ').value(1).toInt();
        };
        const auto update = incoming(command);
        QCOMPARE(post("wrong", update), 403);
        QVERIFY(capture.messages.isEmpty());
        auto unauthorized = update;
        auto message = unauthorized.value("message").toObject();
        message.insert("from", QJsonObject{{"id", 84}, {"is_bot", false}});
        message.insert("chat", QJsonObject{{"id", 84}, {"type", "private"}});
        unauthorized.insert("message", message);
        QCOMPARE(post("fixture_verifier", unauthorized), 200);
        QVERIFY(capture.messages.isEmpty());
        QCOMPARE(commands.size(), 0);
        QCOMPARE(post("fixture_verifier", update), 200);
        QCOMPARE(commands.size(), 1);
        verifyLog(capture.messages, QStringLiteral("webhook"), command, true);

        capture.messages.clear();
        auto large = update;
        message = large.value("message").toObject();
        QJsonArray entities;
        for (int i = 0; i < 512; ++i)
            entities.append(QJsonObject{{"type", "bold"}, {"offset", 0}, {"length", 1},
                {"url", "synthetic-private-url"}});
        message.insert("entities", entities);
        QJsonObject nested{{"text", "synthetic-private-deep-text"}};
        for (int i = 0; i < 64; ++i) nested = QJsonObject{{"reply_to_message", nested}};
        message.insert("reply_to_message", nested);
        large.insert("message", message);
        QCOMPARE(post("fixture_verifier", large), 200);
        QCOMPARE(commands.size(), 2);
        verifyLog(capture.messages, QStringLiteral("webhook"), command, true);
    }
};
QTEST_GUILESS_MAIN(TelegramDebugLoggingTest)
#include "test_telegramdebuglogging.moc"
