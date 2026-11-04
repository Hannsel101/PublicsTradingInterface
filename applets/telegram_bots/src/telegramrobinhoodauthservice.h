#pragma once

#include <QObject>
#include <QStringList>
#include <memory>

class RHSessionManager;

// Native saved-credential authentication only. The caller owns Telegram
// authorization and settles the correlated progress attempt before continuing.
class TelegramRobinhoodAuthService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
public:
    explicit TelegramRobinhoodAuthService(QObject *parent = nullptr);
    explicit TelegramRobinhoodAuthService(RHSessionManager *sessions,
                                         QObject *parent = nullptr, int timeoutMs = 480000);
    ~TelegramRobinhoodAuthService() override;
    bool submit(QString requestId, QString email);
    bool busy() const;
    void continueAfterProgress(QString requestId);
    void cancel();
signals:
    void busyChanged();
    void reportReady(QString requestId, QStringList htmlMessages);
    void completed(QString requestId, QStringList htmlMessages);
private:
    struct State;
    std::unique_ptr<State> d;
};
