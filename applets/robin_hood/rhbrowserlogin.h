#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
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
    QMetaObject::Connection m_profileDestroyed;
    QUrl m_startUrl{QStringLiteral("about:blank")};
    QString m_error;
    QTimer m_poll;
    QTimer m_resetTimeout;
    QElapsedTimer m_resetClock;
};
