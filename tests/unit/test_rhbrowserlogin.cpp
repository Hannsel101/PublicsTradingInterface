#include "rhbrowserlogin.h"
#include <QtTest>

class TestRHBrowserLogin : public QObject
{
    Q_OBJECT
private slots:
    void refreshedSecondaryHeaderBeforeItsCookieNeverDeliversPrimary()
    {
        int primary = 0, secondary = 0;
        QByteArray captured;
        RHBrowserLogin login([&](QByteArray token, QDateTime) {
            captured = token; ++secondary;
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary-A");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login.observeCookie(cookie));
        const auto candidate = RHBrowserLogin::primaryCandidate(
            QUrl("https://api.robinhood.com/user/"), QUrl("https://robinhood.com/"),
            QUrl("https://robinhood.com/"), "GET", false,
            {{"Authorization", "Bearer secondary-B"}});
        login.acceptPrimary(login.m_generation, candidate);
        QCOMPARE(primary, 0); // B can be a refreshed secondary whose cookie event is still queued.
        cookie.setValue("secondary-B");
        QVERIFY(login.observeCookie(cookie));
        login.m_secondaryFallback.start(1);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 200);
        QCOMPARE(captured, QByteArray("secondary-B"));
        QCOMPARE(primary, 0);
    }
    void distinctReadBearerDoesNotEstablishPrimaryEvenAfterTimeout()
    {
        int primary = 0, secondary = 0;
        RHBrowserLogin login([&](QByteArray token, QDateTime) {
            QCOMPARE(token, QByteArray("secondary")); ++secondary;
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        login.acceptPrimary(login.m_generation, "different-read-bearer");
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login.observeCookie(cookie));
        QCOMPARE(primary, 0);
        QVERIFY(!login.m_captureDelivered);
        login.m_secondaryFallback.start(1);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 200);
        login.acceptPrimary(login.m_generation, "late-different-read-bearer");
        QCOMPARE(primary, 0);
        QVERIFY(!login.observeCookie(cookie));
    }
    void untrustedHeaderSyntaxPolicy_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("canonical") << QByteArray("Authorization") << QByteArray("Bearer synthetic") << true;
        QTest::newRow("case-insensitive-header-and-scheme") << QByteArray("aUtHoRiZaTiOn") << QByteArray("bEaReR synthetic") << true;
        QTest::newRow("wrong-name") << QByteArray("Authorization ") << QByteArray("Bearer synthetic") << false;
        for (const auto &value : {QByteArray(), QByteArray("Basic synthetic"), QByteArray("Bearers synthetic"),
             QByteArray("Bearer "), QByteArray("Bearer  synthetic"), QByteArray(" Bearer synthetic"),
             QByteArray("Bearer synthetic "), QByteArray("Bearer synthetic\r\nx"),
             QByteArray("Bearer one,two"), QByteArray("Bearer ="), QByteArray("Bearer x=y"),
             QByteArray("Bearer x;"), QByteArray("Bearer x\\"), QByteArray("Bearer x\x80"),
             QByteArray("Bearer ") + QByteArray(16385, 'x')}) {
            QTest::newRow(qPrintable(QString("invalid-%1").arg(value.size()) + QString::fromLatin1(value.left(24))))
                << QByteArray("Authorization") << value << false;
        }
    }
    void untrustedHeaderSyntaxPolicy()
    {
        QFETCH(QByteArray, name); QFETCH(QByteArray, value); QFETCH(bool, accepted);
        const auto candidate = RHBrowserLogin::primaryCandidate(QUrl("https://api.robinhood.com/accounts/"),
            QUrl("https://robinhood.com/"), QUrl("https://robinhood.com/"), "GET", false, {{name, value}});
        QCOMPARE(!candidate.isEmpty(), accepted);
    }
    void untrustedRequestPolicyAndGeneration()
    {
        const QUrl site("https://robinhood.com/");
        const RHBrowserLogin::Headers headers{{"Authorization", "Bearer synthetic"}};
        const auto candidate = [&](QUrl url, QUrl party, QUrl origin, QByteArray method = "GET", bool redirect = false) {
            return RHBrowserLogin::primaryCandidate(url, party, origin, method, redirect, headers);
        };
        for (const auto *url : {"http://api.robinhood.com/user/", "https://api.robinhood.com:443/user/",
             "https://user@api.robinhood.com/user/", "https://@api.robinhood.com/user/",
             "https://api.robinhood.com/user/#x", "https://api.robinhood.com/user/?x=y",
             "https://api.robinhood.com/user", "https://api.robinhood.com/orders/",
             "https://api.robinhood.com.evil/user/", "https://api.robinhood.com./user/"})
            QVERIFY2(candidate(QUrl(url), site, site).isEmpty(), url);
        const QUrl api("https://api.robinhood.com/user/");
        for (const auto *method : {"POST", "get", "PUT", "DELETE"})
            QVERIFY(candidate(api, site, site, method).isEmpty());
        for (const QUrl &foreign : {QUrl(), QUrl("https://evil.com/"), QUrl("http://robinhood.com/"),
             QUrl("https://robinhood.com:443/"), QUrl("https://user@robinhood.com/")}) {
            QVERIFY(candidate(api, foreign, site).isEmpty());
            QVERIFY(candidate(api, site, foreign).isEmpty());
        }
        QVERIFY(candidate(api, site, site, "GET", true).isEmpty());
        QVERIFY(RHBrowserLogin::primaryCandidate(api, site, site, "GET", false,
            {{"Authorization", "Bearer one"}, {"authorization", "Bearer two"}}).isEmpty());
        int calls = 0;
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++calls; });
        login.acceptPrimary(0, "synthetic");
        QVERIFY(login.start()); const auto old = login.m_generation;
        login.cancel(); login.cancel();
        login.acceptPrimary(old, "synthetic");
        QVERIFY(login.start());
        login.acceptPrimary(old, "synthetic");
        login.m_trustedNavigation = false;
        login.acceptPrimary(login.m_generation, "synthetic");
        QCOMPARE(calls, 0);
        login.navigationChanged(site);
        login.acceptPrimary(login.m_generation, "synthetic");
        QCOMPARE(calls, 0); // Trusted origin still does not prove primary provenance.
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login.observeCookie(cookie));
        QCOMPARE(calls, 0);
    }
    void refreshedSecondaryPermutations_data()
    {
        QTest::addColumn<QByteArray>("events");
        for (const auto *order : {"AHB", "HAB", "ABH", "BHA", "HBA", "BAH"})
            QTest::newRow(order) << QByteArray(order);
    }
    void refreshedSecondaryPermutations()
    {
        QFETCH(QByteArray, events);
        int primary = 0, secondary = 0;
        QByteArray captured, latest;
        RHBrowserLogin login([&](QByteArray token, QDateTime) {
            captured = token; ++secondary;
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary-A");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        for (const char event : events) {
            if (event == 'H') {
                login.acceptPrimary(login.m_generation, "secondary-B");
                login.acceptPrimary(login.m_generation, "unclassified-read-bearer");
            } else {
                latest = event == 'A' ? "secondary-A" : "secondary-B";
                cookie.setValue(latest);
                QVERIFY(login.observeCookie(cookie));
            }
            QCOMPARE(primary, 0);
            QVERIFY(!login.m_captureDelivered);
        }
        login.m_secondaryFallback.start(1);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 200);
        QCOMPARE(captured, latest);
        QCOMPARE(primary, 0);
    }
    void primaryAndSecondaryEventOrdering_data()
    {
        QTest::addColumn<int>("order");
        QTest::newRow("cookie-before-primary") << 0;
        QTest::newRow("primary-before-cookie") << 1;
        QTest::newRow("secondary-header-primary-header-cookie") << 2;
        QTest::newRow("primary-header-secondary-header-cookie") << 3;
    }
    void primaryAndSecondaryEventOrdering()
    {
        QFETCH(int, order);
        int primary = 0, secondary = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime) { ++secondary; }, nullptr,
                             RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray token) {
            QCOMPARE(token, QByteArray("primary")); ++primary;
        });
        QVERIFY(login.start());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        if (order == 0) QVERIFY(login.observeCookie(cookie));
        if (order == 2) login.acceptPrimary(login.m_generation, "secondary");
        login.acceptPrimary(login.m_generation, "primary");
        if (order == 3) login.acceptPrimary(login.m_generation, "secondary");
        if (order != 0) {
            QCOMPARE(primary, 0);
            QVERIFY(login.observeCookie(cookie));
        }
        QCOMPARE(primary, 0); QCOMPARE(secondary, 0);
        QCOMPARE(login.m_stagedSecondary, QByteArray("secondary"));
        QVERIFY(login.m_secondaryFallback.isActive());
        login.m_secondaryFallback.start(1);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 200);
        QCOMPARE(primary, 0);
        QVERIFY(login.m_stagedSecondary.isEmpty());
        QVERIFY(!login.m_secondaryFallback.isActive());
        login.acceptPrimary(login.m_generation, "primary");
        QVERIFY(!login.observeCookie(cookie));
        QCOMPARE(primary, 0);
    }
    void secondaryBearerNeverBecomesPrimary_data()
    {
        QTest::addColumn<bool>("headerFirst");
        QTest::newRow("header-first") << true;
        QTest::newRow("cookie-first") << false;
    }
    void secondaryBearerNeverBecomesPrimary()
    {
        QFETCH(bool, headerFirst);
        int secondary = 0, primary = 0;
        RHBrowserLogin login([&](QByteArray token, QDateTime) {
            QCOMPARE(token, QByteArray("secondary")); ++secondary;
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        if (headerFirst) login.acceptPrimary(login.m_generation, "secondary");
        QVERIFY(login.observeCookie(cookie));
        login.acceptPrimary(login.m_generation, "secondary");
        QCOMPARE(primary, 0); QCOMPARE(secondary, 0);
        login.m_secondaryFallback.start(1);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 200);
        QCOMPARE(primary, 0);
        login.acceptPrimary(login.m_generation, "late-primary");
        QCOMPARE(primary, 0);
    }
    void noCandidateRetentionAcrossFloodCancelAndRestart()
    {
        int primary = 0, secondary = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime) { ++secondary; }, nullptr,
                             RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        for (int i = 0; i < 1000; ++i)
            login.acceptPrimary(login.m_generation, QByteArray(16384, 'x'));
        QVERIFY(login.m_stagedSecondary.isEmpty());
        QVERIFY(!login.m_captureDelivered);
        const auto stale = login.m_generation;
        login.cancel();
        QVERIFY(login.start());
        login.acceptPrimary(stale, "stale");
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login.observeCookie(cookie));
        login.acceptPrimary(stale, "stale-after-cookie");
        login.acceptPrimary(login.m_generation, "ambiguous-current");
        QCOMPARE(primary, 0);
        login.cancel();
        QVERIFY(login.m_stagedSecondary.isEmpty());
        QVERIFY(!login.m_secondaryFallback.isActive());
        QTest::qWait(20);
        QCOMPARE(secondary, 0);
    }
    void secondaryRefreshRetainsFallbackBound()
    {
        int primary = 0, secondary = 0;
        RHBrowserLogin login([&](QByteArray token, QDateTime) {
            QCOMPARE(token, QByteArray("new-secondary")); ++secondary;
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "old-secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login.observeCookie(cookie));
        login.m_secondaryFallback.start(100);
        cookie.setValue("new-secondary");
        QVERIFY(login.observeCookie(cookie));
        QVERIFY(login.m_secondaryFallback.remainingTime() <= 100);
        login.acceptPrimary(login.m_generation, "old-secondary");
        login.acceptPrimary(login.m_generation, "new-secondary");
        QCOMPARE(primary, 0);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 300);
    }
    void invalidCookieCannotEnablePrimaryOrStageFallback()
    {
        int primary = 0;
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        login.acceptPrimary(login.m_generation, "primary");
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        cookie.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(-1));
        QVERIFY(!login.observeCookie(cookie));
        QCOMPARE(primary, 0);
        cookie.setExpirationDate({});
        cookie.setSecure(false);
        QVERIFY(!login.observeCookie(cookie));
        QCOMPARE(primary, 0);
        login.m_trustedNavigation = false;
        cookie.setSecure(true);
        QVERIFY(!login.observeCookie(cookie));
        QCOMPARE(primary, 0);
        login.cancel();
    }
    void expiredStagedCookieCannotAuthorizePrimaryOrFallback()
    {
        int primary = 0, secondary = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime) { ++secondary; }, nullptr,
                             RHBrowserLogin::BackendMode::Test);
        login.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login.start());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login.observeCookie(cookie));
        login.m_stagedExpiry = QDateTime::currentDateTimeUtc().addSecs(-1);
        login.acceptPrimary(login.m_generation, "primary");
        QCOMPARE(primary, 0);
        login.m_secondaryFallback.start(1);
        QTRY_VERIFY_WITH_TIMEOUT(login.m_stagedSecondary.isEmpty(), 200);
        QCOMPARE(primary, 0); QCOMPARE(secondary, 0);
    }
    void secondaryFallbackConsumerMayDestroyBridgeSynchronously()
    {
        int secondary = 0, primary = 0;
        std::unique_ptr<RHBrowserLogin> login;
        login = std::make_unique<RHBrowserLogin>([&](QByteArray token, QDateTime) {
            QCOMPARE(token, QByteArray("secondary")); ++secondary;
            login.reset();
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        login->setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(login->start());
        login->acceptPrimary(login->m_generation, "ambiguous");
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(login->observeCookie(cookie));
        login->m_secondaryFallback.start(1);
        QTRY_VERIFY_WITH_TIMEOUT(!login, 200);
        QCOMPARE(secondary, 1);
        QCOMPARE(primary, 0);
    }
    void failedPageDoubleCancelAndDestructionPreserveCleanupEpoch()
    {
        std::shared_ptr<std::atomic<quint64>> epoch;
        quint64 cleanupGeneration = 0;
        QObject cleanupReceiver;
        bool cleanupRan = false;
        bool cleanupEligible = false;
        {
            RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
            epoch = login.m_nativeEpoch;
            connect(&login, &RHBrowserLogin::activeChanged, &login, [&] {
                if (login.active()) return;
                // The native cleanup queued by stop() uses this shared epoch and
                // generation, with no QObject dependency. Exercise its guard
                // without touching an Android CookieManager or real credentials.
                cleanupGeneration = login.m_generation;
                QTimer::singleShot(0, &cleanupReceiver,
                    [epoch, cleanupGeneration, &cleanupRan, &cleanupEligible] {
                        cleanupRan = true;
                        cleanupEligible = epoch->load(std::memory_order_acquire) == cleanupGeneration;
                    });
                // Session failure synchronously cancels from activeChanged.
                login.cancel();
                login.cancel();
            });
            QVERIFY(login.start());
            const auto activeGeneration = login.m_generation;
            login.pageFailed();
            QVERIFY(!login.active());
            QVERIFY(cleanupGeneration != activeGeneration);
            QCOMPARE(login.m_generation, cleanupGeneration);
            QCOMPARE(epoch->load(std::memory_order_acquire), cleanupGeneration);
        } // Destructor also calls stop(), before the queued cleanup runs.
        QCOMPARE(epoch->load(std::memory_order_acquire), cleanupGeneration);
        QCoreApplication::processEvents();
        QVERIFY(cleanupRan);
        QVERIFY(cleanupEligible);
    }
    void restartSupersedesCleanupAndCancelledCallbacks()
    {
        int calls = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime) { ++calls; },
                             nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.start());
        const auto activeGeneration = login.m_generation;
        const auto epoch = login.m_nativeEpoch;
        QCOMPARE(epoch->load(std::memory_order_acquire), activeGeneration);
        login.cancel();
        const auto cleanupGeneration = login.m_generation;
        QVERIFY(cleanupGeneration != activeGeneration);
        QCOMPARE(epoch->load(std::memory_order_acquire), cleanupGeneration);
        const QByteArray header("__Host-Web-App-Secondary-Access-Token=synthetic");
        login.androidPollResult(activeGeneration, header, true);
        QCOMPARE(calls, 0);
        QVERIFY(!login.active());
        bool cleanupRan = false;
        bool cleanupEligible = true;
        QObject cleanupReceiver;
        QTimer::singleShot(0, &cleanupReceiver,
            [epoch, cleanupGeneration, &cleanupRan, &cleanupEligible] {
                cleanupRan = true;
                cleanupEligible = epoch->load(std::memory_order_acquire) == cleanupGeneration;
            });
        login.cancel();
        QVERIFY(login.start());
        QVERIFY(login.m_generation != cleanupGeneration);
        QCOMPARE(epoch->load(std::memory_order_acquire), login.m_generation);
        QCoreApplication::processEvents();
        QVERIFY(cleanupRan);
        QVERIFY(!cleanupEligible); // Old cleanup cannot delete the new login cookie.
        login.androidPollResult(activeGeneration, header, true);
        login.androidPollResult(cleanupGeneration, {}, false);
        QCOMPARE(calls, 0);
        QVERIFY(login.active());
        QVERIFY(login.error().isEmpty());
        login.androidPollResult(login.m_generation, header, true);
        QCOMPARE(calls, 1);
    }
    void androidResetTimeoutWithoutNativeReply()
    {
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.start());
        login.m_resetting = true;
        login.m_resetTimeout.start(1);
        QTRY_VERIFY_WITH_TIMEOUT(!login.active(), 200);
        QVERIFY(!login.error().isEmpty());
        QVERIFY(!login.m_poll.isActive());
    }
    void androidResetGateAndStaleGeneration()
    {
        int calls = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime until) { ++calls; QVERIFY(!until.isValid()); },
                             nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.start());
        const auto oldGeneration = login.m_generation;
        login.cancel();
        QVERIFY(login.start());
        login.m_resetting = true;
        login.m_resetClock.start();
        login.m_trustedNavigation = false;
        login.setStartUrl(QUrl("about:blank"));
        const QByteArray token("__Host-Web-App-Secondary-Access-Token=synthetic");
        login.androidPollResult(oldGeneration, {}, true);
        QCOMPARE(login.startUrl(), QUrl("about:blank"));
        login.androidPollResult(login.m_generation, token, true);
        QCOMPARE(calls, 0);
        QCOMPARE(login.startUrl(), QUrl("about:blank"));
        login.androidPollResult(login.m_generation, {}, true);
        QCOMPARE(login.startUrl(), login.loginUrl());
        login.androidPollResult(login.m_generation, token, true);
        QCOMPARE(calls, 0); // No trusted top-level navigation yet.
        login.navigationChanged(login.loginUrl());
        login.androidPollResult(login.m_generation, token, true);
        QCOMPARE(calls, 1);
        QVERIFY(login.active());
        login.androidPollResult(login.m_generation, token, true);
        QCOMPARE(calls, 1);
        login.androidPollResult(login.m_generation, {}, false);
        QVERIFY(login.active());
        login.finish();
        QVERIFY(!login.active());
    }
    void androidNativeFailureClosesGate()
    {
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.start());
        login.androidPollResult(login.m_generation, {}, false);
        QVERIFY(!login.active());
        QVERIFY(!login.error().isEmpty());
    }
    void androidCookieHeaderPolicy_data()
    {
        QTest::addColumn<QByteArray>("header"); QTest::addColumn<bool>("ok"); QTest::addColumn<bool>("present");
        QTest::newRow("absent") << QByteArray("other=value") << true << false;
        QTest::newRow("empty-jar") << QByteArray() << true << false;
        QTest::newRow("exact") << QByteArray("other=x; __Host-Web-App-Secondary-Access-Token=synthetic==; another=y") << true << true;
        QTest::newRow("wrong-name") << QByteArray("Web-App-Secondary-Access-Token=x") << true << false;
        QTest::newRow("duplicate") << QByteArray("__Host-Web-App-Secondary-Access-Token=a; __Host-Web-App-Secondary-Access-Token=b") << false << true;
        QTest::newRow("empty") << QByteArray("__Host-Web-App-Secondary-Access-Token=") << false << true;
        QTest::newRow("trailing-space") << QByteArray("__Host-Web-App-Secondary-Access-Token=x ") << false << true;
        QTest::newRow("control") << QByteArray("__Host-Web-App-Secondary-Access-Token=x\r\n") << false << true;
        QTest::newRow("unicode") << QByteArray("__Host-Web-App-Secondary-Access-Token=\xc3\xa9") << false << true;
    }
    void androidCookieHeaderPolicy()
    {
        QFETCH(QByteArray, header); QFETCH(bool, ok); QFETCH(bool, present);
        QNetworkCookie cookie; bool found = false;
        QCOMPARE(RHBrowserLogin::parseAndroidCookie(header, cookie, found), ok);
        QCOMPARE(found, present);
        if (ok && present) {
            QCOMPARE(cookie.value(), QByteArray("synthetic=="));
            QVERIFY(!cookie.expirationDate().isValid());
            QVERIFY(cookie.isSecure());
            QCOMPARE(cookie.domain(), QString("robinhood.com"));
            QCOMPARE(cookie.path(), QString("/"));
        }
    }
    void automaticBackendContract()
    {
        RHBrowserLogin login;
#if defined(Q_OS_ANDROID)
        QCOMPARE(login.backend(), QStringLiteral("unavailable"));
#elif defined(PUBLICS_RH_WEBENGINE)
        QCOMPARE(login.backend(), QStringLiteral("webengine"));
#endif
        if (login.backend() == QStringLiteral("unavailable")) {
            QVERIFY(!login.available());
            QVERIFY(!login.start());
            QVERIFY(!login.error().isEmpty());
            QVERIFY(login.viewSource().isEmpty());
        } else {
            QVERIFY(login.available());
            QCOMPARE(login.backend(), QStringLiteral("webengine"));
            QCOMPARE(login.viewSource(), QUrl("qrc:/qt/qml/RobinHoodInterface/DesktopWebLogin.qml"));
        }
    }
    void productionAndroidFailsClosed()
    {
#if defined(Q_OS_ANDROID)
        int calls = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime) { ++calls; });
        QVERIFY(!login.start());
        QVERIFY(!login.available());
        QCOMPARE(login.backend(), QStringLiteral("unavailable"));
        QVERIFY(login.viewSource().isEmpty());
        QVERIFY(!login.active());
        QVERIFY(login.error().contains(QStringLiteral("Android")));
        QVERIFY(login.error().contains(QStringLiteral("pre-navigation interceptor")));
        login.navigationChanged(login.loginUrl());
        login.androidPollResult(login.m_generation,
            QByteArray("__Host-Web-App-Secondary-Access-Token=synthetic"), true);
        QCOMPARE(calls, 0);
#else
        // This assertion is exercised on Android builds; desktop keeps WebEngine.
        RHBrowserLogin login;
        QVERIFY(login.backend() != QStringLiteral("android-webview"));
#endif
    }
    void nativeRejectsUnusableProfile()
    {
        RHBrowserLogin login;
        if (login.backend() != "webengine") {
            QCOMPARE(login.backend(), QStringLiteral("unavailable"));
            QVERIFY(!login.available());
            return;
        }
        QVERIFY(login.start());
        QCOMPARE(login.startUrl(), QUrl("about:blank"));
        QObject invalidProfile;
        login.attachProfile(&invalidProfile);
        QVERIFY(!login.active());
        QVERIFY(!login.error().isEmpty());
    }
    void navigationPolicy_data()
    {
        QTest::addColumn<QUrl>("url"); QTest::addColumn<bool>("allowed");
        for (const auto &url : {"https://robinhood.com/", "https://login.robinhood.com/login", "https://robinhood.com:443/"})
            QTest::newRow(url) << QUrl(url) << true;
        for (const auto &url : {"http://robinhood.com/", "https://robinhood.com:8443/", "https://evilrobinhood.com/", "https://robinhood.com.evil.com/", "https://user:pass@robinhood.com/", "https://@robinhood.com/", "file:///tmp/x", "javascript:alert(1)", "data:text/html,x", "https://robinhood.com./", ""})
            QTest::newRow(url) << QUrl(url) << false;
    }
    void navigationPolicy()
    {
        QFETCH(QUrl, url); QFETCH(bool, allowed);
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        QCOMPARE(login.isAllowedNavigation(url), allowed);
    }
    void untrustedNavigationTerminatesCapture()
    {
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.start());
        QVERIFY(login.isAllowedNavigation(QUrl("about:blank")));
        login.navigationChanged(login.loginUrl());
        QVERIFY(!login.isAllowedNavigation(QUrl("about:blank")));
        login.navigationChanged(QUrl("https://evil.com/"));
        QVERIFY(!login.active());
        QVERIFY(!login.error().isEmpty());
        QVERIFY(login.start());
        QVERIFY(login.error().isEmpty());
        login.pageFailed();
        QVERIFY(!login.active());
        QVERIFY(!login.error().isEmpty());
    }
    void noSecretMetaObjectSurface()
    {
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        const auto *meta = login.metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i);
            QVERIFY(!QByteArray(property.name()).toLower().contains("token"));
        }
        for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
            const auto name = meta->method(i).name().toLower();
            QVERIFY(!name.contains("token"));
            QVERIFY(!name.contains("cookie"));
        }
        for (const auto *name : {"available", "backend", "viewSource", "loginUrl"})
            QVERIFY(meta->property(meta->indexOfProperty(name)).isConstant());
        for (const auto *name : {"active", "startUrl", "error"})
            QVERIFY(meta->property(meta->indexOfProperty(name)).hasNotifySignal());
    }
    void rejectsUnsafeCookies_data()
    {
        QTest::addColumn<QNetworkCookie>("cookie");
        const auto base = [] {
            QNetworkCookie c("__Host-Web-App-Secondary-Access-Token", "synthetic");
            c.setDomain("robinhood.com"); c.setPath("/"); c.setSecure(true);
            return c;
        };
        auto c = base(); c.setName("access_token"); QTest::newRow("wrong-name") << c;
        c = base(); c.setName("__host-Web-App-Secondary-Access-Token"); QTest::newRow("case-name") << c;
        for (const auto &domain : {"", "evil.com", "api.robinhood.com", ".robinhood.com", "robinhood.com.", "ROBINHOOD.COM"}) {
            c = base(); c.setDomain(domain); QTest::newRow(qPrintable(QString("domain-%1").arg(domain))) << c;
        }
        c = base(); c.setSecure(false); QTest::newRow("insecure") << c;
        c = base(); c.setPath("/login"); QTest::newRow("wrong-path") << c;
        c = base(); c.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(-1)); QTest::newRow("expired") << c;
        c = base(); c.setValue({}); QTest::newRow("empty") << c;
        c = base(); c.setValue(QByteArray(16385, 'x')); QTest::newRow("oversize") << c;
        for (const int byte : {0, 9, 10, 13, 31, 32, 34, 44, 59, 92, 127, 128, 255}) {
            c = base(); c.setValue(QByteArray("test") + char(byte));
            QTest::newRow(qPrintable(QString("unsafe-byte-%1").arg(byte))) << c;
        }
    }
    void rejectsUnsafeCookies()
    {
        QFETCH(QNetworkCookie, cookie);
        int calls = 0;
        RHBrowserLogin login([&](QByteArray, QDateTime) { ++calls; }, nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.start());
        QVERIFY(!login.observeCookie(cookie));
        QVERIFY(login.active());
        QCOMPARE(calls, 0);
    }
    void capturesValidCookieOnce()
    {
        int calls = 0;
        QByteArray captured;
        QDateTime expiry;
        RHBrowserLogin login([&](QByteArray token, QDateTime until) {
            ++calls; captured = token; expiry = until;
        }, nullptr, RHBrowserLogin::BackendMode::Test);
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "synthetic-token");
        cookie.setDomain("robinhood.com");
        cookie.setPath("/");
        cookie.setSecure(true);
        cookie.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(600));
        QVERIFY(!login.observeCookie(cookie));
        QVERIFY(login.start());
        QVERIFY(login.observeCookie(cookie));
        QCOMPARE(calls, 1);
        QCOMPARE(captured, cookie.value());
        QCOMPARE(expiry, cookie.expirationDate());
        QVERIFY(login.active());
        QVERIFY(!login.observeCookie(cookie));
        QCOMPARE(calls, 1);
        login.finish();
        QVERIFY(!login.active());
        QVERIFY(!login.observeCookie(cookie));
    }
    void testBackendLifecycle()
    {
        RHBrowserLogin login({}, nullptr, RHBrowserLogin::BackendMode::Test);
        QVERIFY(login.available());
        QVERIFY(!login.active());
        QCOMPARE(login.loginUrl(), QUrl("https://robinhood.com/"));
        QSignalSpy active(&login, &RHBrowserLogin::activeChanged);
        QVERIFY(login.start());
        QVERIFY(login.active());
        QCOMPARE(active.count(), 1);
        login.cancel();
        QVERIFY(!login.active());
        QCOMPARE(active.count(), 2);
    }
};
QTEST_GUILESS_MAIN(TestRHBrowserLogin)
#include "test_rhbrowserlogin.moc"
