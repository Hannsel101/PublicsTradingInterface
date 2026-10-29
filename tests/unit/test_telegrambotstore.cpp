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
class TestTelegramBotStore : public QObject {
    Q_OBJECT
private slots:
    void invalidRobinHoodPathFailsBeforeKeychain() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        QVERIFY(QDir().mkpath(path));
        TelegramBotStore store(path, memory.backend());
        bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.calls, 0);
    }
    void tokenValidationMatchesRuntime() {
        QTemporaryDir dir; MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
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
        { TelegramBotStore store(path, memory.backend());
          store.add("Trading bot", "123456:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi", [&](bool ok, QString savedId, QString error) {
              QVERIFY2(ok, qPrintable(error)); id = savedId; QVERIFY(!id.isEmpty()); finished = true;
          }); QTRY_VERIFY(finished); }
        QCOMPARE(memory.data.size(), 2);
        for (const auto &blob : std::as_const(memory.data)) {
            QVERIFY(!blob.contains("Trading bot")); QVERIFY(!blob.contains("123456:"));
        }
        const auto saved = memory.data;
        TelegramBotStore fresh(path, memory.backend());
        finished = false;
        fresh.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
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
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
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
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
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
        TelegramBotStore store(path, memory.backend()); bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished);
        QCOMPARE(memory.data, before); QVERIFY(!memory.data.contains(masterAddress));
    }
    void malformedCatalog_data() {
        QTest::addColumn<QByteArray>("plain");
        QTest::newRow("syntax") << QByteArray("not json");
        QTest::newRow("array-root") << QByteArray("[]");
        QTest::newRow("wrong-version") << QByteArray("{\"version\":2,\"bots\":[]}");
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
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
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
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
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
        TelegramBotStore store(dir.filePath("profiles.dat"), [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion done) {
            if (++calls == failCall) done(TelegramBotStore::Status::Failure, {});
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
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.add("Bot", sampleToken, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); finished = true;
        }); QTRY_VERIFY(finished); QCOMPARE(memory.data, before);
    }
    void concurrentStoresUseRobinHoodLockAndPreserveBothEntries() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat");
        TelegramBotStore::BinaryCompletion pending;
        auto delegate = memory.backend(); bool hold = true;
        TelegramBotStore first(path, [&](bool write, QString service, QString key, QByteArray bytes, TelegramBotStore::BinaryCompletion done) {
            if (hold) { hold = false; pending = std::move(done); }
            else delegate(write, service, key, bytes, std::move(done));
        });
        TelegramBotStore second(path, memory.backend());
        bool firstDone = false, busyDone = false, lockedDone = false;
        first.add("First", sampleToken, [&](bool ok, QString, QString error) { QVERIFY2(ok, qPrintable(error)); firstDone = true; });
        QTRY_VERIFY(bool(pending));
        QLockFile rhLock(path + ".lock"); rhLock.setStaleLockTime(0);
        QVERIFY(!rhLock.tryLock(0));
        first.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
            QVERIFY(!ok); QVERIFY(entries.isEmpty()); QVERIFY(error.contains("busy")); busyDone = true;
        });
        second.add("Second", "123:another_token", [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(error.contains("locked")); lockedDone = true;
        });
        QTRY_VERIFY(busyDone && lockedDone); QVERIFY(!firstDone); QCOMPARE(memory.calls, 0);
        pending(TelegramBotStore::Status::Missing, {}); pending = {};
        QTRY_VERIFY(firstDone);
        QVERIFY(rhLock.tryLock(0)); rhLock.unlock();
        bool saved = false;
        second.add("Second", "123:another_token", [&](bool ok, QString, QString error) { QVERIFY2(ok, qPrintable(error)); saved = true; });
        QTRY_VERIFY(saved);
        bool loaded = false;
        first.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(entries.size(), 2); loaded = true;
        }); QTRY_VERIFY(loaded);
    }
    void externallyHeldRobinHoodLockBlocksBeforeKeychain() {
        QTemporaryDir dir; MemoryKeychain memory;
        const auto path = dir.filePath("profiles.dat"); QLockFile rhLock(path + ".lock");
        rhLock.setStaleLockTime(0); QVERIFY(rhLock.tryLock(0));
        TelegramBotStore store(path, memory.backend()); bool finished = false;
        store.load([&](bool ok, TelegramBotStore::Entries, QString error) {
            QVERIFY(!ok); QVERIFY(error.contains("locked")); finished = true;
        }); QTRY_VERIFY(finished); QCOMPARE(memory.calls, 0);
    }
    void destructionCancelsLateAndDuplicateCompletions() {
        QTemporaryDir dir;
        TelegramBotStore::BinaryCompletion pending;
        bool called = false;
        auto *store = new TelegramBotStore(dir.filePath("profiles.dat"), [&](bool, QString, QString, QByteArray, TelegramBotStore::BinaryCompletion done) { pending = std::move(done); });
        store->load([&](bool, TelegramBotStore::Entries, QString) { called = true; });
        QTRY_VERIFY(bool(pending)); delete store;
        pending(TelegramBotStore::Status::Missing, {});
        pending(TelegramBotStore::Status::Missing, {});
        QCoreApplication::processEvents(); QVERIFY(!called);
        QLockFile lock(dir.filePath("profiles.dat.lock")); QVERIFY(lock.tryLock(0));
    }
    void reentrantCompletionCanDeleteStore() {
        QTemporaryDir dir; MemoryKeychain memory;
        QPointer<TelegramBotStore> store = new TelegramBotStore(dir.filePath("profiles.dat"), memory.backend());
        bool finished = false;
        store->add("Bot", sampleToken, [&](bool ok, QString, QString error) {
            QVERIFY2(ok, qPrintable(error)); delete store.data(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); finished = true;
        }); QTRY_VERIFY(finished); QVERIFY(store.isNull());
    }
    void duplicateCompletionsAreIgnored() {
        QTemporaryDir dir; int calls = 0;
        TelegramBotStore store(dir.filePath("profiles.dat"), [](bool, QString, QString, QByteArray, TelegramBotStore::BinaryCompletion done) {
            done(TelegramBotStore::Status::Missing, {}); done(TelegramBotStore::Status::Failure, {});
        });
        store.load([&](bool ok, TelegramBotStore::Entries, QString error) { QVERIFY2(ok, qPrintable(error)); ++calls; });
        QTRY_COMPARE(calls, 1); QCoreApplication::processEvents(); QCOMPARE(calls, 1);
    }
    void duplicateBotDoesNotModifyCatalog() {
        QTemporaryDir dir; MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
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
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend()); bool finished = false;
        store.add(nickname, token, [&](bool ok, QString id, QString error) {
            QVERIFY(!ok); QVERIFY(id.isEmpty()); QVERIFY(!error.isEmpty()); if (!token.isEmpty()) QVERIFY(!error.contains(token)); finished = true;
        }); QTRY_VERIFY(finished); QCOMPARE(memory.calls, 0);
    }
    void missingCatalogDoesNotAccessMaster() {
        QTemporaryDir dir;
        MemoryKeychain memory;
        TelegramBotStore store(dir.filePath("profiles.dat"), memory.backend());
        QCOMPARE(memory.calls, 0);
        bool finished = false;
        store.load([&](bool ok, TelegramBotStore::Entries entries, QString error) {
            QVERIFY(ok); QVERIFY(entries.isEmpty()); QVERIFY(error.isEmpty()); finished = true;
        });
        QTRY_VERIFY(finished);
        QCOMPARE(memory.calls, 1);
        QVERIFY(memory.data.isEmpty());
    }
};
QTEST_GUILESS_MAIN(TestTelegramBotStore)
#include "test_telegrambotstore.moc"
