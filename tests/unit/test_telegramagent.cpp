#include "telegramagent.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

// Loopback-only, independently specified Telegram User/Update/Message fixtures.
class AgentServer final : public QTcpServer
{
public:
    struct Request { QByteArray path; QJsonObject body; QPointer<QTcpSocket> socket; };
    QList<Request> requests;
    AgentServer()
    {
        if (!listen(QHostAddress::LocalHost)) qFatal("Cannot listen on loopback");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    *bytes += socket->readAll();
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (const auto &line : bytes->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    requests.append({bytes->left(bytes->indexOf('\r')).split(' ').value(1),
                        QJsonDocument::fromJson(bytes->mid(end + 4, length)).object(), socket});
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); }
    static void raw(const Request &r, QByteArray body, int status = 200)
    {
        if (!r.socket) return;
        r.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        r.socket->disconnectFromHost();
    }
    static void answer(const Request &r, QJsonValue value)
    { raw(r, QJsonDocument(QJsonObject{{"ok", true}, {"result", value}}).toJson(QJsonDocument::Compact)); }
    static void receipt(const Request &r)
    { answer(r, QJsonObject{{"message_id", 90}, {"date", 1700000001}, {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}); }
    static QJsonObject update(int id, QString text)
    { return {{"update_id", id}, {"message", QJsonObject{{"message_id", id}, {"date", 1700000000}, {"text", text},
        {"from", QJsonObject{{"id", 42}, {"is_bot", false}}}, {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}}; }
    static void fail(const Request &r, const QString &kind)
    {
        if (kind == "lost") { raw(r, "{", 503); return; }
        if (kind == "invalid") { answer(r, QJsonObject{{"message_id", 90}, {"date", 1700000001},
            {"chat", QJsonObject{{"id", 84}, {"type", "private"}}}}); return; }
        if (kind == "oversized") { raw(r, QByteArray(1024 * 1024 + 1, 'x')); return; }
        raw(r, "{\"ok\":false,\"error_code\":403}", 403);
    }
};

class TestTelegramAgent final : public QObject
{
    Q_OBJECT
private slots:
    void authenticationNotificationFailure_data()
    {
        QTest::addColumn<QString>("failure");
        QTest::addColumn<bool>("completeWhileInFlight");
        for (const auto &failure : {QString("lost"), QString("invalid"), QString("oversized")})
            for (bool complete : {false, true})
                QTest::newRow(qPrintable(failure + (complete ? "-terminal-queued" : "-progress"))) << failure << complete;
    }
    void authenticationNotificationFailure()
    {
        QFETCH(QString, failure); QFETCH(bool, completeWhileInFlight);
        AgentServer server; QTemporaryDir ledger; TelegramAgent agent(server.root());
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy auth(&agent, &TelegramAgent::robinhoodAuthenticationCommandReceived);
        QSignalSpy trades(&agent, &TelegramAgent::tradeCommandReceived);
        QSignalSpy finished(&agent, &TelegramAgent::tradeReportFinished);
        QSignalSpy errors(&agent, &TelegramAgent::errorOccurred);
        agent.start("123456:synthetic");
        QTRY_COMPARE(server.requests.size(), 1);
        AgentServer::answer(server.requests[0], QJsonObject{{"id", 123456}, {"is_bot", true}, {"first_name", "Test"}});
        QTRY_COMPARE(server.requests.size(), 2);
        AgentServer::answer(server.requests[1], QJsonArray{AgentServer::update(10, "authenticate_robinhood_account a@example.test"),
            AgentServer::update(11, "publics_auto_buy AAPL")});
        QTRY_COMPARE(auth.size(), 1);
        const auto ticket = auth[0][0].toString();
        QVERIFY(agent.reportTradeCommand(ticket, {"Progress"}));
        QTRY_COMPARE(server.requests.size(), 3);
        if (completeWhileInFlight) agent.completeTradeCommand(ticket, {"Eight minute timeout"});
        AgentServer::fail(server.requests[2], failure);
        QTRY_COMPARE(errors.size(), 1);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(agent.active());
        QCOMPARE(trades.size(), 0);
        if (!completeWhileInFlight) {
            QTest::qWait(300); QCOMPARE(server.requests.size(), 3);
            agent.completeTradeCommand(ticket, {"Eight minute timeout"});
        }
        QTRY_COMPARE(server.requests.size(), 4);
        QCOMPARE(server.requests[3].body["text"].toString(), QString("Eight minute timeout"));
        QCOMPARE(server.requests[3].body["chat_id"].toInteger(), qint64(42));
        // Final notification failure also settles without repeating authentication.
        AgentServer::fail(server.requests[3], failure);
        QTRY_COMPARE(errors.size(), 2);
        QTRY_COMPARE(trades.size(), 1);
        QCOMPARE(auth.size(), 1); QCOMPARE(finished.size(), 1);
        QCOMPARE(server.requests.size(), 4);
        agent.stop();
        // A healthy new intake still cannot replay either durable admission.
        agent.start("123456:synthetic");
        QTRY_COMPARE(server.requests.size(), 5);
        AgentServer::answer(server.requests[4], QJsonObject{{"id", 123456}, {"is_bot", true}, {"first_name", "Test"}});
        QTRY_COMPARE(server.requests.size(), 6);
        AgentServer::answer(server.requests[5], QJsonArray{AgentServer::update(10, "authenticate_robinhood_account a@example.test")});
        QTRY_COMPARE(server.requests.size(), 7);
        QVERIFY(server.requests[6].path.endsWith("/getUpdates"));
        QCOMPARE(auth.size(), 1); QCOMPARE(trades.size(), 1);
    }
    void hardHttpStatusCannotRecoverFromBodyFailure_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("oversized");
        for (int status : {401, 403, 409})
            for (bool oversized : {true, false}) {
                // Qt withholds HTTP401 metadata until its body completes; a
                // header-only timeout is unclassified, not a known hard status.
                if (status == 401 && !oversized) continue;
                QTest::newRow(qPrintable(QString::number(status) + (oversized ? "-oversized" : "-held-body")))
                    << status << oversized;
            }
    }
    void hardHttpStatusCannotRecoverFromBodyFailure()
    {
        QFETCH(int,status); QFETCH(bool,oversized);
        AgentServer server; QTemporaryDir ledger; TelegramAgent agent(server.root());
        agent.setAllowedChatIds({"42"}); agent.setCommandLedgerPathForTests(ledger.path());
        QSignalSpy auth(&agent,&TelegramAgent::robinhoodAuthenticationCommandReceived);
        QSignalSpy errors(&agent,&TelegramAgent::errorOccurred);
        QSignalSpy finished(&agent,&TelegramAgent::tradeReportFinished);
        agent.start("123456:synthetic"); QTRY_COMPARE(server.requests.size(),1);
        AgentServer::answer(server.requests[0],QJsonObject{{"id",123456},{"is_bot",true},{"first_name","Test"}});
        QTRY_COMPARE(server.requests.size(),2);
        AgentServer::answer(server.requests[1],QJsonArray{
            AgentServer::update(10,"authenticate_robinhood_account a@example.test")});
        QTRY_COMPARE(auth.size(),1);
        const auto ticket=auth[0][0].toString();
        QVERIFY(agent.reportTradeCommand(ticket,{"Progress"}));
        QTRY_COMPARE(server.requests.size(),3);
        agent.completeTradeCommand(ticket,{"Terminal"});
        const auto request=server.requests[2];
        if (oversized) AgentServer::raw(request,QByteArray(1024*1024+1,'x'),status);
        else {
            QVERIFY(request.socket);
            request.socket->write("HTTP/1.1 "+QByteArray::number(status)+
                " Test\r\nContent-Type: application/json\r\nContent-Length: 100000\r\n\r\n");
            request.socket->flush(); // Headers are known; hold the body past the absolute deadline.
        }
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(),1,12000);
        QVERIFY(!agent.active());
        QTRY_COMPARE(finished.size(),1);
        QTest::qWait(50); QCOMPARE(server.requests.size(),3); QCOMPARE(auth.size(),1);
    }
    void notificationObserverLifecycle_data()
    {
        QTest::addColumn<QString>("action"); QTest::addColumn<QString>("failure");
        for (const auto &action : {QString("delete"), QString("restart"), QString("revoke"), QString("hard-error")})
            for (const auto &failure : {QString("lost"), QString("invalid"), QString("oversized")})
                QTest::newRow(qPrintable(action + "-" + failure)) << action << failure;
    }
    void notificationObserverLifecycle()
    {
        QFETCH(QString, action); QFETCH(QString, failure);
        AgentServer server; QTemporaryDir ledger;
        QPointer<TelegramAgent> agent = new TelegramAgent(server.root());
        agent->setAllowedChatIds({"42"}); agent->setCommandLedgerPathForTests(ledger.path());
        QSignalSpy auth(agent, &TelegramAgent::robinhoodAuthenticationCommandReceived);
        QSignalSpy errors(agent, &TelegramAgent::errorOccurred);
        agent->start("123456:synthetic");
        QTRY_COMPARE(server.requests.size(), 1);
        AgentServer::answer(server.requests[0], QJsonObject{{"id", 123456}, {"is_bot", true}, {"first_name", "Test"}});
        QTRY_COMPARE(server.requests.size(), 2);
        AgentServer::answer(server.requests[1], QJsonArray{AgentServer::update(10, "authenticate_robinhood_account a@example.test")});
        QTRY_COMPARE(auth.size(), 1);
        const auto ticket = auth[0][0].toString();
        QVERIFY(agent->reportTradeCommand(ticket, {"Progress"}));
        QTRY_COMPARE(server.requests.size(), 3);
        agent->completeTradeCommand(ticket, {"Terminal"});
        connect(agent, &TelegramAgent::errorOccurred, &server, [&] {
            if (action == "delete") delete agent.data();
            else if (action == "restart") { agent->stop(); agent->start("123456:replacement"); }
            else if (action == "revoke") agent->setAllowedChatIds({});
        });
        AgentServer::fail(server.requests[2], action == "hard-error" ? "forbidden" : failure);
        QTRY_COMPARE(errors.size(), 1);
        QTest::qWait(300);
        if (action == "delete") QVERIFY(!agent);
        else if (action == "restart") {
            QCOMPARE(server.requests.size(), 4);
            QVERIFY(server.requests[3].path.endsWith("/getMe"));
        } else if (action == "revoke") {
            // Revocation denies commands/reports; read-only polling may continue.
            QTRY_COMPARE(server.requests.size(), 4);
            QVERIFY(server.requests[3].path.endsWith("/getUpdates"));
            AgentServer::answer(server.requests[3], QJsonArray{
                AgentServer::update(11, "authenticate_robinhood_account a@example.test")});
            QTRY_COMPARE(server.requests.size(), 5);
            QVERIFY(server.requests[4].path.endsWith("/getUpdates"));
            QCOMPARE(auth.size(), 1);
        } else {
            QCOMPARE(server.requests.size(), 3);
            if (action == "hard-error") QVERIFY(!agent->active());
        }
        delete agent.data();
    }
};
QTEST_GUILESS_MAIN(TestTelegramAgent)
#include "test_telegramagent.moc"
