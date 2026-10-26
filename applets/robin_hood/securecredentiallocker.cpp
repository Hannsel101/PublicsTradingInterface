#include "securecredentiallocker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <keychain.h>
#endif

namespace {
const QString service = QStringLiteral("PublicsTradingInterface.RobinHood.CredentialLocker");
const QString masterKeyName = QStringLiteral("AES256GCM.MasterKey.v1");
const QByteArray header("PTILOCK\0\1", 9);
constexpr int nonceSize = 12;
constexpr int tagSize = 16;
constexpr int overhead = 9 + nonceSize + tagSize;
constexpr int maxPlaintext = 1024 * 1024;
constexpr int maxAccounts = 1024;
bool validId(const QString &id) { return !id.isEmpty() && id.size() <= 256; }
bool validFields(const QString &email, const QString &password)
{
    return !email.isEmpty() && email.size() <= 4096
        && !password.isEmpty() && password.size() <= 16384;
}
const auto ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

struct SecretBytes {
    QByteArray data;
    ~SecretBytes() { SecureCredentialLocker::wipe(data); }
};
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

bool encrypt(const QByteArray &plain, const QByteArray &key, QByteArray &file)
{
    QByteArray nonce(nonceSize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonceSize) != 1) return false;
    CipherContext ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) return false;
    QByteArray cipher(plain.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    QByteArray tag(tagSize, Qt::Uninitialized);
    int n = 0, total = 0;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonceSize, nullptr) != 1
        || EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                             reinterpret_cast<const unsigned char *>(key.constData()),
                             reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
        || EVP_EncryptUpdate(ctx.get(), nullptr, &n,
                             reinterpret_cast<const unsigned char *>(header.constData()), header.size()) != 1
        || EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()), &n,
                             reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) != 1)
        return false;
    total = n;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()) + total, &n) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tagSize, tag.data()) != 1) return false;
    cipher.resize(total + n);
    file = header + nonce + tag + cipher;
    return true;
}

bool decrypt(const QByteArray &file, const QByteArray &key, QByteArray &plain)
{
    if (file.size() < overhead + 2 || file.size() > overhead + maxPlaintext
        || file.first(header.size()) != header) return false;
    const QByteArray nonce = file.mid(header.size(), nonceSize);
    QByteArray tag = file.mid(header.size() + nonceSize, tagSize);
    const QByteArray cipher = file.mid(overhead);
    CipherContext ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) return false;
    plain.resize(cipher.size() + EVP_MAX_BLOCK_LENGTH);
    int n = 0, total = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonceSize, nullptr) != 1
        || EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                             reinterpret_cast<const unsigned char *>(key.constData()),
                             reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
        || EVP_DecryptUpdate(ctx.get(), nullptr, &n,
                             reinterpret_cast<const unsigned char *>(header.constData()), header.size()) != 1
        || EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()), &n,
                             reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size()) != 1)
        return false;
    total = n;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tagSize, tag.data()) != 1
        || EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()) + total, &n) != 1)
        return false;
    plain.resize(total + n);
    return true;
}

SecureCredentialLocker::KeyProvider systemKeyProvider(QObject *owner)
{
    return [owner](bool allowCreate, SecureCredentialLocker::KeyCompletion done) {
        auto *read = new QKeychain::ReadPasswordJob(service, owner);
        read->setInsecureFallback(false);
        read->setKey(masterKeyName);
        QObject::connect(read, &QKeychain::Job::finished, owner,
                         [owner, read, allowCreate, done = std::move(done)]() mutable {
            if (read->error() == QKeychain::NoError) {
                QByteArray key = read->binaryData();
                const bool valid = key.size() == 32;
                done(valid, key, valid ? QString() : QStringLiteral("Invalid master key."));
                SecureCredentialLocker::wipe(key);
                return;
            }
            // Permission/backend errors are NOT missing-key errors.
            if (read->error() != QKeychain::EntryNotFound || !allowCreate) {
                done(false, {}, QStringLiteral("Master key unavailable."));
                return;
            }
            auto secret = std::make_shared<SecretBytes>();
            secret->data.resize(32);
            if (RAND_bytes(reinterpret_cast<unsigned char *>(secret->data.data()), 32) != 1) {
                done(false, {}, QStringLiteral("Secure random generation failed."));
                return;
            }
            auto *write = new QKeychain::WritePasswordJob(service, owner);
            write->setInsecureFallback(false);
            write->setKey(masterKeyName);
            write->setBinaryData(secret->data);
            QObject::connect(write, &QKeychain::Job::finished, owner,
                             [write, secret, done = std::move(done)]() mutable {
                const bool ok = write->error() == QKeychain::NoError;
                done(ok, ok ? secret->data : QByteArray(),
                     ok ? QString() : QStringLiteral("Master key could not be saved."));
            });
            write->start();
        });
        read->start();
    };
}
}

SecureCredentialLocker::PendingCredentials::~PendingCredentials()
{
    clear();
}

void SecureCredentialLocker::PendingCredentials::clear()
{
    wipe(m_key);
    wipe(m_ciphertext);
}

bool SecureCredentialLocker::PendingCredentials::isEmpty() const
{
    return m_key.isEmpty() || m_ciphertext.isEmpty();
}

bool SecureCredentialLocker::PendingCredentials::set(const QString &email, const QString &password)
{
    clear();
    if (!validFields(email, password)) return false;
    SecretBytes key{QByteArray(32, Qt::Uninitialized)};
    if (RAND_bytes(reinterpret_cast<unsigned char *>(key.data.data()), 32) != 1) return false;
    SecretBytes plaintext{QJsonDocument(QJsonObject{{"email", email}, {"password", password}})
                              .toJson(QJsonDocument::Compact)};
    QByteArray ciphertext;
    if (!encrypt(plaintext.data, key.data, ciphertext)) return false;
    m_key = std::move(key.data);
    m_ciphertext = std::move(ciphertext);
    return true;
}

bool SecureCredentialLocker::PendingCredentials::read(QString &email, QString &password) const
{
    wipe(email);
    wipe(password);
    SecretBytes plaintext;
    if (m_key.size() != 32 || !decrypt(m_ciphertext, m_key, plaintext.data)) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(plaintext.data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto record = document.object();
    if (record.size() != 2 || !record.value("email").isString()
        || !record.value("password").isString()) return false;
    QString decodedEmail = record.value("email").toString();
    QString decodedPassword = record.value("password").toString();
    const bool valid = validFields(decodedEmail, decodedPassword);
    if (valid) {
        email = decodedEmail;
        password = decodedPassword;
    }
    wipe(decodedEmail);
    wipe(decodedPassword);
    return valid;
}

struct SecureCredentialLocker::Operation {
    Action action;
    QString id, email, password;
    Completion done;
    ReadCompletion readDone;
    ~Operation() { wipe(email); wipe(password); }
};

SecureCredentialLocker::SecureCredentialLocker(QObject *parent)
    : SecureCredentialLocker(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                             + QStringLiteral("/profiles.dat"), {}, parent)
{
    m_provider = systemKeyProvider(this);
}

SecureCredentialLocker::SecureCredentialLocker(QString path, KeyProvider provider, QObject *parent)
    : QObject(parent), m_path(std::move(path)), m_provider(std::move(provider))
{
    m_unlockRetry.setInterval(100);
    connect(&m_unlockRetry, &QTimer::timeout, this, [this] {
        if (!m_busy) releaseLock();
    });
}
SecureCredentialLocker::~SecureCredentialLocker() = default;

void SecureCredentialLocker::wipe(QString &value)
{
    if (!value.isEmpty()) OPENSSL_cleanse(value.data(), size_t(value.size()) * sizeof(QChar));
    value.clear();
}
void SecureCredentialLocker::wipe(QByteArray &value)
{
    if (!value.isEmpty()) OPENSSL_cleanse(value.data(), size_t(value.size()));
    value.clear();
}

void SecureCredentialLocker::save(const QString &accountId, const QString &email,
                                  const QString &password, Completion done)
{
    auto op = std::make_shared<Operation>();
    op->action = Action::Save; op->id = accountId; op->email = email; op->password = password;
    op->done = std::move(done);
    begin(std::move(op));
}
void SecureCredentialLocker::read(const QString &accountId, ReadCompletion done)
{
    auto op = std::make_shared<Operation>();
    op->action = Action::Read; op->id = accountId; op->readDone = std::move(done);
    begin(std::move(op));
}
void SecureCredentialLocker::remove(const QString &accountId, Completion done)
{
    auto op = std::make_shared<Operation>();
    op->action = Action::Remove; op->id = accountId; op->done = std::move(done);
    begin(std::move(op));
}

void SecureCredentialLocker::begin(std::shared_ptr<Operation> op)
{
    if (m_busy) {
        QTimer::singleShot(0, this, [op] {
            if (op->done) op->done(false, QStringLiteral("Credential locker is busy."));
            if (op->readDone) op->readDone(false, {}, {}, QStringLiteral("Credential locker is busy."));
        });
        return;
    }
    m_busy = true;
    QTimer::singleShot(0, this, [this, op] {
        if (!validId(op->id) || (op->action == Action::Save && !validFields(op->email, op->password))) {
            finish(op, false, QStringLiteral("Invalid credential fields or field limit exceeded.")); return;
        }
        if (m_path.isEmpty() || !m_provider) {
            finish(op, false, QStringLiteral("Credential locker is not configured.")); return;
        }
        const QFileInfo info(m_path);
        if (!QDir().mkpath(info.absolutePath())) {
            finish(op, false, QStringLiteral("Credential directory unavailable.")); return;
        }
        // Canonical parent prevents path aliases from choosing different locks.
        const QString canonicalDir = QFileInfo(info.absolutePath()).canonicalFilePath();
        m_path = QDir(canonicalDir).filePath(info.fileName());
        if (!releaseLock()) {
            finish(op, false, QStringLiteral("Credential lock cleanup is pending.")); return;
        }
        m_lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
        // Never break a live lock just because a keychain prompt took a long time.
        m_lock->setStaleLockTime(0);
        if (!m_lock->tryLock(0)) {
            finish(op, false, QStringLiteral("Credential file is locked.")); return;
        }
        const QFileInfo target(m_path);
        if (target.isSymLink() || (target.exists() && !target.isFile())) {
            finish(op, false, QStringLiteral("Invalid credential file.")); return;
        }
        // Legacy token-only profiles have no encrypted file/key to delete.
        // Decide under the same lock used by creation; never touch the keychain.
        if (op->action == Action::Remove && !target.exists()) {
            finish(op, true, {}); return;
        }
        const bool allowCreate = op->action == Action::Save && !target.exists();
        QPointer<SecureCredentialLocker> guard(this);
        auto completed = std::make_shared<bool>(false);
        m_provider(allowCreate, [guard, op, completed](bool ok, QByteArray key, QString) mutable {
            auto secret = std::make_shared<SecretBytes>();
            secret->data = std::move(key);
            if (!guard || *completed) return;
            *completed = true;
            // Immediate provider completion must not recurse into client callbacks.
            QTimer::singleShot(0, guard, [guard, op, ok, secret] {
                if (guard) guard->execute(op, ok, std::move(secret->data));
            });
        });
    });
}

void SecureCredentialLocker::execute(std::shared_ptr<Operation> op, bool ok, QByteArray key)
{
    SecretBytes secret{std::move(key)};
    if (!ok || secret.data.size() != 32) {
        finish(op, false, QStringLiteral("Master key unavailable or invalid.")); return;
    }
    QJsonObject accounts;
    const QFileInfo info(m_path);
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        finish(op, false, QStringLiteral("Invalid credential file.")); return;
    }
    if (info.exists()) {
        QFile input(m_path);
        if (info.size() > overhead + maxPlaintext || !input.open(QIODevice::ReadOnly)) {
            finish(op, false, QStringLiteral("Credential file unavailable or oversized.")); return;
        }
        const QByteArray encrypted = input.read(overhead + maxPlaintext + 1);
        SecretBytes plaintext;
        if (input.error() != QFileDevice::NoError || !input.atEnd()
            || !decrypt(encrypted, secret.data, plaintext.data)) {
            finish(op, false, QStringLiteral("Credential file authentication failed.")); return;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(plaintext.data, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            finish(op, false, QStringLiteral("Invalid credential JSON.")); return;
        }
        accounts = document.object();
        if (accounts.size() > maxAccounts) {
            finish(op, false, QStringLiteral("Credential account limit exceeded.")); return;
        }
        for (auto i = accounts.constBegin(); i != accounts.constEnd(); ++i) {
            const auto record = i.value().toObject();
            if (!validId(i.key()) || !i.value().isObject() || record.size() != 2
                || !record.value("email").isString() || !record.value("password").isString()
                || !validFields(record.value("email").toString(), record.value("password").toString())) {
                finish(op, false, QStringLiteral("Invalid credential record.")); return;
            }
        }
    }
    if (op->action == Action::Read) {
        if (!accounts.contains(op->id)) {
            finish(op, false, QStringLiteral("Account credentials not found.")); return;
        }
        const auto account = accounts.value(op->id).toObject();
        finish(op, true, {}, account.value("email").toString(), account.value("password").toString());
        return;
    }
    if (op->action == Action::Save) {
        if (!accounts.contains(op->id) && accounts.size() >= maxAccounts) {
            finish(op, false, QStringLiteral("Credential account limit exceeded.")); return;
        }
        accounts.insert(op->id, QJsonObject{{"email", op->email}, {"password", op->password}});
    } else {
        if (!accounts.contains(op->id)) { finish(op, true, {}); return; }
        accounts.remove(op->id);
    }
    SecretBytes plaintext{QJsonDocument(accounts).toJson(QJsonDocument::Compact)};
    QByteArray encrypted;
    if (plaintext.data.size() > maxPlaintext || !encrypt(plaintext.data, secret.data, encrypted)) {
        finish(op, false, QStringLiteral("Credential encryption failed or data limit exceeded.")); return;
    }
    QSaveFile output(m_path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(ownerOnly)
        || output.write(encrypted) != encrypted.size() || !output.commit()) {
        output.cancelWriting();
        finish(op, false, QStringLiteral("Atomic credential save failed.")); return;
    }
    finish(op, true, {});
}

bool SecureCredentialLocker::releaseLock()
{
    // Qt's Unix unlock() forgets ownership even if unlink fails. Keep the
    // original QLockFile (including its native lock) until access is restored;
    // never force-remove a supposedly stale lock belonging to another owner.
    if (m_lock && m_lock->isLocked()
        && !QFileInfo(QFileInfo(m_path).absolutePath()).isWritable()) {
        m_unlockRetry.start();
        return false;
    }
    m_lock.reset();
    m_unlockRetry.stop();
    return true;
}

void SecureCredentialLocker::finish(std::shared_ptr<Operation> op, bool ok, QString error,
                                    QString email, QString password)
{
    releaseLock();
    m_busy = false;
    // Finish operation state before callbacks, which may destroy this object.
    // A permission-blocked lock remains owned until release can be retried.
    wipe(op->email); wipe(op->password);
    if (op->done) op->done(ok, error);
    if (op->readDone) op->readDone(ok, email, password, error);
    wipe(email); wipe(password);
}
