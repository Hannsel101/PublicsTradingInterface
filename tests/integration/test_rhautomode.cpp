#include "telegramrobinhoodfixture.h"
#include "rhautomodefixture.h"
#include "rhautotradecontroller.h"
using TelegramRobinhoodFixture::Fixture;

class RHAutoModeTest : public QObject {
    Q_OBJECT
    template<class F> static void drain(F &f, RHAutoTradeController &controller, int maximumMs = 30000) {
        QElapsedTimer elapsed; elapsed.start();
        while (controller.busy() && elapsed.elapsed() < maximumMs) {
            QCoreApplication::processEvents();
            if (!f.timers.isEmpty()) {
                QCOMPARE(f.delays.last(), 2500);
                QVERIFY(controller.busy()); QVERIFY(!controller.tradeResultsComplete());
                auto timer = f.timers.takeFirst(); timer();
            }
            QTest::qWait(1);
        }
        QVERIFY2(!controller.busy(), qPrintable(controller.error()));
    }
private slots:
    void sellUsesFreshFractionalAccountSpecificQuantities() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        QVERIFY(controller.submit("AAPL,MSFT", "sell")); drain(f, controller);
        QCOMPARE(f.orders.size(), 2); QCOMPARE(f.delays.size(), 6);
        QCOMPARE(f.orders[0].value("quantity").toString(), "5.5");
        QCOMPARE(f.orders[1].value("quantity").toString(), "2");
        for (const auto &order : f.orders) QCOMPARE(order.value("side").toString(), "sell");
        QCOMPARE(controller.tradeResults().size(), 6); QVERIFY(!f.sessions->busy());
        QCOMPARE(f.requests.count("GET /positions/"), 6); // Fresh sizing for every account/ticker row.
    }
    void nativeChunksNeverTruncateOrReorderLargeInput() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies(); int acquires = 0, releases = 0;
        const auto acquire = deps.acquire; const auto release = deps.release;
        deps.acquire = [&](auto done) { ++acquires; acquire(std::move(done)); };
        deps.release = [&] { ++releases; release(); };
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        QStringList symbols; for (int i = 0; i < 43; ++i) symbols.append(QString("T%1").arg(i));
        QVERIFY(controller.submit(QString(3000, ' ') + symbols.join(','), "buy")); drain(f, controller);
        QCOMPARE(f.orders.size(), symbols.size()); QCOMPARE(controller.tradeResults().size(), symbols.size());
        QCOMPARE(acquires, 1); QCOMPARE(releases, 1); QCOMPARE(f.delays.size(), symbols.size());
        for (int i = 0; i < symbols.size(); ++i)
            QCOMPARE(f.orders[i].value("instrument").toString(), f.brokerageOrigin + "/instruments/" + symbols[i] + "/");
    }
    void submissionEvidence_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"sent", "unconfirmed", "rejected", "network", "preview"}) QTest::newRow(mode) << QString(mode);
    }
    void submissionEvidence() {
        QFETCH(QString, mode); Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        f.malformedPost = mode == "unconfirmed"; f.dropPost = mode == "network";
        f.postStatus = mode == "rejected" ? 400 : 200;
        auto deps = f.dependencies(); if (mode == "preview") deps.loopbackLiveOrdersForTests = false;
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        QVERIFY(controller.submit("AAPL,MSFT", "buy")); drain(f, controller);
        const auto rows = controller.tradeResults(); QCOMPARE(rows.size(), 2);
        const auto first = rows.first().toMap(); const auto native = first.value("nativeRow").toMap();
        const bool preview = mode == "preview" && !f.network.stockLiveOrdersEnabled(false);
        QCOMPARE(first.value("profileId").toString(), "first");
        if (preview) {
            QCOMPARE(first.value("status").toString(), "preview"); QVERIFY(first.value("preview").toBool());
            QVERIFY(!first.value("success").toBool()); QCOMPARE(f.orders.size(), 0);
            QVERIFY(first.value("message").toString().contains("not sent"));
        } else if (mode == "unconfirmed") {
            QCOMPARE(first.value("status").toString(), "sent"); QVERIFY(first.value("success").toBool());
            QCOMPARE(native.value("status").toString(), "unknown"); QCOMPARE(f.orders.size(), 1);
            QVERIFY(first.value("message").toString().contains("confirmation unavailable"));
            QVERIFY(!first.value("message").toString().contains("filled"));
            QCOMPARE(rows.last().toMap().value("status").toString(), "failed");
        } else if (mode == "network") {
            QCOMPARE(first.value("status").toString(), "warning"); QVERIFY(!first.value("success").toBool());
            QVERIFY(first.value("message").toString().contains("no automatic retry")); QCOMPARE(f.orders.size(), 1);
        } else if (mode == "rejected") {
            QCOMPARE(first.value("status").toString(), "failed"); QVERIFY(!first.value("success").toBool());
            QCOMPARE(native.value("status").toString(), "failed"); QCOMPARE(f.orders.size(), 2);
        } else {
            QCOMPARE(first.value("status").toString(), "sent"); QVERIFY(first.value("success").toBool());
            QVERIFY(native.value("orderPostAttempted").toBool()); QVERIFY(native.value("orderPostTransportOk").toBool());
        }
        for (const auto &raw : rows) {
            const auto row = raw.toMap();
            QVERIFY(!row.value("message").toString().contains("PROVIDER-SECRET"));
            QVERIFY(!row.value("message").toString().contains("fixture-first"));
        }
    }
    void unavailableSavedProfileIsRecordedAndNextProfileContinues() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        for (const auto *name : {"missing", "valid"}) {
            QVERIFY(f.sessions->saveAuthenticatedSession(name, QString("fixture-") + name, 3600)); QTRY_VERIFY(!f.sessions->busy());
        }
        f.store.tokens.remove(0);
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        QVERIFY(controller.submit("AAPL,MSFT", "buy")); drain(f, controller);
        QCOMPARE(f.orders.size(), 2); QCOMPARE(controller.tradeResults().size(), 4);
        QCOMPARE(controller.tradeResults().first().toMap().value("sessionName").toString(), "missing");
        QVERIFY(!controller.tradeResults().first().toMap().value("success").toBool());
        QCOMPARE(controller.tradeResults().last().toMap().value("sessionName").toString(), "valid");
        QVERIFY(controller.tradeResults().last().toMap().value("success").toBool());
        QVERIFY(!controller.error().isEmpty());
    }
    void rejectedSessionContinuesToNextAdmittedLoginWithoutRetry() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        for (const auto *name : {"rejected", "valid"}) {
            QVERIFY(f.sessions->saveAuthenticatedSession(name, QString("fixture-") + name, 3600)); QTRY_VERIFY(!f.sessions->busy());
        }
        auto deps = f.dependencies(); const auto acquire = deps.acquire;
        deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto s, auto reason) mutable {
            f.failedReadPath = s.sessionIndex == 0 ? "/accounts/" : QString(); f.readStatus = 401;
            done(ok, std::move(s), reason);
        }); };
        const auto schedule = deps.schedule;
        deps.schedule = [&, schedule](int delay, auto callback) {
            schedule(delay, [&, callback = std::move(callback)] { f.failedReadPath.clear(); callback(); });
        };
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        QVERIFY(controller.submit("AAPL,MSFT", "buy")); drain(f, controller);
        QCOMPARE(f.orders.size(), 2); QCOMPARE(controller.tradeResults().size(), 4);
        QVERIFY(!controller.tradeResults().first().toMap().value("success").toBool());
        QVERIFY(controller.tradeResults().last().toMap().value("success").toBool());
        QVERIFY(!controller.error().isEmpty());
    }
    void admissionDoesNotReadCredentialsAndHonorsManualGate() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        const auto reads = f.store.reads;
        RHAutoTradeController controller(f.network, *f.sessions);
        QVERIFY(controller.available()); QCOMPARE(f.store.reads, reads);
        controller.setExternalBusy(true); QVERIFY(!controller.available()); QVERIFY(!controller.submit("AAPL", "buy"));
        QCOMPARE(f.store.reads, reads); controller.setExternalBusy(false); QVERIFY(controller.available());
        controller.clearResults(); QVERIFY(controller.tradeResults().isEmpty()); QVERIFY(!controller.tradeResultsComplete());
    }
    void missingDependencyFailsBeforeAnyCredentialRead_data() {
        QTest::addColumn<int>("missing");
        for (int i = 0; i < 5; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void missingDependencyFailsBeforeAnyCredentialRead() {
        QFETCH(int, missing); Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies();
        if (missing == 0) deps.acquire = {}; if (missing == 1) deps.current = {};
        if (missing == 2) deps.currentLocal = {}; if (missing == 3) deps.currentAsync = {};
        if (missing == 4) deps.release = {};
        const auto reads = f.store.reads;
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        QVERIFY(!controller.available()); QVERIFY(!controller.submit("AAPL", "buy"));
        QCOMPARE(f.store.reads, reads); QVERIFY(!controller.busy()); QVERIFY(f.orders.isEmpty());
    }
    void delayedAcquisitionCancellationAndDestructionReleaseExactlyOnce_data() {
        QTest::addColumn<bool>("destroy"); QTest::newRow("cancel") << false; QTest::newRow("destroy") << true;
    }
    void delayedAcquisitionCancellationAndDestructionReleaseExactlyOnce() {
        QFETCH(bool, destroy); Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies(); int releases = 0; std::function<void()> delivery;
        const auto acquire = deps.acquire; const auto release = deps.release;
        deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto s, auto reason) mutable {
            delivery = [ok, s = std::move(s), reason, done = std::move(done)]() mutable { done(ok, std::move(s), reason); };
        }); };
        deps.release = [&] { ++releases; release(); };
        auto controller = std::make_unique<RHAutoTradeController>(f.network, *f.sessions, deps);
        QVERIFY(controller->submit("AAPL", "buy")); QTRY_VERIFY(bool(delivery)); QVERIFY(f.sessions->busy());
        if (destroy) controller.reset(); else controller->cancel();
        QCOMPARE(releases, 1); QVERIFY(!f.sessions->busy());
        delivery(); QTest::qWait(20); QCOMPARE(releases, 1); QVERIFY(f.orders.isEmpty());
        if (controller) { QVERIFY(!controller->busy()); QVERIFY(controller->tradeResultsComplete()); }
    }
    void destroyedSessionManagerSettlesWithoutItsOutgoingSignals() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies(); std::function<void()> delivery; const auto acquire = deps.acquire;
        deps.acquire = [&, acquire](auto done) { acquire([&, done = std::move(done)](bool ok, auto s, auto reason) mutable {
            delivery = [ok, s = std::move(s), reason, done = std::move(done)]() mutable { done(ok, std::move(s), reason); };
        }); };
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        QVERIFY(controller.submit("AAPL", "buy")); QTRY_VERIFY(bool(delivery));
        f.sessions.reset(); QTRY_VERIFY(!controller.busy()); QVERIFY(controller.tradeResultsComplete());
        delivery(); QTest::qWait(20); QVERIFY(f.orders.isEmpty());
    }
    void completionObserverCanImmediatelyStartReplacement() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        bool replaced = false;
        connect(&controller, &RHAutoTradeController::stateChanged, this, [&] {
            if (!controller.busy() && controller.tradeResultsComplete() && !replaced) {
                replaced = true; QVERIFY(controller.submit("MSFT", "buy"));
            }
        });
        QVERIFY(controller.submit("AAPL", "buy")); drain(f, controller);
        QVERIFY(replaced); QCOMPARE(f.orders.size(), 2);
        QCOMPARE(controller.tradeResults().size(), 1);
        QCOMPARE(controller.tradeResults().first().toMap().value("symbol").toString(), "MSFT");
    }
    void credentialRevisionChangeHaltsAllRemainingWork() {
        Fixture f; QVERIFY(f.ready);
        for (const auto *name : {"first", "second"}) {
            QVERIFY(f.sessions->saveAuthenticatedSession(name, QString("fixture-") + name, 3600)); QTRY_VERIFY(!f.sessions->busy());
        }
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        const auto reads = f.store.readIndices.size();
        QVERIFY(controller.submit("AAPL,MSFT", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        auto doc = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
        auto profiles = doc.value("sessions").toArray(); auto first = profiles.first().toObject();
        first.insert("credential_revision", "999999"); profiles[0] = first; doc.insert("sessions", profiles);
        QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)); metadata.write(QJsonDocument(doc).toJson()); metadata.close();
        drain(f, controller); QCOMPARE(f.orders.size(), 1); QVERIFY(!f.sessions->busy());
        QVERIFY(!f.store.readIndices.mid(reads).contains(1));
        QCOMPARE(controller.tradeResults().size(), 8); // Six known rows + two unopened-profile ticker rows.
        QVERIFY(controller.tradeResults().first().toMap().value("success").toBool());
        QVERIFY(!controller.tradeResults().last().toMap().value("success").toBool());
    }
    void modelResetRevokesAdmittedWork() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        bool reset = false;
        connect(&controller, &RHAutoTradeController::stateChanged, this, [&] {
            if (controller.busy() && !reset) { reset = true; QVERIFY(f.sessions->load()); }
        });
        QVERIFY(controller.submit("AAPL,MSFT", "buy"));
        QVERIFY(reset); QVERIFY(!controller.busy()); QVERIFY(controller.tradeResultsComplete());
        QTest::qWait(20); QVERIFY(f.orders.isEmpty());
        QVERIFY(!controller.error().isEmpty());
    }
    void ownedSavedSessionReusesUnexpiredOAuthGrantBeforeLeaseAcquisition() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        const QJsonObject grant{{"access_token", "fixture-primary"}, {"refresh_token", "fixture-refresh"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 3600}};
        QVERIFY(f.sessions->acceptOAuthGrant(grant, "integration-client", "fixture-device"));
        QTRY_VERIFY(!f.sessions->busy()); QCOMPARE(f.sessions->rowCount(), 1);
        const auto priorSecret = f.store.tokens.value(0);
        const auto priorExpiry = f.sessions->data(f.sessions->index(0), RHSessionManager::ExpiresAtRole);
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        QSignalSpy prompts(f.sessions.get(), &RHSessionManager::reauthenticationRequested);
        QVERIFY(controller.submit("AAPL,MSFT", "buy")); drain(f, controller);
        QCOMPARE(f.renewals.size(), 0); QCOMPARE(f.orders.size(), 2); QCOMPARE(controller.tradeResults().size(), 2);
        QCOMPARE(f.store.tokens.value(0), priorSecret);
        QCOMPARE(f.sessions->data(f.sessions->index(0), RHSessionManager::ExpiresAtRole), priorExpiry);
        QCOMPARE(controller.tradeResults().first().toMap().value("profileId").toString(), "integration-owner");
        QVERIFY(controller.error().isEmpty()); QCOMPARE(prompts.size(), 0);
    }
    void sessionExpiryDuringCooldownRetiresOnlyThatLogin() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 2)); QTRY_VERIFY(!f.sessions->busy());
        QVERIFY(f.sessions->saveAuthenticatedSession("second", "fixture-second", 3600)); QTRY_VERIFY(!f.sessions->busy());
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        QSignalSpy prompts(f.sessions.get(), &RHSessionManager::reauthenticationRequested);
        QVERIFY(controller.submit("AAPL,MSFT", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(f.orders.size(), 1); QTest::qWait(2100); drain(f, controller);
        QCOMPARE(f.orders.size(), 3); QCOMPARE(controller.tradeResults().size(), 4);
        QCOMPARE(controller.tradeResults().last().toMap().value("sessionName").toString(), "second");
        QVERIFY(controller.tradeResults().last().toMap().value("success").toBool());
        QCOMPARE(prompts.size(), 0); QVERIFY(!f.requests.contains("POST /oauth2/token/"));
    }
    void reauthenticationRevokesAdmissionBeforeAnyOrder() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser);
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        bool requested = false;
        connect(&controller, &RHAutoTradeController::stateChanged, this, [&] {
            if (controller.busy() && !requested) { requested = true; QVERIFY(f.sessions->beginBrowserLogin("first")); }
        });
        QVERIFY(controller.submit("AAPL", "buy")); QVERIFY(requested); QVERIFY(!controller.busy());
        QVERIFY(controller.tradeResultsComplete()); QVERIFY(!controller.available());
        QTest::qWait(20); QVERIFY(f.orders.isEmpty()); QVERIFY(!controller.error().isEmpty());
        f.sessions->cancelSignIn();
    }
    void destructionFromDirectNativeObserversIsSafe_data() {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"admission", "native-results", "post", "cooldown"}) QTest::newRow(phase) << QString(phase);
    }
    void destructionFromDirectNativeObserversIsSafe() {
        QFETCH(QString, phase); Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies(); int releases = 0; const auto release = deps.release;
        deps.release = [&] { ++releases; release(); };
        auto controller = std::make_unique<RHAutoTradeController>(f.network, *f.sessions, deps);
        if (phase == "post") f.onOrder = [&] { controller.reset(); };
        connect(controller.get(), &RHAutoTradeController::stateChanged, this, [&] {
            if (!controller) return;
            if ((phase == "admission" && controller->busy())
                || (phase == "native-results" && !controller->tradeResults().isEmpty())) controller.reset();
        });
        QVERIFY(controller->submit("AAPL,MSFT", "buy"));
        if (phase == "cooldown") { QTRY_COMPARE(f.timers.size(), 1); controller.reset(); }
        QTRY_VERIFY(!controller); QVERIFY(!f.sessions->busy());
        QCOMPARE(releases, phase == "admission" ? 0 : 1);
        const auto orders = f.orders.size();
        for (const auto &timer : std::exchange(f.timers, {})) timer();
        QTest::qWait(20); QCOMPARE(f.orders.size(), orders); QCOMPARE(releases, phase == "admission" ? 0 : 1);
    }
    void cancellationDuringSessionReleaseDoesNotDuplicateCompletedRows() {
        Fixture f; QVERIFY(f.ready); f.accountCount = 1;
        for (const auto *name : {"first", "second"}) {
            QVERIFY(f.sessions->saveAuthenticatedSession(name, QString("fixture-") + name, 3600)); QTRY_VERIFY(!f.sessions->busy());
        }
        auto deps = f.dependencies(); bool acquired = false;
        const auto acquire = deps.acquire;
        deps.acquire = [&, acquire](auto done) { acquired = true; acquire(std::move(done)); };
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        connect(f.sessions.get(), &RHSessionManager::busyChanged, this, [&] {
            if (acquired && !f.sessions->busy() && controller.busy()) controller.cancel();
        });
        QVERIFY(controller.submit("AAPL", "buy")); drain(f, controller);
        QCOMPARE(f.orders.size(), 1); QCOMPARE(controller.tradeResults().size(), 2);
        QVERIFY(controller.tradeResults().first().toMap().value("success").toBool());
        QCOMPARE(controller.tradeResults().last().toMap().value("sessionName").toString(), "second");
        QVERIFY(!controller.tradeResults().last().toMap().value("success").toBool());
    }
    void destructionDuringSavedSessionActivationCancelsOwnedVerification() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto controller = std::make_unique<RHAutoTradeController>(f.network, *f.sessions, f.dependencies());
        bool destroyed = false;
        connect(f.sessions.get(), &RHSessionManager::busyChanged, this, [&] {
            if (f.sessions->busy() && controller) { controller.reset(); destroyed = true; }
        });
        QVERIFY(controller->submit("AAPL", "buy"));
        QTRY_VERIFY(destroyed); QVERIFY(!f.sessions->busy());
        QTest::qWait(30); QVERIFY(f.orders.isEmpty());
    }
    void cancellationIsReentrantDuringNativeNotifications() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies(); int releases = 0; const auto release = deps.release;
        deps.release = [&] { ++releases; release(); };
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        bool armed = false;
        connect(&controller, &RHAutoTradeController::stateChanged, this, [&] {
            if (armed && controller.busy()) controller.cancel();
        });
        QVERIFY(controller.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); armed = true;
        controller.cancel();
        QVERIFY(!controller.busy()); QVERIFY(controller.tradeResultsComplete());
        QCOMPARE(releases, 1); QVERIFY(!f.sessions->busy()); QCOMPARE(f.orders.size(), 1);
        const auto timer = f.timers.takeFirst(); timer(); QTest::qWait(20); QCOMPARE(f.orders.size(), 1);
    }
    void unsupportedFreshProductDoesNotBlockLaterSupportedAccounts() {
        RHAutoModeFixture::Fixture f; QVERIFY(f.ready); f.accountCount = 4;
        f.transform = [](const QString &path, QJsonObject object) {
            if (path != "/accounts/") return object;
            auto rows = object.value("results").toArray();
            const QStringList types{"joint", "individual", "ira_traditional", "ira_roth"};
            for (int i = 0; i < rows.size(); ++i) {
                auto row = rows[i].toObject(); row.insert("brokerage_account_type", types[i]);
                row.insert("nickname", "Standard Brokerage IRA Roth"); rows[i] = row;
            }
            object.insert("results", rows); return object;
        };
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600));
        QTRY_VERIFY(!f.sessions->busy());
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        QVERIFY(controller.submit("AAPL,MSFT", "buy"));
        for (int i = 0; i < 6; ++i) {
            QTRY_COMPARE_WITH_TIMEOUT(f.timers.size(), 1, 3000);
            QCOMPARE(f.orders.size(), i + 1); QCOMPARE(f.delays.last(), 2500);
            QCOMPARE(f.orders.last().value("account").toString(), f.brokerageOrigin + QString("/accounts/a%1/").arg(i / 2 + 2));
            auto timer = f.timers.takeFirst(); timer();
        }
        QTRY_VERIFY(!controller.busy()); QCOMPARE(f.orders.size(), 6);
        QVERIFY(controller.error().isEmpty());
    }
    void snapshotsSparseProfilesAndRunsAccountMajorWithFinalCooldown() {
        Fixture f; QVERIFY(f.ready);
        for (const auto &name : {"first", "expired", "deleted", "last"}) {
            QVERIFY(f.sessions->saveAuthenticatedSession(name, QString("fixture-") + name, 3600));
            QTRY_VERIFY(!f.sessions->busy());
        }
        QVERIFY(f.sessions->deleteSavedSession(2)); QTRY_VERIFY(!f.sessions->busy());
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        auto doc = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
        auto profiles = doc.value("sessions").toArray();
        auto expired = profiles[1].toObject();
        expired.insert("created_at", QDateTime::currentDateTimeUtc().addSecs(-20).toString(Qt::ISODateWithMs));
        expired.insert("expires_at", QDateTime::currentDateTimeUtc().addSecs(-10).toString(Qt::ISODateWithMs));
        profiles[1] = expired;
        auto firstOwner = profiles[0].toObject(); firstOwner.insert("account_id", "owner-first"); profiles[0] = firstOwner;
        auto lastOwner = profiles[2].toObject(); lastOwner.insert("account_id", "owner-last"); profiles[2] = lastOwner;
        doc.insert("sessions", profiles);
        QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
        metadata.write(QJsonDocument(doc).toJson()); metadata.close(); QVERIFY(f.sessions->load());
        auto deps = f.dependencies(); int acquires = 0, releases = 0;
        const auto acquire = deps.acquire; const auto release = deps.release;
        deps.acquire = [&](auto done) { ++acquires; acquire(std::move(done)); };
        deps.release = [&] { ++releases; release(); };
        RHAutoTradeController controller(f.network, *f.sessions, deps);
        const int priorReads = f.store.readIndices.size();
        QVERIFY(controller.submit(" aapl,MSFT,AaPl ", "buy"));
        QVERIFY(!controller.submit("AAPL", "buy"));
        for (int i = 0; i < 12; ++i) {
            QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(f.orders.size(), i + 1); QCOMPARE(f.delays.last(), 2500);
            QVERIFY(controller.busy()); QVERIFY(!controller.tradeResultsComplete());
            QVERIFY(f.sessions->busy()); QCOMPARE(acquires, i < 6 ? 1 : 2);
            QCOMPARE(releases, i < 6 ? 0 : 1);
            const auto order = f.orders.last();
            QCOMPARE(order.value("account").toString(), f.brokerageOrigin + QString("/accounts/a%1/").arg((i % 6) / 2 + 1));
            QCOMPARE(order.value("instrument").toString(), f.brokerageOrigin + "/instruments/" + (i % 2 ? "MSFT/" : "AAPL/"));
            auto timer = f.timers.takeFirst(); timer();
        }
        QTRY_VERIFY(!controller.busy()); QVERIFY(controller.tradeResultsComplete());
        QCOMPARE(releases, 2); QCOMPARE(controller.tradeResults().size(), 12);
        QCOMPARE(controller.tradeResults().first().toMap().value("sessionName").toString(), "first");
        QCOMPARE(controller.tradeResults().first().toMap().value("profileId").toString(), "owner-first");
        QCOMPARE(controller.tradeResults().last().toMap().value("profileId").toString(), "owner-last");
        QCOMPARE(controller.tradeResults().last().toMap().value("sessionIndex").toInt(), 3);
        QVERIFY(!f.store.readIndices.mid(priorReads).contains(1));
        QVERIFY(!f.store.readIndices.mid(priorReads).contains(2));
        QVERIFY(!f.sessions->busy());
    }
    void rejectsInvalidInputBeforeReadingCredentials() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("first", "fixture-first", 3600));
        QTRY_VERIFY(!f.sessions->busy());
        RHAutoTradeController controller(f.network, *f.sessions, f.dependencies());
        const int reads = f.store.reads;
        for (const auto &input : {QString(), QString("AAPL,,MSFT"), QString("AA PL"),
                                 QString::fromUtf8("ſPY"), QString(4097, 'A')}) {
            QVERIFY(!controller.submit(input, "buy"));
            QVERIFY(!controller.busy());
        }
        QVERIFY(!controller.submit("AAPL", "other"));
        QCOMPARE(f.store.reads, reads);
        QVERIFY(f.orders.isEmpty());
    }
};
QTEST_GUILESS_MAIN(RHAutoModeTest)
#include "test_rhautomode.moc"
