#pragma once
#include <QObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <functional>
#include <memory>

// QObject-thread-only. Completion is asynchronous; destruction cancels callbacks.
// Nonsecret metadata and opaque password boxes live in Telegram-only keychain
// data. Private fields require only the user password, never an OS-stored key.
// Old shared-key catalogs are not imported or overwritten by production code.
// Buffer wiping is best effort: detached Qt/caller copies, Qt JSON and backend
// internals may retain transient plaintext. No reinstall-survival promise is made
// beyond retained OS keychain entries and stable application/keychain identity.
class TelegramBotStore : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 maximumAllowedChatId = 4503599627370495LL;
    static bool isSupportedChatId(const QString &id);
    struct Entry {
        QString id;
        QString nickname;
        // Available only for legacy records awaiting explicit migration.
        QStringList allowedChatIds;
        bool passwordProtected = false;
        bool hasAllowedChatIds = false;
    };
    using Entries = QList<Entry>;
    using LoadCompletion = std::function<void(bool, Entries, QString)>;
    using AddCompletion = std::function<void(bool, QString, QString)>;
    using TokenCompletion = std::function<void(bool, QString, QString)>;
    struct ActivationSnapshot { QString token; QStringList allowedChatIds; QString sessionSecret; };
    using ActivationCompletion = std::function<void(bool, ActivationSnapshot, QString)>;
    enum class Status { Success, Missing, Failure };
    using BinaryCompletion = std::function<void(Status, QByteArray)>;
    // C++ test seam. Complete on the store thread; Missing means confirmed absence,
    // not an empty/malformed value. write=true must report success only after commit.
    using BinaryBackend = std::function<void(bool write, QString service, QString key,
                                             QByteArray data, BinaryCompletion)>;
    explicit TelegramBotStore(QObject *parent = nullptr);
    // Only the parent directory is used to locate telegram-password-vault.lock;
    // the supplied file is never opened or inspected in normal production mode.
    TelegramBotStore(QString storagePath, BinaryBackend backend, QObject *parent = nullptr);
    // Explicit, injected-backend-only compatibility fixture. Never used by GUI.
    enum class LegacyFixture { Enabled };
    TelegramBotStore(QString fixtureProfilesPath, BinaryBackend backend, LegacyFixture, QObject *parent = nullptr);
    ~TelegramBotStore() override;
    virtual void load(LoadCompletion);
    virtual void add(QString nickname, QString token, AddCompletion);
    // Upsert by nickname. Empty token retains an existing credential, never creates one.
    virtual void saveWithAllowedChats(QString nickname, QString token, QStringList allowedChatIds, AddCompletion);
    virtual void readToken(QString id, TokenCompletion);
    // Owning token and policy from one authenticated catalog read under the same lock.
    // Activation must never pair readToken() with cached load() metadata.
    virtual void readActivationSnapshot(QString id, ActivationCompletion);
    // Exact passwords, never normalized or persisted. Existing protected records
    // require their current password even when replacing the BotFather token.
    virtual void saveProtectedBot(QString id, QString nickname, QString botToken,
                                  QStringList allowedChatIds, QString password,
                                  QString confirmation, AddCompletion);
    virtual void readActivationSnapshot(QString id, QString password, ActivationCompletion);
    virtual void remove(QString id, AddCompletion);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
