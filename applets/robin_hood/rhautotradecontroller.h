#pragma once
#include "rhtradecontroller.h"
#include <memory>

class RHSessionManager;

// QObject-thread confined. Borrows the applet's existing network/session graph;
// both may disappear during a batch (tracked with QPointer), never owned here.
class RHAutoTradeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool liveOrdersEnabled READ liveOrdersEnabled CONSTANT)
    Q_PROPERTY(QVariantList tradeResults READ tradeResults NOTIFY stateChanged)
    Q_PROPERTY(QString currentTransactionTitle READ currentTransactionTitle NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool tradeResultsComplete READ tradeResultsComplete NOTIFY stateChanged)
public:
    RHAutoTradeController(RHNetworkEngine &, RHSessionManager &, QObject *parent = nullptr);
    RHAutoTradeController(RHNetworkEngine &, RHSessionManager &, RHTradeController::Dependencies,
                          QObject *parent = nullptr);
    ~RHAutoTradeController() override;
    bool available() const;
    bool busy() const;
    bool liveOrdersEnabled() const;
    QVariantList tradeResults() const;
    QString currentTransactionTitle() const;
    QString error() const;
    bool tradeResultsComplete() const;
    Q_INVOKABLE bool submit(QString input, QString side);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearResults();
    void setExternalBusy(bool busy);
signals:
    void stateChanged();
private:
    struct State;
    std::unique_ptr<State> d;
};
