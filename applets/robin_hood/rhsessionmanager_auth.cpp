#include "rhsessionmanager.h"
#include "rhnetworkengine.h"
#include "rhbrowserlogin.h"
#include "securecredentiallocker.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <utility>

bool RHSessionManager::signIn(const QString &, const QString &, const QString &)
{
    setError(QStringLiteral("Password sign-in is disabled. Use browser sign-in."));
    return false;
}

void RHSessionManager::setBrowserLoginPending(bool pending)
{
    if (m_browserLoginPending == pending)
        return;
    m_browserLoginPending = pending;
    emit browserLoginPendingChanged();
}

void RHSessionManager::setBrowserLogin(RHBrowserLogin *browser)
{
    if (m_browser == browser || (m_busy && !m_browserLoginPending))
        return;
    cancelSignIn();
    if (m_browser)
        disconnect(m_browser, nullptr, this, nullptr);
    m_browser = browser;
    if (!browser)
        return;
    connect(browser, &RHBrowserLogin::activeChanged, this, [this] {
        if (m_browserLoginPending && m_browser && !m_browser->active())
            failLogin(QStringLiteral("Browser sign-in ended before verification completed."));
    });
    connect(browser, &RHBrowserLogin::errorChanged, this, [this] {
        if (m_browserLoginPending && m_browser && !m_browser->error().isEmpty())
            failLogin(QStringLiteral("Browser sign-in failed. Please retry."));
    });
    connect(browser, &QObject::destroyed, this, [this] {
        if (m_browserLoginPending)
            failLogin(QStringLiteral("Browser sign-in is unavailable."));
    });
}

bool RHSessionManager::beginBrowserLogin(const QString &accountId)
{
    if (m_busy || !m_loaded || !m_network.isConfigured()) {
        setError(QStringLiteral("Cannot sign in while busy or without network configuration."));
        return false;
    }
    if (!m_browser || !m_browser->available()) {
        setError(QStringLiteral("Secure browser sign-in is unavailable. Desktop requires QtWebEngineQuick. Android QtWebView sign-in is disabled until native pre-navigation enforcement is implemented."));
        return false;
    }
    if (!accountId.isEmpty()) {
        bool found = false;
        for (const Session &session : m_sessions)
            found |= (session.accountId.isEmpty() ? session.accountName : session.accountId) == accountId;
        if (!found) {
            setError(QStringLiteral("The selected saved account is unavailable."));
            return false;
        }
    }
    clearActiveSession();
    ++m_authGeneration;
    m_browserTarget = accountId;
    m_browserVerifying = false;
    setError({});
    setBusy(true);
    setReauthenticating(!accountId.isEmpty());
    setBrowserLoginPending(true);
    if (!m_browser->start()) {
        failLogin(QStringLiteral("Could not open browser sign-in. Check the installed Qt browser module."));
        return false;
    }
    return m_browserLoginPending;
}

void RHSessionManager::readBrowserEndpoint(const QString &endpoint, const QByteArray &token,
                                          std::function<void(QJsonObject)> done)
{
    QString problem;
    QNetworkReply *reply = m_network.sendAuthenticated(endpoint, token,
        RHNetworkEngine::AuthKind::WebSecondaryBearer, &problem);
    if (!reply) {
        failLogin(QStringLiteral("Browser token could not be verified using the configured read-only endpoint."));
        return;
    }
    m_reply = reply;
    const quint64 generation = m_authGeneration;
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, done = std::move(done)] {
        if (generation != m_authGeneration) {
            reply->deleteLater();
            return;
        }
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        QByteArray bytes = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        SecureCredentialLocker::wipe(bytes);
        reply->deleteLater();
        if (!ok || !document.isObject()) {
            // Never echo server bodies, token contents, URLs or error strings.
            failLogin(status == 401 || status == 403
                ? QStringLiteral("The secondary browser token is not accepted by the read-only API (HTTP %1). The session was not saved.").arg(status)
                : QStringLiteral("Browser token verification failed (HTTP %1). The session was not saved.").arg(status));
            return;
        }
        done(document.object());
    });
}

void RHSessionManager::acceptBrowserToken(QByteArray token, QDateTime expiresAt)
{
    if (!m_browserLoginPending || !m_busy || m_browserVerifying || !m_browser || !m_browser->active()) {
        SecureCredentialLocker::wipe(token);
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!RHNetworkEngine::safeBearer(token) || (expiresAt.isValid() && expiresAt <= now)) {
        SecureCredentialLocker::wipe(token);
        failLogin(QStringLiteral("Browser returned an expired or unsafe token."));
        return;
    }
    // Cookie expiry is an upper bound, not an OAuth lifetime. Session cookies
    // get one hour; even persistent cookies are capped to 24 hours.
    m_browserExpiry = expiresAt.isValid() ? qMin(expiresAt.toUTC(), now.addSecs(86400)) : now.addSecs(3600);
    m_browserVerifying = true;
    readBrowserEndpoint(QStringLiteral("UserProfile"), token, [this, token](const QJsonObject &profile) {
        const QString id = profile.value("id").toString().trimmed();
        const QString email = profile.value("email").toString().trimmed();
        const QString username = profile.value("username").toString().trimmed();
        const QString label = email.isEmpty() ? username : email;
        if (id.isEmpty() || label.isEmpty()) {
            failLogin(QStringLiteral("Browser token did not return a usable account identity."));
            return;
        }
        if (!m_browserTarget.isEmpty()) {
            const Session *selected = nullptr;
            for (const Session &session : m_sessions)
                if ((session.accountId.isEmpty() ? session.accountName : session.accountId) == m_browserTarget)
                    selected = &session;
            const bool matches = selected && (selected->accountId.isEmpty()
                ? (selected->accountName.compare(email, Qt::CaseInsensitive) == 0
                    || selected->accountName.compare(username, Qt::CaseInsensitive) == 0)
                : selected->accountId == id);
            if (!matches) {
                failLogin(QStringLiteral("Signed-in identity does not match the selected saved account."));
                return;
            }
        }
        readBrowserEndpoint(QStringLiteral("Accounts"), token, [this, token, id, label](const QJsonObject &response) {
            if (!response.value("results").isArray() || m_browserExpiry <= QDateTime::currentDateTimeUtc()) {
                failLogin(QStringLiteral("Browser token did not verify usable accounts and expiry."));
                return;
            }
            const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(m_browserExpiry);
            if (seconds <= 0) {
                failLogin(QStringLiteral("Browser token expired during verification."));
                return;
            }
            // Stop cancellation before entering the keychain/metadata transaction.
            // Keep busy throughout; do not expose a false-idle transition.
            setBrowserLoginPending(false);
            m_browserVerifying = false;
            ++m_authGeneration;
            if (m_browser)
                m_browser->finish();
            if (!saveSession(label, QString::fromLatin1(token), seconds, QStringLiteral("WebSecondaryBearer"), id, true)) {
                setReauthenticating(false);
                setBusy(false);
            }
            m_browserTarget.clear();
        });
    });
    SecureCredentialLocker::wipe(token);
}

void RHSessionManager::endApproval()
{
    ++m_authGeneration;
    setBrowserLoginPending(false);
    m_browserVerifying = false;
    m_browserTarget.clear();
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (m_browser)
        m_browser->cancel();
    m_pendingCredentials.clear();
}

void RHSessionManager::failLogin(const QString &message)
{
    endApproval();
    setReauthenticating(false);
    setError(message);
    setBusy(false);
}

void RHSessionManager::cancelSignIn()
{
    if (!m_browserLoginPending)
        return;
    endApproval();
    setReauthenticating(false);
    setError({});
    setBusy(false);
}
