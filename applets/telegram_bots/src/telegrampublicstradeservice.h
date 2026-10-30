#ifndef TELEGRAMPUBLICSTRADESERVICE_H
#define TELEGRAMPUBLICSTRADESERVICE_H

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QStringList>
#include <functional>

class AutoTradeController;
class PublicAuthClient;

// An admission/result adapter. Brokerage execution remains owned by Publics Auto Mode.
class TelegramPublicsTradeService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
public:
    explicit TelegramPublicsTradeService(QObject *parent = nullptr);
    // The caller owns and wires the existing backend graph; it must outlive its job.
    TelegramPublicsTradeService(AutoTradeController *controller, PublicAuthClient *auth,
                               QObject *parent = nullptr);
    bool submit(QString requestId, QStringList symbols, QString side);
    bool busy() const;

signals:
    void busyChanged();
    void completed(QString requestId, QStringList htmlMessages);

private:
    friend class TelegramPublicsTradesTest;
    enum class Outcome { Submitted, Preview, Failed, NotSubmitted, Skipped, Unconfirmed, SetupFailed };
    struct ResultRow {
        QString keyLabel;
        QString nickname;
        QString accountId;
        QString accountType;
        QString symbol;
        Outcome outcome = Outcome::Unconfirmed;
    };
    void connectBackend();
    void notifyBusy();
    void startController();
    void queueCompletion();
    void finish();
    QList<ResultRow> snapshot() const;
    static QStringList formatResults(const QList<ResultRow> &rows, const QString &side);

    QPointer<AutoTradeController> m_controller;
    QPointer<PublicAuthClient> m_auth;
    QHash<QString, QString> m_nicknames;
    std::function<void()> m_indexLoader;
    QString m_requestId;
    QStringList m_symbols;
    QString m_side;
    quint64 m_generation = 0;
    bool m_active = false;
    bool m_lastBusy = false;
    bool m_waitingIndex = false;
    bool m_discoveryFailed = false;
    bool m_started = false;
    bool m_completionQueued = false;
};

#endif
