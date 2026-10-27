#include "rhbrowserlogin.h"
#include <QScopeGuard>
#if defined(Q_OS_ANDROID) && defined(PUBLICS_RH_ANDROID_WEBVIEW)
#include <QtCore/qcoreapplication_platform.h>
#include <QJniObject>
#include <QVariantMap>
#include <QtWebView/QtWebView>
#endif
#if defined(PUBLICS_RH_WEBENGINE)
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <QtWebEngineCore/QWebEngineCookieStore>
#endif
#include <utility>

namespace {
constexpr int captureWaitMs = 9000;
void wipe(QByteArray &value)
{
    // Best effort: browser-owned and implicitly shared Qt copies are not ours.
    value.fill('\0');
    value.clear();
}
}

namespace {
const QByteArray cookieName = QByteArrayLiteral("__Host-Web-App-Secondary-Access-Token");
bool safeValue(const QByteArray &value)
{
    if (value.isEmpty() || value.size() > 16 * 1024) return false;
    for (const unsigned char byte : value) {
        if (byte < 0x21 || byte > 0x7e || byte == '"' || byte == ','
            || byte == ';' || byte == '\\') return false;
    }
    return true;
}
}

void RHBrowserLogin::setPrimaryTokenHandler(PrimaryTokenHandler handler)
{
    if (!m_active) m_primaryHandler = std::move(handler);
}

QByteArray RHBrowserLogin::primaryCandidate(const QUrl &url, const QUrl &firstParty,
    const QUrl &initiator, const QByteArray &method, bool redirect, const Headers &headers)
{
    const auto official = [](const QUrl &origin) {
        return origin.isValid() && origin.scheme() == QStringLiteral("https")
            && origin.authority() == QStringLiteral("robinhood.com")
            && !origin.hasFragment();
    };
    if (method != "GET" || redirect || !url.isValid()
        || url.scheme() != QStringLiteral("https")
        || url.authority() != QStringLiteral("api.robinhood.com")
        || url.hasFragment() || url.hasQuery()
        || (url.path(QUrl::FullyEncoded) != QStringLiteral("/user/")
            && url.path(QUrl::FullyEncoded) != QStringLiteral("/accounts/"))
        || !official(firstParty) || !official(initiator)) return {};
    QByteArray authorization;
    const auto discardAuthorization = qScopeGuard([&authorization] { wipe(authorization); });
    bool found = false;
    for (const auto &header : headers) {
        if (header.first.compare("Authorization", Qt::CaseInsensitive) != 0) continue;
        if (found) return {};
        found = true;
        authorization = header.second;
    }
    if (authorization.left(7).compare("Bearer ", Qt::CaseInsensitive) != 0) return {};
    auto token = authorization.sliced(7);
    bool accepted = false;
    const auto discardToken = qScopeGuard([&token, &accepted] { if (!accepted) wipe(token); });
    if (token.isEmpty() || token.size() > 16 * 1024) return {};
    bool padding = false;
    for (const unsigned char byte : token) {
        if (byte == '=') { padding = true; continue; }
        if (padding || !((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_'
            || byte == '~' || byte == '+' || byte == '/')) return {};
    }
    if (token.front() == '=') return {};
    accepted = true;
    return token;
}

void RHBrowserLogin::acceptPrimary(quint64 generation, QByteArray token)
{
    Q_UNUSED(generation);
    // Fail closed, including already queued native observations. A bearer on
    // /user/ or /accounts/ proves neither token class nor trading permission;
    // a later cookieAdded event may identify it as a refreshed secondary.
    // No documented native/provider provenance contract is available here.
    // Never retain candidates or promote them after a timeout/quiet period.
    wipe(token);
}

bool RHBrowserLogin::parseAndroidCookie(const QByteArray &header, QNetworkCookie &cookie, bool &present)
{
    present = false;
    QByteArray value;
    for (auto field : header.split(';')) {
        while (field.startsWith(' ') || field.startsWith('\t')) field.remove(0, 1);
        const auto equals = field.indexOf('=');
        if (equals < 0 || field.first(equals) != cookieName) continue;
        if (present) return false;
        present = true;
        value = field.sliced(equals + 1);
    }
    if (!present) return true;
    if (!safeValue(value)) return false;
    cookie = QNetworkCookie(cookieName, value);
    cookie.setDomain(QStringLiteral("robinhood.com"));
    cookie.setSecure(true);
    cookie.setPath(QStringLiteral("/"));
    // CookieManager supplies neither expiry nor attributes. __Host- is enforced
    // by the native browser; invalid expiry deliberately requests the C++ fallback.
    return true;
}

RHBrowserLogin::RHBrowserLogin(TokenHandler handler, QObject *parent, BackendMode mode)
    : QObject(parent), m_handler(std::move(handler)), m_mode(mode)
{
    m_poll.setInterval(500);
    m_poll.setTimerType(Qt::PreciseTimer);
    connect(&m_poll, &QTimer::timeout, this, &RHBrowserLogin::pollAndroid);
    m_resetTimeout.setSingleShot(true);
    m_resetTimeout.setInterval(5000);
    m_resetTimeout.setTimerType(Qt::PreciseTimer);
    connect(&m_resetTimeout, &QTimer::timeout, this, [this] {
        if (!m_active || !m_resetting) return;
        stop();
        setError(QStringLiteral("Timed out clearing the previous sign-in cookie."));
    });
}
RHBrowserLogin::~RHBrowserLogin() { stop(); }
bool RHBrowserLogin::available() const { return backend() != QStringLiteral("unavailable"); }
bool RHBrowserLogin::active() const { return m_active; }
QString RHBrowserLogin::backend() const
{
    if (m_mode == BackendMode::Test) return QStringLiteral("test");
#if defined(Q_OS_ANDROID)
    // QtWebView URL notifications arrive after dispatch, not before navigation.
    // Fail closed until a native pre-navigation interceptor is verified. Keep
    // the JNI cookie bridge available for that future guarded implementation.
    return QStringLiteral("unavailable");
#elif defined(PUBLICS_RH_WEBENGINE)
    return QStringLiteral("webengine");
#else
    return QStringLiteral("unavailable");
#endif
}
QUrl RHBrowserLogin::viewSource() const
{
    if (backend() == QStringLiteral("webengine"))
        return QUrl(QStringLiteral("qrc:/qt/qml/RobinHoodInterface/DesktopWebLogin.qml"));
    if (backend() == QStringLiteral("android-webview"))
        return QUrl(QStringLiteral("qrc:/qt/qml/RobinHoodInterface/AndroidWebLogin.qml"));
    return {};
}
QUrl RHBrowserLogin::loginUrl() const { return QUrl(QStringLiteral("https://robinhood.com/")); }
QUrl RHBrowserLogin::startUrl() const { return m_startUrl; }
QString RHBrowserLogin::error() const { return m_error; }
void RHBrowserLogin::setError(const QString &message) { if (m_error != message) { m_error = message; emit errorChanged(); } }
void RHBrowserLogin::setStartUrl(const QUrl &url) { if (m_startUrl != url) { m_startUrl = url; emit startUrlChanged(); } }
bool RHBrowserLogin::start()
{
    if (!available()) {
#if defined(Q_OS_ANDROID)
        setError(QStringLiteral("Secure Android browser sign-in is unavailable: a verified native pre-navigation interceptor is required."));
#else
        setError(QStringLiteral("Embedded browser sign-in is unavailable in this Qt build."));
#endif
        return false;
    }
    if (m_active) return false;
    ++m_generation;
    m_nativeEpoch->store(m_generation, std::memory_order_release);
    m_active = true;
    m_captureDelivered = false;
    m_secondaryClock.invalidate();
    m_initialBlank = true;
    m_trustedNavigation = m_mode == BackendMode::Test;
    setError({});
    setStartUrl(m_mode == BackendMode::Test ? loginUrl() : QUrl(QStringLiteral("about:blank")));
    emit activeChanged();
    if (m_active && backend() == QStringLiteral("android-webview")) {
        m_resetting = true;
        m_resetClock.start();
        m_resetTimeout.start();
        clearAndroidCookie();
        m_poll.start();
        pollAndroid();
    }
    return m_active;
}
void RHBrowserLogin::disconnectProfile()
{
    disconnect(m_cookieConnection);
    disconnect(m_cookieRemovedConnection);
    disconnect(m_profileDestroyed);
    m_profile = nullptr;
}
void RHBrowserLogin::stop()
{
    // Only an active cancellation advances the epoch. Reentrant cancel() and
    // destruction must not invalidate that cancellation's queued native cleanup;
    // start() advances it again before a new login can acquire a cookie.
    if (!m_active) return;
    ++m_generation;
    m_nativeEpoch->store(m_generation, std::memory_order_release);
    clearStagedSecondary();
    m_secondaryClock.invalidate();
    m_poll.stop();
    m_resetTimeout.stop();
    disconnectProfile();
    m_trustedNavigation = false;
    m_resetting = false;
    m_pollPending = false;
    const bool wasActive = std::exchange(m_active, false);
    if (wasActive) clearAndroidCookie();
    setStartUrl(QUrl(QStringLiteral("about:blank")));
    if (wasActive) emit activeChanged();
}
void RHBrowserLogin::cancel() { stop(); }
void RHBrowserLogin::finish() { stop(); }
void RHBrowserLogin::prepareApplication()
{
#if defined(PUBLICS_RH_WEBENGINE)
    QtWebEngineQuick::initialize();
#endif
}
void RHBrowserLogin::initializeApplication()
{
#if defined(Q_OS_ANDROID) && defined(PUBLICS_RH_ANDROID_WEBVIEW)
    QtWebView::initialize();
#endif
}
void RHBrowserLogin::attachProfile(QObject *object)
{
    if (!m_active || m_mode == BackendMode::Test) return;
#if defined(PUBLICS_RH_WEBENGINE)
    auto *profile = qobject_cast<QQuickWebEngineProfile *>(object);
    if (profile && m_profile == profile) return;
    if (!profile || !profile->isOffTheRecord() || m_profile
        || profile->property("_publicsRhLoginUsed").toBool()) {
        stop();
        setError(QStringLiteral("Sign-in requires a fresh, off-the-record browser profile."));
        return;
    }
    profile->setProperty("_publicsRhLoginUsed", true);
    m_profile = profile; // QML owns the profile, never this bridge.
    const auto generation = m_generation;
    // Primary capture is disabled: request metadata has no authoritative token
    // class/permission provenance. Do not install a header interceptor at all.
    const QPointer<QQuickWebEngineProfile> guardedProfile(profile);
    m_cookieConnection = connect(profile->cookieStore(), &QWebEngineCookieStore::cookieAdded,
        this, [this, generation, guardedProfile](const QNetworkCookie &cookie) {
            if (!m_active || generation != m_generation || !guardedProfile
                || m_profile != guardedProfile || !guardedProfile->isOffTheRecord()) return;
            acceptCookie(cookie);
        }, Qt::QueuedConnection);
    m_cookieRemovedConnection = connect(profile->cookieStore(), &QWebEngineCookieStore::cookieRemoved,
        this, [this, generation](const QNetworkCookie &cookie) {
            if (!m_active || m_captureDelivered || generation != m_generation
                || cookie.name() != cookieName || cookie.domain() != QStringLiteral("robinhood.com")
                || cookie.path() != QStringLiteral("/") || !cookie.isSecure()) return;
            if (cookie.value() == m_stagedSecondary) clearStagedSecondary();
        }, Qt::QueuedConnection);
    m_profileDestroyed = connect(profile, &QObject::destroyed, this, [this, generation] {
        if (m_active && generation == m_generation) {
            stop();
            setError(QStringLiteral("The sign-in browser was closed."));
        }
    });
    setStartUrl(loginUrl());
#else
    Q_UNUSED(object);
#endif
}
void RHBrowserLogin::detachProfile(QObject *profile)
{
    if (profile && m_profile == profile) stop();
}
void RHBrowserLogin::navigationChanged(const QUrl &url)
{
    if (!m_active) return;
    if (!isAllowedNavigation(url)) {
        stop();
        setError(QStringLiteral("Navigation outside the trusted Robinhood HTTPS site was blocked."));
        return;
    }
    m_trustedNavigation = url.scheme() == QStringLiteral("https");
    if (m_trustedNavigation) m_initialBlank = false;
}
bool RHBrowserLogin::isAllowedNavigation(const QUrl &url) const
{
    if (url == QUrl(QStringLiteral("about:blank"))) return m_active && m_initialBlank;
    if (!url.isValid() || url.scheme() != QStringLiteral("https")
        || url.authority().contains(QLatin1Char('@'))
        || (url.port(-1) != -1 && url.port(-1) != 443)) return false;
    const auto host = url.host();
    return host == QStringLiteral("robinhood.com")
        || (host.endsWith(QStringLiteral(".robinhood.com"))
            && !host.startsWith(QLatin1Char('.')) && !host.contains(QStringLiteral("..")));
}
void RHBrowserLogin::pageFailed()
{
    if (!m_active) return;
    stop();
    setError(QStringLiteral("The embedded browser could not load the sign-in page. Embedded-browser challenges may prevent sign-in."));
}
bool RHBrowserLogin::observeCookie(const QNetworkCookie &cookie)
{
    return m_mode == BackendMode::Test && acceptCookie(cookie);
}
bool RHBrowserLogin::acceptCookie(const QNetworkCookie &cookie)
{
    if (!m_active || m_captureDelivered || !m_trustedNavigation
        || cookie.name() != QByteArrayLiteral("__Host-Web-App-Secondary-Access-Token")
        || cookie.domain() != QStringLiteral("robinhood.com")
        || !cookie.isSecure() || cookie.path() != QStringLiteral("/")
        || (cookie.expirationDate().isValid()
            && cookie.expirationDate() <= QDateTime::currentDateTimeUtc())) return false;
    const auto value = cookie.value();
    if (value.isEmpty() || value.size() > 16 * 1024) return false;
    // RFC 6265 cookie-octet: no whitespace, controls, quotes, separators or non-ASCII.
    for (const unsigned char byte : value) {
        if (byte < 0x21 || byte > 0x7e || byte == '"' || byte == ','
            || byte == ';' || byte == '\\') return false;
    }
    if (m_primaryHandler) {
        // Keep only the latest validated secondary, within the original fallback
        // bound. This timer preserves legacy read-only delivery, NOT provenance.
        const bool waiting = m_secondaryFallback.isActive();
        wipe(m_stagedSecondary);
        m_stagedSecondary = value;
        m_stagedExpiry = cookie.expirationDate();
        if (waiting) return true;
        if (!m_secondaryClock.isValid()) m_secondaryClock.start();
        const auto generation = m_generation;
        m_secondaryFallback.setSingleShot(true);
        m_secondaryFallback.setTimerType(Qt::PreciseTimer);
        m_fallbackConnection = connect(&m_secondaryFallback, &QTimer::timeout, this,
            [this, generation] {
                if (!m_active || m_captureDelivered || generation != m_generation) return;
                auto staged = std::move(m_stagedSecondary);
                const auto discard = qScopeGuard([&staged] { wipe(staged); });
                const auto expiry = m_stagedExpiry;
                clearStagedSecondary();
                if (staged.isEmpty() || (expiry.isValid()
                    && expiry <= QDateTime::currentDateTimeUtc())) return;
                deliverSecondary(std::move(staged), expiry);
            });
        m_secondaryFallback.start(int(qMax<qint64>(0, captureWaitMs - m_secondaryClock.elapsed())));
        return true;
    }
    deliverSecondary(value, cookie.expirationDate());
    return true;
}
void RHBrowserLogin::clearStagedSecondary()
{
    m_secondaryFallback.stop();
    disconnect(m_fallbackConnection);
    // Best effort only: Qt/browser buffers can hold implicitly shared copies.
    wipe(m_stagedSecondary);
    m_stagedExpiry = {};
}
void RHBrowserLogin::deliverSecondary(QByteArray value, QDateTime expiry)
{
    const auto discard = qScopeGuard([&value] { wipe(value); });
    clearStagedSecondary();
    m_captureDelivered = true;
    m_poll.stop();
    m_resetTimeout.stop();
    disconnect(m_cookieConnection);
    disconnect(m_cookieRemovedConnection);
    // Keep the QML-owned profile and active state alive while the C++ consumer
    // verifies and stores the token. Only its finish()/cancel() closes the view.
    const auto handler = m_handler;
    if (handler) handler(std::move(value), expiry);
}
void RHBrowserLogin::androidPollResult(quint64 generation, const QByteArray &header, bool nativeOk)
{
    if (!m_active || m_captureDelivered || generation != m_generation) return;
    m_pollPending = false;
    if (!nativeOk) {
        stop();
        setError(QStringLiteral("The native sign-in cookie service is unavailable."));
        return;
    }
    QNetworkCookie cookie;
    bool present = false;
    const bool valid = parseAndroidCookie(header, cookie, present);
    if (m_resetting) {
        if (m_resetClock.hasExpired(5000)) {
            stop();
            setError(QStringLiteral("Timed out clearing the previous sign-in cookie."));
        } else if (!present && valid) {
            m_resetting = false;
            m_resetTimeout.stop();
            setStartUrl(loginUrl());
        }
        return;
    }
    if (valid && present && m_trustedNavigation) acceptCookie(cookie);
}
void RHBrowserLogin::pollAndroid()
{
#if defined(Q_OS_ANDROID) && defined(PUBLICS_RH_ANDROID_WEBVIEW)
    if (m_mode == BackendMode::Test || !m_active || m_pollPending
        || (!m_resetting && !m_trustedNavigation)) return;
    m_pollPending = true;
    const auto generation = m_generation;
    const auto epoch = m_nativeEpoch;
    const QPointer<RHBrowserLogin> guard(this);
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread(
        [epoch, generation]() -> QVariant {
            if (epoch->load(std::memory_order_acquire) != generation) return {};
            const auto manager = QJniObject::callStaticObjectMethod(
                "android/webkit/CookieManager", "getInstance", "()Landroid/webkit/CookieManager;");
            if (!manager.isValid()) return QVariantMap{{QStringLiteral("ok"), false}};
            const auto url = QJniObject::fromString(QStringLiteral("https://robinhood.com/"));
            const auto header = manager.callObjectMethod(
                "getCookie", "(Ljava/lang/String;)Ljava/lang/String;", url.object<jstring>());
            // A null string is the native API's normal empty-jar result.
            return QVariantMap{{QStringLiteral("ok"), true},
                               {QStringLiteral("header"), header.isValid() ? header.toString().toUtf8() : QByteArray{}}};
        }).then(this, [guard, generation](const QVariant &result) {
            if (!guard) return;
            const auto fields = result.toMap();
            guard->androidPollResult(generation, fields.value(QStringLiteral("header")).toByteArray(),
                                     fields.value(QStringLiteral("ok")).toBool());
        });
#endif
}
void RHBrowserLogin::clearAndroidCookie()
{
#if defined(Q_OS_ANDROID) && defined(PUBLICS_RH_ANDROID_WEBVIEW)
    if (m_mode == BackendMode::Test) return;
    const auto generation = m_generation;
    const auto epoch = m_nativeEpoch;
    // QtWebView/Android shares the application's native CookieManager. There is
    // no per-WebView profile isolation here. Delete ONLY this host-only token;
    // never removeAllCookies() (which would also affect the Publics applet).
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([epoch, generation] {
        if (epoch->load(std::memory_order_acquire) != generation) return;
        const auto manager = QJniObject::callStaticObjectMethod(
            "android/webkit/CookieManager", "getInstance", "()Landroid/webkit/CookieManager;");
        if (!manager.isValid()) return;
        const auto url = QJniObject::fromString(QStringLiteral("https://robinhood.com/"));
        const auto deletion = QJniObject::fromString(QStringLiteral(
            "__Host-Web-App-Secondary-Access-Token=; Max-Age=0; Path=/; Secure"));
        manager.callMethod<void>("setCookie", "(Ljava/lang/String;Ljava/lang/String;)V",
                                 url.object<jstring>(), deletion.object<jstring>());
        // No flush(): do not force credential cookies onto persistent storage.
    });
#endif
}
