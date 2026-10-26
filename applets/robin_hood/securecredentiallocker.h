#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

class QLockFile;

// Use on its QObject thread. Completions are asynchronous and may start another
// operation. Concurrent calls fail busy; destruction cancels pending callbacks.
class SecureCredentialLocker : public QObject
{
    Q_OBJECT
public:
    // RAM-only encrypted staging, independent of the persistent locker/keychain.
    // Not thread-safe. Wiping is best effort: Qt JSON, caller/shared Qt copies
    // and allocator copies can outlive transient plaintext buffers.
    class PendingCredentials final
    {
    public:
        PendingCredentials() = default;
        ~PendingCredentials();
        PendingCredentials(const PendingCredentials &) = delete;
        PendingCredentials &operator=(const PendingCredentials &) = delete;
        PendingCredentials(PendingCredentials &&) = delete;
        PendingCredentials &operator=(PendingCredentials &&) = delete;

        bool set(const QString &email, const QString &password);
        bool read(QString &email, QString &password) const;
        void clear();
        bool isEmpty() const;

    private:
        QByteArray m_key, m_ciphertext;
    };

    using Completion = std::function<void(bool, QString)>;
    using ReadCompletion = std::function<void(bool, QString, QString, QString)>;
    using KeyCompletion = std::function<void(bool, QByteArray, QString)>;
    // Called per operation that needs a key, under the file lock. Complete on
    // this object's thread (immediately is supported). Never create unless allowed.
    using KeyProvider = std::function<void(bool allowCreate, KeyCompletion)>;

    explicit SecureCredentialLocker(QObject *parent = nullptr);
    SecureCredentialLocker(QString path, KeyProvider provider, QObject *parent = nullptr);
    ~SecureCredentialLocker() override;

    void save(const QString &accountId, const QString &email, const QString &password, Completion done);
    void read(const QString &accountId, ReadCompletion done);
    void remove(const QString &accountId, Completion done);

    // Best effort for these buffers only: detaches shared Qt data. Qt JSON,
    // framework/keychain internals, caller copies and allocator copies may remain.
    static void wipe(QString &value);
    static void wipe(QByteArray &value);

private:
    enum class Action { Save, Read, Remove };
    struct Operation;
    void begin(std::shared_ptr<Operation> operation);
    bool releaseLock();
    void execute(std::shared_ptr<Operation> operation, bool ok, QByteArray key);
    void finish(std::shared_ptr<Operation> operation, bool ok, QString error,
                QString email = {}, QString password = {});
    QString m_path;
    KeyProvider m_provider;
    std::unique_ptr<QLockFile> m_lock;
    QTimer m_unlockRetry;
    bool m_busy = false;
};
