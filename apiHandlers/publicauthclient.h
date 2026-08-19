#ifndef PUBLICAUTHCLIENT_H
#define PUBLICAUTHCLIENT_H

// QT6
#include <QObject>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QtQml>
#include <QDebug>

// Windows API
#include <windows.h>
#include <wincred.h>

class PublicAuthClient : public QObject {
    Q_OBJECT
public:

    // session active is set to true when tokens are returned by the authorization request
    Q_PROPERTY(bool sessionActive READ sessionActive WRITE setSessionActive NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QStringList secretKeys READ secretKeys WRITE setSecretKeys NOTIFY secretKeysChanged FINAL)

    /*
     * Default constructor which accepts a secret key or can be left as empty
     * */
    PublicAuthClient(const QString secretKey = "", QObject *parent = nullptr)
        : QObject(parent), m_secretKey(secretKey) {}

    /*
     * If a non empty secret key has been passed in by the user then a network request
     * is sent to public api brokerage to receive session tokens
     *
     * Session tokens are only active for a set amount of time between 5 and 60 minutes
     * before another session token needs to be requested. This time can be configured.
     * */
    Q_INVOKABLE void requestToken();

    /*
     * List and securely store API keys into windows credential manager
     * */
    QStringList listStoredApiKeys();
    bool storeNextApiKey(const QString &userName, const QString &apiKey);


    /*
     * Setters and getters
     * */
    bool sessionActive() const;
    void setSessionActive(bool newSessionActive);

    QStringList secretKeys() const;
    void setSecretKeys(const QStringList &newSecretKeys);

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

    void secretKeysChanged();

private:
    QString m_secretKey;
    QStringList m_secretKeys;
    QNetworkAccessManager m_manager;
    bool m_sessionActive = false;
};

#endif // PUBLICAUTHCLIENT_H
