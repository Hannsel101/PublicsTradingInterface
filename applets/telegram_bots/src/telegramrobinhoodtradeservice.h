#pragma once
#include "rhtradecontroller.h"
#include <QObject>
#include <QStringList>
#include <memory>

class RHSessionManager;
// QObject-thread confined. Injected dependencies are borrowed and must remain on
// this thread. The default graph is independent of the applet's UI selection and
// is created only after an authorized submit, never during service construction.
class TelegramRobinhoodTradeService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    explicit TelegramRobinhoodTradeService(QObject *parent = nullptr);
    TelegramRobinhoodTradeService(RHNetworkEngine *network, RHSessionManager *sessions,
                                 RHTradeController::Dependencies dependencies, QObject *parent = nullptr);
    ~TelegramRobinhoodTradeService() override;
    bool submit(QString requestId, QStringList symbols, QString side);
    bool busy() const;
signals:
    void busyChanged();
    void completed(QString requestId, QStringList htmlMessages);
private:
    struct State;
    std::unique_ptr<State> d;
};
