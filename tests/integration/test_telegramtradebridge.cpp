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
#include "auth/publicauthclient.h"
#include "api/publicapiworker.h"
#include "trading/autotradecontroller.h"
#include "../unit/telegramcontrollertestserver.h"

namespace {
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
        ControllerBotServer telegram;
        ControllerBotServer health;
        BridgeBotStore store;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            auto *agent = new TelegramAgent(telegram.root(), health.healthUrl(), owner);
            agent->setCommandLedgerPathForTests(directory.filePath(QStringLiteral("admissions.json")));
            return agent;
        });
        TelegramBotsApplet applet(&controller, &service);
        applet.start();
        QTRY_COMPARE(controller.bots().size(), 2);
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
        QTRY_COMPARE(telegram.polls.size(), 1);
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
        QVERIFY(controller.stopAll());
        QVERIFY(applet.canStop());
    }
};
QTEST_GUILESS_MAIN(TelegramTradeBridgeTest)
#include "test_telegramtradebridge.moc"
