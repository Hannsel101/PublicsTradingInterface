#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>

#include "telegrambotsapplet.h"
#include "telegrambotscontroller.h"
#include "telegrampublicstradeservice.h"
#include "telegramrobinhoodtradeservice.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include "securecredentiallocker.h"
#include "telegramrobinhoodallsessionsfixture.h"
#include "telegramcombinedtradeservice.h"
#include "auth/publicauthclient.h"
#include "api/publicapiworker.h"
#include "trading/autotradecontroller.h"
#include "../unit/telegramcontrollertestserver.h"

namespace {
class BridgeRobinhoodStore final : public RHCredentialStore {
public:
    int reads = 0;
    void write(int, const QString &, Completion done) override { done(false, QStringLiteral("Read-only fixture")); }
    void read(int, ReadCompletion done) override { ++reads; done(true, {}, {}); }
    void remove(int, Completion done) override { done(false, QStringLiteral("Read-only fixture")); }
};

class BridgeBotStore final : public TelegramBotStore {
public:
    BridgeBotStore() : TelegramBotStore(QString{}, BinaryBackend{}) {}
    void load(LoadCompletion done) override
    {
        done(true, {{QStringLiteral("one"), QStringLiteral("First bot"), {QStringLiteral("42")}, true, true},
                    {QStringLiteral("two"), QStringLiteral("Other bot"), {QStringLiteral("42")}, true, true}}, {});
    }
    void saveProtectedBot(QString, QString, QString, QStringList, QString, QString, AddCompletion done) override {
        done(false, {}, QStringLiteral("Fixture read-only"));
    }
    void remove(QString, AddCompletion done) override { done(false, {}, QStringLiteral("Fixture read-only")); }
    void readActivationSnapshot(QString id, QString, ActivationCompletion done) override
    {
        done(true, {id == QStringLiteral("one") ? QStringLiteral("123456:synthetic")
                                               : QStringLiteral("987654:synthetic"), {QStringLiteral("42")}, QString(64, QLatin1Char('a'))}, {});
    }
    void readToken(QString id, TokenCompletion done) override
    {
        done(true, id == QStringLiteral("one") ? QStringLiteral("123456:synthetic")
                                               : QStringLiteral("987654:synthetic"), {});
    }
};

class BridgeBroker final : public QTcpServer {
public:
    QList<QJsonObject> orders;
    QPointer<QTcpSocket> held;
    bool holdFirst = true;
    int portfolioReads = 0;
    BridgeBroker()
    {
        if (!listen(QHostAddress::LocalHost)) qFatal("Cannot bind bridge fixture");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                const auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int size = 0;
                    for (const auto &line : buffer->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                    if (buffer->size() < end + 4 + size) return;
                    socket->setProperty("handled", true);
                    const auto path = buffer->split(' ').value(1);
                    const auto body = QJsonDocument::fromJson(buffer->mid(end + 4, size)).object();
                    if (path.endsWith("/personal/access-tokens")) {
                        answer(socket, QJsonObject{{"accessToken", "bridge-fixture-token"}});
                    } else if (path.endsWith("/trading/account")) {
                        answer(socket, QJsonObject{{"accounts", QJsonArray{
                            QJsonObject{{"accountId", "A"}, {"accountType", "BROKERAGE"}},
                            QJsonObject{{"accountId", "B"}, {"accountType", "ROTH_IRA"}}}}});
                    } else if (path.endsWith("/portfolio/v2")) {
                        ++portfolioReads;
                        answer(socket, QJsonObject{{"positions", QJsonArray{}}});
                    } else if (path.endsWith("/order") || path.endsWith("/preflight/single-leg")) {
                        orders.append(body);
                        socket->setProperty("order", path.endsWith("/order"));
                        socket->setProperty("orderId", body.value("orderId").toString());
                        if (holdFirst) { held = socket; return; }
                        answerOrder(socket);
                    } else {
                        answer(socket, QJsonObject{}, 404);
                    }
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl root() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(serverPort())); }
    static void answer(QTcpSocket *socket, const QJsonObject &object, int code = 200)
    {
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(code)
            + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
    static void answerOrder(QTcpSocket *socket)
    {
        answer(socket, socket->property("order").toBool()
            ? QJsonObject{{"orderId", socket->property("orderId").toString()}}
            : QJsonObject{{"orderValue", "100"}, {"estimatedExecutionFee", "0"}});
    }
    void release()
    {
        holdFirst = false;
        if (held) { const auto socket = held; held = nullptr; answerOrder(socket); }
    }
};
}

class TelegramTradeBridgeTest final : public QObject {
    Q_OBJECT
private slots:
    void combinedNoticesAreCorrelatedWithoutAdvancingSettlement()
    {
        TelegramRobinhoodAllSessionsFixture::Fixture rh;
        QVERIFY(rh.ready);
        rh.saveNative("active@example.test", "active");
        TelegramRobinhoodTradeService robinhood(&rh.network, rh.sessions.get(), rh.dependencies());
        TelegramCombinedTradeService combined(nullptr, &robinhood);
        QSignalSpy boundaries(&combined, &TelegramCombinedTradeService::reportReady);
        QSignalSpy notices(&combined, SIGNAL(noticeReady(QString,QStringList)));
        QSignalSpy completed(&combined, &TelegramCombinedTradeService::completed);
        QVERIFY(notices.isValid());
        const QStringList warning{QStringLiteral("<b>Expiry warning</b>")};
        emit robinhood.reportReady("foreign", warning);
        QCOMPARE(notices.size(), 0);
        QVERIFY(combined.submit("root-request", {"AAPL"}, "BUY"));
        QTRY_COMPARE(boundaries.size(), 1);
        QVERIFY(rh.orders.isEmpty());
        emit robinhood.reportReady("root-request", warning);
        QCOMPARE(notices.size(), 0);
        combined.continueAfterReport("root-request");
        QTRY_COMPARE(rh.timers.size(), 1);
        QString childId;
        connect(&robinhood, &TelegramRobinhoodTradeService::completed, &combined,
                [&](QString id, QStringList) {
            childId = id;
            // Terminal delivery is deferred; the matching child still owns this phase.
            emit robinhood.reportReady(id, warning);
            QCOMPARE(completed.size(), 0);
            QVERIFY(combined.busy());
        });
        QElapsedTimer clock; clock.start();
        while (robinhood.busy() && clock.elapsed() < 15000) {
            QCoreApplication::processEvents();
            if (!rh.timers.isEmpty()) { auto cooldown = rh.timers.takeFirst(); cooldown(); }
            QTest::qWait(1);
        }
        QVERIFY(!robinhood.busy());
        QVERIFY(!childId.isEmpty());
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first()[0].toString(), QStringLiteral("root-request"));
        QCOMPARE(notices.first()[1].toStringList(), warning);
        QCOMPARE(boundaries.size(), 1);
        emit robinhood.reportReady(childId + "/stale", warning);
        QCOMPARE(notices.size(), 1);
        combined.continueAfterReport("root-request");
        QTRY_COMPARE(completed.size(), 1);
        emit robinhood.reportReady(childId, warning);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(rh.orders.size(), 3);
        QCOMPARE(rh.renewals.size(), 1);
    }

    void nativeRobinhoodAndCombinedCommandsUseRealComposition_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<bool>("combined");
        QTest::addColumn<bool>("sell");
        QTest::addColumn<bool>("loseAck");
        QTest::addColumn<bool>("expired");
        QTest::newRow("robinhood-buy") << QStringLiteral("robinhood_auto_buy aapl") << false << false << false << false;
        QTest::newRow("robinhood-sell") << QStringLiteral("robinhood_auto_sell aApL") << false << true << false << false;
        QTest::newRow("combined-buy") << QStringLiteral("auto_buy aapl") << true << false << false << false;
        QTest::newRow("combined-sell") << QStringLiteral("auto_sell AaPl") << true << true << false << false;
        QTest::newRow("combined-lost-publics-report-ack") << QStringLiteral("auto_buy AAPL") << true << false << true << false;
        QTest::newRow("robinhood-expired-warning") << QStringLiteral("robinhood_auto_buy AAPL") << false << false << false << true;
        QTest::newRow("combined-expired-warning") << QStringLiteral("auto_buy AAPL") << true << false << false << true;
    }

    void nativeRobinhoodAndCombinedCommandsUseRealComposition()
    {
        QFETCH(QString, command);
        QFETCH(bool, combined);
        QFETCH(bool, sell);
        QFETCH(bool, loseAck);
        QFETCH(bool, expired);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        BridgeBroker publicsBroker;
        publicsBroker.holdFirst = combined && !sell;
        const auto group = QStringLiteral("Combined_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        PublicAuthClient auth(QString(), group, group, publicsBroker.root(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) {
                done(QStringLiteral("synthetic-broker-key"), {});
            });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0")});
        PublicApiWorker worker(publicsBroker.root());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.accountPauseMs = policy.symbolPauseMs = 0;
        policy.gatewayCooldownMs = policy.retryBaseMs = 0;
        worker.setRequestPolicy(policy);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        connect(&auth, &PublicAuthClient::selectedApiKeyLabelChanged, &worker, [&] {
            if (auth.selectedApiKeyLabel().isEmpty()) worker.clearSubAccountsList();
        });
        AutoTradeController trader(&auth, &worker);
        TelegramPublicsTradeService publics(&trader, &auth);
        TelegramRobinhoodAllSessionsFixture::Fixture rh;
        QVERIFY(rh.ready);
        rh.saveNative("first@example.test", "first");
        rh.saveNative("second@example.test", "second");
        if (expired) {
            rh.saveNative("expired@example.test", "expired");
            QVERIFY(rh.expire(2));
        }
        TelegramRobinhoodTradeService robinhood(&rh.network, rh.sessions.get(), rh.dependencies());
        QSignalSpy publicsCompleted(&publics, &TelegramPublicsTradeService::completed);
        QSignalSpy robinhoodCompleted(&robinhood, &TelegramRobinhoodTradeService::completed);
        ControllerBotServer telegram;
        ControllerBotServer otherTelegram;
        telegram.loseFirstMessageAcknowledgement = loseAck;
        BridgeBotStore store;
        int runtimeCount = 0;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            auto *agent = new TelegramAgent(++runtimeCount == 1 ? telegram.root() : otherTelegram.root(), owner);
            agent->setCommandLedgerPathForTests(directory.filePath(QStringLiteral("ledger")));
            return agent;
        });
        TelegramBotsApplet applet(&controller, &publics, &robinhood);
        applet.start();
        QVERIFY(controller.startBotWithPassword(QStringLiteral("one"), QStringLiteral("fixture password 123")));
        QTRY_COMPARE(telegram.polls.size(), 1);
        QVERIFY(controller.startBotWithPassword(QStringLiteral("two"), QStringLiteral("fixture password 123")));
        QTRY_COMPARE(otherTelegram.polls.size(), 1);
        int messagesAtFirstRhOrder = -1;
        bool intermediateSettled = false;
        bool settledAtFirstRhOrder = false;
        connect(&controller, &TelegramBotsController::tradeReportFinished, &controller,
            [&](const QString &, const QString &) { intermediateSettled = true; });
        rh.onOrder = [&] {
            if (messagesAtFirstRhOrder < 0) {
                messagesAtFirstRhOrder = int(telegram.messages.size());
                settledAtFirstRhOrder = intermediateSettled;
            }
        };
        const QJsonObject update{{"update_id", 1000},
            {"message", QJsonObject{{"message_id", 1000}, {"date", 1700000000}, {"text", command},
                {"from", QJsonObject{{"id", 42}, {"is_bot", false}}},
                {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}};
        ControllerBotServer::answer(telegram.polls.takeFirst(), QJsonArray{update});
        controller.selectBot(QStringLiteral("two"));
        if (combined && !sell) {
            QTRY_COMPARE(publicsBroker.orders.size(), 1);
            QVERIFY(rh.orders.isEmpty());
            QVERIFY(telegram.messages.isEmpty());
            QVERIFY(controller.busy());
            QVERIFY(!controller.stopAll());
            publicsBroker.release();
        }
        for (int i = 0; i < 6; ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(rh.timers.size(), 1, 5000);
            QCOMPARE(rh.delays.last(), 2500);
            QCOMPARE(robinhoodCompleted.size(), 0);
            QVERIFY(controller.busy());
            QVERIFY(!applet.canStop());
            QTRY_COMPARE(telegram.messages.size(), int(combined) + int(expired));
            if (combined)
                QVERIFY(telegram.messages.first().value("text").toString().contains("Publics Auto"));
            if (expired) {
                const auto text = telegram.messages.at(int(combined)).value("text").toString();
                QVERIFY(text.contains("Robinhood Reauthentication Required"));
                QVERIFY(text.contains("expired@example.test"));
                QVERIFY(!text.contains("first@example.test"));
                QVERIFY(!text.contains("second@example.test"));
            }
            auto cooldown = rh.timers.takeFirst();
            cooldown();
        }
        QTRY_COMPARE(robinhoodCompleted.size(), 1);
        QTRY_COMPARE(telegram.messages.size(), loseAck ? 1 : int(combined) + int(expired) + 1);
        QCOMPARE(publicsCompleted.size(), combined ? 1 : 0);
        if (combined) QVERIFY(settledAtFirstRhOrder);
        // Warnings are informational, not another continuation/settlement gate.
        QVERIFY(messagesAtFirstRhOrder >= int(combined));
        QCOMPARE(rh.orders.size(), sell ? 4 : 6);
        QCOMPARE(rh.delays.size(), 6);
        QCOMPARE(rh.renewals.size(), 2 + int(expired)); // Initial sign-ins only.
        QCOMPARE(rh.requests.count("GET /positions/"), sell ? 6 : 0);
        for (qsizetype i = 0; i < rh.orderHeaders.size(); ++i) {
            const auto &headers = rh.orderHeaders[i];
            const bool firstProfile = i < (sell ? 2 : 3);
            QVERIFY(headers.contains(firstProfile ? "Bearer native-fixture-first" : "Bearer native-fixture-second"));
            QVERIFY(!headers.contains(firstProfile ? "Bearer native-fixture-second" : "Bearer native-fixture-first"));
        }
        for (qsizetype i = 0; i < rh.orders.size(); ++i) {
            const auto &order = rh.orders[i];
            QCOMPARE(order.value("side").toString(), sell ? QStringLiteral("sell") : QStringLiteral("buy"));
            QCOMPARE(order.value("instrument").toString(), rh.brokerageOrigin + "/instruments/AAPL/");
            const auto account = i % (sell ? 2 : 3) + 1;
            QCOMPARE(order.value("account").toString(), rh.brokerageOrigin + QString("/accounts/a%1/").arg(account));
            QCOMPARE(order.value("quantity").toString(), sell ? (account == 1 ? QStringLiteral("5.5") : QStringLiteral("2")) : QStringLiteral("1"));
        }
        QCOMPARE(publicsBroker.orders.size(), combined && !sell ? 2 : 0);
        QCOMPARE(publicsBroker.portfolioReads, combined ? 2 : 0);
        const auto report = telegram.messages.last();
        const auto rhText = loseAck ? robinhoodCompleted.first().at(1).toStringList().join('\n')
                                   : report.value("text").toString();
        QVERIFY(rhText.contains(sell ? "Robinhood Auto Sell Results" : "Robinhood Auto Buy Results"));
        QVERIFY(rhText.contains("🟢 Sent Trades"));
        QVERIFY(!rhText.contains("🟢 Filled"));
        QVERIFY(rhText.contains("first@example.test"));
        QVERIFY(rhText.contains("second@example.test"));
        QVERIFY(!rhText.contains("native-fixture"));
        QCOMPARE(report.value("chat_id").toInteger(), qint64(42));
        QCOMPARE(controller.selectedId(), QStringLiteral("two"));
        QVERIFY(otherTelegram.messages.isEmpty());
        QVERIFY(!controller.busy());
        if (!loseAck) {
            QTRY_COMPARE(telegram.polls.size(), 1);
            ControllerBotServer::answer(telegram.polls.takeFirst(), QJsonArray{update});
            QTRY_COMPARE(telegram.polls.size(), 1);
        } else {
            // Uncertain delivery stops intake; it must not replay either broker.
            QTest::qWait(100);
            QVERIFY(rh.timers.isEmpty());
        }
        QCOMPARE(rh.orders.size(), sell ? 4 : 6);
        QCOMPARE(publicsBroker.orders.size(), combined && !sell ? 2 : 0);
        QCOMPARE(robinhoodCompleted.size(), 1);
        QCOMPARE(rh.renewals.size(), 2 + int(expired));
        QVERIFY(controller.stopAll());
    }

    void authorizedCommandUsesRealAutoModeAndReturnsToOriginAfterWholeBatch()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        BridgeBroker broker;
        const auto group = QStringLiteral("Bridge_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        PublicAuthClient auth(QString(), group, group, broker.root(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) {
                done(QStringLiteral("synthetic-broker-key"), {});
            });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0")});
        QVERIFY(auth.setApiKeyNickname(QStringLiteral("PublicsApiKey0"), QStringLiteral("Desk & Main")));
        PublicApiWorker worker(broker.root());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.accountPauseMs = policy.symbolPauseMs = 0;
        policy.gatewayCooldownMs = policy.retryBaseMs = 0;
        worker.setRequestPolicy(policy);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        connect(&auth, &PublicAuthClient::selectedApiKeyLabelChanged, &worker, [&] {
            if (auth.selectedApiKeyLabel().isEmpty()) worker.clearSubAccountsList();
        });
        AutoTradeController trader(&auth, &worker);
        TelegramPublicsTradeService service(&trader, &auth);
        RHNetworkEngine robinhoodNetwork;
        BridgeRobinhoodStore robinhoodStore;
        SecureCredentialLocker locker(directory.filePath(QStringLiteral("rh-locker.json")),
            [](bool, SecureCredentialLocker::KeyCompletion done) {
                done(false, {}, QStringLiteral("No fixture key"));
            });
        RHSessionManager sessions(robinhoodNetwork, directory.filePath(QStringLiteral("rh-sessions.json")),
            &robinhoodStore, nullptr, 180000, 2500, &locker);
        TelegramRobinhoodTradeService robinhood(&robinhoodNetwork, &sessions, {});
        QSignalSpy robinhoodBusy(&robinhood, &TelegramRobinhoodTradeService::busyChanged);
        ControllerBotServer telegram;
        ControllerBotServer otherTelegram;
        ControllerBotServer health;
        BridgeBotStore store;
        int runtimeCount = 0;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            auto *agent = new TelegramAgent(++runtimeCount == 1 ? telegram.root() : otherTelegram.root(),
                health.healthUrl(), owner);
            agent->setCommandLedgerPathForTests(directory.filePath(QStringLiteral("admissions.json")));
            return agent;
        });
        TelegramBotsApplet applet(&controller, &service, &robinhood);
        applet.start();
        QTRY_COMPARE(controller.bots().size(), 2);
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
        QTRY_COMPARE(telegram.polls.size(), 1);
        QVERIFY(controller.startBotWithPassword(QStringLiteral("two"), "test password 123"));
        QTRY_COMPARE(otherTelegram.polls.size(), 1);
        const auto poll = telegram.polls.takeFirst();
        QVERIFY(poll);
        ControllerBotServer::answer(poll, QJsonArray{QJsonObject{{"update_id", 1000},
            {"message", QJsonObject{{"message_id", 1000}, {"date", 1700000000},
                {"text", "publics_auto_buy AAPL"},
                {"from", QJsonObject{{"id", 42}, {"is_bot", false}, {"first_name", "Owner"}}},
                {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}}});
        QTRY_COMPARE_WITH_TIMEOUT(broker.orders.size(), 1, 5000);
        QVERIFY(service.busy());
        QVERIFY(!applet.canStop());
        QVERIFY(!controller.stopAll());
        QVERIFY(telegram.messages.isEmpty()); // No partial batch notification.
        controller.selectBot(QStringLiteral("two"));
        QCOMPARE(controller.selectedId(), QStringLiteral("two"));
        const auto otherPoll = otherTelegram.polls.takeFirst();
        QVERIFY(otherPoll);
        ControllerBotServer::answer(otherPoll, QJsonArray{QJsonObject{{"update_id", 1000},
            {"message", QJsonObject{{"message_id", 1000}, {"date", 1700000000},
                {"text", "robinhood_auto_buy AAPL"},
                {"from", QJsonObject{{"id", 42}, {"is_bot", false}}},
                {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}}});
        QTRY_COMPARE(otherTelegram.messages.size(), 1);
        QVERIFY(otherTelegram.messages.first().value("text").toString().contains("Another trading batch is running"));
        QCOMPARE(robinhoodBusy.size(), 0);
        QCOMPARE(robinhoodStore.reads, 0);
        QVERIFY(controller.busy());
        QVERIFY(telegram.messages.isEmpty());
        broker.release();
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(telegram.messages.size(), 1, 5000);
        QCOMPARE(broker.orders.size(), 2);
        for (const auto &order : broker.orders) QCOMPARE(order.value("quantity").toString(), QStringLiteral("1"));
        const auto report = telegram.messages.first();
        QCOMPARE(report.value("chat_id").toInteger(), qint64(42));
        QCOMPARE(report.value("parse_mode").toString(), QStringLiteral("HTML"));
        QVERIFY(report.value("text").toString().contains(QStringLiteral("<b>Publics Auto Buy Results</b>")));
        QVERIFY(report.value("text").toString().contains(QStringLiteral("Desk &amp; Main")));
        QCOMPARE(controller.selectedId(), QStringLiteral("two"));
        QTRY_COMPARE(otherTelegram.polls.size(), 1);
        const auto authPoll = otherTelegram.polls.takeFirst();
        QVERIFY(authPoll);
        ControllerBotServer::answer(authPoll, QJsonArray{QJsonObject{{"update_id", 1001},
            {"message", QJsonObject{{"message_id", 1001}, {"date", 1700000000},
                {"text", "robinhood_auto_sell AAPL"},
                {"from", QJsonObject{{"id", 42}, {"is_bot", false}}},
                {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}}});
        QTRY_COMPARE(otherTelegram.messages.size(), 2);
        QCOMPARE(otherTelegram.messages.last().value("text").toString(),
            QStringLiteral("<b>Robinhood Auto Sell Results</b>\n🔴 No initially unexpired saved Robinhood sessions; no orders were sent."));
        QCOMPARE(telegram.messages.size(), 1);
        QCOMPARE(broker.orders.size(), 2);
        QVERIFY(!robinhood.busy());
        QVERIFY(!controller.busy());
        QVERIFY(controller.stopAll());
        QVERIFY(applet.canStop());
    }
};
QTEST_GUILESS_MAIN(TelegramTradeBridgeTest)
#include "test_telegramtradebridge.moc"
