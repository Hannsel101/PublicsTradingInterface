#include "telegrambotstore.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QHash>
#include <QFile>
#include <QLockFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <openssl/evp.h>

namespace {
const QString catalogAddress = "PublicsTradingInterface.TelegramBots/AES256GCM.Catalog.v1";
const QString passwordAddress = "PublicsTradingInterface.TelegramBots.PasswordVault/PasswordCatalog.v1";
const QString masterAddress = "PublicsTradingInterface.RobinHood.CredentialLocker/AES256GCM.MasterKey.v1";
const QString sampleToken = "123456:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi";
// Independent test fixture writer, not a fake encryption backend.
QByteArray envelope(const QByteArray &plain, const QByteArray &key) {
    const QByteArray header("PTITGBOT\0\1", 10);
    const QByteArray nonce(12, 'n');
    QByteArray cipher(plain.size() + 16, Qt::Uninitialized), tag(16, Qt::Uninitialized);
    auto *ctx = EVP_CIPHER_CTX_new();
    int n = 0, total = 0;
    bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_EncryptUpdate(ctx, nullptr, &n, reinterpret_cast<const unsigned char *>(header.constData()), int(header.size())) == 1
        && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &n,
             reinterpret_cast<const unsigned char *>(plain.constData()), int(plain.size())) == 1;
    total = n;
    ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + total, &n) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    cipher.resize(total + n);
    return header + nonce + tag + cipher;
}
const QString password = "correct horse battery staple";
const QString fixtureId = "11111111-1111-1111-1111-111111111111";
// Independently decrypt the OS-key envelope: this must reveal only metadata and
// opaque password ciphertext, never the protected credentials or password.
QByteArray openEnvelope(const QByteArray &blob, const QByteArray &key, QByteArray binding = {}) {
    if (blob.size() < 40) return {};
    const auto header = blob.left(10), nonce = blob.mid(10, 12);
    auto tag = blob.mid(22, 16);
    const auto cipher = blob.mid(38);
    QByteArray plain(cipher.size() + 16, Qt::Uninitialized);
    auto *ctx = EVP_CIPHER_CTX_new(); int n = 0, total = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_DecryptUpdate(ctx, nullptr, &n, reinterpret_cast<const unsigned char *>(header.constData()), int(header.size())) == 1;
    if (ok && !binding.isEmpty()) ok = EVP_DecryptUpdate(ctx, nullptr, &n,
        reinterpret_cast<const unsigned char *>(binding.constData()), int(binding.size())) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &n,
        reinterpret_cast<const unsigned char *>(cipher.constData()), int(cipher.size())) == 1;
    total = n;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1
        && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + total, &n) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    plain.resize(total + n); return plain;
}
QByteArray innerFixture(const QString &id, const QJsonObject &payload) {
    const QByteArray marker("PTIPASS\0\1", 9), salt(16, 's'), nonce(12, 'n');
    QByteArray key(32, Qt::Uninitialized);
    const auto bytes = password.toUtf8();
    if (EVP_PBE_scrypt(bytes.constData(), size_t(bytes.size()), reinterpret_cast<const unsigned char *>(salt.constData()),
        16, uint64_t(1) << 17, 8, 1, uint64_t(192) * 1024 * 1024,
        reinterpret_cast<unsigned char *>(key.data()), 32) != 1) return {};
    const QByteArray header("PTITGBOT\0\1", 10), binding = marker + salt + id.toUtf8();
    const auto plain = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray cipher(plain.size() + 16, Qt::Uninitialized), tag(16, Qt::Uninitialized);
    auto *ctx = EVP_CIPHER_CTX_new(); int n = 0, total = 0;
    bool ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_EncryptUpdate(ctx, nullptr, &n, reinterpret_cast<const unsigned char *>(header.constData()), int(header.size())) == 1
        && EVP_EncryptUpdate(ctx, nullptr, &n, reinterpret_cast<const unsigned char *>(binding.constData()), int(binding.size())) == 1
        && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &n,
            reinterpret_cast<const unsigned char *>(plain.constData()), int(plain.size())) == 1;
    total = n;
    ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + total, &n) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    cipher.resize(total + n); return marker + salt + header + nonce + tag + cipher;
}
QJsonObject protectedRecord(const QString &id, const QByteArray &inner, bool hasIds = true) {
    return {{"id", id}, {"nickname", "Desk"}, {"passwordEnvelope", QString::fromLatin1(inner.toBase64())}, {"hasAllowedChatIds", hasIds}};
}
}

struct MemoryKeychain {
    QHash<QString, QByteArray> data;
    int calls = 0;
    TelegramBotStore::BinaryBackend backend() {
        return [this](bool write, QString service, QString key, QByteArray bytes,
                      TelegramBotStore::BinaryCompletion done) {
            ++calls;
            const QString address = service + '/' + key;
            if (write) { data[address] = bytes; done(TelegramBotStore::Status::Success, {}); }
            else if (data.contains(address)) done(TelegramBotStore::Status::Success, data[address]);
            else done(TelegramBotStore::Status::Missing, {});
        };
    }
};
// The old encrypted-catalog suite is intentionally confined to an explicit
// injected fixture constructor; production never enters this compatibility path.
class LegacyTelegramBotStore : public TelegramBotStore {
public:
    LegacyTelegramBotStore(QString path, BinaryBackend backend, QObject *parent = nullptr)
        : TelegramBotStore(std::move(path), std::move(backend), LegacyFixture::Enabled, parent) {}
};
class TestTelegramBotStore : public QObject {
    Q_OBJECT
private slots:
    void passwordProtectedRoundTrip() {
        QTemporaryDir dir; MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
        const QString password = "correct horse battery staple";
        QString id; bool done = false;
        store.saveProtectedBot({}, "Private", sampleToken, {"42"}, password, password,
            [&](bool ok, QString saved, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); id = saved; });
        QTRY_VERIFY(done); QVERIFY(!id.isEmpty()); done = false;
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 1);
            QVERIFY(entries.first().passwordProtected); QVERIFY(entries.first().hasAllowedChatIds);
            QVERIFY(entries.first().allowedChatIds.isEmpty());
        }); QTRY_VERIFY(done); done = false;
        store.readActivationSnapshot(id, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.token, sampleToken);
            QCOMPARE(snapshot.allowedChatIds, QStringList{"42"}); QCOMPARE(snapshot.sessionSecret.size(), 43);
        }); QTRY_VERIFY(done);
    }
    void independentPasswordVaultNeverAccessesRobinhood() {
        QTemporaryDir dir; MemoryKeychain memory;
        // Existing legacy data is opaque and must survive new-vault operations.
        memory.data[catalogAddress] = "opaque-legacy-catalog-must-not-be-opened";
        memory.data[masterAddress] = "unavailable-legacy-master-must-not-be-read";
        const auto legacyCatalog = memory.data[catalogAddress], legacyMaster = memory.data[masterAddress];
        const auto profiles = dir.filePath("profiles.dat");
        QFile rh(profiles); QVERIFY(rh.open(QIODevice::WriteOnly)); rh.write("untouched-robinhood-data"); rh.close();
        QLockFile rhLock(profiles + ".lock"); QVERIFY(rhLock.tryLock(0));
        const auto backend = memory.backend(); int forbidden = 0;
        TelegramBotStore store(profiles, [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion completion) {
            if (service != "PublicsTradingInterface.TelegramBots.PasswordVault") { ++forbidden; completion(TelegramBotStore::Status::Failure, {}); return; }
            backend(write, service, key, bytes, std::move(completion));
        }); QString id; bool done = false;
        store.saveProtectedBot({}, "Desk", sampleToken, {}, password, password,
            [&](bool ok, QString saved, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); id = saved; });
        QTRY_VERIFY(done); QCOMPARE(forbidden, 0); QCOMPARE(memory.data[catalogAddress], legacyCatalog);
        QCOMPARE(memory.data[masterAddress], legacyMaster); done = false;
        // Unlock is still independent after the old master is removed entirely.
        memory.data.remove(masterAddress);
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 1); });
        QTRY_VERIFY(done); done = false;
        store.readActivationSnapshot(id, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.token, sampleToken);
        }); QTRY_VERIFY(done); done = false;
        store.remove(id, [&](bool ok, QString, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); });
        QTRY_VERIFY(done); QCOMPARE(forbidden, 0); QCOMPARE(memory.data[catalogAddress], legacyCatalog);
        QVERIFY(!memory.data.contains(masterAddress)); QVERIFY(rh.open(QIODevice::ReadOnly)); QCOMPARE(rh.readAll(), QByteArray("untouched-robinhood-data"));
    }
    void passwordValidation_data() {
        QTest::addColumn<QString>("first"); QTest::addColumn<QString>("second");
        QTest::newRow("mismatch") << password << (password + " ");
        QTest::newRow("short") << QString("elevenchars") << QString("elevenchars");
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("bounded") << QString(1025, 'p') << QString(1025, 'p');
        QTest::newRow("invalid-unicode") << (QString(12, 'p') + QChar(0xd800)) << (QString(12, 'p') + QChar(0xd800));
    }
    void passwordValidation() {
        QFETCH(QString, first); QFETCH(QString, second);
        QTemporaryDir dir; MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool done = false;
        store.saveProtectedBot({}, "Desk", sampleToken, {}, first, second,
            [&](bool ok, QString id, QString error) { done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(memory.calls, 0); QVERIFY(memory.data.isEmpty());
    }
    void protectedCredentialsRequirePasswordEveryReadAndEdit() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        TelegramBotStore store(path, memory.backend()); QString id; bool done = false;
        // Spaces are significant and are not stripped by setup or activation.
        const auto exactPassword = " " + password + " ";
        store.saveProtectedBot({}, "Desk", sampleToken, {}, exactPassword, exactPassword,
            [&](bool ok, QString saved, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); id = saved; });
        QTRY_VERIFY(done); const auto before = memory.data;
        for (int variant = 0; variant < 4; ++variant) {
            done = false;
            auto callback = [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
                done = true; QVERIFY(!ok); QVERIFY(!error.isEmpty()); QVERIFY(snapshot.token.isEmpty());
                QVERIFY(snapshot.allowedChatIds.isEmpty()); QVERIFY(snapshot.sessionSecret.isEmpty());
            };
            if (variant == 0) store.readActivationSnapshot(id, callback);
            else if (variant == 1) store.readActivationSnapshot(id, password, callback);
            else if (variant == 2) store.readActivationSnapshot(id, "wrong password long enough", callback);
            else store.readToken(id, [&](bool ok, QString token, QString error) {
                done = true; QVERIFY(!ok); QVERIFY(token.isEmpty()); QVERIFY(!error.isEmpty());
            });
            QTRY_VERIFY(done); QCOMPARE(memory.data, before);
        }
        done = false;
        store.saveProtectedBot(id, "Hijacked", "777:new_token", {"99"}, password, password,
            [&](bool ok, QString saved, QString error) { done = true; QVERIFY(!ok); QVERIFY(saved.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(memory.data, before);
        done = false;
        store.saveWithAllowedChats("Desk", "777:new_token", {"99"}, [&](bool ok, QString saved, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(saved.isEmpty()); QVERIFY(!error.isEmpty());
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before);
        done = false;
        TelegramBotStore fresh(path, memory.backend());
        fresh.readActivationSnapshot(id, exactPassword, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.token, sampleToken); QVERIFY(snapshot.allowedChatIds.isEmpty());
            QCOMPARE(snapshot.sessionSecret.size(), 43);
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before);
    }
    void protectedEditUsesCurrentPasswordAndFreshSaltNoncePersistentSecret() {
        QTemporaryDir dir; MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); QString id, secret; bool done = false;
        store.saveProtectedBot({}, "Desk", sampleToken, {"42"}, password, password,
            [&](bool ok, QString saved, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); id = saved; });
        QTRY_VERIFY(done); done = false;
        const auto firstPlain = memory.data[passwordAddress];
        QVERIFY(!firstPlain.contains(sampleToken.toUtf8())); QVERIFY(!firstPlain.contains(password.toUtf8()));
        const auto firstRecord = QJsonDocument::fromJson(firstPlain).object()["bots"].toArray().first().toObject();
        QCOMPARE(firstRecord.size(), 4); QVERIFY(!firstRecord.contains("token")); QVERIFY(!firstRecord.contains("allowedChatIds"));
        const auto firstInner = QByteArray::fromBase64(firstRecord["passwordEnvelope"].toString().toLatin1());
        store.readActivationSnapshot(id, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); secret = snapshot.sessionSecret;
        }); QTRY_VERIFY(done); done = false;
        store.saveProtectedBot(id, "Renamed", {}, {"4503599627370495"}, password, password,
            [&](bool ok, QString saved, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(saved, id); });
        QTRY_VERIFY(done); done = false;
        const auto secondPlain = memory.data[passwordAddress];
        const auto secondRecord = QJsonDocument::fromJson(secondPlain).object()["bots"].toArray().first().toObject();
        const auto secondInner = QByteArray::fromBase64(secondRecord["passwordEnvelope"].toString().toLatin1());
        QVERIFY(firstInner.mid(9, 16) != secondInner.mid(9, 16));
        QVERIFY(firstInner.mid(35, 12) != secondInner.mid(35, 12));
        QVERIFY(!secondPlain.contains(secret.toUtf8())); QVERIFY(!secondPlain.contains("4503599627370495"));
        const auto beforeReads = memory.data;
        for (int i = 0; i < 2; ++i) {
            done = false;
            store.readActivationSnapshot(id, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
                done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.token, sampleToken);
                QCOMPARE(snapshot.sessionSecret, secret); QCOMPARE(snapshot.allowedChatIds, QStringList{"4503599627370495"});
            }); QTRY_VERIFY(done); QCOMPARE(memory.data, beforeReads);
        }
        for (const auto &bytes : std::as_const(memory.data)) {
            QVERIFY(!bytes.contains(password.toUtf8())); QVERIFY(!bytes.contains(secret.toUtf8())); QVERIFY(!bytes.contains(sampleToken.toUtf8()));
        }
    }
    void passwordEnvelopeTampering_data() {
        QTest::addColumn<int>("position");
        QTest::newRow("salt") << 9; QTest::newRow("nonce") << 35;
        QTest::newRow("tag") << 47; QTest::newRow("ciphertext") << 63;
        QTest::newRow("uuid-aad") << -1; QTest::newRow("policy-metadata") << -2;
    }
    void passwordEnvelopeTampering() {
        QFETCH(int, position);
        QTemporaryDir dir; MemoryKeychain memory; memory.data[masterAddress] = QByteArray(32, 'k');
        auto inner = innerFixture(fixtureId, {{"token", sampleToken}, {"allowedChatIds", QJsonArray{"42"}},
            {"sessionSecret", QString(43, 'A')}}); QVERIFY(!inner.isEmpty());
        QString id = fixtureId;
        if (position >= 0) inner[position] = char(inner.at(position) ^ 1);
        if (position == -1) id = "22222222-2222-2222-2222-222222222222";
        memory.data[passwordAddress] = QJsonDocument(QJsonObject{{"version", 3},
            {"bots", QJsonArray{protectedRecord(id, inner, position != -2)}}}).toJson();
        const auto before = memory.data;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool done = false;
        store.readActivationSnapshot(id, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(!error.isEmpty()); QVERIFY(snapshot.token.isEmpty());
            QVERIFY(snapshot.allowedChatIds.isEmpty()); QVERIFY(snapshot.sessionSecret.isEmpty());
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before);
    }
    void missingSessionSecretGeneratedOnceAndCommittedBeforeActivation() {
        QTemporaryDir dir; MemoryKeychain memory; memory.data[masterAddress] = QByteArray(32, 'k');
        const auto inner = innerFixture(fixtureId, {{"token", sampleToken}, {"allowedChatIds", QJsonArray{"42"}}});
        memory.data[passwordAddress] = QJsonDocument(QJsonObject{{"version", 3},
            {"bots", QJsonArray{protectedRecord(fixtureId, inner)}}}).toJson();
        const auto before = memory.data; bool failWrite = true; int writes = 0;
        auto delegate = memory.backend();
        TelegramBotStore store(dir.filePath("profiles.dat"), [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion callback) {
            if (write) { ++writes; if (failWrite) { callback(TelegramBotStore::Status::Failure, {}); return; } }
            delegate(write, service, key, bytes, std::move(callback));
        }); bool done = false; QString secret;
        store.readActivationSnapshot(fixtureId, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(snapshot.sessionSecret.isEmpty()); QVERIFY(!error.isEmpty());
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before); QCOMPARE(writes, 1);
        failWrite = false; done = false;
        store.readActivationSnapshot(fixtureId, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.token, sampleToken); secret = snapshot.sessionSecret; QCOMPARE(secret.size(), 43);
        }); QTRY_VERIFY(done); QCOMPARE(writes, 2); QVERIFY(memory.data != before); const auto committed = memory.data;
        done = false;
        store.readActivationSnapshot(fixtureId, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.sessionSecret, secret);
        }); QTRY_VERIFY(done); QCOMPARE(writes, 2); QCOMPARE(memory.data, committed);
    }
    void atomicLegacyMigrationAndFailureRollback() {
        QTemporaryDir dir; MemoryKeychain memory; memory.data[masterAddress] = QByteArray(32, 'k');
        const QJsonObject legacy{{"id", fixtureId}, {"nickname", "Desk"}, {"token", sampleToken}, {"allowedChatIds", QJsonArray{"1234567890123"}}};
        memory.data[catalogAddress] = envelope(QJsonDocument(QJsonObject{{"version", 2}, {"bots", QJsonArray{legacy}}}).toJson(), memory.data[masterAddress]);
        const auto before = memory.data; bool failWrite = true; int writes = 0;
        auto delegate = memory.backend();
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion callback) {
            if (write) { ++writes; if (failWrite) { callback(TelegramBotStore::Status::Failure, {}); return; } }
            delegate(write, service, key, bytes, std::move(callback));
        }); bool done = false;
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QVERIFY(!entries.first().passwordProtected);
            QCOMPARE(entries.first().allowedChatIds, QStringList{"1234567890123"}); QVERIFY(entries.first().hasAllowedChatIds);
        }); QTRY_VERIFY(done); done = false;
        store.saveProtectedBot(fixtureId, "Desk", {}, {"42"}, password, password,
            [&](bool ok, QString id, QString error) { done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(writes, 1); QCOMPARE(memory.data, before); failWrite = false; done = false;
        store.saveProtectedBot(fixtureId, "Desk", {}, {"42"}, password, password,
            [&](bool ok, QString id, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(id, fixtureId); });
        QTRY_VERIFY(done); QCOMPARE(writes, 2); QCOMPARE(memory.data[masterAddress], before[masterAddress]);
        const auto plain = openEnvelope(memory.data[catalogAddress], memory.data[masterAddress]);
        QVERIFY(!plain.contains(sampleToken.toUtf8())); QVERIFY(!plain.contains("1234567890123"));
        const auto record = QJsonDocument::fromJson(plain).object()["bots"].toArray().first().toObject();
        QCOMPARE(record.size(), 4); QVERIFY(!record.contains("token")); QVERIFY(!record.contains("allowedChatIds")); done = false;
        store.readActivationSnapshot(fixtureId, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.token, sampleToken); QCOMPARE(snapshot.allowedChatIds, QStringList{"42"});
        }); QTRY_VERIFY(done);
    }
    void deletionIsTransactionalAndNeverTouchesMasterOrReplayLedger() {
        QTemporaryDir dir; MemoryKeychain memory; memory.data[masterAddress] = QByteArray(32, 'k');
        const QJsonObject first{{"id", fixtureId}, {"nickname", "Desk"}, {"token", sampleToken}, {"allowedChatIds", QJsonArray{"42"}}};
        auto other = first; other["id"] = "22222222-2222-2222-2222-222222222222"; other["nickname"] = "Other"; other["token"] = "987:other";
        memory.data[catalogAddress] = envelope(QJsonDocument(QJsonObject{{"version", 2}, {"bots", QJsonArray{first, other}}}).toJson(), memory.data[masterAddress]);
        memory.data["ReplayLedger"] = "immutable-ledger";
        const auto before = memory.data; auto delegate = memory.backend(); bool failWrite = true; int writes = 0;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion callback) {
            if (write) { ++writes; QCOMPARE(service + '/' + key, catalogAddress); if (failWrite) { callback(TelegramBotStore::Status::Failure, {}); return; } }
            delegate(write, service, key, bytes, std::move(callback));
        }); bool done = false;
        store.remove(fixtureId, [&](bool ok, QString id, QString error) { done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(memory.data, before); failWrite = false; done = false;
        store.remove(fixtureId, [&](bool ok, QString id, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(id, fixtureId); });
        QTRY_VERIFY(done); QCOMPARE(writes, 2); QCOMPARE(memory.data[masterAddress], before[masterAddress]); QCOMPARE(memory.data["ReplayLedger"], before["ReplayLedger"]);
        done = false;
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 1); QCOMPARE(entries.first().id, other["id"].toString());
        }); QTRY_VERIFY(done); const auto committed = memory.data; done = false;
        store.remove(fixtureId, [&](bool ok, QString, QString error) { done = true; QVERIFY(!ok); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(writes, 2); QCOMPARE(memory.data, committed);
    }
    void productionLegacyWriterFailsBeforeKeychain() {
        TelegramBotStore store; bool done = false;
        store.add("Desk", sampleToken, [&](bool ok, QString id, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(error.contains("password", Qt::CaseInsensitive));
        }); QTRY_VERIFY(done); done = false;
        store.saveWithAllowedChats("Desk", sampleToken, {"42"}, [&](bool ok, QString id, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(error.contains("password", Qt::CaseInsensitive));
        }); QTRY_VERIFY(done);
    }
    void kdfKeepsGuiResponsiveAndDestructionCancelsCommit() {
        QTemporaryDir dir; MemoryKeychain memory; memory.data[masterAddress] = QByteArray(32, 'k');
        const auto before = memory.data;
        auto store = std::make_unique<TelegramBotStore>(dir.filePath("profiles.dat"), memory.backend());
        bool callback = false; int heartbeats = 0;
        QTimer timer; timer.setInterval(0); connect(&timer, &QTimer::timeout, this, [&] { ++heartbeats; }); timer.start();
        store->saveProtectedBot({}, "Desk", sampleToken, {"42"}, password, password,
            [&](bool, QString, QString) { callback = true; });
        // The vault read completed and worker started on the next queued turn.
        QTRY_VERIFY(memory.calls >= 1); QTest::qWait(20); QVERIFY(heartbeats >= 3); QVERIFY(!callback);
        store.reset();
        QLockFile lock(dir.filePath("telegram-password-vault.lock")); QVERIFY(lock.tryLock(0)); lock.unlock();
        QTest::qWait(700); QVERIFY(!callback); QCOMPARE(memory.data, before);
        TelegramBotStore replacement(dir.filePath("profiles.dat"), memory.backend()); bool done = false;
        replacement.saveProtectedBot({}, "Desk", sampleToken, {}, password, password,
            [&](bool ok, QString, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); });
        QTRY_VERIFY(done); QVERIFY(heartbeats >= 10);
    }
    void protectedDeleteFailurePreservesOtherRecordsAndOpaqueLegacy() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto inner = innerFixture(fixtureId, {{"token", sampleToken}, {"allowedChatIds", QJsonArray{"42"}}, {"sessionSecret", QString(43, 'A')}});
        auto other = protectedRecord("22222222-2222-2222-2222-222222222222", inner); other["nickname"] = "Other";
        memory.data[passwordAddress] = QJsonDocument(QJsonObject{{"version", 3},
            {"bots", QJsonArray{protectedRecord(fixtureId, inner), other}}}).toJson();
        memory.data[catalogAddress] = "opaque-legacy"; memory.data["ReplayLedger"] = "retain";
        const auto before = memory.data; auto delegate = memory.backend(); bool failWrite = true;
        TelegramBotStore store(dir.filePath("unused"), [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion completion) {
            QCOMPARE(service + '/' + key, passwordAddress);
            if (write && failWrite) completion(TelegramBotStore::Status::Failure, {});
            else delegate(write, service, key, bytes, std::move(completion));
        }); bool done = false;
        store.remove(fixtureId, [&](bool ok, QString id, QString error) { done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(memory.data, before); failWrite = false; done = false;
        store.remove(fixtureId, [&](bool ok, QString id, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(id, fixtureId); });
        QTRY_VERIFY(done); QCOMPARE(memory.data[catalogAddress], before[catalogAddress]); QCOMPARE(memory.data["ReplayLedger"], before["ReplayLedger"]);
        const auto records = QJsonDocument::fromJson(memory.data[passwordAddress]).object()["bots"].toArray();
        QCOMPARE(records.size(), 1); QCOMPARE(records.first().toObject(), other);
    }
    void perBotSecretsDifferAndProtectedCompletionMayDestroyStore() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("unused");
        QPointer<TelegramBotStore> store = new TelegramBotStore(path, memory.backend());
        bool done = false; QString first, second, firstSecret;
        store->saveProtectedBot({}, "First", sampleToken, {}, password, password,
            [&](bool ok, QString id, QString error) { done = true; QVERIFY2(ok, qPrintable(error)); first = id; });
        QTRY_VERIFY(done); done = false;
        store->saveProtectedBot({}, "Second", "987:second", {}, password, password,
            [&](bool ok, QString id, QString error) {
                done = true; QVERIFY2(ok, qPrintable(error)); second = id;
                delete store.data(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }); QTRY_VERIFY(done); QVERIFY(!store);
        TelegramBotStore restored(path, memory.backend()); done = false;
        restored.readActivationSnapshot(first, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); firstSecret = snapshot.sessionSecret;
        }); QTRY_VERIFY(done); done = false;
        restored.readActivationSnapshot(second, password, [&](bool ok, TelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY2(ok, qPrintable(error)); QCOMPARE(snapshot.sessionSecret.size(), 43); QVERIFY(snapshot.sessionSecret != firstSecret);
        }); QTRY_VERIFY(done);
    }
    void invalidProtectedPolicyFailsBeforeAnyIo_data() { invalidChatPolicies_data(); }
    void invalidProtectedPolicyFailsBeforeAnyIo() {
        QFETCH(QStringList, policy); QTemporaryDir dir; MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("unused"), memory.backend()); bool done = false;
        store.saveProtectedBot({}, "Desk", sampleToken, policy, password, password,
            [&](bool ok, QString id, QString error) { done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(memory.calls, 0);
    }
    void malformedNewVaultNeverOverwritten_data() {
        QTest::addColumn<QByteArray>("value");
        QTest::newRow("empty-not-missing") << QByteArray();
        QTest::newRow("malformed") << QByteArray("not JSON");
        QTest::newRow("old-schema") << QByteArray("{\"version\":2,\"bots\":[]}");
        QTest::newRow("plaintext-record") << QJsonDocument(QJsonObject{{"version", 3}, {"bots", QJsonArray{
            QJsonObject{{"id", fixtureId}, {"nickname", "Desk"}, {"token", sampleToken}, {"allowedChatIds", QJsonArray{"42"}}}}}}).toJson();
    }
    void malformedNewVaultNeverOverwritten() {
        QFETCH(QByteArray, value); QTemporaryDir dir; MemoryKeychain memory;
        memory.data[passwordAddress] = value; const auto before = memory.data;
        TelegramBotStore store(dir.filePath("unused"), memory.backend()); bool done = false;
        store.saveProtectedBot({}, "Desk", sampleToken, {}, password, password,
            [&](bool ok, QString id, QString error) { done = true; QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); });
        QTRY_VERIFY(done); QCOMPARE(memory.data, before);
    }
    void invalidChatPolicies_data() {
        QTest::addColumn<QStringList>("policy");
        for (const auto &value : QStringList{"0", "-123", "+123", "01", "1.0", "1e3", "１２３", "4503599627370496", "9223372036854775807", "9223372036854775808", "", " 123"})
            QTest::newRow(qPrintable(value.isEmpty() ? "empty-element" : value)) << QStringList{value};
        QTest::newRow("duplicate") << QStringList{"123", "123"};
    }
    void invalidChatPolicies() {
        QFETCH(QStringList, policy);
        QTemporaryDir dir; MemoryKeychain memory;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool done = false;
        store.saveWithAllowedChats("Desk", sampleToken, policy, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); done = true;
        }); QTRY_VERIFY(done); QCOMPARE(memory.calls, 0);
    }
    void unsupportedLegacyPolicyFailsClosedWithoutGrantingSubset() {
        QTemporaryDir dir; MemoryKeychain memory;
        memory.data[masterAddress] = QByteArray(32, 'k');
        const QString id = "11111111-1111-1111-1111-111111111111";
        const QJsonObject record{{"id", id}, {"nickname", "Desk"}, {"token", sampleToken},
                                 {"allowedChatIds", QJsonArray{"42", "4503599627370496"}}};
        memory.data[catalogAddress] = envelope(QJsonDocument(QJsonObject{{"version", 2}, {"bots", QJsonArray{record}}}).toJson(), memory.data[masterAddress]);
        const auto before = memory.data;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
        bool done = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(entries.isEmpty()); QVERIFY(error.contains("chat policy", Qt::CaseInsensitive));
        }); QTRY_VERIFY(done); done = false;
        store.readActivationSnapshot(id, [&](bool ok, LegacyTelegramBotStore::ActivationSnapshot snapshot, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(snapshot.token.isEmpty()); QVERIFY(snapshot.allowedChatIds.isEmpty());
            QVERIFY(error.contains("chat policy", Qt::CaseInsensitive));
        }); QTRY_VERIFY(done); done = false;
        store.saveWithAllowedChats("Desk", {}, {"42"}, [&](bool ok, QString saved, QString error) {
            done = true; QVERIFY(!ok); QVERIFY(saved.isEmpty()); QVERIFY(!error.isEmpty());
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before);
    }
    void legacyPolicyIsEmptyAndFailedUpdatePreservesCiphertext() {
        QTemporaryDir dir; MemoryKeychain memory;
        memory.data[masterAddress] = QByteArray(32, 'k');
        const QJsonObject record{{"id", "11111111-1111-1111-1111-111111111111"}, {"nickname", "Desk"}, {"token", sampleToken}};
        memory.data[catalogAddress] = envelope(QJsonDocument(QJsonObject{{"version", 1}, {"bots", QJsonArray{record}}}).toJson(), memory.data[masterAddress]);
        const auto before = memory.data;
        auto delegate = memory.backend();
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), [delegate](bool write, QString service, QString key, QByteArray bytes, LegacyTelegramBotStore::BinaryCompletion completion) {
            if (write) completion(LegacyTelegramBotStore::Status::Failure, {});
            else delegate(write, service, key, bytes, std::move(completion));
        });
        bool done = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 1); QVERIFY(entries.first().allowedChatIds.isEmpty()); done = true;
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before);
        done = false;
        store.saveWithAllowedChats("Desk", {}, {"123"}, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); done = true;
        }); QTRY_VERIFY(done); QCOMPARE(memory.data, before);
    }
    void legacyMigrationAndPolicyRevocationRetainCredentials() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        memory.data[masterAddress] = QByteArray(32, 'k');
        const QString legacyId = "11111111-1111-1111-1111-111111111111";
        const QJsonObject record{{"id", legacyId}, {"nickname", "Desk"}, {"token", sampleToken}};
        memory.data[catalogAddress] = envelope(QJsonDocument(QJsonObject{{"version", 1}, {"bots", QJsonArray{record}}}).toJson(), memory.data[masterAddress]);
        LegacyTelegramBotStore store(path, memory.backend()); bool done = false;
        store.saveWithAllowedChats("Second", "987:synthetic", {"42"}, [&](bool ok, QString, QString error) {
            QVERIFY2(ok, qPrintable(error)); done = true;
        }); QTRY_VERIFY(done); done = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 2);
            QCOMPARE(entries.first().id, legacyId); QVERIFY(entries.first().allowedChatIds.isEmpty());
            QCOMPARE(entries.last().allowedChatIds, QStringList{"42"}); done = true;
        }); QTRY_VERIFY(done);
        for (const QStringList policy : {QStringList{"123"}, QStringList{}}) {
            done = false;
            store.saveWithAllowedChats("Desk", {}, policy, [&](bool ok, QString id, QString error) {
                QVERIFY2(ok, qPrintable(error)); QCOMPARE(id, legacyId); done = true;
            }); QTRY_VERIFY(done);
        }
        LegacyTelegramBotStore fresh(path, memory.backend()); done = false;
        fresh.readToken(legacyId, [&](bool ok, QString token, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(token, sampleToken); done = true;
        }); QTRY_VERIFY(done); done = false;
        fresh.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 2);
            QVERIFY(entries.first().allowedChatIds.isEmpty()); QCOMPARE(entries.last().allowedChatIds, QStringList{"42"}); done = true;
        }); QTRY_VERIFY(done);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
    }
    void allowedChatsEncryptedAndRestored() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        const QStringList policy{"1", "1234567890123", "4503599627370495"};
        bool done = false; QString id;
        {
            LegacyTelegramBotStore store(path, memory.backend());
            store.saveWithAllowedChats("Desk", sampleToken, policy, [&](bool ok, QString saved, QString error) {
                QVERIFY2(ok, qPrintable(error)); id = saved; done = true;
            }); QTRY_VERIFY(done);
        }
        for (const auto &blob : std::as_const(memory.data))
            // A single digit may occur by chance in ciphertext or the random key.
            for (const auto &chat : policy) if (chat.size() > 1) QVERIFY(!blob.contains(chat.toUtf8()));
        LegacyTelegramBotStore restored(path, memory.backend()); done = false;
        restored.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 1);
            QCOMPARE(entries.first().id, id); QCOMPARE(entries.first().allowedChatIds, policy); done = true;
        }); QTRY_VERIFY(done);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
    }
    void invalidRobinHoodPathFailsBeforeKeychain() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        QVERIFY(QDir().mkpath(path));
        LegacyTelegramBotStore store(path, memory.backend());
        bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.calls, 0);
    }
    void tokenValidationMatchesRuntime() {
        QTemporaryDir dir; MemoryKeychain memory;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
        bool finished = false;
        store.add("Bot", "012345:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi", [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.calls, 0);
        finished = false;
        store.add("Bot", "123:valid_token", [&](bool ok, QString id, QString error) {
            QVERIFY2(ok, qPrintable(error)); QVERIFY(!id.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
    }
    void encryptedCatalogSurvivesFreshObjects() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        QString id; bool finished = false;
        { LegacyTelegramBotStore store(path, memory.backend());
          store.add("Trading bot", "123456:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi", [&](bool ok, QString savedId, QString error) {
              QVERIFY2(ok, qPrintable(error)); id = savedId; QVERIFY(!id.isEmpty()); finished = true;
          }); QTRY_VERIFY(finished); }
        QCOMPARE(memory.data.size(), 2);
        for (const auto &blob : std::as_const(memory.data)) {
            QVERIFY(!blob.contains("Trading bot")); QVERIFY(!blob.contains("123456:"));
        }
        const auto saved = memory.data;
        LegacyTelegramBotStore fresh(path, memory.backend());
        finished = false;
        fresh.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 1);
            QCOMPARE(entries.first().id, id); QCOMPARE(entries.first().nickname, "Trading bot"); finished = true;
        }); QTRY_VERIFY(finished);
        finished = false;
        fresh.readToken(id, [&](bool ok, QString token, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(token, "123456:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi"); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.data, saved);
    }
    void reuseRobinHoodMasterAndFreshNonces() {
        QTemporaryDir dir; MemoryKeychain memory;
        const QByteArray key(32, 'k'); memory.data[masterAddress] = key;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
        bool finished = false;
        store.add("First", sampleToken, [&](bool ok, QString, QString error) {
            QVERIFY2(ok, qPrintable(error)); finished = true;
        }); QTRY_VERIFY(finished);
        const auto first = memory.data[catalogAddress];
        QCOMPARE(memory.data[masterAddress], key);
        finished = false;
        store.add("Second", "123:another_token", [&](bool ok, QString, QString error) {
            QVERIFY2(ok, qPrintable(error)); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.data[masterAddress], key);
        QVERIFY(memory.data[catalogAddress].mid(10, 12) != first.mid(10, 12));
        finished = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 2); finished = true;
        }); QTRY_VERIFY(finished);
        QVERIFY(!QFile::exists(dir.filePath("profiles.dat")));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
    }
    void missingMasterNeverRegeneratedWithExistingCiphertext_data() {
        QTest::addColumn<bool>("rhFile");
        QTest::newRow("robin-hood-file") << true;
        QTest::newRow("telegram-catalog") << false;
    }
    void missingMasterNeverRegeneratedWithExistingCiphertext() {
        QFETCH(bool, rhFile);
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        if (rhFile) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write("ciphertext"), 10); }
        else memory.data[catalogAddress] = envelope("{\"version\":1,\"bots\":[]}", QByteArray(32, 'k'));
        const auto before = memory.data;
        LegacyTelegramBotStore store(path, memory.backend()); bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.data, before); QVERIFY(!memory.data.contains(masterAddress));
    }
    void malformedCatalog_data() {
        QTest::addColumn<QByteArray>("plain");
        QTest::newRow("syntax") << QByteArray("not json");
        QTest::newRow("array-root") << QByteArray("[]");
        QTest::newRow("wrong-version") << QByteArray("{\"version\":4,\"bots\":[]}");
        QTest::newRow("wrong-bots") << QByteArray("{\"version\":1,\"bots\":{}}");
        QTest::newRow("bad-id") << QByteArray("{\"version\":1,\"bots\":[{\"id\":\"invalid\",\"nickname\":\"Bot\",\"token\":\"123:abc\"}]}");
        QTest::newRow("bad-nickname") << QByteArray("{\"version\":1,\"bots\":[{\"id\":\"11111111-1111-1111-1111-111111111111\",\"nickname\":\"\",\"token\":\"123:abc\"}]}");
        QTest::newRow("bad-token") << QByteArray("{\"version\":1,\"bots\":[{\"id\":\"11111111-1111-1111-1111-111111111111\",\"nickname\":\"Bot\",\"token\":\"not a token\"}]}");
        const QJsonObject record{{"id", "11111111-1111-1111-1111-111111111111"}, {"nickname", "Bot"}, {"token", sampleToken}};
        QTest::newRow("duplicate-record") << QJsonDocument(QJsonObject{{"version", 1}, {"bots", QJsonArray{record, record}}}).toJson();
        auto second = record; second["id"] = "22222222-2222-2222-2222-222222222222";
        QTest::newRow("duplicate-name-token") << QJsonDocument(QJsonObject{{"version", 1}, {"bots", QJsonArray{record, second}}}).toJson();
    }
    void malformedCatalog() {
        QFETCH(QByteArray, plain);
        QTemporaryDir dir; MemoryKeychain memory;
        memory.data[masterAddress] = QByteArray(32, 'k');
        memory.data[catalogAddress] = envelope(plain, memory.data[masterAddress]);
        QVERIFY(!memory.data[catalogAddress].isEmpty());
        const auto before = memory.data;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY(!ok); QVERIFY(entries.isEmpty()); QVERIFY(!error.isEmpty()); QVERIFY(!error.contains(sampleToken)); finished = true;
        }); QTRY_VERIFY(finished);
        finished = false;
        store.add("New Bot", "123:other_token", [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.data, before);
    }
    void envelopeCorruption_data() {
        QTest::addColumn<int>("position");
        QTest::newRow("header") << 0;
        QTest::newRow("nonce") << 10;
        QTest::newRow("tag") << 22;
        QTest::newRow("ciphertext") << 40;
        QTest::newRow("truncated") << -1;
        QTest::newRow("oversized") << -2;
        QTest::newRow("empty-entry-not-missing") << -3;
    }
    void envelopeCorruption() {
        QFETCH(int, position);
        QTemporaryDir dir; MemoryKeychain memory;
        memory.data[masterAddress] = QByteArray(32, 'k');
        auto blob = envelope("{\"version\":1,\"bots\":[]}", memory.data[masterAddress]);
        if (position >= 0) blob[position] = char(blob.at(position) ^ 1);
        else if (position == -1) blob.truncate(20);
        else if (position == -2) blob.resize(1024 * 1024 + 100);
        else blob.clear();
        memory.data[catalogAddress] = blob;
        const auto before = memory.data;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY(!ok); QVERIFY(entries.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.data, before);
    }
    void backendFailures_data() {
        QTest::addColumn<int>("failCall");
        QTest::newRow("catalog-read") << 1;
        QTest::newRow("master-read") << 2;
        QTest::newRow("master-write") << 3;
        QTest::newRow("catalog-write") << 4;
    }
    void backendFailures() {
        QFETCH(int, failCall);
        QTemporaryDir dir; MemoryKeychain memory;
        auto delegate = memory.backend(); int calls = 0;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), [&](bool write, QString service, QString key, QByteArray bytes, LegacyTelegramBotStore::BinaryCompletion done) {
            if (++calls == failCall) done(LegacyTelegramBotStore::Status::Failure, {});
            else delegate(write, service, key, bytes, std::move(done));
        });
        bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); QVERIFY(!error.contains(sampleToken)); finished = true;
        }); QTRY_VERIFY(finished);
        QVERIFY(!memory.data.contains(catalogAddress));
        if (failCall < 4) QVERIFY(!memory.data.contains(masterAddress));
    }
    void invalidMasterNeverOverwritten_data() {
        QTest::addColumn<int>("size");
        QTest::newRow("empty") << 0;
        QTest::newRow("short") << 31;
        QTest::newRow("long") << 33;
    }
    void invalidMasterNeverOverwritten() {
        QFETCH(int, size);
        QTemporaryDir dir; MemoryKeychain memory; memory.data[masterAddress] = QByteArray(size, 'k');
        const auto before = memory.data;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished); QCOMPARE(memory.data, before);
    }
    void concurrentStoresUseRobinHoodLockAndPreserveBothEntries() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        LegacyTelegramBotStore::BinaryCompletion pending;
        auto delegate = memory.backend(); bool hold = true;
        LegacyTelegramBotStore first(path, [&](bool write, QString service, QString key, QByteArray bytes, LegacyTelegramBotStore::BinaryCompletion done) {
            if (hold) { hold = false; pending = std::move(done); }
            else delegate(write, service, key, bytes, std::move(done));
        });
        LegacyTelegramBotStore second(path, memory.backend());
        bool firstDone = false, busyDone = false, lockedDone = false;
        first.add("First", sampleToken, [&](bool ok, QString, QString error) { QVERIFY2(ok, qPrintable(error)); firstDone = true; });
        QTRY_VERIFY(bool(pending));
        QLockFile rhLock(path + ".lock"); rhLock.setStaleLockTime(0);
        QVERIFY(!rhLock.tryLock(0));
        first.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY(!ok); QVERIFY(entries.isEmpty()); QVERIFY(error.contains("busy")); busyDone = true;
        });
        second.add("Second", "123:another_token", [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(error.contains("locked")); lockedDone = true;
        });
        QTRY_VERIFY(busyDone && lockedDone); QVERIFY(!firstDone); QCOMPARE(memory.calls, 0);
        pending(LegacyTelegramBotStore::Status::Missing, {}); pending = {};
        QTRY_VERIFY(firstDone);
        QVERIFY(rhLock.tryLock(0)); rhLock.unlock();
        bool saved = false;
        second.add("Second", "123:another_token", [&](bool ok, QString, QString error) { QVERIFY2(ok, qPrintable(error)); saved = true; });
        QTRY_VERIFY(saved);
        bool loaded = false;
        first.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 2); loaded = true;
        }); QTRY_VERIFY(loaded);
    }
    void externallyHeldRobinHoodLockBlocksBeforeKeychain() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat"); QLockFile rhLock(path + ".lock");
        rhLock.setStaleLockTime(0); QVERIFY(rhLock.tryLock(0));
        LegacyTelegramBotStore store(path, memory.backend()); bool finished = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries, QString error) {
            QVERIFY(!ok); QVERIFY(error.contains("locked")); finished = true;
        }); QTRY_VERIFY(finished); QCOMPARE(memory.calls, 0);
    }
    void destructionCancelsLateAndDuplicateCompletions() {
        QTemporaryDir dir;
        LegacyTelegramBotStore::BinaryCompletion pending;
        bool called = false;
        auto *store = new LegacyTelegramBotStore(dir.filePath("profiles.dat"), [&](bool, QString, QString, QByteArray, LegacyTelegramBotStore::BinaryCompletion done) { pending = std::move(done); });
        store->load([&](bool, LegacyTelegramBotStore::Entries, QString) { called = true; });
        QTRY_VERIFY(bool(pending)); delete store;
        pending(LegacyTelegramBotStore::Status::Missing, {});
        pending(LegacyTelegramBotStore::Status::Missing, {});
        QCoreApplication::processEvents(); QVERIFY(!called);
        QLockFile lock(dir.filePath("profiles.dat.lock")); QVERIFY(lock.tryLock(0));
    }
    void reentrantCompletionCanDeleteStore() {
        QTemporaryDir dir; MemoryKeychain memory;
        QPointer<LegacyTelegramBotStore> store = new LegacyTelegramBotStore(dir.filePath("profiles.dat"), memory.backend());
        bool finished = false;
        store->add("Bot", sampleToken, [&](bool ok, QString, QString error) {
            QVERIFY2(ok, qPrintable(error)); delete store.data(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); finished = true;
        }); QTRY_VERIFY(finished); QVERIFY(store.isNull());
    }
    void duplicateCompletionsAreIgnored() {
        QTemporaryDir dir; int calls = 0;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), [](bool, QString, QString, QByteArray, LegacyTelegramBotStore::BinaryCompletion done) {
            done(LegacyTelegramBotStore::Status::Missing, {}); done(LegacyTelegramBotStore::Status::Failure, {});
        });
        store.load([&](bool ok, LegacyTelegramBotStore::Entries, QString error) { QVERIFY2(ok, qPrintable(error)); ++calls; });
        QTRY_COMPARE(calls, 1); QCoreApplication::processEvents(); QCOMPARE(calls, 1);
    }
    void duplicateBotDoesNotModifyCatalog() {
        QTemporaryDir dir; MemoryKeychain memory;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString, QString error) { QVERIFY2(ok, qPrintable(error)); finished = true; }); QTRY_VERIFY(finished);
        const auto before = memory.data;
        for (int variant = 0; variant < 2; ++variant) {
            finished = false;
            store.add(variant == 0 ? "bOT" : "Other", variant == 0 ? "123:other_token" : sampleToken, [&](bool ok, QString id, QString error) {
                QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
            }); QTRY_VERIFY(finished); QCOMPARE(memory.data, before);
        }
    }
    void invalidFieldsFailBeforeKeychain_data() {
        QTest::addColumn<QString>("nickname"); QTest::addColumn<QString>("token");
        QTest::newRow("empty-name") << QString() << sampleToken;
        QTest::newRow("padded-name") << QString(" Bot ") << sampleToken;
        QTest::newRow("long-name") << QString(129, 'b') << sampleToken;
        QTest::newRow("control-name") << QString("Bot\n") << sampleToken;
        QTest::newRow("empty-token") << QString("Bot") << QString();
        QTest::newRow("invalid-token") << QString("Bot") << QString("bad token");
        QTest::newRow("long-token") << QString("Bot") << (QString("123:") + QString(257, 'a'));
    }
    void invalidFieldsFailBeforeKeychain() {
        QFETCH(QString, nickname); QFETCH(QString, token);
        QTemporaryDir dir; MemoryKeychain memory;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.add(nickname, token, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); if (!token.isEmpty()) QVERIFY(!error.contains(token)); finished = true;
        }); QTRY_VERIFY(finished); QCOMPARE(memory.calls, 0);
    }
    void missingCatalogDoesNotAccessMaster() {
        QTemporaryDir dir;
        MemoryKeychain memory;
        LegacyTelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
        QCOMPARE(memory.calls, 0);
        bool finished = false;
        store.load([&](bool ok, LegacyTelegramBotStore::Entries entries, QString error) {
            QVERIFY(ok); QVERIFY(entries.isEmpty()); QVERIFY(error.isEmpty()); finished = true;
        });
        QTRY_VERIFY(finished);
        QCOMPARE(memory.calls, 1);
        QVERIFY(memory.data.isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestTelegramBotStore)
#include "test_telegrambotstore.moc"
