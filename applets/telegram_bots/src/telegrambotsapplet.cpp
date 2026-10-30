#include "telegrambotsapplet.h"
#include "telegrambotscontroller.h"
#include "telegrampublicstradeservice.h"
#include "telegramrobinhoodtradeservice.h"
#include "telegramcombinedtradeservice.h"
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <type_traits>
#include <utility>

TelegramBotsApplet::TelegramBotsApplet(QObject *parent)
    : AppletInterface(parent), m_controller(new TelegramBotsController(this)),
      m_trades(new TelegramPublicsTradeService(this)),
      m_robinhood(new TelegramRobinhoodTradeService(this)),
      m_combined(new TelegramCombinedTradeService(m_trades, m_robinhood, this))
{ connectController(); connectTrading(); }

TelegramBotsApplet::TelegramBotsApplet(TelegramBotsController *controller, QObject *parent)
    : TelegramBotsApplet(controller, nullptr, nullptr, parent) {}

TelegramBotsApplet::TelegramBotsApplet(TelegramBotsController *controller,
                                     TelegramPublicsTradeService *service, QObject *parent)
    : TelegramBotsApplet(controller, service, nullptr, parent) {}

TelegramBotsApplet::TelegramBotsApplet(TelegramBotsController *controller,
                                     TelegramPublicsTradeService *publics,
                                     TelegramRobinhoodTradeService *robinhood, QObject *parent)
    : AppletInterface(parent), m_controller(controller), m_trades(publics), m_robinhood(robinhood),
      m_combined(new TelegramCombinedTradeService(publics, robinhood, this))
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
        m_controller->setTradingBusy(m_tradeOrigin.has_value()
            || (m_trades && m_trades->busy()) || (m_robinhood && m_robinhood->busy())
            || (m_combined && m_combined->busy()));
}

void TelegramBotsApplet::connectTrading()
{
    if (!m_controller) return;
    const auto bind = [this](auto *service, auto requested, Broker broker, const QString &name) {
        using Service = std::remove_pointer_t<decltype(service)>;
        const QPointer<Service> domain(service);
        connect(m_controller, requested, this,
            [this, domain, broker, name](QString botId, QString requestId, QString side, QStringList symbols) {
            const QPointer<TelegramBotsApplet> guard(this);
            const auto reject = [this, &botId, &requestId](const QString &reason) {
                if (m_controller) m_controller->completeTradeCommand(botId, requestId, {reason});
            };
            if (!domain) {
                reject(QStringLiteral("🔴 %1 trading is unavailable. No order was submitted.").arg(name));
                return;
            }
            if (m_tradeOrigin || (m_trades && m_trades->busy()) || (m_robinhood && m_robinhood->busy())
                || (m_combined && m_combined->busy())) {
                reject(QStringLiteral("🔴 Another trading batch is running. This command was not submitted."));
                return;
            }
            // Both brokers share one lease; pin the origin before synchronous notifications.
            m_tradeOrigin = TradeOrigin{botId, requestId, broker};
            syncTradingBusy();
            if (!guard || !domain) return;
            const bool accepted = domain->submit(requestId, std::move(symbols), std::move(side));
            if (!guard || accepted) return;
            // A synchronous completion may already have admitted a replacement job.
            if (!m_tradeOrigin || m_tradeOrigin->broker != broker || m_tradeOrigin->requestId != requestId) return;
            m_tradeOrigin.reset();
            reject(QStringLiteral("🔴 %1 could not accept this command. No order was submitted.").arg(name));
            if (guard) syncTradingBusy();
        });
        if (!service) return;
        connect(service, &Service::busyChanged, this, &TelegramBotsApplet::syncTradingBusy);
        connect(service, &Service::completed, this,
            [this, broker](QString requestId, QStringList messages) {
            if (!m_tradeOrigin || m_tradeOrigin->broker != broker
                || m_tradeOrigin->requestId != requestId || requestId.isEmpty()) return;
            const QString botId = m_tradeOrigin->botId;
            m_tradeOrigin.reset();
            const QPointer<TelegramBotsApplet> guard(this);
            if (m_controller) m_controller->completeTradeCommand(botId, requestId, std::move(messages));
            if (guard) syncTradingBusy();
        });
        connect(service, &QObject::destroyed, this, [this, broker, name] {
            if (!m_tradeOrigin || m_tradeOrigin->broker != broker) { syncTradingBusy(); return; }
            const auto origin = std::exchange(m_tradeOrigin, std::nullopt);
            const QPointer<TelegramBotsApplet> guard(this);
            if (m_controller)
                m_controller->completeTradeCommand(origin->botId, origin->requestId,
                    {QStringLiteral("🟡 %1 batch outcome is unknown. Check the broker before submitting again.").arg(name)});
            if (guard) syncTradingBusy();
        });
    };
    bind(m_trades.data(), &TelegramBotsController::tradeRequested, Broker::Publics, QStringLiteral("Publics"));
    bind(m_robinhood.data(), &TelegramBotsController::robinhoodTradeRequested, Broker::Robinhood, QStringLiteral("Robinhood"));
    bind(m_combined.data(), &TelegramBotsController::combinedTradeRequested, Broker::Combined, QStringLiteral("Combined"));
    if (m_combined) {
        connect(m_combined, &TelegramCombinedTradeService::reportReady, this,
            [this](QString requestId, QStringList messages) {
            if (!m_tradeOrigin || m_tradeOrigin->broker != Broker::Combined
                || m_tradeOrigin->requestId != requestId) return;
            const auto botId = m_tradeOrigin->botId;
            const QPointer<TelegramBotsApplet> guard(this);
            const bool admitted = m_controller
                && m_controller->reportTradeCommand(botId, requestId, std::move(messages));
            if (!guard || admitted) return;
            // Lost notification context cannot replay or strand the already-admitted job.
            QTimer::singleShot(0, this, [this, requestId] {
                if (m_combined && m_tradeOrigin && m_tradeOrigin->broker == Broker::Combined
                    && m_tradeOrigin->requestId == requestId)
                    m_combined->continueAfterReport(requestId);
            });
        });
        connect(m_controller, &TelegramBotsController::tradeReportFinished, this,
            [this](QString botId, QString requestId) {
            if (m_combined && m_tradeOrigin && m_tradeOrigin->broker == Broker::Combined
                && m_tradeOrigin->botId == botId && m_tradeOrigin->requestId == requestId)
                m_combined->continueAfterReport(requestId);
        });
    }
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
