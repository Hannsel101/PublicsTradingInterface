#ifndef AUTOTRADECONTROLLER_H
#define AUTOTRADECONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QVariantList>

class PublicAuthClient;
class PublicApiWorker;

class AutoTradeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList keyLabels READ keyLabels NOTIFY keyLabelsChanged FINAL)
    Q_PROPERTY(QVariantList tradeResults READ tradeResults NOTIFY tradeResultsChanged FINAL)
    Q_PROPERTY(QString currentTransactionTitle READ currentTransactionTitle NOTIFY tradeResultsChanged FINAL)
    Q_PROPERTY(bool tradeResultsVisible READ tradeResultsVisible NOTIFY tradeResultsVisibleChanged FINAL)
    Q_PROPERTY(bool tradeResultsComplete READ tradeResultsComplete NOTIFY tradeResultsCompleteChanged FINAL)
    Q_PROPERTY(bool tradeResultsBusy READ tradeResultsBusy NOTIFY tradeResultsCompleteChanged FINAL)

public:
    explicit AutoTradeController(PublicAuthClient *authClient, PublicApiWorker *worker,
                                 QObject *parent = nullptr);

    QStringList keyLabels() const;
    QVariantList tradeResults() const;
    QString currentTransactionTitle() const;
    bool tradeResultsVisible() const;
    bool tradeResultsComplete() const;
    bool tradeResultsBusy() const;

    Q_INVOKABLE void submit(const QString &symbol, const QString &side);
    Q_INVOKABLE void dismissTradeResults();

signals:
    void keyLabelsChanged();
    void tradeResultsChanged();
    void tradeResultsVisibleChanged();
    void tradeResultsCompleteChanged();

private:
    enum class Stage { Idle, LoadingKey, Authorizing, LoadingAccounts, Trading, CoolingDown };
    void startNextKey();
    void checkKeyLoaded();
    void checkTradeComplete();
    void syncAccountResults();
    void failCurrentKey(const QString &message);
    void finishCurrentKey();
    void setKeyResult(const QString &label, const QString &status, const QString &message);

    PublicAuthClient *m_authClient;
    PublicApiWorker *m_worker;
    QStringList m_labels;
    QVariantList m_results;
    QString m_title;
    QString m_symbol;
    QString m_side;
    QString m_currentLabel;
    int m_nextKey = 0;
    bool m_visible = false;
    bool m_complete = true;
    Stage m_stage = Stage::Idle;
};

#endif
