#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>
#include <optional>

class TelegramPublicsTradeService;
class TelegramRobinhoodTradeService;

// Thread-confined coordinator; services are borrowed, never cancelled or owned.
// Publics report delivery must settle before the caller explicitly continues.
class TelegramCombinedTradeService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
public:
    using Completion = std::function<void(QString, QStringList)>;
    // Narrow offline seam: a broker-stage admission and its terminal reports only.
    // lifetime and callbacks must share this object's thread; busy is a pure query.
    struct Stage {
        QPointer<QObject> lifetime;
        std::function<bool(QString, QStringList, QString, Completion)> submit;
        std::function<bool()> busy;
    };
    struct Dependencies { Stage publics; Stage robinhood; };

    TelegramCombinedTradeService(TelegramPublicsTradeService *publics,
                                 TelegramRobinhoodTradeService *robinhood,
                                 QObject *parent = nullptr);
    explicit TelegramCombinedTradeService(Dependencies dependencies, QObject *parent = nullptr);
    // requestId is the caller's unique command identity; do not reuse it for a new job.
    bool submit(QString requestId, QStringList symbols, QString side);
    bool busy() const;
    void continueAfterReport(QString requestId);

signals:
    void busyChanged();
    void reportReady(QString requestId, QStringList htmlMessages);
    void completed(QString requestId, QStringList htmlMessages);

private:
    enum class Phase { Idle, Publics, WaitingReport, Robinhood };
    void observeLifetimes();
    void notifyBusy();
    void startStage(Phase phase, quint64 generation);
    void queueResult(Phase phase, quint64 generation, QString stageId, QStringList messages);
    void receiveResult(Phase phase, quint64 generation, const QString &stageId, QStringList messages);
    QStringList unavailableReport(Phase phase) const;

    Dependencies m_dependencies;
    Phase m_phase = Phase::Idle;
    quint64 m_generation = 0;
    QString m_requestId;
    QString m_stageId;
    QStringList m_symbols;
    QString m_side;
    std::optional<QStringList> m_deferredReport;
    bool m_resultQueued = false;
    bool m_submitting = false;
    bool m_stageRefused = false;
    bool m_lastBusy = false;
};
