#include "telegrambotscontroller.h"
#include "telegrambotstore.h"
#include "telegrambotruntime.h"
#include "telegramcontrollertestserver.h"


#include <QtTest>

class MemoryBotStore final : public TelegramBotStore
{
public:
    Entries entries{{QStringLiteral("one"), QStringLiteral("Desk")},
                    {QStringLiteral("two"), QStringLiteral("Second")}};
    void load(LoadCompletion done) override { done(true, entries, {}); }
    AddCompletion pendingAdd;
    TokenCompletion pendingRead;
    bool holdRead = false;
    bool failRead = false;
    void add(QString, QString, AddCompletion done) override { pendingAdd = std::move(done); }
    void readToken(QString id, TokenCompletion done) override {
        if (holdRead) { pendingRead = std::move(done); return; }
        if (failRead) { done(false, {}, QStringLiteral("Credentials unavailable")); return; }
        done(true, id == QStringLiteral("two") ? QStringLiteral("987654:synthetic") : QStringLiteral("123456:synthetic"), {});
    }
};

class TelegramBotsControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void stoppingWebhookHoldsStartAndNavigationUntilCleanupFinishes()
    {
        ControllerBotServer server;
        server.holdWebhookDeletion = true;
        QTcpServer receiverPort, inspectorPort;
        QVERIFY(receiverPort.listen(QHostAddress::LocalHost));
        QVERIFY(inspectorPort.listen(QHostAddress::LocalHost));
        TelegramAgent::WebhookOptions options;
        options.tunnel.port = receiverPort.serverPort();
        options.tunnel.inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/tunnels").arg(inspectorPort.serverPort()));
        options.tunnel.executable = QString::fromUtf8(PUBLICS_WEBHOOK_TEST_EXECUTABLE);
        options.tunnel.argumentPrefix = {QStringLiteral("--fake-ngrok"), QString::number(inspectorPort.serverPort()),
            QString::number(receiverPort.serverPort())};
        options.tunnel.discoveryDelayMs = 20;
        receiverPort.close(); inspectorPort.close();
        MemoryBotStore store;
        TelegramBotsController controller(&store, [&server, options](QObject *owner) {
            return new TelegramAgent(server.root(), server.healthUrl(), options, owner);
        });
        controller.load();
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBot());
        QTRY_VERIFY_WITH_TIMEOUT(controller.bots().first().toMap().value(QStringLiteral("active")).toBool(), 10000);
        QVERIFY(controller.stopBot());
        QTRY_COMPARE_WITH_TIMEOUT(server.deletions.size(), 1, 3000);
        QVERIFY(controller.hasRunningBots());
        QVERIFY(!controller.canStart());
        QVERIFY(!controller.canStop());
        QVERIFY(!controller.bots().first().toMap().value(QStringLiteral("active")).toBool());
        QVERIFY(!controller.stopAll());
        server.finishWebhookDeletion();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasRunningBots(), 5000);
        QVERIFY(controller.canStart());
        QVERIFY(!controller.canStop());
    }
    void healthFailureUpdatesOnlyItsBotAndRecoveryRestoresActive()
    {
        ControllerBotServer server;
        ControllerBotServer health;
        health.healthStatus = 503;
        MemoryBotStore store;
        TelegramBotsController controller(&store, [&server, &health](QObject *owner) {
            return new TelegramBotRuntime(server.root(), health.healthUrl(), owner);
        });
        controller.load();
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBot());
        QTRY_VERIFY(controller.bots().first().toMap().value(QStringLiteral("active")).toBool());
        QTRY_COMPARE(server.polls.size(), 1);
        controller.selectBot(QStringLiteral("two")); // Health result belongs to its originating bot.
        server.sendStatus(10);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.bots().first().toMap().value(QStringLiteral("active")).toBool(), 1500);
        QVERIFY(!controller.bots().last().toMap().value(QStringLiteral("active")).toBool());
        QTRY_COMPARE(server.messages.size(), 1);
        QCOMPARE(server.messages[0].value("text").toString(), QStringLiteral("Inactive"));
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.hasRunningBots()); // Unhealthy is not disconnected.
        QVERIFY(controller.canStop());
        QVERIFY(!controller.canStart()); // Never admit a competing poller.
        health.healthStatus = 200;
        QTRY_COMPARE(server.polls.size(), 1);
        server.sendStatus(11);
        QTRY_VERIFY_WITH_TIMEOUT(controller.bots().first().toMap().value(QStringLiteral("active")).toBool(), 1500);
        QTRY_COMPARE(server.messages.size(), 2);
        QCOMPARE(server.messages[1].value("text").toString(), QStringLiteral("Active"));
        QVERIFY(controller.stopAll());
        QVERIFY(!controller.hasRunningBots());
    }
    void credentialReadFailureNeverActivatesBot()
    {
        MemoryBotStore store;
        store.failRead = true;
        TelegramBotsController controller(&store);
        controller.load();
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBot());
        QVERIFY(!controller.busy());
        QVERIFY(!controller.hasRunningBots());
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(controller.canStart());
        QVERIFY(!controller.canStop());
    }
    void destroyingControllerCancelsPendingCredentialRead()
    {
        MemoryBotStore store;
        store.holdRead = true;
        int runtimesCreated = 0;
        auto controller = std::make_unique<TelegramBotsController>(&store, [&runtimesCreated](QObject *owner) {
            ++runtimesCreated;
            return new TelegramBotRuntime(owner);
        });
        controller->load();
        controller->selectBot(QStringLiteral("one"));
        QVERIFY(controller->startBot());
        QVERIFY(controller->busy());
        controller.reset();
        store.pendingRead(true, QStringLiteral("123456:synthetic"), {});
        QCOMPARE(runtimesCreated, 0);
    }
    void failedSaveDoesNotPublishBot()
    {
        MemoryBotStore store;
        TelegramBotsController controller(&store);
        controller.load();
        QSignalSpy saved(&controller, &TelegramBotsController::botSaved);
        QVERIFY(controller.saveBot(QStringLiteral("Third"), QStringLiteral("123456:synthetic")));
        store.pendingAdd(false, {}, QStringLiteral("Storage failed"));
        QVERIFY(!controller.busy());
        QCOMPARE(controller.bots().size(), 2);
        QCOMPARE(saved.size(), 0);
        QCOMPARE(controller.error(), QStringLiteral("Storage failed"));
    }

    void stopAllCancelsEveryRunningBot()
    {
        ControllerBotServer server;
        MemoryBotStore store;
        TelegramBotsController controller(&store, [&server](QObject *owner) {
            return new TelegramBotRuntime(server.root(), owner);
        });
        controller.load();
        for (const auto &id : {QStringLiteral("one"), QStringLiteral("two")}) {
            controller.selectBot(id);
            QVERIFY(controller.startBot());
        }
        QTRY_VERIFY_WITH_TIMEOUT(controller.bots().first().toMap().value(QStringLiteral("active")).toBool()
                                 && controller.bots().last().toMap().value(QStringLiteral("active")).toBool(), 1500);
        QVERIFY(controller.stopAll());
        QVERIFY(!controller.hasRunningBots());
        QVERIFY(controller.canStart());
        QVERIFY(!controller.canStop());
        for (const auto &value : controller.bots()) {
            QVERIFY(!value.toMap().value(QStringLiteral("active")).toBool());
            QVERIFY(!value.toMap().value(QStringLiteral("starting")).toBool());
        }
        QVERIFY(controller.saveBot(QStringLiteral("Third"), QStringLiteral("123456:synthetic")));
        QVERIFY(!controller.stopAll());
        store.pendingAdd(false, {}, QStringLiteral("Storage failed"));
        QVERIFY(controller.stopAll());
    }

    void selectedBotAuthenticatesBeforeActiveAndStops()
    {
        ControllerBotServer server;
        MemoryBotStore store;
        TelegramBotsController controller(&store, [&server](QObject *owner) {
            return new TelegramBotRuntime(server.root(), owner);
        });
        controller.load();
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.startBot());
        QVERIFY(!controller.canStart());
        QVERIFY(controller.canStop());
        QVERIFY(controller.hasRunningBots());
        QVERIFY(!controller.bots().first().toMap().value(QStringLiteral("active")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(controller.bots().first().toMap().value(QStringLiteral("active")).toBool(), 1500);
        QVERIFY(!controller.bots().last().toMap().value(QStringLiteral("active")).toBool());
        QVERIFY(controller.stopBot());
        QVERIFY(!controller.hasRunningBots());
        QVERIFY(controller.canStart());
        QVERIFY(!controller.canStop());
    }

    void savePublishesOnlyAfterSecureCommit()
    {
        MemoryBotStore store;
        TelegramBotsController controller(&store);
        controller.load();
        QSignalSpy saved(&controller, &TelegramBotsController::botSaved);
        QVERIFY(controller.saveBot(QStringLiteral("Third"), QStringLiteral("123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijk")));
        QVERIFY(controller.busy());
        QCOMPARE(controller.bots().size(), 2);
        QCOMPARE(saved.size(), 0);
        QVERIFY(!controller.saveBot(QStringLiteral("Fourth"), QStringLiteral("123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijk")));
        store.pendingAdd(true, QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), {});
        QVERIFY(!controller.busy());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(controller.bots().size(), 3);
        QCOMPARE(controller.bots().last().toMap().value(QStringLiteral("nickname")).toString(), QStringLiteral("Third"));
        QVERIFY(!controller.canStart());
        store.pendingAdd(true, QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), {});
        QCOMPARE(controller.bots().size(), 3);
        QCOMPARE(saved.size(), 1);
    }

    void loadingMetadataDoesNotStartOrSelectBots()
    {
        MemoryBotStore store;
        TelegramBotsController controller(&store);
        controller.load();
        QCOMPARE(controller.bots().size(), 2);
        QVERIFY(!controller.busy());
        QVERIFY(!controller.hasRunningBots());
        QVERIFY(!controller.canStart());
        QVERIFY(!controller.canStop());
        for (const auto &value : controller.bots()) {
            const auto row = value.toMap();
            QVERIFY(!row.contains(QStringLiteral("token")));
            QVERIFY(!row.value(QStringLiteral("active")).toBool());
        }
        controller.selectBot(QStringLiteral("one"));
        QVERIFY(controller.canStart());
        QVERIFY(!controller.canStop());
        controller.selectBot(QStringLiteral("unknown"));
        QVERIFY(!controller.canStart());
        QVERIFY(!controller.canStop());
    }
};

QTEST_GUILESS_MAIN(TelegramBotsControllerTest)
#include "test_telegrambotscontroller.moc"
