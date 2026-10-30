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
    if (m_tradingBusy) return true;
    for (const auto &runtime : m_runtimes)
        if (runtime && (runtime->active() || runtime->starting() || runtime->stopping())) return true;
    return false;
}

bool TelegramBotsController::canStart() const
{
    const auto runtime = m_runtimes.value(m_selectedId);
    return !busy() && !m_stoppingAll && !m_selectedId.isEmpty()
        && (!runtime || (!runtime->active() && !runtime->starting() && !runtime->stopping()));
}

bool TelegramBotsController::canStop() const
{
    const auto runtime = m_runtimes.value(m_selectedId);
    return !busy() && !m_stoppingAll && runtime && !runtime->stopping()
        && (runtime->active() || runtime->starting());
}

QVariantList TelegramBotsController::bots() const
{
    QVariantList rows;
    rows.reserve(m_entries.size());
    for (const auto &entry : m_entries) {
        const auto runtime = m_runtimes.value(entry.id);
        rows.append(QVariantMap{{QStringLiteral("id"), entry.id}, {QStringLiteral("nickname"), entry.nickname},
                                {QStringLiteral("allowedChatIds"), entry.passwordProtected ? QStringList{} : entry.allowedChatIds},
                                {QStringLiteral("passwordProtected"), entry.passwordProtected},
                                {QStringLiteral("hasAllowedChatIds"), entry.hasAllowedChatIds || (!entry.passwordProtected && !entry.allowedChatIds.isEmpty())},
                                {QStringLiteral("active"), runtime && runtime->active() && runtime->healthy()},
                                {QStringLiteral("starting"), runtime && runtime->starting()},
                                {QStringLiteral("stopping"), runtime && runtime->stopping()}});
    }
    return rows;
}

void TelegramBotsController::load()
{
    if (m_busy || m_stoppingAll || hasRunningBots()) return;
    lockBotSettings();
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
    if (busy() || m_stoppingAll) return false;
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

void TelegramBotsController::setTradingBusy(bool busy)
{
    if (m_tradingBusy == busy) return;
    m_tradingBusy = busy;
    emit stateChanged();
}

bool TelegramBotsController::saveBotWithAllowedChats(QString nickname, QString token, QString text)
{
    if (busy() || m_stoppingAll) { token.fill(QChar(0)); return false; }
    // Policy is immutable for a live/starting/stopping runtime. Stop it first.
    for (const auto &entry : std::as_const(m_entries)) {
        if (entry.nickname.compare(nickname, Qt::CaseInsensitive) != 0) continue;
        const auto runtime = m_runtimes.value(entry.id);
        if (runtime && (runtime->active() || runtime->starting() || runtime->stopping())) {
            token.fill(QChar(0));
            m_error = tr("Stop this bot before changing its allowed chats. Stopping does not reverse submitted orders.");
            emit stateChanged(); return false;
        }
    }
    QStringList policy;
    bool valid = text.size() <= 32768;
    if (!text.trimmed().isEmpty()) {
        const auto parts = text.split(QRegularExpression(QStringLiteral("[,\\r\\n]")), Qt::KeepEmptyParts);
        // Blank lines are allowed; missing comma-delimited IDs are not.
        if (QRegularExpression(QStringLiteral("(^|,)\\s*(,|$)")).match(text).hasMatch()) valid = false;
        for (const auto &part : parts) {
            const auto id = part.trimmed();
            if (id.isEmpty()) continue;
            if (!TelegramBotStore::isSupportedChatId(id)) { valid = false; break; }
            if (!policy.contains(id)) policy.append(id);
        }
    }
    const bool existing = std::any_of(m_entries.cbegin(), m_entries.cend(), [&nickname](const auto &e) {
        return e.nickname.compare(nickname, Qt::CaseInsensitive) == 0;
    });
    static const QRegularExpression tokenPattern(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (!m_store || !valid || policy.size() > 1024 || nickname.isEmpty() || nickname.size() > 128 || nickname.trimmed() != nickname
        || !(tokenPattern.match(token).hasMatch() || (existing && token.isEmpty()))) {
        token.fill(QChar(0));
        m_error = tr("Enter a valid nickname, token and positive decimal private Chat IDs (maximum 4503599627370495).");
        emit stateChanged(); return false;
    }
    m_busy = true; m_error.clear(); const auto operation = ++m_operation;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged(); if (!guard || !m_store) { token.fill(QChar(0)); return false; }
    m_store->saveWithAllowedChats(nickname, std::move(token), policy,
        [guard, operation, nickname, policy](bool ok, QString id, QString error) {
        if (!guard || !guard->m_busy || guard->m_operation != operation) return;
        ok = ok && !id.isEmpty();
        guard->m_error = ok ? QString() : error.isEmpty() ? tr("Secure storage failed.") : error;
        if (ok) {
            auto found = std::find_if(guard->m_entries.begin(), guard->m_entries.end(), [&id](const auto &entry) { return entry.id == id; });
            if (found != guard->m_entries.end()) found->allowedChatIds = policy;
            else guard->m_entries.append({id, nickname, policy});
            guard->m_selectedId.clear();
        }
        guard->m_busy = false;
        if (ok) emit guard->botsChanged();
        if (guard) emit guard->stateChanged();
        if (guard && ok) emit guard->botSaved(id);
    });
    return true;
}

bool TelegramBotsController::saveProtectedBot(QString id, QString nickname, QString token, QString text, QString password, QString confirmation)
{
    const auto wipe = [&] { token.fill(QChar(0)); password.fill(QChar(0)); confirmation.fill(QChar(0)); };
    if (busy() || m_stoppingAll || !m_store) { wipe(); return false; }
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [&id](const auto &e) { return e.id == id; });
    const bool existing = found != m_entries.cend();
    if ((!id.isEmpty() && !existing) || (existing && (!stopped(id) || (found->passwordProtected && m_unlockedSettingsId != id)))) {
        wipe(); m_error = tr("Unlock this stopped bot before editing its settings."); emit stateChanged(); return false;
    }
    QStringList policy;
    bool valid = text.size() <= 32768;
    if (!text.trimmed().isEmpty()) {
        if (QRegularExpression(QStringLiteral("(^|,)\\s*(,|$)")).match(text).hasMatch()) valid = false;
        for (const auto &part : text.split(QRegularExpression(QStringLiteral("[,\\r\\n]")), Qt::KeepEmptyParts)) {
            const auto chatId = part.trimmed();
            if (chatId.isEmpty()) continue;
            if (!TelegramBotStore::isSupportedChatId(chatId)) valid = false;
            if (!policy.contains(chatId)) policy.append(chatId);
        }
    }
    static const QRegularExpression tokenPattern(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (password != confirmation) m_error = tr("Passwords do not match.");
    else if (password.size() < 12) m_error = tr("Use a password of at least 12 characters.");
    else if (!valid || policy.size() > 1024 || nickname.isEmpty() || nickname.size() > 128 || nickname.trimmed() != nickname
             || (existing && found->nickname != nickname) || !(tokenPattern.match(token).hasMatch() || (existing && token.isEmpty())))
        m_error = tr("Enter a valid nickname, token and positive decimal private Chat IDs (maximum 4503599627370495).");
    else m_error.clear();
    if (!m_error.isEmpty()) { wipe(); emit stateChanged(); return false; }
    m_busy = true;
    const auto operation = ++m_operation;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged();
    if (!guard || !m_store) { wipe(); return false; }
    const bool hasIds = !policy.isEmpty();
    m_store->saveProtectedBot(id, nickname, std::move(token), std::move(policy), std::move(password), std::move(confirmation),
        [guard, operation, nickname, hasIds](bool ok, QString savedId, QString) {
        if (!guard || !guard->m_busy || guard->m_operation != operation) return;
        ok = ok && !savedId.isEmpty();
        if (ok) {
            auto entry = std::find_if(guard->m_entries.begin(), guard->m_entries.end(), [&savedId](const auto &e) { return e.id == savedId; });
            TelegramBotStore::Entry updated{savedId, nickname, {}, true, hasIds};
            if (entry == guard->m_entries.end()) guard->m_entries.append(updated); else *entry = updated;
            guard->m_unlockedSettingsId.clear(); guard->m_selectedId.clear();
        }
        guard->m_error = ok ? QString() : tr("Unable to save encrypted bot. Check your password and try again.");
        guard->m_busy = false;
        if (ok) emit guard->botsChanged();
        if (guard && guard->m_operation == operation) emit guard->stateChanged();
        if (guard && ok && guard->m_operation == operation) emit guard->botSaved(savedId);
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
    const QPointer<TelegramBotRuntime> captured(runtime);
    connect(runtime, &TelegramBotRuntime::inspectorPortConflict, this, [this, id, captured] {
        if (m_destroying || m_stoppingAll || !captured || m_runtimes.value(id) != captured
            || !captured->starting() || captured->stopping() || m_inspectorConflicts.contains(id)) return;
        m_inspectorConflicts.insert(id, captured);
        emit inspectorPortConflict(id);
    });
    connect(runtime, &TelegramBotRuntime::stateChanged, this, [this, id, captured] {
        if (!captured || !captured->starting() || captured->stopping()) m_inspectorConflicts.remove(id);
    });
    connect(runtime, &TelegramBotRuntime::tradeCommandReceived, this,
        [this, id](QString requestId, QString side, QStringList symbols) {
            if (!m_destroying && !m_stoppingAll) emit tradeRequested(id, requestId, side, symbols);
        });
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

bool TelegramBotsController::stopped(const QString &id) const
{
    if (id.isEmpty() || !std::any_of(m_entries.cbegin(), m_entries.cend(), [&id](const auto &e) { return e.id == id; })) return false;
    const auto runtime = m_runtimes.value(id);
    return !runtime || (!runtime->active() && !runtime->starting() && !runtime->stopping());
}

bool TelegramBotsController::canDelete() const
{
    return !busy() && !m_stoppingAll && stopped(m_selectedId);
}

bool TelegramBotsController::deleteBot(QString id)
{
    if (busy() || m_stoppingAll || !m_store || !stopped(id)) return false;
    m_busy = true; m_error.clear(); m_unlockedSettingsId.clear();
    const auto operation = ++m_operation;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged();
    if (!guard || !m_store) return false;
    m_store->remove(id, [guard, operation, id](bool ok, QString, QString) {
        if (!guard || !guard->m_busy || guard->m_operation != operation) return;
        if (ok) {
            guard->m_entries.removeIf([&id](const auto &e) { return e.id == id; });
            if (auto runtime = guard->m_runtimes.take(id)) runtime->deleteLater();
            if (guard->m_selectedId == id) guard->m_selectedId.clear();
        }
        guard->m_error = ok ? QString() : tr("Could not delete the encrypted bot. Please retry.");
        guard->m_busy = false;
        if (ok) emit guard->botsChanged();
        if (guard && guard->m_operation == operation) emit guard->stateChanged();
        if (guard && ok && guard->m_operation == operation) emit guard->botDeleted(id);
    });
    return true;
}

bool TelegramBotsController::startBot()
{
    if (!canStart()) return false;
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [this](const auto &e) { return e.id == m_selectedId; });
    if (found == m_entries.cend()) return false;
    const auto id = found->id;
    if (!(found->hasAllowedChatIds || (!found->passwordProtected && !found->allowedChatIds.isEmpty()))) {
        emit missingChatIds(id); return false;
    }
    if (!found->passwordProtected) { emit passwordSetupRequired(id); return false; }
    emit passwordRequested(id);
    return true;
}

bool TelegramBotsController::startBotWithPassword(QString id, QString password)
{
    return readProtected(std::move(id), std::move(password), false);
}

bool TelegramBotsController::unlockBotSettings(QString id, QString password)
{
    return readProtected(std::move(id), std::move(password), true);
}

void TelegramBotsController::lockBotSettings()
{
    m_unlockedSettingsId.clear();
    // Retire an in-flight unlock before the editor is unloaded.
    ++m_settingsEpoch;
}

bool TelegramBotsController::readProtected(QString id, QString password, bool settings)
{
    if (busy() || m_stoppingAll || !m_store || !stopped(id)) { password.fill(QChar(0)); return false; }
    const auto entry = std::find_if(m_entries.cbegin(), m_entries.cend(), [&id](const auto &e) { return e.id == id; });
    if (!entry->passwordProtected) { password.fill(QChar(0)); emit passwordSetupRequired(id); return false; }
    m_busy = true; m_error.clear();
    const auto operation = ++m_operation;
    const auto settingsEpoch = m_settingsEpoch;
    const QPointer<TelegramBotsController> guard(this);
    emit stateChanged();
    if (!guard || !m_store) { password.fill(QChar(0)); return false; }
    m_store->readActivationSnapshot(id, std::move(password), [guard, operation, settingsEpoch, id, settings](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString) {
        const auto wipe = [&snapshot] { snapshot.token.fill(QChar(0)); snapshot.sessionSecret.fill(QChar(0)); snapshot.allowedChatIds.clear(); };
        if (!guard || !guard->m_busy || guard->m_operation != operation) { wipe(); return; }
        if (settings && guard->m_settingsEpoch != settingsEpoch) { wipe(); guard->m_busy = false; emit guard->stateChanged(); return; }
        ok = ok && !guard->m_tradingBusy && guard->stopped(id);
        const bool missing = ok && !settings && snapshot.allowedChatIds.isEmpty();
        if (missing) ok = false;
        if (!ok) guard->m_error = missing ? QString() : tr("Unable to unlock bot. Check your password and try again.");
        if (ok && settings) {
            guard->m_unlockedSettingsId = id;
            const auto ids = snapshot.allowedChatIds;
            wipe();
            emit guard->settingsUnlocked(id, ids);
        } else if (ok) {
            auto *runtime = guard->runtimeFor(id);
            if (!guard) { wipe(); return; }
            if (runtime) {
                runtime->setAllowedChatIds(std::move(snapshot.allowedChatIds));
                runtime->start(std::move(snapshot.token), std::move(snapshot.sessionSecret));
            } else { ok = false; guard->m_error = tr("Bot runtime is unavailable."); }
        }
        wipe();
        if (!guard || guard->m_operation != operation) return;
        guard->m_busy = false;
        if (missing) emit guard->missingChatIds(id);
        if (guard && guard->m_operation == operation) emit guard->stateChanged();
        if (!guard || guard->m_operation != operation) return;
        if (settings) emit guard->settingsUnlockFinished(id, ok);
        else emit guard->activationFinished(id, ok);
    });
    return true;
}

void TelegramBotsController::completeTradeCommand(QString botId, QString requestId, QStringList htmlMessages)
{
    if (const auto runtime = m_runtimes.value(botId))
        runtime->completeTradeCommand(std::move(requestId), std::move(htmlMessages));
}

bool TelegramBotsController::stopBot()
{
    if (!canStop()) return false;
    const auto runtime = m_runtimes.value(m_selectedId);
    m_error.clear();
    runtime->stop();
    return true;
}

bool TelegramBotsController::resolveInspectorConflict(QString botId, QString action)
{
    if (m_destroying || m_stoppingAll || (action != QStringLiteral("restart") && action != QStringLiteral("cancel"))
        || (action == QStringLiteral("restart") && busy())) return false;
    const auto runtime = m_inspectorConflicts.value(botId);
    if (!runtime || m_runtimes.value(botId) != runtime || !runtime->starting() || runtime->stopping()) return false;
    m_inspectorConflicts.remove(botId);
    if (action == QStringLiteral("cancel")) lockBotSettings();
    // Agent consumes its generation-bound choice synchronously; its host-facing
    // notification is already deferred out of the native conflict stack.
    runtime->resolveInspectorConflict(action);
    return true;
}

bool TelegramBotsController::stopAll()
{
    if (busy() || m_stoppingAll) return false;
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
