#include "telegrampublicstradeservice.h"

#include "api/publicapiworker.h"
#include "auth/publicauthclient.h"
#include "trading/autotradecontroller.h"

#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <iterator>

TelegramPublicsTradeService::TelegramPublicsTradeService(QObject *parent)
    : QObject(parent)
{
    auto *auth = new PublicAuthClient(QString(), this);
    auto *worker = new PublicApiWorker(this);
    m_auth = auth;
    m_indexLoader = [auth]() { auth->listStoredApiKeys(); };
    m_controller = new AutoTradeController(auth, worker, this);
    connect(auth, &PublicAuthClient::tokenReceived, worker, &PublicApiWorker::processToken);
    connect(auth, &PublicAuthClient::selectedApiKeyLabelChanged, worker, [auth, worker]() {
        if (auth->selectedApiKeyLabel().isEmpty()) worker->clearSubAccountsList();
    });
    connectBackend();
    // Applets are eagerly registered. Do not touch the keychain until authorized admission.
}

TelegramPublicsTradeService::TelegramPublicsTradeService(AutoTradeController *controller,
                                                       PublicAuthClient *auth, QObject *parent)
    : QObject(parent), m_controller(controller), m_auth(auth)
{
    connectBackend();
}

void TelegramPublicsTradeService::connectBackend()
{
    m_lastBusy = busy();
    if (!m_controller || !m_auth) return;
    connect(m_controller, &AutoTradeController::tradeResultsCompleteChanged, this, [this]() {
        if (m_active && m_started && m_controller->tradeResultsComplete()) queueCompletion();
        notifyBusy();
    });
    connect(m_auth, &PublicAuthClient::apiKeyIndexLoadingChanged, this, [this]() {
        if (m_active && m_waitingIndex && !m_auth->apiKeyIndexLoading())
            QTimer::singleShot(0, this, &TelegramPublicsTradeService::startController);
        notifyBusy();
    });
    connect(m_auth, &PublicAuthClient::apiKeyLoadingChanged, this, &TelegramPublicsTradeService::notifyBusy);
    const auto destroyed = [this]() { if (m_active) queueCompletion(); else notifyBusy(); };
    connect(m_controller, &QObject::destroyed, this, destroyed);
    connect(m_auth, &QObject::destroyed, this, destroyed);
}

bool TelegramPublicsTradeService::busy() const
{
    return m_active || (m_controller && m_controller->tradeResultsBusy())
        || (m_auth && (m_auth->apiKeyLoading() || m_auth->apiKeyIndexLoading()));
}

void TelegramPublicsTradeService::notifyBusy()
{
    const bool current = busy();
    if (m_lastBusy == current) return;
    m_lastBusy = current;
    emit busyChanged();
}

bool TelegramPublicsTradeService::submit(QString requestId, QStringList symbols, QString side)
{
    if (!m_controller || !m_auth || busy() || requestId.trimmed().isEmpty() || symbols.isEmpty()) return false;
    side = side.trimmed().toUpper();
    if (side != QStringLiteral("BUY") && side != QStringLiteral("SELL")) return false;
    static const QRegularExpression ticker(QStringLiteral("\\A[A-Za-z]+\\z"));
    QStringList normalized;
    QSet<QString> seen;
    // AutoTradeController removes all nonletters. Reject, never silently trade a different ticker.
    for (const auto &item : symbols) {
        const auto input = item.trimmed();
        if (!ticker.match(input).hasMatch()) return false;
        const auto symbol = input.toUpper();
        if (!seen.contains(symbol)) { normalized.append(symbol); seen.insert(symbol); }
    }
    m_active = true;
    ++m_generation;
    m_requestId = std::move(requestId);
    m_symbols = std::move(normalized);
    m_side = std::move(side);
    m_waitingIndex = bool(m_indexLoader);
    m_discoveryFailed = false;
    m_started = false;
    m_completionQueued = false;
    QPointer<TelegramPublicsTradeService> guard(this);
    notifyBusy();
    if (!guard) return true;
    if (m_waitingIndex) {
        const auto loadIndex = m_indexLoader;
        loadIndex();
        if (!guard) return true;
        // Index loading may also complete synchronously; start only outside that signal stack.
        QTimer::singleShot(0, this, &TelegramPublicsTradeService::startController);
    } else startController();
    return true; // Admission receipt, never evidence of brokerage completion.
}

void TelegramPublicsTradeService::startController()
{
    if (!m_active || m_started || m_completionQueued || !m_auth || !m_controller || m_auth->apiKeyIndexLoading()) return;
    if (m_waitingIndex && m_auth->apiKeyIndexState() != PublicAuthClient::KeyIndexState::Ready) {
        m_waitingIndex = false;
        m_discoveryFailed = true;
        queueCompletion();
        return;
    }
    m_waitingIndex = false;
    m_started = true;
    const QPointer<TelegramPublicsTradeService> guard(this);
    m_auth->refreshApiKeyNicknames();
    if (!guard || !m_auth || !m_controller) return;
    m_nicknames.clear();
    for (const auto &label : m_controller->keyLabels())
        m_nicknames.insert(label, m_auth->apiKeyDisplayName(label));
    m_controller->submit(m_symbols.join(QLatin1Char(',')), m_side);
    // A direct controller observer may destroy this adapter during submit().
    if (!guard) return;
    // No saved keys and credential loading failures can finish synchronously.
    if (m_controller && m_controller->tradeResultsComplete()) queueCompletion();
}

void TelegramPublicsTradeService::queueCompletion()
{
    if (!m_active || m_completionQueued) return;
    m_completionQueued = true;
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation]() {
        if (m_active && generation == m_generation) finish();
    });
}

QList<TelegramPublicsTradeService::ResultRow> TelegramPublicsTradeService::snapshot() const
{
    QList<ResultRow> result;
    if (m_discoveryFailed) {
        for (const auto &symbol : m_symbols)
            result.append(ResultRow{.nickname = QStringLiteral("Saved key discovery"),
                                    .symbol = symbol, .outcome = Outcome::SetupFailed});
        return result;
    }
    if (!m_controller) return result;
    const bool preview = m_controller->currentTransactionTitle().startsWith(QStringLiteral("Preflight "));
    for (const auto &value : m_controller->tradeResults()) {
        const auto row = value.toMap();
        ResultRow typed;
        typed.keyLabel = row.value(QStringLiteral("keyLabel")).toString();
        typed.nickname = m_nicknames.value(typed.keyLabel, typed.keyLabel);
        if (typed.nickname.isEmpty()) typed.nickname = QStringLiteral("No saved key");
        typed.accountId = row.value(QStringLiteral("accountId")).toString();
        typed.accountType = row.value(QStringLiteral("accountType")).toString();
        typed.symbol = row.value(QStringLiteral("symbol")).toString();
        const QString status = row.value(QStringLiteral("status")).toString();
        const bool posted = !row.value(QStringLiteral("orderId")).toString().isEmpty();
        // Backend success is preflight/acknowledgement, not a typed fill confirmation.
        if (status == QStringLiteral("success") && row.value(QStringLiteral("success")).toBool() && posted)
            typed.outcome = preview ? Outcome::Preview : Outcome::Submitted;
        else if (status == QStringLiteral("not_submitted")) typed.outcome = Outcome::NotSubmitted;
        else if (status == QStringLiteral("failed")) {
            if (typed.accountId.isEmpty() || typed.symbol.isEmpty()) typed.outcome = Outcome::SetupFailed;
            else if (!posted && m_side == QStringLiteral("SELL")) typed.outcome = Outcome::Skipped;
            else typed.outcome = Outcome::Failed;
        }
        if (typed.outcome == Outcome::SetupFailed && typed.symbol.isEmpty()) {
            // Setup rows have no ticker/account fields. Preserve the admitted ticker snapshot.
            for (const auto &symbol : m_symbols) {
                typed.symbol = symbol;
                result.append(typed);
            }
        } else result.append(std::move(typed));
    }
    return result;
}

QStringList TelegramPublicsTradeService::formatResults(const QList<ResultRow> &rows, const QString &side)
{
    struct Group {
        QString key, nickname, account;
        QStringList successes, failures, skipped, notSubmitted, unconfirmed;
        bool preview = false;
        bool setupFailed = false;
    };
    QList<Group> groups;
    for (const auto &row : rows) {
        auto it = std::find_if(groups.begin(), groups.end(), [&row](const Group &g) {
            return g.key == row.keyLabel && g.account == row.accountId;
        });
        if (it == groups.end()) {
            groups.append(Group{.key = row.keyLabel, .nickname = row.nickname, .account = row.accountId});
            it = std::prev(groups.end());
        }
        switch (row.outcome) {
        case Outcome::Submitted: it->successes.append(row.symbol); break;
        case Outcome::Preview: it->successes.append(row.symbol); it->preview = true; break;
        case Outcome::Failed: it->failures.append(row.symbol); break;
        case Outcome::Skipped: it->failures.append(row.symbol); it->skipped.append(row.symbol); break;
        case Outcome::NotSubmitted: it->failures.append(row.symbol); it->notSubmitted.append(row.symbol); break;
        case Outcome::Unconfirmed: it->unconfirmed.append(row.symbol); break;
        case Outcome::SetupFailed: it->setupFailed = true; it->failures.append(row.symbol); break;
        }
    }
    QStringList lines;
    for (const auto &g : groups) {
        const QString who = g.nickname + (g.account.isEmpty() ? QString() : QStringLiteral(" Account#") + g.account);
        if (!g.successes.isEmpty())
            lines.append(QStringLiteral("🟢 %1 Successful Trades: %2 (%3)").arg(who, g.successes.join(QStringLiteral(", ")),
                g.preview ? QStringLiteral("Preflight only; no live orders submitted")
                          : QStringLiteral("submission acknowledged; fills not confirmed")));
        if (!g.failures.isEmpty())
            lines.append(QStringLiteral("🔴 %1 Failed Trades: %2").arg(who, g.failures.join(QStringLiteral(", "))));
        if (!g.skipped.isEmpty())
            lines.append(QStringLiteral("%1 Skipped/no holdings; no sell order submitted: %2").arg(who, g.skipped.join(QStringLiteral(", "))));
        if (!g.notSubmitted.isEmpty())
            lines.append(QStringLiteral("%1 Not submitted; holdings unavailable or session ended: %2").arg(who, g.notSubmitted.join(QStringLiteral(", "))));
        if (!g.unconfirmed.isEmpty())
            lines.append(QStringLiteral("🟡 %1 Unconfirmed/pending outcomes: %2. Check Public before submitting again; fills not confirmed.")
                .arg(who, g.unconfirmed.join(QStringLiteral(", "))));
        if (g.setupFailed)
            lines.append(QStringLiteral("🔴 %1 Account/key setup failed; no trades submitted for this key/account.").arg(who));
    }
    if (lines.isEmpty()) lines.append(QStringLiteral("🔴 Account/key setup unavailable; outcome could not be confirmed. Check Public before submitting again."));
    const QString title = side == QStringLiteral("SELL") ? QStringLiteral("Publics Auto Sell Results") : QStringLiteral("Publics Auto Buy Results");
    const QString header = QStringLiteral("<b>%1</b>\n\n").arg(title);
    // Count decoded UTF-16 units conservatively (astral characters count twice).
    // Split plain text FIRST, then escape each fragment: entities/tags cannot be severed.
    const qsizetype capacity = 4095 - title.size() - 2;
    const QString body = lines.join(QLatin1Char('\n'));
    QStringList messages;
    for (qsizetype offset = 0; offset < body.size();) {
        qsizetype count = qMin(capacity, body.size() - offset);
        if (offset + count < body.size()) {
            const auto window = QStringView(body).mid(offset, count);
            const qsizetype newline = window.lastIndexOf(QLatin1Char('\n'));
            if (newline >= 0) count = newline + 1;
            else {
                const qsizetype separator = window.lastIndexOf(QStringLiteral(", "));
                if (separator >= 0) count = separator + 2;
            }
            if (body.at(offset + count - 1).isHighSurrogate()) --count;
        }
        messages.append(header + body.mid(offset, count).toHtmlEscaped());
        offset += count;
    }
    return messages;
}

void TelegramPublicsTradeService::finish()
{
    if (!m_active) return;
    const QString requestId = m_requestId;
    const QStringList messages = formatResults(snapshot(), m_side);
    // Fully settle this job before direct observers can admit a replacement or delete us.
    m_active = false;
    m_waitingIndex = false;
    m_discoveryFailed = false;
    m_started = false;
    m_completionQueued = false;
    m_requestId.clear();
    m_symbols.clear();
    m_side.clear();
    m_nicknames.clear();
    QPointer<TelegramPublicsTradeService> guard(this);
    notifyBusy();
    if (guard) emit completed(requestId, messages);
}
