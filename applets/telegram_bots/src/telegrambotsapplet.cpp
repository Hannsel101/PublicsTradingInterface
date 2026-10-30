#include "telegrambotsapplet.h"
#include "telegrambotscontroller.h"
#include "telegrampublicstradeservice.h"
#include <QQmlApplicationEngine>
#include <QQmlContext>

TelegramBotsApplet::TelegramBotsApplet(QObject *parent)
    : AppletInterface(parent), m_controller(new TelegramBotsController(this)),
      m_trades(new TelegramPublicsTradeService(this)) { connectController(); connectTrading(); }

TelegramBotsApplet::TelegramBotsApplet(TelegramBotsController *controller, QObject *parent)
    : TelegramBotsApplet(controller, nullptr, parent) {}

TelegramBotsApplet::TelegramBotsApplet(TelegramBotsController *controller,
                                     TelegramPublicsTradeService *service, QObject *parent)
    : AppletInterface(parent), m_controller(controller), m_trades(service)
{ connectController(); connectTrading(); }

void TelegramBotsApplet::connectController()
{
    if (m_controller)
        connect(m_controller, &TelegramBotsController::stateChanged,
                this, &AppletInterface::navigationAvailabilityChanged);
}

void TelegramBotsApplet::syncTradingBusy()
{
    if (m_controller)
        m_controller->setTradingBusy(!m_tradeRequestId.isEmpty() || (m_trades && m_trades->busy()));
}

void TelegramBotsApplet::connectTrading()
{
    if (!m_controller) return;
    connect(m_controller, &TelegramBotsController::tradeRequested, this,
        [this](QString botId, QString requestId, QString side, QStringList symbols) {
        const QPointer<TelegramBotsApplet> guard(this);
        const auto reject = [this, &botId, &requestId](const QString &reason) {
            if (m_controller) m_controller->completeTradeCommand(botId, requestId, {reason});
        };
        if (!m_trades) {
            reject(QStringLiteral("🔴 Publics trading is unavailable. No order was submitted."));
            return;
        }
        if (!m_tradeRequestId.isEmpty() || m_trades->busy()) {
            reject(QStringLiteral("🔴 Another Publics batch is running. This command was not submitted."));
            return;
        }
        // Pin the origin before any backend or direct state notification can reenter.
        m_originBotId = botId;
        m_tradeRequestId = requestId;
        syncTradingBusy();
        if (!guard || !m_trades) return;
        const bool accepted = m_trades->submit(requestId, std::move(symbols), std::move(side));
        if (!guard || accepted) return;
        m_originBotId.clear();
        m_tradeRequestId.clear();
        reject(QStringLiteral("🔴 Publics could not accept this command. No order was submitted."));
        if (guard) syncTradingBusy();
    });
    if (!m_trades) return;
    connect(m_trades, &TelegramPublicsTradeService::busyChanged,
            this, &TelegramBotsApplet::syncTradingBusy);
    connect(m_trades, &TelegramPublicsTradeService::completed, this,
        [this](QString requestId, QStringList messages) {
        if (requestId != m_tradeRequestId || requestId.isEmpty()) return;
        const QString botId = m_originBotId;
        m_originBotId.clear();
        m_tradeRequestId.clear();
        const QPointer<TelegramBotsApplet> guard(this);
        if (m_controller) m_controller->completeTradeCommand(botId, requestId, std::move(messages));
        if (guard) syncTradingBusy();
    });
    connect(m_trades, &QObject::destroyed, this, [this] {
        const QString botId = m_originBotId;
        const QString requestId = m_tradeRequestId;
        m_originBotId.clear();
        m_tradeRequestId.clear();
        const QPointer<TelegramBotsApplet> guard(this);
        if (m_controller && !requestId.isEmpty())
            m_controller->completeTradeCommand(botId, requestId,
                {QStringLiteral("🟡 Publics batch outcome is unknown. Check Public before submitting again.")});
        if (guard) syncTradingBusy();
    });
    syncTradingBusy();
}

void TelegramBotsApplet::initialize(QQmlApplicationEngine &engine)
{
    engine.rootContext()->setContextProperty(QStringLiteral("TelegramBots"), m_controller.data());
}

void TelegramBotsApplet::start()
{
    if (m_controller) m_controller->load();
}

bool TelegramBotsApplet::canStop() const
{
    return m_controller && !m_controller->busy() && !m_controller->hasRunningBots();
}

void TelegramBotsApplet::stop()
{
    if (m_controller && m_controller->hasRunningBots()) m_controller->stopAll();
}
