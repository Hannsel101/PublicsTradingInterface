#pragma once

#include "rhoauthgrant.h"
#include <QObject>
#include <QUrl>
#include <functional>
#include <memory>

// Isolated compatibility spike. No identity or trading authorization is inferred.
class NativeAuthClient : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Requesting, WaitingForCode, WaitingForApproval, Succeeded, Failed, Cancelled };
    struct TestTransport {
        QUrl origin;
        int requestTimeoutMs = 16000;
        int workflowTimeoutMs = 120000;
        int pollIntervalMs = 5000;
    };
    explicit NativeAuthClient(QObject *parent = nullptr);
    // Only explicit numeric loopback origins are accepted; invalid input fails closed.
    explicit NativeAuthClient(TestTransport transport, QObject *parent = nullptr);
    ~NativeAuthClient() override;
    bool start(const QString &username, const QString &password, const QString &deviceToken,
               const QString &mfaCode = {});
    void submitCode(const QString &code);
    void cancel();
    State state() const;
    bool busy() const;
    QString statusText() const;
    using GrantHandler = std::function<void(RHOAuthGrant)>;
    void setGrantHandler(GrantHandler handler);
signals:
    void changed();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
