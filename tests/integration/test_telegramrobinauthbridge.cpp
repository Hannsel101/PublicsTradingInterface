#include "telegramrobinauthbridgefixture.h"
#include "../unit/telegramcontrollertestserver.h"
#include "telegrambotsapplet.h"
#include "telegrambotscontroller.h"
#include "telegramrobinhoodauthservice.h"
#include <utility>

namespace {
// Only external storage and transports are substituted. Authentication, admission,
// progress settlement, identity checks and persistence all run production code.
class MemoryBotStore final : public TelegramBotStore {
public:
    MemoryBotStore() : TelegramBotStore(QString{}, BinaryBackend{}) {}
    void load(LoadCompletion done) override {
        done(true, {{"one", "Origin bot", {"42"}, true, true},
                    {"two", "Selected bot", {"42"}, true, true}}, {});
    }
    void readActivationSnapshot(QString id, QString, ActivationCompletion done) override {
        done(true, {id == "one" ? "123456:synthetic" : "987654:synthetic",
                    {"42"}, QString(64, 'a')}, {});
    }
    void readToken(QString id, TokenCompletion done) override {
        done(true, id == "one" ? "123456:synthetic" : "987654:synthetic", {});
    }
};

struct Bridge {
    TelegramRobinAuthBridgeFixture::Fixture rh;
    ControllerBotServer telegram;
    ControllerBotServer competingTelegram;
    int agentsCreated = 0;
    MemoryBotStore bots;
    TelegramRobinhoodAuthService auth;
    TelegramBotsController controller;
    TelegramBotsApplet applet;
    explicit Bridge(int totalBudgetMs = 4000)
        : auth(rh.manager.get(), nullptr, totalBudgetMs),
          controller(&bots, [this](QObject *owner) {
              auto *agent = new TelegramAgent(agentsCreated++ == 0 ? telegram.root() : competingTelegram.root(), owner);
              agent->setCommandLedgerPathForTests(rh.dir.filePath("telegram-ledger"));
              return agent;
          }), applet(&controller, nullptr, nullptr, &auth, nullptr) {}
    void seed(int seconds = 1800) {
        QVERIFY(rh.manager);
        rh.http.responses = {rh.grant(seconds), rh.profile(), rh.accounts()};
        QVERIFY(rh.manager->beginNativeLogin());
        QVERIFY(rh.manager->signIn("owner@example.test", "fixture-password"));
        QTRY_VERIFY(!rh.manager->busy());
        QCOMPARE(rh.manager->rowCount(), 1);
        QCOMPARE(rh.store.writes, 1);
        QCOMPARE(rh.http.requests.size(), 3);
    }
    void start() {
        applet.start();
        QTRY_COMPARE(controller.bots().size(), 2);
        QVERIFY(controller.startBotWithPassword("one", "fixture password 123"));
        QTRY_COMPARE(telegram.polls.size(), 1);
    }
    void send(qint64 id, const QString &text = "/authenticate_robinhood_account owner@example.test",
              qint64 actor = 42, qint64 chat = 42, const QString &kind = "private", bool bot = false) {
        sendVia(telegram, id, text, actor, chat, kind, bot);
    }
    void sendVia(ControllerBotServer &server, qint64 id, const QString &text,
                 qint64 actor = 42, qint64 chat = 42, const QString &kind = "private", bool bot = false) {
        QVERIFY(!server.polls.isEmpty());
        ControllerBotServer::answer(server.polls.takeFirst(), QJsonArray{QJsonObject{
            {"update_id", id}, {"message", QJsonObject{{"message_id", id}, {"date", 1700000000},
                {"text", text}, {"from", QJsonObject{{"id", actor}, {"is_bot", bot}}},
                {"chat", QJsonObject{{"id", chat}, {"type", kind}}}}}}});
    }
    QStringList texts() const {
        QStringList result;
        for (const auto &message : telegram.messages) result.append(message["text"].toString());
        return result;
    }
    void queuePush() {
        rh.http.responses.append(R"({"verification_workflow":{"id":"workflow"}})");
        rh.http.responses.append(R"({"id":"machine"})");
        rh.http.responses.append(R"({"context":{"sheriff_challenge":{"type":"prompt","status":"issued","id":"challenge"}}})");
        rh.http.statuses = {200, 200, 200, 400, 200, 200};
    }
    void approve(bool wrongIdentity = false) {
        // The seventh request is held at the push challenge. These responses are
        // independent synthetic provider shapes, never request echoes.
        rh.http.responses.append(R"({"challenge_status":"validated"})");
        rh.http.responses.append(R"({"type_context":{"result":"workflow_status_approved"}})");
        rh.http.responses.append(rh.grant());
        rh.http.responses.append(wrongIdentity
            ? QByteArray(R"({"id":"different-owner","email":"owner@example.test"})") : rh.profile());
        rh.http.responses.append(rh.accounts());
        if (rh.http.peer) TelegramRobinAuthBridgeFixture::Server::respond(rh.http.peer, rh.http.responses[6]);
    }
    void verifyOrigin() const {
        QCOMPARE(telegram.messageBotIds.size(), telegram.messages.size());
        for (const auto id : telegram.messageBotIds) QCOMPARE(id, 123456);
        for (const auto &message : telegram.messages) QCOMPARE(message["chat_id"].toVariant().toString(), "42");
    }
};
const QString progress = "Authentication for account: owner@example.test in progress...";
const QString success = "Authentication Successful for owner@example.test";
const QString timeout = "Authentication Failed for owner@example.test, reason: 8 minute timeout reached";
}

class TelegramRobinAuthBridgeTest final : public QObject {
    Q_OBJECT
private slots:
    void realPushSuccessPinsOriginAndSettlesProgressBeforeLogin() {
        Bridge f; f.seed(); f.start(); f.queuePush();
        QSignalSpy completed(&f.auth, &TelegramRobinhoodAuthService::completed);
        bool progressSettled = false, settledBeforeLogin = false;
        connect(&f.controller, &TelegramBotsController::tradeReportFinished, &f.controller,
            [&](QString, QString) { progressSettled = true; });
        f.rh.http.onRequest = [&] {
            if (f.rh.http.requests.size() == 4) settledBeforeLogin = progressSettled;
        };
        f.send(1000);
        QTRY_COMPARE(f.rh.http.requests.size(), 7);
        QCOMPARE(f.texts(), QStringList{progress});
        QVERIFY(settledBeforeLogin);
        QVERIFY(f.auth.busy()); QVERIFY(f.controller.busy()); QVERIFY(!f.applet.canStop());
        QVERIFY(!f.controller.canStart()); QVERIFY(!f.controller.canStop());
        f.controller.selectBot("two");
        f.approve();
        QTRY_COMPARE(f.texts(), QStringList({progress, success}));
        QTRY_VERIFY(!f.auth.busy()); QTRY_VERIFY(!f.controller.busy());
        QCOMPARE(completed.size(), 1); QCOMPARE(f.rh.store.writes, 2);
        QCOMPARE(f.rh.manager->rowCount(), 1);
        QCOMPARE(f.rh.http.requests.size(), 11);
        QVERIFY(f.rh.http.requests[3].contains("password=fixture-password"));
        QVERIFY(f.rh.http.requests[9].startsWith("GET /user/"));
        QVERIFY(f.rh.http.requests[10].startsWith("GET /accounts/"));
        QFile metadata(f.rh.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto bytes = metadata.readAll();
        QVERIFY(!bytes.contains("fixture-password")); QVERIFY(!bytes.contains("owner@example.test"));
        const auto row = QJsonDocument::fromJson(bytes).object()["sessions"].toArray()[0].toObject();
        QCOMPARE(row["credential_digest"].toString(), QString::fromLatin1(QCryptographicHash::hash(
            f.rh.store.values[0].toUtf8(), QCryptographicHash::Sha256).toHex()));
        f.verifyOrigin();
        QTRY_COMPARE(f.telegram.polls.size(), 1); f.send(1000);
        QTRY_COMPARE(f.telegram.polls.size(), 1); QTest::qWait(60);
        QCOMPARE(f.rh.http.requests.size(), 11); QCOMPARE(f.rh.store.writes, 2);
        QCOMPARE(completed.size(), 1); QCOMPARE(f.texts(), QStringList({progress, success}));
    }

    void totalDeadlineDuringPushRejectsLateApprovalAndReplay_data() {
        QTest::addColumn<bool>("loseProgressAck");
        QTest::newRow("acknowledged-progress") << false;
        QTest::newRow("lost-progress-ack") << true;
    }
    void totalDeadlineDuringPushRejectsLateApprovalAndReplay() {
        QFETCH(bool, loseProgressAck);
        Bridge f(2000); f.seed(); f.telegram.loseFirstMessageAcknowledgement = loseProgressAck;
        f.start(); f.queuePush();
        const auto original = f.rh.store.values;
        QSignalSpy completed(&f.auth, &TelegramRobinhoodAuthService::completed);
        f.send(2000); QTRY_COMPARE(f.rh.http.requests.size(), 7);
        QTRY_COMPARE_WITH_TIMEOUT(f.texts(), QStringList({progress, timeout}), 4000);
        QCOMPARE(completed.size(), 1);
        QTRY_VERIFY(!f.auth.busy()); QTRY_VERIFY(!f.controller.busy());
        f.controller.selectBot("two"); f.approve();
        QTRY_COMPARE(f.telegram.polls.size(), 1); f.send(2000);
        QTRY_COMPARE(f.telegram.polls.size(), 1); QTest::qWait(80);
        QCOMPARE(f.rh.http.requests.size(), 7); QCOMPARE(f.rh.store.values, original);
        QCOMPARE(f.rh.store.writes, 1); QCOMPARE(completed.size(), 1);
        QCOMPARE(f.texts(), QStringList({progress, timeout})); f.verifyOrigin();
    }

    void lookupDeadlineRetainsCleanupReservationAndRejectsCompetingCommands() {
        Bridge f(2000); f.seed(); f.start();
        QVERIFY(f.controller.startBotWithPassword("two", "fixture password 123"));
        QTRY_COMPARE(f.competingTelegram.polls.size(), 1);
        f.rh.store.holdRead = true;
        const auto reads = f.rh.store.reads;
        QSignalSpy completed(&f.auth, &TelegramRobinhoodAuthService::completed);
        f.send(3000); QTRY_COMPARE(f.rh.store.pendingReads.size(), 1);
        QVERIFY(f.controller.busy()); QVERIFY(f.auth.busy()); QVERIFY(f.rh.manager->busy());
        QCOMPARE(f.rh.store.reads, reads + 1);
        // Different updates are not replays. Neither may start another lookup or
        // a broker operation while the authentication owns its cleanup lease.
        f.sendVia(f.competingTelegram, 3001, "/authenticate_robinhood_account other@example.test");
        QTRY_COMPARE(f.competingTelegram.messages.size(), 1);
        QCOMPARE(f.competingTelegram.messages[0]["text"].toString(),
            "Authentication Failed: Another trading or authentication operation is running.");
        QVERIFY(f.rh.manager->busy()); QVERIFY(f.texts().isEmpty()); QCOMPARE(completed.size(), 0);
        QTRY_COMPARE(f.competingTelegram.polls.size(), 1);
        f.sendVia(f.competingTelegram, 3002, "/robinhood_auto_buy AAPL");
        QTRY_COMPARE(f.competingTelegram.messages.size(), 2);
        QCOMPARE(f.competingTelegram.messages[1]["text"].toString(),
            "🔴 Another trading batch is running. This command was not submitted.");
        QVERIFY(f.texts().isEmpty()); QCOMPARE(completed.size(), 0);
        QCOMPARE(f.rh.store.reads, reads + 1); QCOMPARE(f.rh.http.requests.size(), 3);
        QTRY_VERIFY_WITH_TIMEOUT(f.texts().contains(timeout), 2000);
        QCOMPARE(completed.size(), 1); QCOMPARE(f.texts().count(timeout), 1);
        QVERIFY(!f.texts().contains(progress));
        QVERIFY(f.auth.busy()); QVERIFY(f.controller.busy()); QVERIFY(f.rh.manager->busy());
        QVERIFY(!f.controller.canStart()); QVERIFY(!f.controller.canStop()); QVERIFY(!f.applet.canStop());
        RHSessionManager contender(f.rh.network, f.rh.dir.filePath("sessions.json"), &f.rh.store,
            nullptr, 2000, 1, &f.rh.locker);
        QVERIFY(!contender.saveAuthenticatedSession("other", "synthetic", 1800));
        const auto replies = f.texts();
        auto read = f.rh.store.pendingReads.takeFirst(); read();
        QTRY_VERIFY(!f.rh.manager->busy()); QTRY_VERIFY(!f.auth.busy()); QTRY_VERIFY(!f.controller.busy());
        QTest::qWait(60); QCOMPARE(f.texts(), replies); QCOMPARE(completed.size(), 1);
        QCOMPARE(f.rh.store.writes, 1); QCOMPARE(f.rh.http.requests.size(), 3); f.verifyOrigin();
    }

    void privateActorAndChatAuthorizationPrecedesEverySecureLookup_data() {
        QTest::addColumn<qint64>("actor"); QTest::addColumn<qint64>("chat");
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("bot");
        QTest::newRow("unlisted-human") << qint64(43) << qint64(43) << QString("private") << false;
        QTest::newRow("actor-mismatch") << qint64(43) << qint64(42) << QString("private") << false;
        QTest::newRow("destination-mismatch") << qint64(42) << qint64(43) << QString("private") << false;
        QTest::newRow("group") << qint64(42) << qint64(42) << QString("group") << false;
        QTest::newRow("bot-sender") << qint64(42) << qint64(42) << QString("private") << true;
    }
    void privateActorAndChatAuthorizationPrecedesEverySecureLookup() {
        QFETCH(qint64, actor); QFETCH(qint64, chat); QFETCH(QString, kind); QFETCH(bool, bot);
        Bridge f; f.seed(); f.start(); const auto reads = f.rh.store.reads;
        QSignalSpy requested(&f.controller, &TelegramBotsController::robinhoodAuthenticationRequested);
        f.send(4000, "/authenticate_robinhood_account owner@example.test", actor, chat, kind, bot);
        QTRY_COMPARE(f.telegram.polls.size(), 1); QTest::qWait(40);
        QCOMPARE(requested.size(), 0); QCOMPARE(f.rh.store.reads, reads);
        QCOMPARE(f.rh.store.writes, 1); QCOMPARE(f.rh.http.requests.size(), 3);
        QVERIFY(f.telegram.messages.isEmpty()); QVERIFY(!f.auth.busy()); QVERIFY(!f.controller.busy());
    }

    void unknownEmailDoesNotCallProviderOrWriteCredentials() {
        Bridge f; f.seed(); f.start();
        f.send(5000, "/authenticate_robinhood_account missing@example.test");
        QTRY_COMPARE(f.texts(), QStringList{"Account Session Not Stored in List of Sessions"});
        QCOMPARE(f.rh.http.requests.size(), 3); QCOMPARE(f.rh.store.writes, 1);
        QTRY_VERIFY(!f.controller.busy()); f.verifyOrigin();
    }

    void farValidTokenReportsActualStoredExpiryWithoutAuthentication() {
        Bridge f; f.seed(604800); f.start();
        const auto expiry = f.rh.manager->data(f.rh.manager->index(0), RHSessionManager::ExpiresAtRole)
            .toDateTime().toUTC().toString(Qt::ISODateWithMs);
        QVERIFY(!expiry.isEmpty()); QVERIFY(expiry.endsWith('Z'));
        const auto values = f.rh.store.values;
        QSignalSpy progressReports(&f.auth, &TelegramRobinhoodAuthService::reportReady);
        f.send(6000);
        QTRY_COMPARE(f.texts(), QStringList{QString("Authentication not performed for owner@example.test, reason: token expires at %1").arg(expiry)});
        QCOMPARE(progressReports.size(), 0); QCOMPARE(f.rh.http.requests.size(), 3);
        QCOMPARE(f.rh.store.writes, 1); QCOMPARE(f.rh.store.values, values);
        QTRY_VERIFY(!f.auth.busy()); QTRY_VERIFY(!f.controller.busy()); f.verifyOrigin();
    }

    void pushFailuresCannotPublishApprovalOrReplaceCredentials_data() {
        QTest::addColumn<QString>("failure");
        QTest::newRow("push-denied") << QString("denied");
        QTest::newRow("network-failed") << QString("network");
        QTest::newRow("wrong-verified-identity") << QString("identity");
    }
    void pushFailuresCannotPublishApprovalOrReplaceCredentials() {
        QFETCH(QString, failure); Bridge f; f.seed(); f.start(); f.queuePush();
        const auto values = f.rh.store.values;
        QSignalSpy completed(&f.auth, &TelegramRobinhoodAuthService::completed);
        f.send(7000); QTRY_COMPARE(f.rh.http.requests.size(), 7);
        if (failure == "identity") f.approve(true);
        else if (failure == "network") { QVERIFY(f.rh.http.peer); f.rh.http.peer->abort(); }
        else TelegramRobinAuthBridgeFixture::Server::respond(f.rh.http.peer, R"({"challenge_status":"denied"})");
        QTRY_COMPARE(completed.size(), 1); QTRY_COMPARE(f.texts().size(), 2);
        QVERIFY2(f.texts()[1].startsWith("Authentication Failed"), qPrintable(f.texts()[1]));
        QVERIFY(!f.texts().contains(success)); QCOMPARE(f.texts()[0], progress);
        QCOMPARE(f.rh.store.values, values); QCOMPARE(f.rh.store.writes, 1);
        QTRY_VERIFY(!f.auth.busy()); QTRY_VERIFY(!f.controller.busy());
        for (const auto &text : f.texts()) {
            QVERIFY(!text.contains("fixture-password")); QVERIFY(!text.contains("native-access"));
        }
        QTest::qWait(60); QCOMPARE(completed.size(), 1); f.verifyOrigin();
    }
};
QTEST_GUILESS_MAIN(TelegramRobinAuthBridgeTest)
#include "test_telegramrobinauthbridge.moc"
