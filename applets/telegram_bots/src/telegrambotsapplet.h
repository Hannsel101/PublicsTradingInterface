#pragma once
#include "appletinterface.h"
#include <QPointer>

class TelegramBotsController;
class TelegramPublicsTradeService;

class TelegramBotsApplet final : public AppletInterface
{
    Q_OBJECT
public:
    explicit TelegramBotsApplet(QObject *parent = nullptr);
    TelegramBotsApplet(TelegramBotsController *controller, QObject *parent = nullptr);
    TelegramBotsApplet(TelegramBotsController *controller, TelegramPublicsTradeService *service,
                       QObject *parent = nullptr);
    QString id() const override { return QStringLiteral("telegram_bots"); }
    void initialize(QQmlApplicationEngine &engine) override;
    void start() override;
    bool canStop() const override;
    void stop() override;
private:
    void connectController();
    void connectTrading();
    void syncTradingBusy();
    QPointer<TelegramBotsController> m_controller;
    QPointer<TelegramPublicsTradeService> m_trades;
    QString m_originBotId;
    QString m_tradeRequestId;
};
