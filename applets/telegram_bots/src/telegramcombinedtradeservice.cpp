#include "telegramcombinedtradeservice.h"
#include "telegrampublicstradeservice.h"
#include "telegramrobinhoodtradeservice.h"

#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <utility>

namespace {
template<class T>
TelegramCombinedTradeService::Stage borrowedStage(T *service)
{
    const QPointer<T> guard(service);
    return {service,
            [guard](QString id, QStringList symbols, QString side,
                    TelegramCombinedTradeService::Completion) {
                return guard && guard->submit(std::move(id), std::move(symbols), std::move(side));
            },
            [guard] { return guard && guard->busy(); }};
}
}

TelegramCombinedTradeService::TelegramCombinedTradeService(
    TelegramPublicsTradeService *publics, TelegramRobinhoodTradeService *robinhood, QObject *parent)
    : TelegramCombinedTradeService(Dependencies{borrowedStage(publics), borrowedStage(robinhood)}, parent)
{
    if (publics) {
        connect(publics, &TelegramPublicsTradeService::busyChanged, this, &TelegramCombinedTradeService::notifyBusy);
        connect(publics, &TelegramPublicsTradeService::completed, this,
                [this](QString id, QStringList messages) {
            queueResult(Phase::Publics, m_generation, std::move(id), std::move(messages));
        });
    }
    if (robinhood) {
        connect(robinhood, &TelegramRobinhoodTradeService::busyChanged, this, &TelegramCombinedTradeService::notifyBusy);
        connect(robinhood, &TelegramRobinhoodTradeService::completed, this,
                [this](QString id, QStringList messages) {
            queueResult(Phase::Robinhood, m_generation, std::move(id), std::move(messages));
        });
    }
}

TelegramCombinedTradeService::TelegramCombinedTradeService(Dependencies dependencies, QObject *parent)
    : QObject(parent), m_dependencies(std::move(dependencies))
{
    m_lastBusy = busy();
    observeLifetimes();
}

void TelegramCombinedTradeService::observeLifetimes()
{
    for (const auto phase : {Phase::Publics, Phase::Robinhood}) {
        const auto &stage = phase == Phase::Publics ? m_dependencies.publics : m_dependencies.robinhood;
        if (!stage.lifetime) continue;
        connect(stage.lifetime, &QObject::destroyed, this, [this, phase] {
            if (m_phase == phase)
                queueResult(phase, m_generation, m_stageId, unavailableReport(phase));
            else notifyBusy();
        });
    }
}

bool TelegramCombinedTradeService::busy() const
{
    const auto running = [](const Stage &stage) {
        return stage.lifetime && stage.busy && stage.busy();
    };
    return m_phase != Phase::Idle || running(m_dependencies.publics) || running(m_dependencies.robinhood);
}

void TelegramCombinedTradeService::notifyBusy()
{
    const bool current = busy();
    if (current == m_lastBusy) return;
    m_lastBusy = current;
    emit busyChanged();
}

bool TelegramCombinedTradeService::submit(QString requestId, QStringList symbols, QString side)
{
    if (busy() || requestId.trimmed().isEmpty() || symbols.isEmpty()) return false;
    side = side.trimmed().toUpper();
    if (side != QStringLiteral("BUY") && side != QStringLiteral("SELL")) return false;
    static const QRegularExpression ticker(QStringLiteral("\\A[A-Za-z]+\\z"));
    // Preserve the complete validated snapshot, including order, spelling and duplicates.
    for (const auto &symbol : symbols)
        if (!ticker.match(symbol).hasMatch()) return false;
    m_requestId = std::move(requestId);
    m_symbols = std::move(symbols);
    m_side = std::move(side);
    m_phase = Phase::Publics;
    const auto generation = ++m_generation;
    const QPointer<TelegramCombinedTradeService> guard(this);
    notifyBusy();
    if (!guard) return true;
    QTimer::singleShot(0, this, [this, generation] { startStage(Phase::Publics, generation); });
    return true;
}

void TelegramCombinedTradeService::startStage(Phase phase, quint64 generation)
{
    if (generation != m_generation || m_phase != phase) return;
    m_stageRefused = false;
    m_resultQueued = false;
    m_stageId = m_requestId + QStringLiteral("/combined/%1/%2/%3")
        .arg(generation).arg(phase == Phase::Publics ? QStringLiteral("publics") : QStringLiteral("robinhood"),
                             QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto stageId = m_stageId;
    const auto stage = phase == Phase::Publics ? m_dependencies.publics : m_dependencies.robinhood;
    if (!stage.lifetime || !stage.submit) {
        queueResult(phase, generation, stageId, unavailableReport(phase));
        return;
    }
    const QPointer<TelegramCombinedTradeService> guard(this);
    const auto complete = [guard, phase, generation](QString id, QStringList messages) {
        if (guard) guard->queueResult(phase, generation, std::move(id), std::move(messages));
    };
    m_submitting = true;
    const bool accepted = stage.submit(stageId, m_symbols, m_side, complete);
    if (!guard || generation != m_generation || m_phase != phase) return;
    m_submitting = false;
    m_stageRefused = !accepted;
    auto deferred = std::exchange(m_deferredReport, std::nullopt);
    if (deferred) m_resultQueued = false; // The nested delivery consumed the queued callback.
    if (!accepted) queueResult(phase, generation, stageId, unavailableReport(phase));
    else if (deferred) queueResult(phase, generation, stageId, std::move(*deferred));
}

void TelegramCombinedTradeService::queueResult(Phase phase, quint64 generation,
                                               QString stageId, QStringList messages)
{
    if (generation != m_generation || phase != m_phase || stageId.isEmpty()
        || stageId != m_stageId || m_resultQueued) return;
    m_resultQueued = true;
    QTimer::singleShot(0, this, [this, phase, generation, stageId = std::move(stageId),
                                messages = std::move(messages)]() mutable {
        receiveResult(phase, generation, stageId, std::move(messages));
    });
}

void TelegramCombinedTradeService::receiveResult(Phase phase, quint64 generation,
                                                 const QString &stageId, QStringList messages)
{
    if (generation != m_generation || phase != m_phase || stageId != m_stageId) return;
    // A nested event loop in submit must not publish an uncommitted admission.
    if (m_submitting) {
        if (!m_deferredReport) m_deferredReport = std::move(messages);
        return;
    }
    if (m_stageRefused) messages = unavailableReport(phase);
    const auto requestId = m_requestId;
    if (phase == Phase::Publics) {
        m_phase = Phase::WaitingReport;
        m_stageId.clear();
        emit reportReady(requestId, messages);
        return;
    }
    // Retire every old field before direct idle/completed observers can replace us.
    m_phase = Phase::Idle;
    m_requestId.clear();
    m_stageId.clear();
    m_symbols.clear();
    m_side.clear();
    m_deferredReport.reset();
    m_resultQueued = false;
    m_submitting = false;
    m_stageRefused = false;
    const QPointer<TelegramCombinedTradeService> guard(this);
    notifyBusy();
    if (guard) emit completed(requestId, messages);
}

void TelegramCombinedTradeService::continueAfterReport(QString requestId)
{
    if (m_phase != Phase::WaitingReport || requestId != m_requestId) return;
    m_phase = Phase::Robinhood; // Consume the boundary before scheduling or notifying.
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] { startStage(Phase::Robinhood, generation); });
}

QStringList TelegramCombinedTradeService::unavailableReport(Phase phase) const
{
    const QString broker = phase == Phase::Publics ? QStringLiteral("Publics") : QStringLiteral("Robinhood");
    const QString action = m_side == QStringLiteral("SELL") ? QStringLiteral("Sell") : QStringLiteral("Buy");
    return {QStringLiteral("<b>%1 Auto %2 Results</b>\n\n🔴 %1 trade service unavailable; outcome could not be confirmed. Check %1 before submitting again.")
                .arg(broker, action)};
}
