#include "telegrambotstore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QCoreApplication>
#include <QRunnable>
#include <QThreadPool>
#include <atomic>
#include <QUuid>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <utility>
#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <keychain.h>
#endif

namespace {
const QString passwordCatalogService = QStringLiteral("PublicsTradingInterface.TelegramBots.PasswordVault");
const QString passwordCatalogKey = QStringLiteral("PasswordCatalog.v1");
// Legacy identities below are accessible ONLY in explicit injected fixture mode.
const QString catalogService = QStringLiteral("PublicsTradingInterface.TelegramBots");
const QString catalogKey = QStringLiteral("AES256GCM.Catalog.v1");
// EXACT existing Robin Hood identity: never rotate or replace an existing key.
const QString masterService = QStringLiteral("PublicsTradingInterface.RobinHood.CredentialLocker");
const QString masterKey = QStringLiteral("AES256GCM.MasterKey.v1");
const QByteArray header("PTITGBOT\0\1", 10);
constexpr int nonceSize = 12, tagSize = 16, overhead = 38, maxPlain = 1024 * 1024, maxEntries = 1024;
void wipe(QByteArray &s) { if (!s.isEmpty()) OPENSSL_cleanse(s.data(), size_t(s.size())); s.clear(); }
void wipe(QString &s) { if (!s.isEmpty()) OPENSSL_cleanse(s.data(), size_t(s.size()) * sizeof(QChar)); s.clear(); }
struct Secret { QByteArray data; ~Secret() { wipe(data); } };
struct Record {
    QString id, nickname, token;
    QStringList allowedChatIds;
    QByteArray passwordEnvelope;
    bool hasAllowedChatIds = false;
    ~Record() { wipe(token); for (auto &id : allowedChatIds) wipe(id); }
};
bool validNickname(const QString &s) {
    if (s.isEmpty() || s.size() > 128 || s.trimmed() != s) return false;
    for (const auto c : s) if (!c.isPrint() || c.isHighSurrogate() || c.isLowSurrogate()) return false;
    return true;
}
bool validToken(const QString &s) {
    static const QRegularExpression pattern(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    return pattern.match(s).hasMatch();
}
bool validPolicy(const QStringList &ids) {
    if (ids.size() > 1024) return false;
    QSet<QString> seen;
    for (const auto &id : ids) {
        if (!TelegramBotStore::isSupportedChatId(id) || seen.contains(id)) return false;
        seen.insert(id);
    }
    return true;
}
bool validId(const QString &s) {
    return s.size() == 36 && !QUuid(s).isNull() && QUuid(s).toString(QUuid::WithoutBraces) == s;
}
using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
bool crypt(bool encrypt, const QByteArray &input, const QByteArray &key, QByteArray &output, const QByteArray &binding = {}) {
    if (key.size() != 32 || (encrypt ? input.size() > maxPlain
        : input.size() < overhead + 2 || input.size() > overhead + maxPlain || !input.startsWith(header))) return false;
    QByteArray nonce = encrypt ? QByteArray(nonceSize, Qt::Uninitialized) : input.mid(header.size(), nonceSize);
    QByteArray tag = encrypt ? QByteArray(tagSize, Qt::Uninitialized) : input.mid(header.size() + nonceSize, tagSize);
    if (encrypt && RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonceSize) != 1) return false;
    const auto payload = encrypt ? input : input.mid(overhead);
    Secret result{QByteArray(payload.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized)};
    Context ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, total = 0;
    if (!ctx || EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, nonceSize, nullptr) != 1
        || EVP_CipherInit_ex(ctx.get(), nullptr, nullptr,
             reinterpret_cast<const unsigned char *>(key.constData()),
             reinterpret_cast<const unsigned char *>(nonce.constData()), encrypt) != 1
        || EVP_CipherUpdate(ctx.get(), nullptr, &n,
             reinterpret_cast<const unsigned char *>(header.constData()), int(header.size())) != 1
        || (!binding.isEmpty() && EVP_CipherUpdate(ctx.get(), nullptr, &n,
             reinterpret_cast<const unsigned char *>(binding.constData()), int(binding.size())) != 1)
        || EVP_CipherUpdate(ctx.get(), reinterpret_cast<unsigned char *>(result.data.data()), &n,
             reinterpret_cast<const unsigned char *>(payload.constData()), int(payload.size())) != 1) return false;
    total = n;
    if (!encrypt && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tagSize, tag.data()) != 1) return false;
    if (EVP_CipherFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(result.data.data()) + total, &n) != 1) return false;
    result.data.resize(total + n);
    if (encrypt) {
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tagSize, tag.data()) != 1) return false;
        output = header + nonce + tag + result.data;
    } else output = std::move(result.data);
    return true;
}

// Fixed, versioned parameters: persisted input cannot request cheaper or more
// expensive work. One process-wide worker bounds scrypt memory to 192 MiB.
const QByteArray passwordHeader("PTIPASS\0\1", 9);
constexpr int saltSize = 16, maxPasswordBytes = 4096;
bool validPassword(const QString &password) {
    if (password.size() > 1024 || !password.isValidUtf16()) return false;
    qsizetype characters = password.size();
    for (const auto c : password) if (c.isLowSurrogate()) --characters;
    Secret encoded{password.toUtf8()};
    return characters >= 12 && encoded.data.size() <= maxPasswordBytes;
}
bool validSessionSecret(const QString &secret) {
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{43}\\z"));
    const auto decoded = QByteArray::fromBase64(secret.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    return pattern.match(secret).hasMatch() && decoded.size() == 32
        && decoded.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) == secret.toLatin1();
}
struct PasswordWork {
    QString id, token, sessionSecret;
    QStringList allowedChatIds;
    Secret password;
    QByteArray envelope;
    bool save = false, ok = false, changed = false;
    ~PasswordWork() { wipe(token); wipe(sessionSecret); for (auto &id : allowedChatIds) wipe(id); }
};
bool passwordCrypt(bool encrypt, const QByteArray &input, const QByteArray &password,
                   const QString &id, QByteArray &output) {
    if (password.isEmpty() || password.size() > maxPasswordBytes || !validId(id)) return false;
    if (!encrypt && (input.size() < passwordHeader.size() + saltSize + overhead + 2
        || input.size() > maxPlain + passwordHeader.size() + saltSize + overhead
        || !input.startsWith(passwordHeader))) return false;
    QByteArray salt = encrypt ? QByteArray(saltSize, Qt::Uninitialized) : input.mid(passwordHeader.size(), saltSize);
    if (encrypt && RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), saltSize) != 1) return false;
    Secret key{QByteArray(32, Qt::Uninitialized)};
    if (EVP_PBE_scrypt(password.constData(), size_t(password.size()),
            reinterpret_cast<const unsigned char *>(salt.constData()), saltSize,
            uint64_t(1) << 17, 8, 1, uint64_t(192) * 1024 * 1024,
            reinterpret_cast<unsigned char *>(key.data.data()), 32) != 1) return false;
    QByteArray result;
    if (!crypt(encrypt, encrypt ? input : input.mid(passwordHeader.size() + saltSize),
            key.data, result, passwordHeader + salt + id.toUtf8())) return false;
    output = encrypt ? passwordHeader + salt + result : std::move(result);
    return true;
}
void runPasswordWork(const std::shared_ptr<PasswordWork> &work, const std::shared_ptr<std::atomic_bool> &cancelled) {
    if (cancelled->load()) return;
    if (!work->envelope.isEmpty()) {
        Secret plain;
        if (!passwordCrypt(false, work->envelope, work->password.data, work->id, plain.data)) return;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(plain.data, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return;
        const auto object = doc.object();
        if ((object.size() != 2 && object.size() != 3) || !object.contains("token")
            || !object.value("allowedChatIds").isArray()
            || (object.size() == 3 && !object.contains("sessionSecret"))) return;
        const auto token = object.value("token").toString();
        QStringList ids;
        for (const auto &id : object.value("allowedChatIds").toArray()) {
            if (!id.isString()) return;
            ids.append(id.toString());
        }
        if (!validToken(token) || !validPolicy(ids)) return;
        work->sessionSecret = object.value("sessionSecret").toString();
        if (object.contains("sessionSecret") && !validSessionSecret(work->sessionSecret)) return;
        if (!work->save || work->token.isEmpty()) work->token = token;
        if (!work->save) work->allowedChatIds = ids;
    }
    if (cancelled->load()) return;
    if (work->sessionSecret.isEmpty()) {
        Secret random{QByteArray(32, Qt::Uninitialized)};
        if (RAND_bytes(reinterpret_cast<unsigned char *>(random.data.data()), 32) != 1) return;
        work->sessionSecret = QString::fromLatin1(random.data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        work->changed = true;
    }
    if (work->save || work->changed) {
        Secret plain{QJsonDocument(QJsonObject{{"token", work->token},
            {"allowedChatIds", QJsonArray::fromStringList(work->allowedChatIds)},
            {"sessionSecret", work->sessionSecret}}).toJson(QJsonDocument::Compact)};
        QByteArray encrypted;
        if (!passwordCrypt(true, plain.data, work->password.data, work->id, encrypted)) return;
        work->envelope = std::move(encrypted);
    }
    wipe(work->password.data);
    work->ok = !cancelled->load();
}
QThreadPool &passwordPool() {
    static QThreadPool pool;
    static const bool configured = [] { pool.setMaxThreadCount(1); return true; }();
    Q_UNUSED(configured);
    return pool;
}

TelegramBotStore::BinaryBackend systemBackend(QObject *owner) {
    return [owner](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion done) {
        QKeychain::Job *job;
        if (write) {
            auto *w = new QKeychain::WritePasswordJob(service, owner);
            w->setBinaryData(bytes); job = w;
        } else job = new QKeychain::ReadPasswordJob(service, owner);
        job->setKey(key); job->setAutoDelete(false); job->setInsecureFallback(false);
        QObject::connect(job, &QKeychain::Job::finished, owner, [job, write, done = std::move(done)]() mutable {
            const auto status = job->error() == QKeychain::NoError ? TelegramBotStore::Status::Success
                : (!write && job->error() == QKeychain::EntryNotFound ? TelegramBotStore::Status::Missing : TelegramBotStore::Status::Failure);
            QByteArray data;
            if (!write && status == TelegramBotStore::Status::Success)
                data = static_cast<QKeychain::ReadPasswordJob *>(job)->binaryData();
            job->setParent(nullptr);
            const QScopedPointer<QKeychain::Job, QScopedPointerDeleteLater> cleanup(job);
            auto completion = std::exchange(done, {});
            if (completion) completion(status, std::move(data));
        });
        job->start();
    };
}
}

struct TelegramBotStore::Private {
    TelegramBotStore *q;
    QString path;
    BinaryBackend backend;
    std::unique_ptr<QLockFile> lock;
    QTimer retry;
    bool busy = false;
    const bool allowLegacyTestWrites;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    enum class Action { Load, Add, Token, SaveProtected, Remove };
    struct Operation {
        Action action;
        QString nickname, token, id, password, confirmation, sessionSecret;
        QStringList allowedChatIds;
        bool replacePolicy = false, passwordProvided = false;
        LoadCompletion load;
        AddCompletion add;
        ActivationCompletion activate;
        QList<Record> records;
        Secret key;
        QByteArray blob;
        bool missing = false;
        ~Operation() { wipe(token); wipe(password); wipe(confirmation); wipe(sessionSecret); for (auto &id : allowedChatIds) wipe(id); }
    };
    using Op = std::shared_ptr<Operation>;
    Private(TelegramBotStore *owner, QString p, BinaryBackend b, bool legacyFixture = false) : q(owner), path(std::move(p)), backend(std::move(b)), allowLegacyTestWrites(legacyFixture && bool(backend)) {
        if (!allowLegacyTestWrites)
            path = QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("telegram-password-vault"));
        retry.setInterval(100);
        QObject::connect(&retry, &QTimer::timeout, q, [this] { if (!busy) release(); });
    }
    bool release() {
        if (lock && lock->isLocked() && !QFileInfo(QFileInfo(path).absolutePath()).isWritable()) {
            retry.start(); return false;
        }
        lock.reset(); retry.stop(); return true;
    }
    void finish(Op op, bool ok, QString error = {}, bool active = true) {
        Entries entries;
        ActivationSnapshot snapshot;
        if (ok && op->action == Action::Load)
            for (const auto &r : std::as_const(op->records)) entries.append({r.id, r.nickname,
                r.passwordEnvelope.isEmpty() ? r.allowedChatIds : QStringList{},
                !r.passwordEnvelope.isEmpty(), r.passwordEnvelope.isEmpty() ? !r.allowedChatIds.isEmpty() : r.hasAllowedChatIds});
        if (ok && op->action == Action::Token)
            snapshot = {op->token, op->allowedChatIds, op->sessionSecret};
        wipe(op->token); wipe(op->password); wipe(op->confirmation); wipe(op->sessionSecret);
        wipe(op->key.data); op->records.clear();
        if (active) { release(); busy = false; }
        // Nothing may access this/private after a user callback.
        if (op->load) op->load(ok, std::move(entries), error);
        else if (op->add) op->add(ok, ok ? op->id : QString(), error);
        else if (op->activate) op->activate(ok, snapshot, error);
        wipe(snapshot.token); wipe(snapshot.sessionSecret);
        for (auto &id : snapshot.allowedChatIds) wipe(id);
    }
    void io(bool write, QString service, QString key, QByteArray data, BinaryCompletion done) {
        QPointer<TelegramBotStore> guard(q);
        auto once = std::make_shared<bool>(false);
        auto call = backend;
        call(write, std::move(service), std::move(key), std::move(data),
             [guard, once, done = std::move(done)](Status status, QByteArray bytes) mutable {
            auto secret = std::make_shared<Secret>(); secret->data = std::move(bytes);
            if (!guard || *once) return;
            *once = true;
            QTimer::singleShot(0, guard, [guard, secret, done = std::move(done), status]() mutable {
                if (guard) done(status, std::move(secret->data));
            });
        });
    }
    void begin(Op op) {
        if (busy) { QTimer::singleShot(0, q, [this, op] { finish(op, false, QStringLiteral("Bot store is busy."), false); }); return; }
        busy = true;
        QTimer::singleShot(0, q, [this, op] {
            if (op->action == Action::Add && !allowLegacyTestWrites) {
                finish(op, false, QStringLiteral("A password is required to save bot credentials.")); return;
            }
            if (op->action == Action::SaveProtected && op->password != op->confirmation) {
                finish(op, false, QStringLiteral("Passwords do not match exactly.")); return;
            }
            if ((op->action == Action::SaveProtected || op->passwordProvided) && !validPassword(op->password)) {
                finish(op, false, QStringLiteral("Password must have at least 12 characters and at most 1024 UTF-16 units / 4096 UTF-8 bytes.")); return;
            }
            wipe(op->confirmation);
            if ((op->action == Action::Add && (!validNickname(op->nickname) || !validPolicy(op->allowedChatIds) || !(validToken(op->token) || (op->replacePolicy && op->token.isEmpty()))))
                || (op->action == Action::SaveProtected && (!validNickname(op->nickname) || !validPolicy(op->allowedChatIds)
                    || (!op->id.isEmpty() && !validId(op->id)) || (!op->token.isEmpty() && !validToken(op->token))
                    || (op->id.isEmpty() && !validToken(op->token))))
                || ((op->action == Action::Token || op->action == Action::Remove) && !validId(op->id))) {
                finish(op, false, QStringLiteral("Invalid bot fields or field limit exceeded.")); return;
            }
            if (path.isEmpty() || !backend) { finish(op, false, QStringLiteral("Bot store is not configured.")); return; }
            if (!allowLegacyTestWrites) { beginPasswordVault(op); return; }
            QFileInfo info(path);
            if (!QDir().mkpath(info.absolutePath())) { finish(op, false, QStringLiteral("Credential lock directory unavailable.")); return; }
            path = QDir(QFileInfo(info.absolutePath()).canonicalFilePath()).filePath(info.fileName());
            if (!release()) { finish(op, false, QStringLiteral("Credential lock cleanup pending.")); return; }
            // Same lock/canonicalization/stale policy as SecureCredentialLocker.
            // Held across catalog RMW AND master-key read/create, including prompts.
            lock = std::make_unique<QLockFile>(path + QStringLiteral(".lock"));
            lock->setStaleLockTime(0);
            if (!lock->tryLock(0)) { finish(op, false, QStringLiteral("Credential store is locked.")); return; }
            const QFileInfo target(path);
            if (target.isSymLink() || (target.exists() && !target.isFile())) {
                finish(op, false, QStringLiteral("Invalid Robin Hood credential path.")); return;
            }
            io(false, catalogService, catalogKey, {}, [this, op](Status status, QByteArray blob) {
                if (status == Status::Failure) { finish(op, false, QStringLiteral("Bot catalog unavailable.")); return; }
                op->missing = status == Status::Missing;
                if (op->missing && op->action != Action::Add && op->action != Action::SaveProtected) {
                    finish(op, op->action == Action::Load, op->action == Action::Load ? QString() : QStringLiteral("Bot not found.")); return;
                }
                if (!op->missing && (blob.size() < overhead + 2 || blob.size() > maxPlain + overhead || !blob.startsWith(header))) {
                    finish(op, false, QStringLiteral("Invalid encrypted bot catalog.")); return;
                }
                op->blob = std::move(blob);
                io(false, masterService, masterKey, {}, [this, op](Status keyStatus, QByteArray key) {
                    op->key.data = std::move(key);
                    if (keyStatus == Status::Success && op->key.data.size() == 32) { execute(op); return; }
                    // A missing master with either encrypted store present is NEVER regenerated.
                    const QFileInfo rh(path);
                    if (keyStatus != Status::Missing || !op->missing || (op->action != Action::Add && op->action != Action::SaveProtected) || rh.exists() || rh.isSymLink()) {
                        finish(op, false, QStringLiteral("Master key unavailable or invalid.")); return;
                    }
                    op->key.data.resize(32);
                    if (RAND_bytes(reinterpret_cast<unsigned char *>(op->key.data.data()), 32) != 1) {
                        finish(op, false, QStringLiteral("Secure random generation failed.")); return;
                    }
                    io(true, masterService, masterKey, op->key.data, [this, op](Status saved, QByteArray) {
                        if (saved != Status::Success) { finish(op, false, QStringLiteral("Master key could not be saved.")); return; }
                        execute(op);
                    });
                });
            });
        });
    }
    void beginPasswordVault(Op op) {
        const QFileInfo info(path);
        if (!QDir().mkpath(info.absolutePath())) { finish(op, false, QStringLiteral("Telegram vault directory unavailable.")); return; }
        path = QDir(QFileInfo(info.absolutePath()).canonicalFilePath()).filePath(info.fileName());
        if (!release()) { finish(op, false, QStringLiteral("Telegram lock cleanup pending.")); return; }
        lock = std::make_unique<QLockFile>(path + QStringLiteral(".lock"));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) { finish(op, false, QStringLiteral("Telegram vault is locked.")); return; }
        io(false, passwordCatalogService, passwordCatalogKey, {}, [this, op](Status status, QByteArray bytes) {
            if (status == Status::Failure) { finish(op, false, QStringLiteral("Telegram password vault unavailable.")); return; }
            op->missing = status == Status::Missing;
            if (op->missing && op->action != Action::SaveProtected) {
                finish(op, op->action == Action::Load, op->action == Action::Load ? QString() : QStringLiteral("Bot not found in password vault; recreate legacy bots manually.")); return;
            }
            if (!op->missing && (bytes.isEmpty() || bytes.size() > maxPlain)) {
                finish(op, false, QStringLiteral("Invalid Telegram password vault; existing data was preserved.")); return;
            }
            op->blob = std::move(bytes); execute(op);
        });
    }
    void execute(Op op) {
        if (!op->missing) {
            Secret plain;
            if (allowLegacyTestWrites) {
                if (!crypt(false, op->blob, op->key.data, plain.data)) { finish(op, false, QStringLiteral("Bot catalog authentication failed.")); return; }
            } else plain.data = op->blob;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(plain.data, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) { finish(op, false, QStringLiteral("Invalid bot catalog.")); return; }
            const auto root = document.object();
            const auto version = root.value("version");
            if (!allowLegacyTestWrites && version != QJsonValue(3)) {
                finish(op, false, QStringLiteral("Unsupported Telegram vault format; recreate legacy bots manually.")); return;
            }
            if (root.size() != 2 || (version != QJsonValue(1) && version != QJsonValue(2) && version != QJsonValue(3)) || !root.value("bots").isArray()
                || root.value("bots").toArray().size() > maxEntries) { finish(op, false, QStringLiteral("Invalid bot catalog.")); return; }
            QSet<QString> ids, names, tokens;
            for (const auto &value : root.value("bots").toArray()) {
                const auto obj = value.toObject();
                Record r{obj.value("id").toString(), obj.value("nickname").toString(), obj.value("token").toString()};
                const bool protectedRecord = version == QJsonValue(3) && obj.contains("passwordEnvelope");
                if (protectedRecord) {
                    const auto encoded = obj.value("passwordEnvelope").toString().toLatin1();
                    r.passwordEnvelope = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
                    if (obj.size() != 4 || !obj.value("hasAllowedChatIds").isBool()
                        || r.passwordEnvelope.toBase64() != encoded
                        || !r.passwordEnvelope.startsWith(passwordHeader)
                        || r.passwordEnvelope.size() < passwordHeader.size() + saltSize + overhead + 2
                        || obj.contains("token") || obj.contains("allowedChatIds")) {
                        finish(op, false, QStringLiteral("Invalid password-protected bot record.")); return;
                    }
                    r.hasAllowedChatIds = obj.value("hasAllowedChatIds").toBool();
                } else {
                    if (!allowLegacyTestWrites) { finish(op, false, QStringLiteral("Unprotected bot records are not permitted; legacy catalog was not modified.")); return; }
                    if (version != QJsonValue(1)) {
                        if (!obj.value("allowedChatIds").isArray()) { finish(op, false, QStringLiteral("Invalid chat policy.")); return; }
                        for (const auto &chat : obj.value("allowedChatIds").toArray()) {
                            if (!chat.isString()) { finish(op, false, QStringLiteral("Invalid chat policy.")); return; }
                            r.allowedChatIds.append(chat.toString());
                        }
                    }
                    if (!validPolicy(r.allowedChatIds)) {
                        finish(op, false, QStringLiteral("Unsupported or invalid chat policy: private Chat IDs must be 1..4503599627370495, without duplicates.")); return;
                    }
                    if (obj.size() != (version == QJsonValue(1) ? 3 : 4) || !validToken(r.token) || tokens.contains(r.token)) {
                        finish(op, false, QStringLiteral("Invalid or duplicate bot record.")); return;
                    }
                    tokens.insert(r.token);
                }
                if (!value.isObject() || !validId(r.id) || !validNickname(r.nickname)
                    || ids.contains(r.id) || names.contains(r.nickname.toCaseFolded())) {
                    finish(op, false, QStringLiteral("Invalid or duplicate bot record.")); return;
                }
                ids.insert(r.id); names.insert(r.nickname.toCaseFolded()); op->records.append(r);
            }
        }
        if (op->action == Action::Load) { finish(op, true); return; }
        if (op->action == Action::Token || op->action == Action::SaveProtected || op->action == Action::Remove) {
            int index = -1;
            for (int i = 0; i < op->records.size(); ++i) if (op->records[i].id == op->id) index = i;
            if (op->action == Action::SaveProtected && op->id.isEmpty()) {
                if (op->records.size() >= maxEntries) { finish(op, false, QStringLiteral("Bot catalog limit exceeded.")); return; }
                op->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                op->records.append({op->id, op->nickname, op->token, op->allowedChatIds});
                index = int(op->records.size()) - 1;
            }
            if (index < 0) { finish(op, false, QStringLiteral("Bot not found.")); return; }
            if (op->action == Action::Remove) {
                op->records.removeAt(index); commit(op); return;
            }
            auto &record = op->records[index];
            if (op->action == Action::SaveProtected) {
                for (int i = 0; i < op->records.size(); ++i) {
                    if (i != index && op->records[i].nickname.compare(op->nickname, Qt::CaseInsensitive) == 0) {
                        finish(op, false, QStringLiteral("Bot nickname already saved.")); return;
                    }
                }
            } else if (record.passwordEnvelope.isEmpty()) {
                if (op->passwordProvided) {
                    finish(op, false, QStringLiteral("Legacy bot must first be edited and password-protected.")); return;
                }
                op->token = record.token; op->allowedChatIds = record.allowedChatIds;
                finish(op, true); return;
            } else if (!op->passwordProvided) {
                finish(op, false, QStringLiteral("A password is required to unlock this bot.")); return;
            }
            startPasswordWork(op, index); return;
        }
        Record *existing = nullptr;
        for (auto &r : op->records) {
            if (r.nickname.compare(op->nickname, Qt::CaseInsensitive) == 0) {
                if (!op->replacePolicy) { finish(op, false, QStringLiteral("Bot already saved.")); return; }
                existing = &r;
            } else if (r.token == op->token) {
                finish(op, false, QStringLiteral("Bot already saved.")); return;
            }
        }
        if (existing) {
            if (!existing->passwordEnvelope.isEmpty()) {
                finish(op, false, QStringLiteral("A password is required to edit this bot.")); return;
            }
            op->id = existing->id;
            if (!op->token.isEmpty()) existing->token = op->token;
            existing->allowedChatIds = op->allowedChatIds;
        } else {
            if (!validToken(op->token)) { finish(op, false, QStringLiteral("A new bot requires a token.")); return; }
            for (const auto &r : std::as_const(op->records)) if (r.token == op->token) {
                finish(op, false, QStringLiteral("Bot already saved.")); return;
            }
            if (op->records.size() >= maxEntries) { finish(op, false, QStringLiteral("Bot catalog limit exceeded.")); return; }
            op->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            op->records.append({op->id, op->nickname, op->token, op->allowedChatIds});
        }
        commit(op);
    }
    void startPasswordWork(Op op, int index) {
        const auto &record = op->records[index];
        auto work = std::make_shared<PasswordWork>();
        work->id = op->id;
        work->save = op->action == Action::SaveProtected;
        work->password.data = op->password.toUtf8(); wipe(op->password);
        work->envelope = record.passwordEnvelope;
        work->token = op->token.isEmpty() ? record.token : op->token;
        work->allowedChatIds = work->save ? op->allowedChatIds : record.allowedChatIds;
        QPointer<TelegramBotStore> guard(q);
        auto cancellation = cancelled;
        auto *task = QRunnable::create([work, cancellation, guard, this, op, index] {
            runPasswordWork(work, cancellation);
            wipe(work->password.data);
            if (cancellation->load()) return;
            // Only inspect QPointer and touch store state on the QObject thread.
            QTimer::singleShot(0, QCoreApplication::instance(), [work, guard, this, op, index] {
                if (!guard) return;
                if (!work->ok) { finish(op, false, QStringLiteral("Incorrect password or damaged bot credentials.")); return; }
                auto &record = op->records[index];
                if (!work->save && record.hasAllowedChatIds != !work->allowedChatIds.isEmpty()) {
                    finish(op, false, QStringLiteral("Bot policy metadata does not match authenticated credentials.")); return;
                }
                record.passwordEnvelope = work->envelope;
                record.hasAllowedChatIds = !work->allowedChatIds.isEmpty();
                wipe(record.token); for (auto &id : record.allowedChatIds) wipe(id); record.allowedChatIds.clear();
                if (work->save) record.nickname = op->nickname;
                else {
                    op->token = work->token; op->allowedChatIds = work->allowedChatIds;
                    op->sessionSecret = work->sessionSecret;
                }
                // Missing session secrets are generated once and committed before
                // exposing an activation snapshot, never rotated on normal Start.
                if (work->save || work->changed) commit(op);
                else finish(op, true);
            });
        });
        if (!passwordPool().tryStart(task)) {
            delete task;
            finish(op, false, QStringLiteral("Password worker is busy; try again."));
        }
    }
    void commit(Op op) {
        QJsonArray bots;
        bool anyProtected = false;
        for (const auto &r : std::as_const(op->records)) {
            if (!r.passwordEnvelope.isEmpty()) {
                anyProtected = true;
                bots.append(QJsonObject{{"id", r.id}, {"nickname", r.nickname},
                    {"passwordEnvelope", QString::fromLatin1(r.passwordEnvelope.toBase64())},
                    {"hasAllowedChatIds", r.hasAllowedChatIds}});
            } else bots.append(QJsonObject{{"id", r.id}, {"nickname", r.nickname}, {"token", r.token}, {"allowedChatIds", QJsonArray::fromStringList(r.allowedChatIds)}});
        }
        Secret plain{QJsonDocument(QJsonObject{{"version", (anyProtected || !allowLegacyTestWrites) ? 3 : 2}, {"bots", bots}}).toJson(QJsonDocument::Compact)};
        if (plain.data.size() > maxPlain) { finish(op, false, QStringLiteral("Telegram vault limit exceeded.")); return; }
        if (!allowLegacyTestWrites) {
            io(true, passwordCatalogService, passwordCatalogKey, plain.data, [this, op](Status status, QByteArray) {
                finish(op, status == Status::Success, status == Status::Success ? QString() : QStringLiteral("Telegram password vault could not be saved."));
            }); return;
        }
        QByteArray encrypted;
        if (!crypt(true, plain.data, op->key.data, encrypted)) { finish(op, false, QStringLiteral("Bot catalog encryption failed or limit exceeded.")); return; }
        io(true, catalogService, catalogKey, std::move(encrypted), [this, op](Status status, QByteArray) {
            finish(op, status == Status::Success, status == Status::Success ? QString() : QStringLiteral("Bot catalog could not be saved."));
        });
    }
};
TelegramBotStore::TelegramBotStore(QObject *parent)
    : TelegramBotStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/telegram-password-vault"), {}, parent) {
    d->backend = systemBackend(this);
}
TelegramBotStore::TelegramBotStore(QString path, BinaryBackend backend, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, std::move(path), std::move(backend))) {}
TelegramBotStore::TelegramBotStore(QString path, BinaryBackend backend, LegacyFixture, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, std::move(path), std::move(backend), true)) {}
TelegramBotStore::~TelegramBotStore() { d->cancelled->store(true); }
bool TelegramBotStore::isSupportedChatId(const QString &id) {
    static const QRegularExpression decimal(QStringLiteral("\\A[1-9][0-9]{0,15}\\z"));
    bool ok = false;
    const auto number = id.toLongLong(&ok);
    return decimal.match(id).hasMatch() && ok && number > 0 && number <= maximumAllowedChatId;
}
void TelegramBotStore::load(LoadCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Load; op->load = std::move(done); d->begin(op);
}
void TelegramBotStore::add(QString nickname, QString token, AddCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Add;
    op->nickname = std::move(nickname); op->token = std::move(token); op->add = std::move(done); d->begin(op);
}
void TelegramBotStore::saveWithAllowedChats(QString nickname, QString token, QStringList allowedChatIds, AddCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Add;
    op->nickname = std::move(nickname); op->token = std::move(token);
    op->allowedChatIds = std::move(allowedChatIds); op->replacePolicy = true;
    op->add = std::move(done); d->begin(op);
}
void TelegramBotStore::readToken(QString id, TokenCompletion done) {
    readActivationSnapshot(std::move(id), [done = std::move(done)](bool ok, ActivationSnapshot snapshot, QString error) {
        if (done) done(ok, std::move(snapshot.token), std::move(error));
    });
}
void TelegramBotStore::saveProtectedBot(QString id, QString nickname, QString botToken,
                                        QStringList allowedChatIds, QString password,
                                        QString confirmation, AddCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::SaveProtected;
    op->id = std::move(id); op->nickname = std::move(nickname); op->token = std::move(botToken);
    op->allowedChatIds = std::move(allowedChatIds); op->password = std::move(password);
    op->confirmation = std::move(confirmation); op->add = std::move(done); d->begin(op);
}
void TelegramBotStore::readActivationSnapshot(QString id, QString password, ActivationCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Token;
    op->id = std::move(id); op->password = std::move(password); op->passwordProvided = true;
    op->activate = std::move(done); d->begin(op);
}
void TelegramBotStore::remove(QString id, AddCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Remove;
    op->id = std::move(id); op->add = std::move(done); d->begin(op);
}
void TelegramBotStore::readActivationSnapshot(QString id, ActivationCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Token;
    op->id = std::move(id); op->activate = std::move(done); d->begin(op);
}
