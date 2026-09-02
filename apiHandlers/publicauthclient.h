#ifndef PUBLICAUTHCLIENT_H
#define PUBLICAUTHCLIENT_H

// QT6
#include <QObject>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QAbstractNetworkCache>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDebug>
#include <QSettings>
#include <QStringList>

#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <keychain.h>
#endif

class PublicAuthClient : public QObject {
    Q_OBJECT
public:

    // session active is set to true when tokens are returned by the authorization request
    Q_PROPERTY(bool sessionActive READ sessionActive WRITE setSessionActive NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QStringList secretKeys READ secretKeys WRITE setSecretKeys NOTIFY secretKeysChanged FINAL)
    Q_PROPERTY(QString secretKey READ secretKey WRITE setSecretKey NOTIFY secretKeyChanged FINAL)
    Q_PROPERTY(QString selectedApiKeyLabel READ selectedApiKeyLabel NOTIFY selectedApiKeyLabelChanged FINAL)
    Q_PROPERTY(bool apiKeyReady READ apiKeyReady NOTIFY apiKeyReadyChanged FINAL)
    Q_PROPERTY(bool apiKeyLoading READ apiKeyLoading NOTIFY apiKeyLoadingChanged FINAL)
    Q_PROPERTY(QString apiKeyError READ apiKeyError NOTIFY apiKeyErrorChanged FINAL)

    /*
     * Default constructor which accepts a secret key or can be left as empty
     * */
    explicit PublicAuthClient(const QString &secretKey = "", QObject *parent = nullptr);
    PublicAuthClient(const QString &secretKey,
                     const QString &keychainServiceName,
                     const QString &settingsGroupName,
                     QObject *parent = nullptr);

    /*
     * If a non empty secret key has been passed in by the user then a network request
     * is sent to public api brokerage to receive session tokens
     *
     * Session tokens are only active for a set amount of time between 5 and 60 minutes
     * before another session token needs to be requested. This time can be configured.
     * */
    Q_INVOKABLE void requestToken();

    /*
     * List API key labels and securely store API keys in the platform keychain.
     * */
    Q_INVOKABLE QStringList listStoredApiKeys();
    Q_INVOKABLE bool storeNextApiKey(const QString &userName, const QString &apiKey);

    static QStringList normalizedStoredApiKeyLabels(const QStringList &labels);
    static QString nextApiKeyLabel(const QStringList &labels);

    /*
     * Clears QNetwork access and cookies cache to ensure smooth transition
     * between api keys and TCP connections to Public's API Endpoints
     * */
    Q_INVOKABLE void clearUserSession();


    /*
     * Setters and getters
     * */
    bool sessionActive() const;
    void setSessionActive(bool newSessionActive);

    QStringList secretKeys() const;
    void setSecretKeys(const QStringList &newSecretKeys);

    QString secretKey() const;
    void setSecretKey(const QString &newSecretKey);

    QString selectedApiKeyLabel() const;
    bool apiKeyReady() const;
    bool apiKeyLoading() const;
    QString apiKeyError() const;

private slots:
    /*
     * through a signal and slots interaction processes an incoming authorization request
     * reply from the publics server.
     *
     * On success the session tokens are stored and {sessionActiveChanged()} is emitted
     * On failure the error reason prints to the terminal
     *
     * TO DO: Add an error indicator for the end user so they know what error occurred
     * */
    void handleReply(QNetworkReply *reply);

signals:
    /*
     * emits that authorization tokens were received and passes the tokens through the
     * signal
     * */
    void tokenReceived(const QString &token);

    /*
     * emits that authrization tokens were received. Used for front end GUI interactions
     * to occur.
     * */
    void sessionActiveChanged();

    /*
     * emits signal for the UI to update when the list of secret api keys changes
     * */
    void secretKeysChanged();

    /*
     * emits signal to ensure the UI knows which secret key is now being used
     * */
    void secretKeyChanged();
    void selectedApiKeyLabelChanged();
    void apiKeyReadyChanged();
    void apiKeyLoadingChanged();
    void apiKeyErrorChanged();

private:
    static constexpr const char *defaultKeychainService = "PublicsTradingInterface";
    static constexpr const char *defaultSettingsGroup = "publicsApiKeys";

    QString m_secretKey;
    QStringList m_secretKeys;
    QNetworkAccessManager *m_manager = nullptr;
    bool m_sessionActive = false;
    QString m_selectedApiKeyLabel;
    bool m_apiKeyReady = false;
    bool m_apiKeyLoading = false;
    QString m_apiKeyError;
    QString m_keychainService;
    QString m_settingsGroup;

    QString keychainKeyForLabel(const QString &label) const;
    void readApiKeyFromKeychain(const QString &label);
    void setSelectedApiKeyLabel(const QString &label);
    void setApiKeyReady(bool ready);
    void setApiKeyLoading(bool loading);
    void setApiKeyError(const QString &error);
};

#endif // PUBLICAUTHCLIENT_H
