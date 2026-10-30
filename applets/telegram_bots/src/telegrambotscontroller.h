#pragma once

#include "telegrambotstore.h"
#include <QPointer>
#include <QVariantList>
#include <QHash>
#include "telegrambotruntime.h"

class TelegramBotsController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList bots READ bots NOTIFY botsChanged)
    Q_PROPERTY(QString maximumAllowedChatId READ maximumAllowedChatId CONSTANT)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool canStart READ canStart NOTIFY stateChanged)
    Q_PROPERTY(bool canStop READ canStop NOTIFY stateChanged)
    Q_PROPERTY(bool canDelete READ canDelete NOTIFY stateChanged)
    Q_PROPERTY(bool hasRunningBots READ hasRunningBots NOTIFY stateChanged)
public:
    explicit TelegramBotsController(QObject *parent = nullptr);
    explicit TelegramBotsController(TelegramBotStore *store, QObject *parent = nullptr);
    using RuntimeFactory = std::function<TelegramBotRuntime *(QObject *)>;
    TelegramBotsController(TelegramBotStore *store, RuntimeFactory factory, QObject *parent = nullptr);
    ~TelegramBotsController() override;
    QVariantList bots() const;
    QString maximumAllowedChatId() const { return QString::number(TelegramBotStore::maximumAllowedChatId); }
    QString selectedId() const { return m_selectedId; }
    bool busy() const { return m_busy || m_tradingBusy; }
    QString error() const { return m_error; }
    bool canStart() const;
    bool canStop() const;
    bool canDelete() const;
    Q_INVOKABLE bool deleteBot(QString botId);
    Q_INVOKABLE bool saveProtectedBot(QString botId, QString nickname, QString token, QString allowedChatIdsText, QString password, QString confirmation);
    Q_INVOKABLE bool startBotWithPassword(QString botId, QString password);
    Q_INVOKABLE bool unlockBotSettings(QString botId, QString password);
    Q_INVOKABLE void lockBotSettings();
    bool hasRunningBots() const;
    Q_INVOKABLE void load();
    Q_INVOKABLE void selectBot(const QString &id);
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool saveBot(QString nickname, QString token);
    Q_INVOKABLE bool saveBotWithAllowedChats(QString nickname, QString token, QString allowedChatIdsText);
    void setTradingBusy(bool busy);
    void completeTradeCommand(QString botId, QString requestId, QStringList htmlMessages);
    Q_INVOKABLE bool startBot();
    Q_INVOKABLE bool stopBot();
    Q_INVOKABLE bool stopAll();
    Q_INVOKABLE bool resolveInspectorConflict(QString botId, QString action);
signals:
    void inspectorPortConflict(QString botId);
    void botsChanged();
    void stateChanged();
    void botSaved(const QString &id);
    void botDeleted(const QString &id);
    void missingChatIds(QString botId);
    void passwordRequested(QString botId);
    void passwordSetupRequired(QString botId);
    void settingsUnlocked(QString botId, QStringList allowedChatIds);
    void activationFinished(QString botId, bool success);
    void settingsUnlockFinished(QString botId, bool success);
    void tradeRequested(QString botId, QString requestId, QString side, QStringList symbols);
private:
    QPointer<TelegramBotStore> m_store;
    TelegramBotStore::Entries m_entries;
    QString m_selectedId;
    QString m_error;
    bool m_busy = false;
    bool m_tradingBusy = false;
    quint64 m_operation = 0;
    RuntimeFactory m_factory;
    QHash<QString, QPointer<TelegramBotRuntime>> m_runtimes;
    QHash<QString, QPointer<TelegramBotRuntime>> m_inspectorConflicts;
    TelegramBotRuntime *runtimeFor(const QString &id);
    bool m_destroying = false;
    bool m_stoppingAll = false;
    QString m_unlockedSettingsId;
    quint64 m_settingsEpoch = 0;
    bool stopped(const QString &id) const;
    bool readProtected(QString id, QString password, bool settings);
};
