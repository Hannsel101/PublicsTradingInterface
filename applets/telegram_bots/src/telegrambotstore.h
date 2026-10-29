#pragma once
#include <QObject>
#include <QList>
#include <QString>
#include <QByteArray>
#include <functional>
#include <memory>

// QObject-thread-only. Completion is asynchronous; destruction cancels callbacks.
// The whole catalog, including nicknames, lives only in encrypted keychain data.
// Buffer wiping is best effort: detached Qt/caller copies, Qt JSON and backend
// internals may retain transient plaintext. No reinstall-survival promise is made
// beyond retained OS keychain entries and stable application/keychain identity.
class TelegramBotStore : public QObject {
    Q_OBJECT
public:
    struct Entry { QString id; QString nickname; };
    using Entries = QList<Entry>;
    using LoadCompletion = std::function<void(bool, Entries, QString)>;
    using AddCompletion = std::function<void(bool, QString, QString)>;
    using TokenCompletion = std::function<void(bool, QString, QString)>;
    enum class Status { Success, Missing, Failure };
    using BinaryCompletion = std::function<void(Status, QByteArray)>;
    // C++ test seam. Complete on the store thread; Missing means confirmed absence,
    // not an empty/malformed value. write=true must report success only after commit.
    using BinaryBackend = std::function<void(bool write, QString service, QString key,
                                             QByteArray data, BinaryCompletion)>;
    explicit TelegramBotStore(QObject *parent = nullptr);
    TelegramBotStore(QString robinHoodProfilesPath, BinaryBackend backend, QObject *parent = nullptr);
    ~TelegramBotStore() override;
    virtual void load(LoadCompletion);
    virtual void add(QString nickname, QString token, AddCompletion);
    virtual void readToken(QString id, TokenCompletion);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
