#include "telegrambotscontroller.h"
#include <algorithm>
#include <QRegularExpression>

TelegramBotsController::TelegramBotsController(QObject *parent)
    : QObject(parent), m_store(new TelegramBotStore(this)) {}

TelegramBotsController::TelegramBotsController(TelegramBotStore *store, QObject *parent)
    : QObject(parent), m_store(store) {}

TelegramBotsController::TelegramBotsController(TelegramBotStore *store, RuntimeFactory factory, QObject *parent)
    : QObject(parent), m_store(store), m_factory(std::move(factory)) {}

TelegramBotsController::~TelegramBotsController()
{
    m_destroying = true;
    for (const auto &runtime : std::as_const(m_runtimes))
        if (runtime) runtime->stop();
}

bool TelegramBotsController::hasRunningBots() const
{
    for (const auto &runtime : m_runtimes)
        if (runtime && (runtime->active() || runtime->starting() || runtime->stopping())) return true;
    return false;
}

bool TelegramBotsController::canStart() const
{
    const auto runtime = m_runtimes.value(m_selectedId);
    return !m_busy && !m_stoppingAll && !m_selectedId.isEmpty()
        && (!runtime || (!runtime->active() && !runtime->starting() && !runtime->stopping()));
}

bool TelegramBotsController::canStop() const
{
    const auto runtime = m_runtimes.value(m_selectedId);
    return !m_busy && !m_stoppingAll && runtime && !runtime->stopping()
        && (runtime->active() || runtime->starting());
}

QVariantList TelegramBotsController::bots() const
{
    QVariantList rows;
    rows.reserve(m_entries.size());
    for (const auto &entry : m_entries) {
        const auto runtime = m_runtimes.value(entry.id);
        rows.append(QVariantMap{{QStringLiteral("id"), entry.id}, {QStringLiteral("nickname"), entry.nickname},
                                {QStringLiteral("active"), runtime && runtime->active() && runtime->healthy()},
                                {QStringLiteral("starting"), runtime && runtime->starting()},
                                {QStringLiteral("stopping"), runtime && runtime->stopping()}});
    }
    return rows;
}

void TelegramBotsController::load()
{
    if (m_busy || m_stoppingAll || hasRunningBots()) return;
    if (!m_store) { m_error = tr("Secure bot storage is unavailable."); emit stateChanged(); return; }
    m_busy = true;
    m_error.clear();
    const auto operation = ++m_operation;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged();
    if (!guard || !m_store) return;
    m_store->load([guard, operation](bool ok, TelegramBotStore::Entries entries, QString error) {
        if (!guard || !guard->m_busy || guard->m_operation != operation) return;
        if (ok) { guard->m_entries = std::move(entries); guard->m_selectedId.clear(); }
        guard->m_error = ok ? QString() : error;
        guard->m_busy = false;
        emit guard->botsChanged();
        if (guard) emit guard->stateChanged();
    });
}

void TelegramBotsController::selectBot(const QString &id)
{
    const bool found = std::any_of(m_entries.cbegin(), m_entries.cend(), [&id](const auto &entry) { return entry.id == id; });
    const QString selected = found ? id : QString();
    if (m_selectedId == selected) return;
    m_selectedId = selected;
    emit stateChanged();
}

void TelegramBotsController::clearError()
{
    if (m_error.isEmpty()) return;
    m_error.clear();
    emit stateChanged();
}

bool TelegramBotsController::saveBot(QString nickname, QString token)
{
    if (m_busy || m_stoppingAll) return false;
    static const QRegularExpression pattern(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (!m_store || nickname.isEmpty() || nickname.size() > 128 || nickname.trimmed() != nickname
        || !pattern.match(token).hasMatch()) {
        m_error = tr("Enter a valid bot nickname and API token.");
        token.fill(QChar(0));
        emit stateChanged();
        return false;
    }
    m_busy = true;
    m_error.clear();
    const auto operation = ++m_operation;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged();
    if (!guard || !m_store) return false;
    m_store->add(nickname, std::move(token), [guard, operation, nickname](bool ok, QString id, QString error) {
        if (!guard || !guard->m_busy || guard->m_operation != operation) return;
        const bool duplicate = std::any_of(guard->m_entries.cbegin(), guard->m_entries.cend(),
                                         [&id](const auto &entry) { return entry.id == id; });
        ok = ok && !id.isEmpty() && !duplicate;
        guard->m_error = ok ? QString() : error.isEmpty() ? tr("Secure storage failed.") : error;
        if (ok) { guard->m_entries.append({id, nickname}); guard->m_selectedId.clear(); }
        guard->m_busy = false;
        if (ok) emit guard->botsChanged();
        if (guard) emit guard->stateChanged();
        if (guard && ok) emit guard->botSaved(id);
    });
    return true;
}

TelegramBotRuntime *TelegramBotsController::runtimeFor(const QString &id)
{
    if (const auto existing = m_runtimes.value(id)) return existing;
    auto *runtime = m_factory ? m_factory(this) : new TelegramBotRuntime(this);
    if (!runtime) return nullptr;
    runtime->setParent(this);
    m_runtimes.insert(id, runtime);
    connect(runtime, &TelegramBotRuntime::stateChanged, this, [this] {
        if (m_destroying || m_stoppingAll) return;
        const QPointer<TelegramBotsController> guard(this);
        emit botsChanged();
        if (guard) emit stateChanged();
    });
    connect(runtime, &TelegramBotRuntime::errorOccurred, this, [this](const QString &message) {
        if (m_destroying || m_stoppingAll) return;
        m_error = message;
        emit stateChanged();
    });
    return runtime;
}

bool TelegramBotsController::startBot()
{
    if (!canStart() || !m_store) return false;
    const QString id = m_selectedId;
    m_busy = true;
    m_error.clear();
    const auto operation = ++m_operation;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged();
    if (!guard || !m_store) return false;
    m_store->readToken(id, [guard, operation, id](bool ok, QString token, QString error) {
        if (!guard || !guard->m_busy || guard->m_operation != operation) {
            token.fill(QChar(0));
            return;
        }
        if (!ok) {
            guard->m_error = error.isEmpty() ? tr("Bot credentials are unavailable.") : error;
            guard->m_busy = false;
            token.fill(QChar(0));
            emit guard->stateChanged();
            return;
        }
        auto *runtime = guard->runtimeFor(id);
        guard->m_busy = false;
        if (runtime) runtime->start(std::move(token));
        else { token.fill(QChar(0)); guard->m_error = tr("Bot runtime is unavailable."); }
        if (guard) emit guard->stateChanged();
    });
    return true;
}

bool TelegramBotsController::stopBot()
{
    if (!canStop()) return false;
    const auto runtime = m_runtimes.value(m_selectedId);
    m_error.clear();
    runtime->stop();
    return true;
}

bool TelegramBotsController::stopAll()
{
    if (m_busy || m_stoppingAll) return false;
    m_stoppingAll = true;
    const QPointer<TelegramBotsController> guard(this);
    const auto runtimes = m_runtimes.values();
    for (const auto &runtime : runtimes) {
        if (runtime) runtime->stop();
        if (!guard) return false;
    }
    m_stoppingAll = false;
    emit botsChanged();
    if (guard) emit stateChanged();
    return guard && !guard->hasRunningBots();
}
