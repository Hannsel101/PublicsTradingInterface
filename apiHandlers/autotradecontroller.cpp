#include "autotradecontroller.h"
#include "publicauthclient.h"
#include "publicapiworker.h"

#include <QTimer>

namespace {
constexpr int betweenKeysMs = 3000;
}

AutoTradeController::AutoTradeController(PublicAuthClient *authClient, PublicApiWorker *worker,
                                         QObject *parent)
    : QObject(parent), m_authClient(authClient), m_worker(worker)
{
    connect(authClient, &PublicAuthClient::secretKeysChanged, this, &AutoTradeController::keyLabelsChanged);
    connect(authClient, &PublicAuthClient::apiKeyLoadingChanged, this, [this]() {
        if (m_stage == Stage::LoadingKey && !m_authClient->apiKeyLoading())
            QTimer::singleShot(0, this, &AutoTradeController::checkKeyLoaded);
    });
    connect(authClient, &PublicAuthClient::tokenReceived, this, [this](const QString &) {
        if (m_stage == Stage::Authorizing) m_stage = Stage::LoadingAccounts;
    });
    connect(authClient, &PublicAuthClient::authorizationFailed, this, [this](const QString &reason) {
        if (m_stage == Stage::Authorizing) failCurrentKey(reason);
    });
    connect(worker, &PublicApiWorker::accountLoadFinished, this, [this](bool success, const QString &reason) {
        if (m_stage != Stage::LoadingAccounts) return;
        if (!success) {
            failCurrentKey(reason);
            return;
        }
        m_stage = Stage::Trading;
#ifdef RELEASE
        m_worker->executeTrade(m_symbol, m_side);
#else
        m_worker->executePreflight(m_symbol, m_side);
#endif
        checkTradeComplete();
    });
    connect(worker, &PublicApiWorker::tradeResultsCompleteChanged, this,
            &AutoTradeController::checkTradeComplete);
    connect(worker, &PublicApiWorker::tradeResultsChanged, this,
            &AutoTradeController::syncAccountResults);
}

QStringList AutoTradeController::keyLabels() const { return m_authClient->secretKeys(); }
QVariantList AutoTradeController::tradeResults() const { return m_results; }
QString AutoTradeController::currentTransactionTitle() const { return m_title; }
bool AutoTradeController::tradeResultsVisible() const { return m_visible; }
bool AutoTradeController::tradeResultsComplete() const { return m_complete; }
bool AutoTradeController::tradeResultsBusy() const { return !m_complete; }

void AutoTradeController::dismissTradeResults()
{
    if (!m_complete || !m_visible) return;
    m_visible = false;
    emit tradeResultsVisibleChanged();
}

void AutoTradeController::submit(const QString &symbol, const QString &side)
{
    if (!m_complete || m_authClient->apiKeyIndexLoading()) return;
    m_symbol = symbol.trimmed().toUpper();
    m_side = side.trimmed().toUpper();
    if (m_symbol.isEmpty() || (m_side != QStringLiteral("BUY") && m_side != QStringLiteral("SELL"))) return;

    m_labels = keyLabels();
    m_nextKey = 0;
    m_results.clear();
#ifdef RELEASE
    const QString mode = QStringLiteral("Market");
#else
    const QString mode = QStringLiteral("Preflight");
#endif
    m_title = QStringLiteral("%1 %2 of %3 across all keys")
                  .arg(mode, m_side == QStringLiteral("BUY") ? QStringLiteral("Buy") : QStringLiteral("Sell"), m_symbol);
    for (const QString &label : m_labels)
        setKeyResult(label, QStringLiteral("pending"), QStringLiteral("Waiting to load key…"));
    if (m_labels.isEmpty())
        setKeyResult({}, QStringLiteral("failed"), QStringLiteral("No saved API keys are available."));
    m_complete = m_labels.isEmpty();
    m_visible = true;
    emit tradeResultsChanged();
    emit tradeResultsCompleteChanged();
    emit tradeResultsVisibleChanged();
    if (!m_complete) startNextKey();
}

void AutoTradeController::startNextKey()
{
    m_currentLabel = m_labels.at(m_nextKey++);
    m_stage = Stage::LoadingKey;
    m_authClient->setSecretKey(m_currentLabel);
    checkKeyLoaded();
}

void AutoTradeController::checkKeyLoaded()
{
    if (m_stage != Stage::LoadingKey || m_authClient->apiKeyLoading()) return;
    if (m_authClient->selectedApiKeyLabel() != m_currentLabel || !m_authClient->apiKeyReady()) {
        failCurrentKey(m_authClient->apiKeyError().isEmpty()
                           ? QStringLiteral("Could not load the saved API key.") : m_authClient->apiKeyError());
        return;
    }
    m_stage = Stage::Authorizing;
    m_authClient->requestToken();
}

void AutoTradeController::checkTradeComplete()
{
    if (m_stage != Stage::Trading || !m_worker->tradeResultsComplete()) return;

    syncAccountResults();
    finishCurrentKey();
}

void AutoTradeController::syncAccountResults()
{
    if (m_stage != Stage::Trading || m_worker->tradeResults().isEmpty()) return;

    const QVariantList accountResults = m_worker->tradeResults();
    int insertionIndex = m_results.size();
    for (int i = m_results.size() - 1; i >= 0; --i) {
        if (m_results.at(i).toMap().value(QStringLiteral("keyLabel")).toString() != m_currentLabel) continue;
        insertionIndex = i;
        m_results.removeAt(i);
    }
    for (const QVariant &item : accountResults) {
        QVariantMap row = item.toMap();
        row.insert(QStringLiteral("keyLabel"), m_currentLabel);
        row.insert(QStringLiteral("accountLabel"), QStringLiteral("%1 · %2")
                       .arg(m_currentLabel, row.value(QStringLiteral("accountLabel")).toString()));
        m_results.insert(insertionIndex++, row);
    }
    emit tradeResultsChanged();
}

void AutoTradeController::failCurrentKey(const QString &message)
{
    setKeyResult(m_currentLabel, QStringLiteral("failed"), message);
    finishCurrentKey();
}

void AutoTradeController::finishCurrentKey()
{
    // Close and invalidate the completed key's session before the inter-key pause.
    m_stage = Stage::CoolingDown;
    m_authClient->clearUserSession();
    m_authClient->setSecretKey(QString());
    m_worker->clearSubAccountsList();
    if (m_nextKey == m_labels.size()) {
        m_stage = Stage::Idle;
        m_complete = true;
        emit tradeResultsCompleteChanged();
    } else {
        QTimer::singleShot(betweenKeysMs, Qt::PreciseTimer, this, [this]() {
            if (m_stage == Stage::CoolingDown) startNextKey();
        });
    }
}

void AutoTradeController::setKeyResult(const QString &label, const QString &status,
                                       const QString &message)
{
    QVariantMap row{{QStringLiteral("keyLabel"), label},
                    {QStringLiteral("accountId"), QString()},
                    {QStringLiteral("accountType"), QString()},
                    {QStringLiteral("accountLabel"), label.isEmpty() ? QStringLiteral("No saved key") : label},
                    {QStringLiteral("status"), status}, {QStringLiteral("message"), message}};
    for (int i = 0; i < m_results.size(); ++i) {
        const QVariantMap previous = m_results.at(i).toMap();
        if (previous.value(QStringLiteral("keyLabel")).toString() == label
            && previous.value(QStringLiteral("accountId")).toString().isEmpty()) {
            m_results[i] = row;
            emit tradeResultsChanged();
            return;
        }
    }
    m_results.append(row);
    emit tradeResultsChanged();
}
