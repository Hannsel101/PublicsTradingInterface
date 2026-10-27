#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <atomic>
#include <memory>

// Tokens cross only this C++ boundary, never the Qt meta-object/QML boundary.
class RHBrowserLogin : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString backend READ backend CONSTANT)
    Q_PROPERTY(QUrl viewSource READ viewSource CONSTANT)
    Q_PROPERTY(QUrl loginUrl READ loginUrl CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QUrl startUrl READ startUrl NOTIFY startUrlChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    using TokenHandler = std::function<void(QByteArray, QDateTime)>;
    using PrimaryTokenHandler = std::function<void(QByteArray)>;
    // Configure on the owning QObject thread, before start(). Not a QML API.
    // Primary capture is currently DISABLED: native request metadata and a
    // distinct secondary cookie do not establish primary/trading provenance.
    // The callback is never invoked. Kept for C++ consumer compatibility; when
    // configured, the legacy callback receives the latest secondary after the
    // existing bounded 9 s fallback. That delay is not a provenance guarantee.
    // Consumers must treat the secondary as read-only and validate API identity
    // before persistence, then finish()/cancel(). No QML token transport.
    void setPrimaryTokenHandler(PrimaryTokenHandler handler);
    enum class BackendMode { Automatic, Test };
    explicit RHBrowserLogin(TokenHandler handler = {}, QObject *parent = nullptr,
                            BackendMode mode = BackendMode::Automatic);
    ~RHBrowserLogin() override;
    bool available() const;
    bool active() const;
    QString backend() const;
    QUrl viewSource() const;
    QUrl loginUrl() const;
    QUrl startUrl() const;
    QString error() const;
    bool start();
    void cancel();
    void finish();
    static void prepareApplication();
    static void initializeApplication();
    Q_INVOKABLE void attachProfile(QObject *profile);
    Q_INVOKABLE void detachProfile(QObject *profile);
    Q_INVOKABLE void navigationChanged(const QUrl &url);
    Q_INVOKABLE bool isAllowedNavigation(const QUrl &url) const;
    Q_INVOKABLE void pageFailed();
    bool observeCookie(const QNetworkCookie &cookie);
signals:
    void activeChanged();
    void startUrlChanged();
    void errorChanged();
private:
    friend class TestRHBrowserLogin;
    friend class RHBrowserNativeTest;
    using Headers = QList<QPair<QByteArray, QByteArray>>;
    // Syntax/origin filter only, NOT authorization evidence; no native collector is installed.
    static QByteArray primaryCandidate(const QUrl &url, const QUrl &firstParty,
        const QUrl &initiator, const QByteArray &method, bool redirect, const Headers &headers);
    void acceptPrimary(quint64 generation, QByteArray token);
    PrimaryTokenHandler m_primaryHandler;
    static bool parseAndroidCookie(const QByteArray &header, QNetworkCookie &cookie, bool &present);
    void androidPollResult(quint64 generation, const QByteArray &header, bool nativeOk);
    void setError(const QString &message);
    void setStartUrl(const QUrl &url);
    void stop();
    void disconnectProfile();
    bool acceptCookie(const QNetworkCookie &cookie);
    void pollAndroid();
    void clearAndroidCookie();
    TokenHandler m_handler;
    BackendMode m_mode;
    bool m_active = false;
    bool m_captureDelivered = false;
    bool m_trustedNavigation = false;
    bool m_initialBlank = true;
    bool m_resetting = false;
    bool m_pollPending = false;
    quint64 m_generation = 0;
    // Native-thread work checks an atomic epoch, never QObject state off-thread.
    std::shared_ptr<std::atomic<quint64>> m_nativeEpoch = std::make_shared<std::atomic<quint64>>(0);
    QPointer<QObject> m_profile;
    QMetaObject::Connection m_cookieConnection;
    QMetaObject::Connection m_cookieRemovedConnection;
    QMetaObject::Connection m_profileDestroyed;
    QUrl m_startUrl{QStringLiteral("about:blank")};
    QString m_error;
    QTimer m_poll;
    QTimer m_resetTimeout;
    QTimer m_secondaryFallback;
    QElapsedTimer m_secondaryClock;
    QByteArray m_stagedSecondary;
    QDateTime m_stagedExpiry;
    QMetaObject::Connection m_fallbackConnection;
    void clearStagedSecondary();
    void deliverSecondary(QByteArray value, QDateTime expiry);
    QElapsedTimer m_resetClock;
};
