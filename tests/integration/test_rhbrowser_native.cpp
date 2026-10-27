#include "rhbrowserlogin.h"

#include <QGuiApplication>
#include <QHostAddress>
#include <QNetworkCookie>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest/QtTest>
#include <memory>
#include <atomic>

// Verify native header visibility separately from authentication provenance.
// This fixture blocks every non-loopback URL; production installs no collector.
class LoopbackInterceptor final : public QWebEngineUrlRequestInterceptor
{
public:
    explicit LoopbackInterceptor(QObject *parent)
        : QWebEngineUrlRequestInterceptor(parent) {}
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (info.requestUrl().scheme() != QStringLiteral("http")
            || info.requestUrl().host() != QStringLiteral("127.0.0.1")) {
            info.block(true);
            return;
        }
        const auto headers = info.httpHeaders();
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            if (it.key().compare("Authorization", Qt::CaseInsensitive) == 0
                && it.value() == "Bearer loopback-synthetic") ++bearerRequests;
        }
    }
    std::atomic<int> bearerRequests{0};
};

class RHBrowserNativeTest final : public QObject
{
    Q_OBJECT
private slots:
    void nativeHeaderVisibilityDoesNotEnablePrimaryCapture()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int loopbackFetches = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        socket->setProperty("request", request);
                        return;
                    }
                    const bool fetch = request.startsWith("GET /user/ ");
                    if (fetch) ++loopbackFetches;
                    const QByteArray body = fetch ? QByteArray("{}") : QByteArray(
                        "<!doctype html><script>fetch('/user/', {headers:"
                        "{'Authorization':'Bearer loopback-synthetic'}})</script>");
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "
                        + QByteArray(fetch ? "application/json" : "text/html")
                        + "\r\nContent-Length: " + QByteArray::number(body.size())
                        + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        QQuickWebEngineProfile profile;
        int primary = 0, secondary = 0;
        RHBrowserLogin browser([&](QByteArray, QDateTime) { ++secondary; });
        browser.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(browser.start());
        browser.attachProfile(&profile);
        QVERIFY(profile.findChildren<QWebEngineUrlRequestInterceptor *>().isEmpty());
        auto *interceptor = new LoopbackInterceptor(&profile);
        profile.setUrlRequestInterceptor(interceptor);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import QtWebEngine
            WebEngineView {
                required property WebEngineProfile fixtureProfile
                required property url fixtureUrl
                profile: fixtureProfile
                url: fixtureUrl
                onNavigationRequested: request => {
                    if (request.url.toString() === fixtureUrl.toString()) request.accept()
                    else request.reject()
                }
            }
        )", QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        std::unique_ptr<QObject> view(component.createWithInitialProperties({
            {QStringLiteral("fixtureProfile"), QVariant::fromValue(&profile)},
            {QStringLiteral("fixtureUrl"), url}
        }));
        QVERIFY2(view, qPrintable(component.errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(loopbackFetches, 1, 5000);
        QCOMPARE(interceptor->bearerRequests.load(), 1);
        QCOMPARE(primary, 0); QCOMPARE(secondary, 0);
        browser.cancel();
    }
    void destroyedProfileAndBridgeInvalidateQueuedPrimary()
    {
        int calls = 0;
        auto profile = std::make_unique<QQuickWebEngineProfile>();
        auto browser = std::make_unique<RHBrowserLogin>();
        browser->setPrimaryTokenHandler([&](QByteArray) { ++calls; });
        QVERIFY(browser->start()); browser->attachProfile(profile.get());
        browser->navigationChanged(browser->loginUrl());
        const auto old = browser->m_generation;
        const auto epoch = browser->m_nativeEpoch;
        QPointer<RHBrowserLogin> guard(browser.get());
        QMetaObject::invokeMethod(browser.get(), [guard, old] {
            if (guard) guard->acceptPrimary(old, "queued-stale-primary");
        }, Qt::QueuedConnection);
        browser->acceptPrimary(old, "pending-primary");
        QVERIFY(!browser->m_captureDelivered);
        profile.reset();
        QVERIFY(!browser->active());
        QVERIFY(browser->m_stagedSecondary.isEmpty());
        QVERIFY(epoch->load() != old);
        browser.reset();
        QCoreApplication::processEvents();
        QCOMPARE(calls, 0);
    }
    void primaryCaptureInstallsNoNativeCollector()
    {
        QQuickWebEngineProfile profile;
        RHBrowserLogin browser;
        browser.setPrimaryTokenHandler([](QByteArray) {});
        QVERIFY(browser.start());
        browser.attachProfile(&profile);
        QVERIFY(profile.findChildren<QWebEngineUrlRequestInterceptor *>().isEmpty());
        browser.cancel();
        QVERIFY(profile.findChildren<QWebEngineUrlRequestInterceptor *>().isEmpty());
    }
    void refreshedSecondaryNativeEventPermutations_data()
    {
        QTest::addColumn<int>("order");
        QTest::newRow("header-before-cookie-emission") << 0;
        QTest::newRow("header-before-queued-cookie-delivery") << 1;
        QTest::newRow("header-after-cookie-delivery") << 2;
    }
    void refreshedSecondaryNativeEventPermutations()
    {
        QFETCH(int, order);
        QQuickWebEngineProfile profile;
        int primary = 0, secondary = 0;
        QByteArray captured;
        RHBrowserLogin browser([&](QByteArray token, QDateTime) {
            captured = token; ++secondary;
        });
        browser.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(browser.start()); browser.attachProfile(&profile);
        browser.navigationChanged(browser.loginUrl());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary-A");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        // Exercise the production queued connections using synthetic store events.
        // Native store ingestion itself is covered separately below.
        profile.cookieStore()->cookieAdded(cookie);
        QCoreApplication::processEvents();
        QCOMPARE(browser.m_stagedSecondary, QByteArray("secondary-A"));
        const auto bound = browser.m_secondaryFallback.remainingTime();
        if (order == 0) browser.acceptPrimary(browser.m_generation, "secondary-B");
        cookie.setValue("secondary-B");
        profile.cookieStore()->cookieAdded(cookie);
        if (order == 1) browser.acceptPrimary(browser.m_generation, "secondary-B");
        QCoreApplication::processEvents();
        if (order == 2) browser.acceptPrimary(browser.m_generation, "secondary-B");
        QCOMPARE(primary, 0);
        QVERIFY(!browser.m_captureDelivered);
        QCOMPARE(browser.m_stagedSecondary, QByteArray("secondary-B"));
        QVERIFY(browser.m_secondaryFallback.remainingTime() <= bound);
        // An old removal arriving late must not erase the new staged identity.
        cookie.setValue("secondary-A");
        profile.cookieStore()->cookieRemoved(cookie);
        QCoreApplication::processEvents();
        QCOMPARE(browser.m_stagedSecondary, QByteArray("secondary-B"));
        browser.m_secondaryFallback.start(1);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 200);
        QCOMPARE(captured, QByteArray("secondary-B"));
        QCOMPARE(primary, 0);
        browser.finish();
    }
    void cancelledCookieEventsCannotPopulateReplacementGeneration()
    {
        QQuickWebEngineProfile oldProfile, newProfile;
        int primary = 0, secondary = 0;
        RHBrowserLogin browser([&](QByteArray, QDateTime) { ++secondary; });
        browser.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(browser.start()); browser.attachProfile(&oldProfile);
        browser.navigationChanged(browser.loginUrl());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "old-secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        oldProfile.cookieStore()->cookieAdded(cookie);
        const auto old = browser.m_generation;
        browser.cancel();
        QVERIFY(browser.start()); browser.attachProfile(&newProfile);
        browser.navigationChanged(browser.loginUrl());
        browser.acceptPrimary(old, "old-header");
        QCoreApplication::processEvents();
        QVERIFY(browser.m_stagedSecondary.isEmpty());
        QVERIFY(!browser.m_secondaryFallback.isActive());
        QCOMPARE(primary, 0); QCOMPARE(secondary, 0);
        browser.cancel();
    }
    void secondaryRemovalInvalidatesIdentityWithoutExtendingBound()
    {
        QQuickWebEngineProfile profile;
        int primary = 0;
        RHBrowserLogin browser;
        browser.setPrimaryTokenHandler([&](QByteArray) { ++primary; });
        QVERIFY(browser.start()); browser.attachProfile(&profile);
        browser.navigationChanged(browser.loginUrl());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(browser.acceptCookie(cookie));
        const auto initialBound = browser.m_secondaryFallback.remainingTime();
        QTest::qWait(30);
        // Synthetic native cookie-store event: no view or remote request needed.
        profile.cookieStore()->cookieRemoved(cookie);
        QCoreApplication::processEvents();
        QVERIFY(browser.m_stagedSecondary.isEmpty());
        QVERIFY(!browser.m_secondaryFallback.isActive());
        browser.acceptPrimary(browser.m_generation, "primary");
        QCOMPARE(primary, 0);
        QVERIFY(browser.acceptCookie(cookie));
        QVERIFY(browser.m_secondaryFallback.remainingTime() < initialBound);
        browser.cancel();
        QVERIFY(browser.m_stagedSecondary.isEmpty());
    }
    void ambiguousBearerKeepsSecondaryFallbackAndCancelsSafely()
    {
        QQuickWebEngineProfile profile;
        int secondary = 0;
        int primary = 0;
        RHBrowserLogin browser([&](QByteArray, QDateTime) { ++secondary; });
        browser.setPrimaryTokenHandler([&](QByteArray token) {
            QCOMPARE(token, QByteArray("synthetic-primary"));
            ++primary;
        });
        QVERIFY(browser.start());
        browser.attachProfile(&profile);
        browser.navigationChanged(browser.loginUrl());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "synthetic-secondary");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(browser.acceptCookie(cookie));
        QCOMPARE(secondary, 0);
        QVERIFY(browser.m_secondaryFallback.isActive());
        browser.acceptPrimary(browser.m_generation, "synthetic-primary");
        QCOMPARE(primary, 0);
        QVERIFY(browser.m_secondaryFallback.isActive());
        QCOMPARE(browser.m_stagedSecondary, QByteArray("synthetic-secondary"));
        QTest::qWait(30);
        QCOMPARE(secondary, 0);
        browser.cancel();
        browser.cancel();
    }
    void secondaryFallbackIsBoundedAndGenerationScoped()
    {
        int secondary = 0;
        QByteArray captured;
        RHBrowserLogin browser([&](QByteArray token, QDateTime) { captured = token; ++secondary; });
        browser.setPrimaryTokenHandler([](QByteArray) { QFAIL("Unexpected primary"); });
        QQuickWebEngineProfile oldProfile;
        QVERIFY(browser.start()); browser.attachProfile(&oldProfile);
        browser.navigationChanged(browser.loginUrl());
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "old-secret");
        cookie.setDomain("robinhood.com"); cookie.setPath("/"); cookie.setSecure(true);
        QVERIFY(browser.acceptCookie(cookie));
        QCOMPARE(secondary, 0);
        const auto old = browser.m_generation;
        browser.cancel();
        QVERIFY(browser.m_stagedSecondary.isEmpty());
        QVERIFY(!browser.m_secondaryFallback.isActive());
        QQuickWebEngineProfile newProfile;
        QVERIFY(browser.start()); browser.attachProfile(&newProfile);
        browser.navigationChanged(browser.loginUrl());
        browser.acceptPrimary(old, "stale-primary");
        cookie.setValue("new-secret");
        QVERIFY(browser.acceptCookie(cookie));
        QVERIFY(browser.m_secondaryFallback.remainingTime() > 7500);
        QVERIFY(browser.m_secondaryFallback.remainingTime() <= 10000);
        QTRY_COMPARE_WITH_TIMEOUT(secondary, 1, 10500);
        QCOMPARE(captured, QByteArray("new-secret"));
        QVERIFY(browser.m_stagedSecondary.isEmpty());
        browser.acceptPrimary(browser.m_generation, "late-primary");
        QVERIFY(!browser.acceptCookie(cookie));
        QCOMPARE(secondary, 1);
        browser.finish();
    }
    void observesHttpOnlyCookieFromIsolatedNativeProfile()
    {
        // Use the same fresh, off-the-record profile constructor and policies
        // as DesktopWebLogin.qml, not the shared default profile or test backend.
        QQuickWebEngineProfile profile;
        profile.setOffTheRecord(true);
        profile.setPersistentCookiesPolicy(QQuickWebEngineProfile::NoPersistentCookies);
        profile.setHttpCacheType(QQuickWebEngineProfile::MemoryHttpCache);
        auto *store = profile.cookieStore();
        QSignalSpy nativeAdded(store, &QWebEngineCookieStore::cookieAdded);
        QVERIFY(nativeAdded.isValid());
        int captures = 0;
        QByteArray captured;
        RHBrowserLogin browser([&](QByteArray value, QDateTime) {
            ++captures;
            captured = std::move(value);
        });
        QCOMPARE(browser.backend(), QStringLiteral("webengine"));
        QVERIFY(browser.start());
        browser.attachProfile(&profile);
        QVERIFY2(browser.active(), qPrintable(browser.error()));
        // Set only the bridge's navigation guard; this does not navigate a view.
        const QUrl origin(QStringLiteral("https://robinhood.com/"));
        browser.navigationChanged(origin);

        // Chromium initializes the profile's cookie backend lazily, when a view
        // first uses the profile. Merely calling setCookie/loadAllCookies on an
        // unused profile leaves these operations pending. Initialize with a blank
        // view, deliberately NOT bound to the bridge's brokerage startUrl.
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import QtWebEngine
            WebEngineView {
                required property WebEngineProfile fixtureProfile
                profile: fixtureProfile
                url: "about:blank"
                onNavigationRequested: request => {
                    if (request.url.toString() === "about:blank")
                        request.accept()
                    else
                        request.reject()
                }
            }
        )", QUrl());
        QTRY_VERIFY_WITH_TIMEOUT(component.status() != QQmlComponent::Loading, 5000);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> view(component.createWithInitialProperties({
            {QStringLiteral("fixtureProfile"), QVariant::fromValue(&profile)}
        }));
        QVERIFY2(view, qPrintable(component.errorString()));
        QCOMPARE(view->property("url").toUrl(), QUrl(QStringLiteral("about:blank")));
        QVERIFY(profile.isOffTheRecord());
        QCOMPARE(profile.persistentCookiesPolicy(), QQuickWebEngineProfile::NoPersistentCookies);
        QCOMPARE(profile.httpCacheType(), QQuickWebEngineProfile::MemoryHttpCache);

        // Inject synthetic data through Chromium, not observeCookie() or an
        // emitted signal. No brokerage page/request, login, or keychain access.
        QNetworkCookie cookie("__Host-Web-App-Secondary-Access-Token", "native-cookie-fixture");
        cookie.setPath(QStringLiteral("/"));
        cookie.setSecure(true);
        cookie.setHttpOnly(true);
        cookie.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(600));
        store->setCookie(cookie, origin);
        QTRY_COMPARE_WITH_TIMEOUT(nativeAdded.size(), 1, 5000);
        const auto nativeCookie = qvariant_cast<QNetworkCookie>(nativeAdded.at(0).at(0));
        QCOMPARE(nativeCookie.name(), cookie.name());
        QCOMPARE(nativeCookie.value(), cookie.value());
        QCOMPARE(nativeCookie.domain(), QStringLiteral("robinhood.com"));
        QCOMPARE(nativeCookie.path(), QStringLiteral("/"));
        QVERIFY(nativeCookie.isSecure());
        QVERIFY(nativeCookie.isHttpOnly());
        QTRY_COMPARE_WITH_TIMEOUT(captures, 1, 5000);
        QCOMPARE(captured, QByteArray("native-cookie-fixture"));

        browser.finish();
        QVERIFY(!browser.active());
        // Prove the native store still delivers events after finish(), while
        // the disconnected bridge does not capture another token.
        cookie.setValue("native-cookie-after-finish");
        store->setCookie(cookie, origin);
        QTRY_COMPARE_WITH_TIMEOUT(nativeAdded.size(), 2, 5000);
        QTest::qWait(100);
        QCOMPARE(captures, 1);
        QSignalSpy nativeRemoved(store, &QWebEngineCookieStore::cookieRemoved);
        store->deleteAllCookies();
        QTRY_COMPARE_WITH_TIMEOUT(nativeRemoved.size(), 1, 5000);
    }
};

int main(int argc, char *argv[])
{
    RHBrowserLogin::prepareApplication();
    QGuiApplication app(argc, argv);
    RHBrowserLogin::initializeApplication();
    RHBrowserNativeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_rhbrowser_native.moc"
