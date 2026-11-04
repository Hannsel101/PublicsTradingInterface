#pragma once
#include "appletinterface.h"
#include <QPointer>
#include <optional>

class TelegramBotsController;
class TelegramPublicsTradeService;
class TelegramRobinhoodTradeService;
class TelegramCombinedTradeService;
class TelegramRobinhoodAuthService;

class TelegramBotsApplet final : public AppletInterface
{
    Q_OBJECT
public:
    explicit TelegramBotsApplet(QObject *parent = nullptr);
    TelegramBotsApplet(TelegramBotsController *controller, QObject *parent = nullptr);
    TelegramBotsApplet(TelegramBotsController *controller, TelegramPublicsTradeService *service,
                       QObject *parent = nullptr);
    TelegramBotsApplet(TelegramBotsController *controller, TelegramPublicsTradeService *publics,
                       TelegramRobinhoodTradeService *robinhood, QObject *parent = nullptr);
    TelegramBotsApplet(TelegramBotsController *controller, TelegramPublicsTradeService *publics,
                       TelegramRobinhoodTradeService *robinhood, TelegramRobinhoodAuthService *authentication,
                       QObject *parent);
    QString id() const override { return QStringLiteral("telegram_bots"); }
    void initialize(QQmlApplicationEngine &engine) override;
    void start() override;
    bool canStop() const override;
    void stop() override;
private:
    enum class Broker { Publics, Robinhood, Combined, Authentication };
    struct TradeOrigin {
        QString botId;
        QString requestId;
        Broker broker;
    };
    void connectController();
    void connectTrading();
    void connectAuthentication();
    bool operationBusy() const;
    void syncTradingBusy();
    QPointer<TelegramBotsController> m_controller;
    QPointer<TelegramPublicsTradeService> m_trades;
    QPointer<TelegramRobinhoodTradeService> m_robinhood;
    QPointer<TelegramCombinedTradeService> m_combined;
    QPointer<TelegramRobinhoodAuthService> m_authentication;
    std::optional<TradeOrigin> m_tradeOrigin;
};
