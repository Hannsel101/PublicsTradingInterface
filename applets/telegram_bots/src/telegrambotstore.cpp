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
struct Record { QString id, nickname, token; ~Record() { wipe(token); } };
bool validNickname(const QString &s) {
    if (s.isEmpty() || s.size() > 128 || s.trimmed() != s) return false;
    for (const auto c : s) if (!c.isPrint() || c.isHighSurrogate() || c.isLowSurrogate()) return false;
    return true;
}
bool validToken(const QString &s) {
    static const QRegularExpression pattern(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    return pattern.match(s).hasMatch();
}
bool validId(const QString &s) {
    return s.size() == 36 && !QUuid(s).isNull() && QUuid(s).toString(QUuid::WithoutBraces) == s;
}
using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
bool crypt(bool encrypt, const QByteArray &input, const QByteArray &key, QByteArray &output) {
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
    enum class Action { Load, Add, Token };
    struct Operation {
        Action action;
        QString nickname, token, id;
        LoadCompletion load;
        AddCompletion add;
        TokenCompletion read;
        QList<Record> records;
        Secret key;
        QByteArray blob;
        bool missing = false;
        ~Operation() { wipe(token); }
    };
    using Op = std::shared_ptr<Operation>;
    Private(TelegramBotStore *owner, QString p, BinaryBackend b) : q(owner), path(std::move(p)), backend(std::move(b)) {
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
        QString token;
        if (ok && op->action == Action::Load)
            for (const auto &r : std::as_const(op->records)) entries.append({r.id, r.nickname});
        if (ok && op->action == Action::Token)
            for (const auto &r : std::as_const(op->records)) if (r.id == op->id) token = r.token;
        wipe(op->token); wipe(op->key.data); op->records.clear();
        if (active) { release(); busy = false; }
        // Nothing may access this/private after a user callback.
        if (op->load) op->load(ok, std::move(entries), error);
        else if (op->add) op->add(ok, ok ? op->id : QString(), error);
        else if (op->read) op->read(ok, token, error);
        wipe(token);
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
            if ((op->action == Action::Add && (!validNickname(op->nickname) || !validToken(op->token)))
                || (op->action == Action::Token && !validId(op->id))) {
                finish(op, false, QStringLiteral("Invalid bot fields or field limit exceeded.")); return;
            }
            if (path.isEmpty() || !backend) { finish(op, false, QStringLiteral("Bot store is not configured.")); return; }
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
                if (op->missing && op->action != Action::Add) {
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
                    if (keyStatus != Status::Missing || !op->missing || op->action != Action::Add || rh.exists() || rh.isSymLink()) {
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
    void execute(Op op) {
        if (!op->missing) {
            Secret plain;
            if (!crypt(false, op->blob, op->key.data, plain.data)) { finish(op, false, QStringLiteral("Bot catalog authentication failed.")); return; }
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(plain.data, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) { finish(op, false, QStringLiteral("Invalid bot catalog.")); return; }
            const auto root = document.object();
            if (root.size() != 2 || root.value("version") != QJsonValue(1) || !root.value("bots").isArray()
                || root.value("bots").toArray().size() > maxEntries) { finish(op, false, QStringLiteral("Invalid bot catalog.")); return; }
            QSet<QString> ids, names, tokens;
            for (const auto &value : root.value("bots").toArray()) {
                const auto obj = value.toObject();
                Record r{obj.value("id").toString(), obj.value("nickname").toString(), obj.value("token").toString()};
                if (!value.isObject() || obj.size() != 3 || !validId(r.id) || !validNickname(r.nickname) || !validToken(r.token)
                    || ids.contains(r.id) || names.contains(r.nickname.toCaseFolded()) || tokens.contains(r.token)) {
                    finish(op, false, QStringLiteral("Invalid or duplicate bot record.")); return;
                }
                ids.insert(r.id); names.insert(r.nickname.toCaseFolded()); tokens.insert(r.token); op->records.append(r);
            }
        }
        if (op->action == Action::Load) { finish(op, true); return; }
        if (op->action == Action::Token) {
            bool found = false;
            for (const auto &r : std::as_const(op->records)) if (r.id == op->id) found = true;
            finish(op, found, found ? QString() : QStringLiteral("Bot not found.")); return;
        }
        for (const auto &r : std::as_const(op->records))
            if (r.nickname.compare(op->nickname, Qt::CaseInsensitive) == 0 || r.token == op->token) {
                finish(op, false, QStringLiteral("Bot already saved.")); return;
            }
        if (op->records.size() >= maxEntries) { finish(op, false, QStringLiteral("Bot catalog limit exceeded.")); return; }
        op->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        op->records.append({op->id, op->nickname, op->token});
        QJsonArray bots;
        for (const auto &r : std::as_const(op->records)) bots.append(QJsonObject{{"id", r.id}, {"nickname", r.nickname}, {"token", r.token}});
        Secret plain{QJsonDocument(QJsonObject{{"version", 1}, {"bots", bots}}).toJson(QJsonDocument::Compact)};
        QByteArray encrypted;
        if (!crypt(true, plain.data, op->key.data, encrypted)) { finish(op, false, QStringLiteral("Bot catalog encryption failed or limit exceeded.")); return; }
        io(true, catalogService, catalogKey, std::move(encrypted), [this, op](Status status, QByteArray) {
            finish(op, status == Status::Success, status == Status::Success ? QString() : QStringLiteral("Bot catalog could not be saved."));
        });
    }
};
TelegramBotStore::TelegramBotStore(QObject *parent)
    : TelegramBotStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/profiles.dat"), {}, parent) {
    d->backend = systemBackend(this);
}
TelegramBotStore::TelegramBotStore(QString path, BinaryBackend backend, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(this, std::move(path), std::move(backend))) {}
TelegramBotStore::~TelegramBotStore() = default;
void TelegramBotStore::load(LoadCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Load; op->load = std::move(done); d->begin(op);
}
void TelegramBotStore::add(QString nickname, QString token, AddCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Add;
    op->nickname = std::move(nickname); op->token = std::move(token); op->add = std::move(done); d->begin(op);
}
void TelegramBotStore::readToken(QString id, TokenCompletion done) {
    auto op = std::make_shared<Private::Operation>(); op->action = Private::Action::Token;
    op->id = std::move(id); op->read = std::move(done); d->begin(op);
}
