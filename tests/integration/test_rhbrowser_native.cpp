#include "rhbrowserlogin.h"

#include <QGuiApplication>
#include <QNetworkCookie>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QtTest/QtTest>
#include <memory>

class RHBrowserNativeTest final : public QObject
{
    Q_OBJECT
private slots:
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
