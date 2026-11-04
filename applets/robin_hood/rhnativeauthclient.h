#pragma once

#include "rhoauthgrant.h"
#include <QObject>
#include <QUrl>
#include <functional>
#include <memory>

// Native grant acquisition only. Session manager independently verifies identity,
// accounts and persistence. No trading authorization is inferred from a grant.
class RHNativeAuthClient : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Requesting, WaitingForCode, WaitingForApproval, Succeeded, Failed, Cancelled };
    struct TestTransport {
        QUrl origin;
        int requestTimeoutMs = 16000;
        int workflowTimeoutMs = 120000;
        int pollIntervalMs = 5000;
    };
    explicit RHNativeAuthClient(QObject *parent = nullptr);
    // Only explicit numeric loopback origins are accepted; invalid input fails closed.
    explicit RHNativeAuthClient(TestTransport transport, QObject *parent = nullptr);
    ~RHNativeAuthClient() override;
    bool start(const QString &username, const QString &password, const QString &deviceToken,
               const QString &mfaCode = {});
    // Opt-in push-only login. The caller supplies its remaining total budget;
    // request deadlines and all transport/security gates remain unchanged.
    bool startRemote(const QString &username, const QString &password, const QString &deviceToken,
                     int timeoutMs = 480000);
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
