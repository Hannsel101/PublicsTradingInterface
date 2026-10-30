#include "telegrambotscontroller.h"
#include "telegrambotstore.h"
#include "telegrambotruntime.h"
#include "telegramcontrollertestserver.h"


#include <QtTest>

class MemoryBotStore final : public TelegramBotStore
{
public:
    MemoryBotStore() : TelegramBotStore(QString{}, BinaryBackend{}) {}
    Entries entries{{QStringLiteral("one"), QStringLiteral("Desk"), {"42"}, true, true},
                    {QStringLiteral("two"), QStringLiteral("Second"), {"42"}, true, true}};
    void load(LoadCompletion done) override { done(true, entries, {}); }
    AddCompletion pendingAdd;
    TokenCompletion pendingRead;
    bool holdRead = false;
    bool failRead = false;
    void add(QString, QString, AddCompletion done) override { pendingAdd = std::move(done); }
    QStringList savedPolicy;
    void saveWithAllowedChats(QString, QString, QStringList policy, AddCompletion done) override {
        savedPolicy = std::move(policy); pendingAdd = std::move(done);
    }
    void remove(QString, AddCompletion done) override { pendingAdd = std::move(done); }
    QString capturedPassword;
    void saveProtectedBot(QString, QString, QString, QStringList policy, QString password, QString confirmation, AddCompletion done) override {
        Q_ASSERT(password == confirmation); capturedPassword = password; savedPolicy = policy; pendingAdd = std::move(done);
    }
    void readActivationSnapshot(QString id, QString password, ActivationCompletion done) override {
        capturedPassword = password;
        QStringList policy;
        for (const auto &entry : entries) if (entry.id == id) policy = entry.allowedChatIds;
        readToken(std::move(id), [done = std::move(done), policy](bool ok, QString token, QString error) {
            done(ok, {std::move(token), policy, QString(64, QLatin1Char('a'))}, std::move(error));
        });
    }
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
    void inspectorConflictCancelUsesCapturedBotAndRetiresPendingStart() {
        ControllerBotServer server; MemoryBotStore store;
        QTcpServer occupied; QVERIFY(occupied.listen(QHostAddress::LocalHost));
        QTemporaryDir ownership;
        QVERIFY(ownership.isValid());
        TelegramAgent::WebhookOptions options;
        options.tunnel.ownershipDirectory = ownership.filePath(QStringLiteral("tunnel-ownership"));
        options.tunnel.inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/tunnels").arg(occupied.serverPort()));
        options.tunnel.executable = QString::fromUtf8(PUBLICS_WEBHOOK_TEST_EXECUTABLE);
        QPointer<TelegramAgent> runtime;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            runtime = new TelegramAgent(server.root(), server.healthUrl(), options, owner); return runtime.data();
        });
        controller.load(); controller.selectBot("one");
        QSignalSpy conflicts(&controller, &TelegramBotsController::inspectorPortConflict);
        QVERIFY(controller.startBotWithPassword("one", "test password 123"));
        QTRY_COMPARE_WITH_TIMEOUT(conflicts.size(), 1, 5000);
        QCOMPARE(conflicts[0][0].toString(), QString("one"));
        controller.selectBot("two");
        QVERIFY(!controller.resolveInspectorConflict("two", "cancel"));
        QVERIFY(!controller.resolveInspectorConflict("one", "invalid"));
        QVERIFY(!controller.resolveInspectorConflict("one", "Restart"));
        controller.setTradingBusy(true);
        QVERIFY(!controller.resolveInspectorConflict("one", "restart"));
        QVERIFY(controller.resolveInspectorConflict("one", "cancel"));
        controller.setTradingBusy(false);
        QVERIFY(!controller.resolveInspectorConflict("one", "cancel"));
        QTRY_VERIFY(!controller.hasRunningBots());
        QVERIFY(occupied.isListening()); QVERIFY(server.deletions.isEmpty());
        QVERIFY(runtime && !runtime->active() && !runtime->starting());
        QCOMPARE(server.requestedMethods, QStringList{"getMe"});
        QVERIFY(controller.startBotWithPassword("one", "test password 123"));
        QTRY_COMPARE_WITH_TIMEOUT(conflicts.size(), 2, 5000);
        QVERIFY(controller.resolveInspectorConflict("one", "cancel"));
        QTRY_VERIFY(!controller.hasRunningBots());
    }
    void inspectorRestartFailureCanPromptAgainWithoutMutatingUnrelatedListener() {
        ControllerBotServer server; MemoryBotStore store;
        QTcpServer occupied; QVERIFY(occupied.listen(QHostAddress::LocalHost));
        QTemporaryDir ownership;
        QVERIFY(ownership.isValid());
        TelegramAgent::WebhookOptions options;
        options.tunnel.ownershipDirectory = ownership.filePath(QStringLiteral("tunnel-ownership"));
        options.tunnel.inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/api/tunnels").arg(occupied.serverPort()));
        options.tunnel.executable = QString::fromUtf8(PUBLICS_WEBHOOK_TEST_EXECUTABLE);
        TelegramBotsController controller(&store, [&](QObject *owner) {
            return new TelegramAgent(server.root(), server.healthUrl(), options, owner);
        });
        controller.load();
        QSignalSpy conflicts(&controller, &TelegramBotsController::inspectorPortConflict);
        QVERIFY(controller.startBotWithPassword("one", "test password 123"));
        QTRY_COMPARE_WITH_TIMEOUT(conflicts.size(), 1, 5000);
        controller.selectBot("two");
        QVERIFY(controller.resolveInspectorConflict("one", "restart"));
        QVERIFY(!controller.resolveInspectorConflict("one", "restart"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasRunningBots(), 5000);
        QVERIFY(!controller.error().isEmpty()); QVERIFY(occupied.isListening());
        QVERIFY(server.deletions.isEmpty());
        QCOMPARE(server.requestedMethods, QStringList{"getMe"});
        QVERIFY(controller.startBotWithPassword("one", "test password 123"));
        QTRY_COMPARE_WITH_TIMEOUT(conflicts.size(), 2, 5000);
        QVERIFY(controller.resolveInspectorConflict("one", "cancel"));
        QTRY_VERIFY(!controller.hasRunningBots());
        QVERIFY(occupied.isListening());
    }
    void passwordAttemptUsesCapturedIdAndNeverPublishesSecrets() {
        MemoryBotStore store; store.holdRead = true;
        ControllerBotServer server; QTemporaryDir ledger;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            auto *runtime = new TelegramBotRuntime(server.root(), owner);
            runtime->setCommandLedgerPathForTests(ledger.path()); return runtime;
        });
        controller.load(); controller.selectBot("one");
        QSignalSpy requested(&controller, &TelegramBotsController::passwordRequested);
        QVERIFY(controller.startBot()); QCOMPARE(requested.size(), 1); QVERIFY(!store.pendingRead);
        QVERIFY(controller.startBotWithPassword("one", "  test password 123  "));
        QCOMPARE(store.capturedPassword, QString("  test password 123  "));
        controller.selectBot("two"); store.pendingRead(true, "123456:synthetic", {});
        QTRY_VERIFY(controller.bots().first().toMap().value("active").toBool());
        QVERIFY(!controller.bots().last().toMap().value("active").toBool());
        for (const auto &row : controller.bots()) {
            QVERIFY(!row.toMap().contains("token")); QVERIFY(!row.toMap().contains("sessionSecret"));
            QVERIFY(!row.toMap().contains("password")); QCOMPARE(row.toMap().value("allowedChatIds").toStringList(), QStringList{});
        }
        QVERIFY(controller.stopAll());
    }
    void protectedSettingsCannotBlindlyOverwriteLockedIds() {
        MemoryBotStore store; TelegramBotsController controller(&store); controller.load();
        QVERIFY(!controller.saveProtectedBot("one", "Desk", {}, {}, "test password 123", "test password 123"));
        QVERIFY(!store.pendingAdd);
        QSignalSpy ids(&controller, &TelegramBotsController::settingsUnlocked);
        QVERIFY(controller.unlockBotSettings("one", "test password 123"));
        QCOMPARE(ids.size(), 1); QCOMPARE(ids[0][0].toString(), QString("one")); QCOMPARE(ids[0][1].toStringList(), QStringList{"42"});
        QVERIFY(!controller.saveProtectedBot("two", "Second", {}, {}, "test password 123", "test password 123"));
        QVERIFY(controller.saveProtectedBot("one", "Desk", {}, {}, "test password 123", "test password 123"));
        store.pendingAdd(false, {}, "raw token must not leak");
        QVERIFY(!controller.error().contains("raw token"));
        QVERIFY(controller.saveProtectedBot("one", "Desk", {}, {}, "test password 123", "test password 123"));
        store.pendingAdd(true, "one", {});
        QVERIFY(controller.bots().first().toMap().value("passwordProtected").toBool());
        QVERIFY(!controller.bots().first().toMap().value("hasAllowedChatIds").toBool());
    }
    void closingEditorRetiresPendingUnlock() {
        MemoryBotStore store; store.holdRead = true;
        TelegramBotsController controller(&store); controller.load();
        QSignalSpy unlocked(&controller, &TelegramBotsController::settingsUnlocked);
        QVERIFY(controller.unlockBotSettings("one", "test password 123"));
        controller.lockBotSettings(); store.pendingRead(true, "123456:synthetic", {});
        QCOMPARE(unlocked.size(), 0); QVERIFY(!controller.busy());
        QVERIFY(!controller.saveProtectedBot("one", "Desk", {}, {}, "test password 123", "test password 123"));
    }
    void legacyStartRequiresExplicitPasswordSetup() {
        MemoryBotStore store; store.entries[0].passwordProtected = false;
        TelegramBotsController controller(&store); controller.load(); controller.selectBot("one");
        QSignalSpy setup(&controller, &TelegramBotsController::passwordSetupRequired);
        QVERIFY(!controller.startBot()); QCOMPARE(setup.size(), 1); QVERIFY(!store.pendingRead);
        QVERIFY(!controller.startBotWithPassword("one", "test password 123")); QVERIFY(!store.pendingRead);
        QCOMPARE(controller.bots().first().toMap().value("allowedChatIds").toStringList(), QStringList{"42"});
        QVERIFY(controller.saveProtectedBot("one", "Desk", {}, "42", "test password 123", "test password 123"));
        QCOMPARE(store.savedPolicy, QStringList{"42"});
    }
    void protectedPasswordValidationRetriesWithoutTrimming() {
        MemoryBotStore store; TelegramBotsController controller(&store); controller.load();
        QVERIFY(!controller.saveProtectedBot({}, "New", "123456:synthetic", {}, "test password 123", "another password"));
        QCOMPARE(controller.error(), QString("Passwords do not match.")); QVERIFY(!store.pendingAdd);
        QVERIFY(!controller.saveProtectedBot({}, "New", "123456:synthetic", {}, "short", "short")); QVERIFY(!store.pendingAdd);
        QVERIFY(controller.saveProtectedBot({}, "New", "123456:synthetic", {}, "  test password 123  ", "  test password 123  "));
        QCOMPARE(store.capturedPassword, QString("  test password 123  ")); QVERIFY(store.savedPolicy.isEmpty());
        store.pendingAdd(true, "new", {}); QVERIFY(controller.bots().last().toMap().value("passwordProtected").toBool());
    }
    void decryptedEmptyIdsNeverCreatesRuntime() {
        MemoryBotStore store; int created = 0;
        TelegramBotsController controller(&store, [&](QObject *owner) { ++created; return new TelegramBotRuntime(owner); });
        controller.load(); controller.selectBot("one"); store.entries[0].allowedChatIds.clear();
        QSignalSpy missing(&controller, &TelegramBotsController::missingChatIds);
        QVERIFY(controller.startBotWithPassword("one", "test password 123"));
        QCOMPARE(missing.size(), 1); QCOMPARE(created, 0); QVERIFY(!controller.busy());
    }
    void deletingCredentialGateIsReservedBeforeSignals() {
        MemoryBotStore store; TelegramBotsController controller(&store); controller.load(); controller.selectBot("one");
        bool attempted = false;
        connect(&controller, &TelegramBotsController::stateChanged, &controller, [&] {
            if (attempted || !controller.busy()) return;
            attempted = true; QVERIFY(!controller.deleteBot("two")); QVERIFY(!controller.startBotWithPassword("two", "test password 123"));
            QVERIFY(!controller.unlockBotSettings("two", "test password 123")); QVERIFY(!controller.stopAll());
        });
        QVERIFY(controller.deleteBot("one")); QVERIFY(attempted); QVERIFY(store.pendingAdd); QVERIFY(!store.pendingRead);
        store.pendingAdd(false, {}, "unsafe provider details"); QCOMPARE(controller.bots().size(), 2);
        QVERIFY(!controller.error().contains("unsafe"));
    }
    void missingChatIdsRequestsSettingsWithoutCredentials() {
        MemoryBotStore store; store.entries[0].allowedChatIds.clear(); store.entries[0].hasAllowedChatIds = false;
        TelegramBotsController controller(&store); controller.load(); controller.selectBot("one");
        QSignalSpy missing(&controller, &TelegramBotsController::missingChatIds);
        QVERIFY(controller.canStart()); QVERIFY(!controller.startBot());
        QCOMPARE(missing.size(), 1); QVERIFY(!store.pendingRead); QVERIFY(!controller.busy());
    }
    void deletionPreservesLedgerAndRuntimeUntilCommit() {
        QTemporaryDir directory; ControllerBotServer server; MemoryBotStore store;
        const auto path = directory.filePath("admission-ledger-sentinel");
        QFile sentinel(path); QVERIFY(sentinel.open(QIODevice::WriteOnly)); sentinel.write("durable admission"); sentinel.close();
        QPointer<TelegramBotRuntime> runtime;
        TelegramBotsController controller(&store, [&](QObject *owner) {
            runtime = new TelegramBotRuntime(server.root(), owner);
            runtime->setCommandLedgerPathForTests(directory.path()); return runtime.data();
        });
        controller.load(); controller.selectBot("one"); QVERIFY(controller.startBotWithPassword("one", "test password 123"));
        QVERIFY(!controller.canDelete()); QVERIFY(!controller.deleteBot("one"));
        QTRY_VERIFY(runtime && runtime->active()); QVERIFY(!controller.deleteBot("one"));
        QVERIFY(controller.stopAll()); QVERIFY(controller.canDelete());
        controller.setTradingBusy(true); QVERIFY(!controller.deleteBot("one")); controller.setTradingBusy(false);
        QVERIFY(controller.deleteBot("one")); QVERIFY(runtime);
        store.pendingAdd(false, {}, "failure"); QVERIFY(runtime); QCOMPARE(controller.bots().size(), 2);
        QVERIFY(controller.deleteBot("one")); store.pendingAdd(true, "one", {});
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!runtime);
        QVERIFY(sentinel.open(QIODevice::ReadOnly)); QCOMPARE(sentinel.readAll(), QByteArray("durable admission"));
    }
    void deletionWaitsForCommitAndRejectsReentry() {
        MemoryBotStore store; TelegramBotsController controller(&store); controller.load(); controller.selectBot("one");
        QVERIFY(controller.canDelete()); QVERIFY(controller.deleteBot("one"));
        QCOMPARE(controller.bots().size(), 2); QVERIFY(!controller.deleteBot("two"));
        store.pendingAdd(false, {}, "Failure"); QCOMPARE(controller.bots().size(), 2);
        QVERIFY(controller.deleteBot("one")); auto retired = store.pendingAdd;
        retired(true, "one", {}); QCOMPARE(controller.bots().size(), 1);
        retired(true, "one", {}); QCOMPARE(controller.bots().size(), 1);
        QVERIFY(controller.deleteBot("two")); auto replacement = store.pendingAdd;
        retired(true, "one", {}); QVERIFY(controller.busy()); QCOMPARE(controller.bots().size(), 1);
        replacement(false, {}, "failed"); QCOMPARE(controller.bots().size(), 1); QVERIFY(!controller.busy());
    }
    void activationUsesRevokedPolicyFromSharedEncryptedCatalog() {
        QTemporaryDir dir; ControllerBotServer server;
        QHash<QString, QByteArray> encrypted;
        int catalogReads = 0, keyReads = 0;
        auto backend = [&](bool write, QString service, QString key, QByteArray bytes,
                           TelegramBotStore::BinaryCompletion done) {
            const auto address = service + '/' + key;
            if (write) { encrypted[address] = bytes; done(TelegramBotStore::Status::Success, {}); }
            else {
                if (service.endsWith("TelegramBots")) ++catalogReads; else ++keyReads;
                done(encrypted.contains(address) ? TelegramBotStore::Status::Success : TelegramBotStore::Status::Missing,
                     encrypted.value(address));
            }
        };
        TelegramBotStore firstStore(dir.filePath("profiles.dat"), backend);
        TelegramBotStore secondStore(dir.filePath("profiles.dat"), backend);
        TelegramBotsController first(&firstStore, [&](QObject *owner) {
            auto *runtime = new TelegramBotRuntime(server.root(), owner);
            runtime->setCommandLedgerPathForTests(dir.filePath("ledger")); return runtime;
        });
        TelegramBotsController second(&secondStore);
        QVERIFY(second.saveProtectedBot({}, "Desk", "123456:synthetic", "42", "test password 123", "test password 123"));
        QTRY_VERIFY(!second.busy()); QVERIFY(second.error().isEmpty());
        first.load(); QTRY_VERIFY(!first.busy()); QCOMPARE(first.bots().size(), 1);
        QCOMPARE(first.bots().first().toMap().value("allowedChatIds").toStringList(), QStringList{});
        const auto id = first.bots().first().toMap().value("id").toString();
        QVERIFY(second.unlockBotSettings(id, "test password 123")); QTRY_VERIFY(!second.busy());
        QVERIFY(second.saveProtectedBot(id, "Desk", {}, {}, "test password 123", "test password 123"));
        QTRY_VERIFY(!second.busy()); QVERIFY(second.error().isEmpty());
        QCOMPARE(second.bots().first().toMap().value("allowedChatIds").toStringList(), QStringList{});
        QSignalSpy trades(&first, &TelegramBotsController::tradeRequested);
        first.selectBot(id); catalogReads = keyReads = 0; QVERIFY(first.startBotWithPassword(first.selectedId(), "test password 123"));
        QTRY_VERIFY(!first.busy());
        QCOMPARE(server.polls.size(), 0);
        QCOMPARE(trades.size(), 0);
        QVERIFY(first.stopAll());
    }
    void unsupportedPolicyRejectedBeforeCommitPreservesSupportedCommand() {
        QTemporaryDir dir; ControllerBotServer server;
        QHash<QString, QByteArray> encrypted;
        int writes = 0;
        auto backend = [&](bool write, QString service, QString key, QByteArray bytes,
                           TelegramBotStore::BinaryCompletion done) {
            const auto address = service + '/' + key;
            if (write) { ++writes; encrypted[address] = bytes; done(TelegramBotStore::Status::Success, {}); }
            else done(encrypted.contains(address) ? TelegramBotStore::Status::Success : TelegramBotStore::Status::Missing,
                      encrypted.value(address));
        };
        TelegramBotStore store(dir.filePath("profiles.dat"), backend);
        TelegramBotsController controller(&store, [&](QObject *owner) {
            auto *runtime = new TelegramBotRuntime(server.root(), owner);
            runtime->setCommandLedgerPathForTests(dir.filePath("ledger")); return runtime;
        });
        QVERIFY(controller.saveProtectedBot({}, "Desk", "123456:synthetic", "42", "test password 123", "test password 123"));
        QTRY_VERIFY(!controller.busy()); QVERIFY(controller.error().isEmpty());
        const auto committed = encrypted; const auto committedWrites = writes;
        const auto id = controller.bots().first().toMap().value("id").toString();
        QVERIFY(controller.unlockBotSettings(id, "test password 123")); QTRY_VERIFY(!controller.busy());
        QVERIFY(!controller.saveProtectedBot(id, "Desk", {}, "42,4503599627370496", "test password 123", "test password 123"));
        QVERIFY(controller.error().contains("4503599627370495"));
        QCOMPARE(encrypted, committed); QCOMPARE(writes, committedWrites);
        controller.load(); QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.bots().first().toMap().value("allowedChatIds").toStringList(), QStringList{});
        controller.selectBot(controller.bots().first().toMap().value("id").toString());
        QSignalSpy trades(&controller, &TelegramBotsController::tradeRequested);
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123")); QTRY_COMPARE(server.polls.size(), 1);
        const QJsonObject update{{"update_id", 10}, {"message", QJsonObject{
            {"message_id", 10}, {"date", 1700000000}, {"text", "publics_auto_buy AAPL"},
            {"from", QJsonObject{{"id", 42}, {"is_bot", false}, {"first_name", "Fixture"}}},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}};
        ControllerBotServer::answer(server.polls.takeFirst(), QJsonArray{update});
        QTRY_COMPARE(trades.size(), 1); QVERIFY(controller.stopAll());
    }
    void persistedPolicyAppliedBeforeActivationAndTradeRoutingUsesOrigin() {
        QTemporaryDir ledger; ControllerBotServer server; MemoryBotStore store;
        store.entries[0].allowedChatIds = {"42"};
        TelegramBotsController controller(&store, [&server, &ledger](QObject *owner) {
            auto *runtime = new TelegramBotRuntime(server.root(), owner);
            runtime->setCommandLedgerPathForTests(ledger.path()); return runtime;
        });
        QSignalSpy trades(&controller, &TelegramBotsController::tradeRequested);
        controller.load(); controller.selectBot("one"); QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
        QTRY_COMPARE(server.polls.size(), 1);
        controller.selectBot("two");
        const QJsonObject update{{"update_id", 10}, {"message", QJsonObject{
            {"message_id", 10}, {"date", 1700000000}, {"text", "publics_auto_buy AAPL,MSFT"},
            {"from", QJsonObject{{"id", 42}, {"is_bot", false}, {"first_name", "Fixture"}}},
            {"chat", QJsonObject{{"id", 42}, {"type", "private"}}}}}};
        ControllerBotServer::answer(server.polls.takeFirst(), QJsonArray{update});
        QTRY_COMPARE(trades.size(), 1);
        QCOMPARE(trades[0][0].toString(), QString("one"));
        QCOMPARE(trades[0][2].toString(), QString("buy"));
        QCOMPARE(trades[0][3].toStringList(), (QStringList{"AAPL", "MSFT"}));
        QVERIFY(!controller.saveBotWithAllowedChats("Desk", {}, "99"));
        controller.completeTradeCommand("missing", trades[0][1].toString(), {"Wrong origin"});
        controller.completeTradeCommand("one", trades[0][1].toString(), {"🟢 Fixture complete"});
        QTRY_COMPARE(server.messages.size(), 1);
        QCOMPARE(server.messages.first().value("chat_id").toVariant().toString(), QString("42"));
        QCOMPARE(server.messages.first().value("text").toString(), QString("🟢 Fixture complete"));
        QVERIFY(controller.stopAll());
    }
    void tradingBusyPreventsPendingCredentialReadFromStartingTransport() {
        MemoryBotStore store; store.holdRead = true;
        int runtimesCreated = 0;
        TelegramBotsController controller(&store, [&runtimesCreated](QObject *owner) {
            ++runtimesCreated; return new TelegramBotRuntime(owner);
        });
        controller.load(); controller.selectBot("one"); QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
        controller.setTradingBusy(true);
        store.pendingRead(true, "123456:synthetic", {});
        QCOMPARE(runtimesCreated, 0); QVERIFY(controller.busy()); QVERIFY(controller.hasRunningBots());
        controller.setTradingBusy(false); QVERIFY(!controller.busy()); QVERIFY(controller.canStart());
    }
    void policySavePublishesOnlyAfterCommitAndLegacySavePreservesIt() {
        MemoryBotStore store; store.entries[0].allowedChatIds = {"42"}; store.entries[0].passwordProtected = false;
        TelegramBotsController controller(&store); controller.load();
        QVERIFY(controller.saveBotWithAllowedChats("Desk", {}, "123,\n456\n123"));
        QCOMPARE(store.savedPolicy, (QStringList{"123", "456"}));
        QCOMPARE(controller.bots().first().toMap().value("allowedChatIds").toStringList(), QStringList{"42"});
        store.pendingAdd(false, {}, "Cannot save");
        QCOMPARE(controller.bots().first().toMap().value("allowedChatIds").toStringList(), QStringList{"42"});
        QVERIFY(controller.saveBotWithAllowedChats("Desk", {}, "123,456"));
        store.pendingAdd(true, "one", {});
        QCOMPARE(controller.bots().size(), 2);
        QCOMPARE(controller.bots().first().toMap().value("allowedChatIds").toStringList(), (QStringList{"123", "456"}));
        QVERIFY(controller.saveBot("Desk", "123:replacement"));
        store.pendingAdd(false, {}, "Already saved");
        QCOMPARE(controller.bots().first().toMap().value("allowedChatIds").toStringList(), (QStringList{"123", "456"}));
    }
    void invalidPolicyTextIsRejected_data() {
        QTest::addColumn<QString>("text");
        for (const auto &text : QStringList{"0", "-42", "+42", "01", "1.2", "1e3", "4503599627370496", "9223372036854775807", "42,4503599627370496", "9223372036854775808", "42,,43", "42,", ",42", "４２", "42 43"})
            QTest::newRow(qPrintable(text)) << text;
    }
    void invalidPolicyTextIsRejected() {
        QFETCH(QString, text); MemoryBotStore store; TelegramBotsController controller(&store); controller.load();
        QVERIFY(!controller.saveBotWithAllowedChats("Desk", {}, text));
        QVERIFY(!store.pendingAdd); QVERIFY(!controller.error().isEmpty()); QVERIFY(!controller.busy());
    }
    void tradingBusyHoldsNavigationAndRejectsStopAndPolicyChanges() {
        MemoryBotStore store; TelegramBotsController controller(&store); controller.load(); controller.selectBot("one");
        controller.setTradingBusy(true);
        QVERIFY(controller.busy()); QVERIFY(controller.hasRunningBots());
        QVERIFY(!controller.canStart()); QVERIFY(!controller.canStop());
        QVERIFY(!controller.startBotWithPassword(controller.selectedId(), "test password 123")); QVERIFY(!controller.stopBot()); QVERIFY(!controller.stopAll());
        QVERIFY(!controller.saveBotWithAllowedChats("Desk", {}, "42"));
        controller.setTradingBusy(false);
        QVERIFY(!controller.busy()); QVERIFY(!controller.hasRunningBots()); QVERIFY(controller.canStart());
    }
    void stoppingWebhookHoldsStartAndNavigationUntilCleanupFinishes()
    {
        ControllerBotServer server;
        server.holdWebhookDeletion = true;
        QTcpServer receiverPort, inspectorPort;
        QVERIFY(receiverPort.listen(QHostAddress::LocalHost));
        QVERIFY(inspectorPort.listen(QHostAddress::LocalHost));
        QTemporaryDir ownership;
        QVERIFY(ownership.isValid());
        TelegramAgent::WebhookOptions options;
        options.tunnel.ownershipDirectory = ownership.filePath(QStringLiteral("tunnel-ownership"));
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
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
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
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
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
        QCOMPARE(server.messages[1].value("text").toString(), QStringLiteral("🟢 Active"));
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
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
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
        QVERIFY(controller->startBotWithPassword(controller->selectedId(), "test password 123"));
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
            QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
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
        QVERIFY(controller.startBotWithPassword(controller.selectedId(), "test password 123"));
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
