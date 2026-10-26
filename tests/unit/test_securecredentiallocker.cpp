#include "securecredentiallocker.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <openssl/evp.h>
#include <cstdio>
#include <type_traits>

using PendingCredentials = SecureCredentialLocker::PendingCredentials;
static_assert(std::is_final_v<PendingCredentials>);
static_assert(!std::is_base_of_v<QObject, PendingCredentials>);
static_assert(!std::is_copy_constructible_v<PendingCredentials>);
static_assert(!std::is_copy_assignable_v<PendingCredentials>);
static_assert(!std::is_move_constructible_v<PendingCredentials>);
static_assert(!std::is_move_assignable_v<PendingCredentials>);

namespace {
struct Result {
    bool called = false;
    bool ok = false;
    QString email, password, error;
    SecureCredentialLocker::Completion completion() {
        return [this](bool success, QString message) { ok = success; error = message; called = true; };
    }
    SecureCredentialLocker::ReadCompletion readCompletion() {
        return [this](bool success, QString e, QString p, QString message) {
            ok = success; email = e; password = p; error = message; called = true;
        };
    }
};
SecureCredentialLocker::KeyProvider constantKey(QByteArray key = QByteArray(32, 'k')) {
    return [key](bool, SecureCredentialLocker::KeyCompletion done) { done(true, key, {}); };
}
QByteArray bytes(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll();
}
bool writeBytes(const QString &path, const QByteArray &data) {
    QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
// Independent EVP fixture producer for authenticated, but invalid, JSON/schema.
QByteArray fixture(const QByteArray &plain) {
    const QByteArray key(32, 'k'), nonce(12, 'n'), aad("PTILOCK\0\1", 9);
    QByteArray cipher(plain.size() + 16, Qt::Uninitialized), tag(16, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    int count = 0, final = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()),
        reinterpret_cast<const unsigned char *>(nonce.constData())) == 1;
    ok = ok && EVP_EncryptUpdate(ctx, nullptr, &count,
        reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) == 1;
    ok = ok && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &count,
        reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) == 1;
    ok = ok && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + count, &final) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    cipher.resize(count + final);
    return aad + nonce + tag + cipher;
}
}

class TestSecureCredentialLocker : public QObject
{
    Q_OBJECT
private slots:
    void pendingStartsEmptyAndClearsFailedReadOutputs()
    {
        SecureCredentialLocker::PendingCredentials pending;
        QVERIFY(pending.isEmpty());
        QString email = "stale-email", password = "stale-password";
        QVERIFY(!pending.read(email, password));
        QVERIFY(email.isEmpty()); QVERIFY(password.isEmpty());
        pending.clear();
        QVERIFY(pending.isEmpty());
    }
    void pendingRoundTripsUnicodeAndJsonEdgeCharacters()
    {
        SecureCredentialLocker::PendingCredentials pending;
        const QString expectedEmail = QString::fromUtf8("  álîce-λ-🔒@example.test\n\"\\");
        QString expectedPassword = QString::fromUtf8("\t密碼🔑\r\n\"\\ ");
        expectedPassword.append(QChar(0));
        expectedPassword.append("tail");
        QVERIFY(pending.set(expectedEmail, expectedPassword));
        QVERIFY(!pending.isEmpty());
        const auto &sealed = pending;
        for (int attempt = 0; attempt < 2; ++attempt) {
            QString email, password;
            QVERIFY(sealed.read(email, password));
            QCOMPARE(email, expectedEmail); QCOMPARE(password, expectedPassword);
            SecureCredentialLocker::wipe(email); SecureCredentialLocker::wipe(password);
        }
    }
    void pendingInvalidReplacementFailsClosed_data()
    {
        QTest::addColumn<QString>("email");
        QTest::addColumn<QString>("password");
        QTest::newRow("empty-email") << QString() << QString("p");
        QTest::newRow("oversized-email") << QString(4097, 'e') << QString("p");
        QTest::newRow("empty-password") << QString("e") << QString();
        QTest::newRow("oversized-password") << QString("e") << QString(16385, 'p');
    }
    void pendingInvalidReplacementFailsClosed()
    {
        QFETCH(QString, email); QFETCH(QString, password);
        SecureCredentialLocker::PendingCredentials pending;
        QVERIFY(pending.set("original", "original-secret"));
        QVERIFY(!pending.set(email, password));
        QVERIFY(pending.isEmpty());
        QString outputEmail = "stale", outputPassword = "stale";
        QVERIFY(!pending.read(outputEmail, outputPassword));
        QVERIFY(outputEmail.isEmpty()); QVERIFY(outputPassword.isEmpty());
        QVERIFY(pending.set("recovered", "new-secret"));
        QVERIFY(pending.read(outputEmail, outputPassword));
        QCOMPARE(outputEmail, "recovered"); QCOMPARE(outputPassword, "new-secret");
    }
    void pendingReplacementClearAndDestructionAreIndependent()
    {
        // No persistent locker, path, provider or event processing is involved.
        PendingCredentials survivor;
        QVERIFY(survivor.set("survivor", "survivor-secret"));
        QString retainedEmail, retainedPassword;
        {
            PendingCredentials pending;
            QVERIFY(pending.set("old-email", "old-password"));
            QVERIFY(pending.set("replacement", "replacement-secret"));
            QVERIFY(pending.read(retainedEmail, retainedPassword));
            QCOMPARE(retainedEmail, "replacement");
            QCOMPARE(retainedPassword, "replacement-secret");
            pending.clear();
            pending.clear();
            QVERIFY(pending.isEmpty());
            QString email = "stale", password = "stale";
            QVERIFY(!pending.read(email, password));
            QVERIFY(email.isEmpty()); QVERIFY(password.isEmpty());
            QVERIFY(pending.set("destroyed", "destroyed-secret"));
        }
        // Caller-owned Qt copies are intentionally not revoked by clear/destruction.
        QCOMPARE(retainedEmail, "replacement");
        QCOMPARE(retainedPassword, "replacement-secret");
        QString email, password;
        QVERIFY(survivor.read(email, password));
        QCOMPARE(email, "survivor"); QCOMPARE(password, "survivor-secret");
        SecureCredentialLocker::wipe(retainedEmail);
        SecureCredentialLocker::wipe(retainedPassword);
        SecureCredentialLocker::wipe(email); SecureCredentialLocker::wipe(password);
    }
    void pendingAcceptsPersistentFieldBounds()
    {
        PendingCredentials pending;
        // Worst-case JSON escaping must still fit the authenticated payload limit.
        const QString expectedEmail(4096, QChar(0));
        const QString expectedPassword(16384, QChar(1));
        QVERIFY(pending.set(expectedEmail, expectedPassword));
        QString email, password;
        QVERIFY(pending.read(email, password));
        QCOMPARE(email, expectedEmail); QCOMPARE(password, expectedPassword);
    }
    void rejectsInvalidInputBeforeFetchingKey_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("email");
        QTest::addColumn<QString>("password");
        QTest::newRow("empty-id") << QString() << QString("a") << QString("p");
        QTest::newRow("long-id") << QString(257, 'a') << QString("a") << QString("p");
        QTest::newRow("empty-email") << QString("a") << QString() << QString("p");
        QTest::newRow("long-email") << QString("a") << QString(4097, 'e') << QString("p");
        QTest::newRow("empty-password") << QString("a") << QString("e") << QString();
        QTest::newRow("long-password") << QString("a") << QString("e") << QString(16385, 'p');
    }
    void rejectsInvalidInputBeforeFetchingKey()
    {
        QFETCH(QString, id); QFETCH(QString, email); QFETCH(QString, password);
        QTemporaryDir dir;
        int fetched = 0;
        const QString path = dir.filePath("profiles.dat");
        SecureCredentialLocker locker(path, [&](bool, SecureCredentialLocker::KeyCompletion done) {
            ++fetched; done(true, QByteArray(32, 'k'), {});
        });
        bool called = false;
        locker.save(id, email, password, [&](bool ok, QString error) {
            QVERIFY(!ok); QVERIFY(!error.isEmpty()); called = true;
        });
        QTRY_VERIFY(called);
        QCOMPARE(fetched, 0);
        QVERIFY(!QFile::exists(path));
    }
    void detectsTamperingWithoutOverwriting_data()
    {
        QTest::addColumn<int>("position");
        QTest::newRow("magic") << 0;
        QTest::newRow("version") << 8;
        QTest::newRow("nonce") << 9;
        QTest::newRow("tag") << 21;
        QTest::newRow("ciphertext") << 37;
        QTest::newRow("truncated") << -1;
        QTest::newRow("empty") << -2;
        QTest::newRow("oversized") << -3;
    }
    void detectsTamperingWithoutOverwriting()
    {
        QFETCH(int, position);
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        SecureCredentialLocker locker(path, constantKey());
        Result saved;
        locker.save("one", "email", "password", saved.completion());
        QTRY_VERIFY(saved.called); QVERIFY(saved.ok);
        QByteArray corrupt = bytes(path);
        if (position >= 0) corrupt[position] = corrupt[position] ^ 1;
        else if (position == -1) corrupt.chop(1);
        else if (position == -2) corrupt.clear();
        else corrupt = QByteArray(1024 * 1024 + 38, 'x');
        QVERIFY(writeBytes(path, corrupt));
        Result read, save, remove;
        locker.read("one", read.readCompletion());
        QTRY_VERIFY(read.called); QVERIFY(!read.ok); QVERIFY(read.email.isEmpty()); QVERIFY(read.password.isEmpty());
        locker.save("two", "other", "secret", save.completion());
        QTRY_VERIFY(save.called); QVERIFY(!save.ok); QVERIFY(!save.error.isEmpty());
        locker.remove("one", remove.completion());
        QTRY_VERIFY(remove.called); QVERIFY(!remove.ok);
        QCOMPARE(bytes(path), corrupt);
    }
    void missingOrInvalidKeyFailsClosed_data()
    {
        QTest::addColumn<bool>("providerOk");
        QTest::addColumn<int>("keySize");
        QTest::newRow("missing") << false << 0;
        QTest::newRow("denied") << false << 32;
        QTest::newRow("short") << true << 31;
        QTest::newRow("long") << true << 33;
        QTest::newRow("wrong") << true << 32;
    }
    void missingOrInvalidKeyFailsClosed()
    {
        QFETCH(bool, providerOk); QFETCH(int, keySize);
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        SecureCredentialLocker original(path, constantKey());
        Result saved;
        original.save("one", "email", "password", saved.completion());
        QTRY_VERIFY(saved.called); QVERIFY(saved.ok);
        const QByteArray before = bytes(path);
        int fetched = 0;
        SecureCredentialLocker broken(path, [&](bool create, SecureCredentialLocker::KeyCompletion done) {
            QVERIFY(!create); ++fetched; done(providerOk, QByteArray(keySize, 'z'), "unavailable");
        });
        Result read, save, remove;
        broken.read("one", read.readCompletion()); QTRY_VERIFY(read.called); QVERIFY(!read.ok);
        QVERIFY(read.email.isEmpty()); QVERIFY(read.password.isEmpty());
        broken.save("two", "email", "password", save.completion()); QTRY_VERIFY(save.called); QVERIFY(!save.ok);
        broken.remove("one", remove.completion()); QTRY_VERIFY(remove.called); QVERIFY(!remove.ok);
        QCOMPARE(fetched, 3); QCOMPARE(bytes(path), before);
    }
    void missingFileReadOrRemoveDoesNotCreateKey()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        int fetched = 0;
        SecureCredentialLocker locker(path, [&](bool create, SecureCredentialLocker::KeyCompletion done) {
            QVERIFY(!create); ++fetched; done(false, {}, "missing");
        });
        Result read, remove;
        locker.read("one", read.readCompletion()); QTRY_VERIFY(read.called); QVERIFY(!read.ok);
        locker.remove("one", remove.completion()); QTRY_VERIFY(remove.called); QVERIFY(remove.ok); QVERIFY(remove.error.isEmpty());
        QCOMPARE(fetched, 1); QVERIFY(!QFile::exists(path));
    }
    void immediateProviderIsAsyncAndClientCanReenter()
    {
        QTemporaryDir dir;
        SecureCredentialLocker locker(dir.filePath("profiles.dat"), constantKey());
        Result read;
        bool saved = false;
        locker.save("one", QString::fromUtf8("alice-\u03bb"), QString::fromUtf8("password-\U0001f512"),
                    [&](bool ok, QString) {
            QVERIFY(ok); saved = true;
            locker.read("one", read.readCompletion());
        });
        QVERIFY(!saved); QTRY_VERIFY(read.called); QVERIFY(read.ok);
        QCOMPARE(read.email, QString::fromUtf8("alice-\u03bb"));
        QCOMPARE(read.password, QString::fromUtf8("password-\U0001f512"));
    }
    void busyOperationsAreRejectedAndLockCoversProvider()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        SecureCredentialLocker::KeyCompletion pending;
        int fetched = 0;
        auto provider = [&](bool, SecureCredentialLocker::KeyCompletion done) { ++fetched; pending = done; };
        SecureCredentialLocker locker(path, provider);
        Result first, busy, other;
        locker.save("one", "email", "password", first.completion());
        locker.read("one", busy.readCompletion());
        QTRY_VERIFY(pending); QTRY_VERIFY(busy.called); QVERIFY(!busy.ok);
        QVERIFY(busy.error.contains("busy"));
        SecureCredentialLocker competitor(path, provider);
        competitor.save("two", "email", "password", other.completion());
        QTRY_VERIFY(other.called); QVERIFY(!other.ok); QCOMPARE(fetched, 1);
        pending(true, QByteArray(32, 'k'), {});
        QTRY_VERIFY(first.called); QVERIFY(first.ok);
        Result after;
        competitor.read("one", after.readCompletion()); QTRY_COMPARE(fetched, 2);
        pending(true, QByteArray(32, 'k'), {});
        QTRY_VERIFY(after.called); QVERIFY(after.ok);
    }
    void repeatedProviderCompletionIsIgnored()
    {
        QTemporaryDir dir;
        SecureCredentialLocker locker(dir.filePath("profiles.dat"), [](bool, SecureCredentialLocker::KeyCompletion done) {
            done(true, QByteArray(32, 'k'), {}); done(false, {}, "second callback");
        });
        int callbacks = 0;
        locker.save("one", "email", "password", [&](bool ok, QString) { QVERIFY(ok); ++callbacks; });
        QTRY_COMPARE(callbacks, 1);
        QTest::qWait(30); QCOMPARE(callbacks, 1);
    }
    void destructionCancelsPendingOperationAndUnlocks()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        SecureCredentialLocker::KeyCompletion pending;
        auto *locker = new SecureCredentialLocker(path, [&](bool, SecureCredentialLocker::KeyCompletion done) { pending = done; });
        bool called = false;
        locker->save("one", "email", "password", [&](bool, QString) { called = true; });
        QTRY_VERIFY(pending);
        delete locker;
        pending(true, QByteArray(32, 'k'), {});
        QTest::qWait(30); QVERIFY(!called); QVERIFY(!QFile::exists(path));
        SecureCredentialLocker next(path, constantKey());
        Result saved;
        next.save("two", "email", "password", saved.completion()); QTRY_VERIFY(saved.called); QVERIFY(saved.ok);
    }
    void usesFreshNonceAndOwnerOnlyPermissions()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        SecureCredentialLocker locker(path, constantKey());
        Result first, second;
        locker.save("one", "email", "password", first.completion()); QTRY_VERIFY(first.called); QVERIFY(first.ok);
        const QByteArray before = bytes(path);
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        locker.save("one", "email", "password", second.completion()); QTRY_VERIFY(second.called); QVERIFY(second.ok);
        const QByteArray after = bytes(path);
        QCOMPARE(before.first(9), QByteArray("PTILOCK\0\1", 9));
        QVERIFY(before.mid(9, 12) != after.mid(9, 12)); QVERIFY(before != after);
        const auto perms = QFile::permissions(path);
        QVERIFY(perms.testFlag(QFileDevice::ReadOwner)); QVERIFY(perms.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(perms & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                          | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther
                          | QFileDevice::ExeOwner)));
    }
    void wipeDetachesSharedBuffersAndClearsOnlyRequestedValue()
    {
        QString string = "secret", copy = string;
        QByteArray array = "secret", arrayCopy = array;
        SecureCredentialLocker::wipe(string); SecureCredentialLocker::wipe(array);
        QVERIFY(string.isEmpty()); QVERIFY(array.isEmpty());
        QCOMPARE(copy, "secret"); QCOMPARE(arrayCopy, QByteArray("secret"));
        SecureCredentialLocker::wipe(string); SecureCredentialLocker::wipe(array);
    }
    void rejectsSymlinkAndNonFileTargets()
    {
        QTemporaryDir dir;
        const QString original = dir.filePath("original"), link = dir.filePath("profiles.dat");
        QVERIFY(writeBytes(original, "do-not-overwrite")); QVERIFY(QFile::link(original, link));
        SecureCredentialLocker locker(link, constantKey());
        Result saved;
        locker.save("one", "email", "password", saved.completion()); QTRY_VERIFY(saved.called); QVERIFY(!saved.ok);
        QCOMPARE(bytes(original), QByteArray("do-not-overwrite"));
        SecureCredentialLocker directory(dir.path(), constantKey());
        Result invalid;
        directory.save("one", "email", "password", invalid.completion()); QTRY_VERIFY(invalid.called); QVERIFY(!invalid.ok);
    }
    void crossProcessLockCoversCreationAndReadModifyWrite()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--locker-child", path});
        QVERIFY(child.waitForStarted(3000)); QVERIFY(child.waitForReadyRead(3000));
        QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("LOCKED"));
        QVERIFY(!QFile::exists(path));
        int fetched = 0;
        SecureCredentialLocker locker(path, [&](bool, SecureCredentialLocker::KeyCompletion done) {
            ++fetched; done(true, QByteArray(32, 'k'), {});
        });
        Result locked;
        locker.save("parent", "email-parent", "password", locked.completion());
        QTRY_VERIFY(locked.called); QVERIFY(!locked.ok); QCOMPARE(fetched, 0);
        QVERIFY(child.waitForFinished(5000)); QCOMPARE(child.exitStatus(), QProcess::NormalExit); QCOMPARE(child.exitCode(), 0);
        Result saved, read;
        locker.save("parent", "email-parent", "password", saved.completion()); QTRY_VERIFY(saved.called); QVERIFY(saved.ok);
        locker.read("child", read.readCompletion()); QTRY_VERIFY(read.called); QVERIFY(read.ok); QCOMPARE(read.email, "email-child");
    }
    void atomicWriteFailureDoesNotDamagePriorFile_data()
    {
        QTest::addColumn<bool>("waitForRetry");
        QTest::newRow("next-operation") << false;
        QTest::newRow("automatic-release") << true;
    }
    void atomicWriteFailureDoesNotDamagePriorFile()
    {
        QFETCH(bool, waitForRetry);
        QTemporaryDir dir;
        const QString folder = dir.filePath("store");
        QVERIFY(QDir().mkpath(folder));
        const QString path = QDir(folder).filePath("profiles.dat");
        SecureCredentialLocker initial(path, constantKey());
        Result saved;
        initial.save("one", "email", "password", saved.completion()); QTRY_VERIFY(saved.called); QVERIFY(saved.ok);
        const QByteArray before = bytes(path);
        // Revoke directory writes after key retrieval. QSaveFile must fail rather
        // than fall back to an in-place update of the still-writable old file.
        bool revoke = true;
        SecureCredentialLocker locker(path, [&](bool, SecureCredentialLocker::KeyCompletion done) {
            if (revoke)
                QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            done(true, QByteArray(32, 'k'), {});
        });
        Result failed;
        locker.save("one", "new-email", "new-password", failed.completion());
        QTRY_VERIFY(failed.called);
        const bool restored = QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(restored); QVERIFY(!failed.ok); QVERIFY(!failed.error.isEmpty());
        QCOMPARE(bytes(path), before);
        revoke = false;
        if (waitForRetry)
            QTRY_VERIFY(!QFile::exists(path + ".lock"));
        // No manual unlink: the original owner must recover its own lock.
        Result recovered;
        locker.read("one", recovered.readCompletion());
        QTRY_VERIFY(recovered.called); QVERIFY(recovered.ok); QCOMPARE(recovered.email, "email");
        QTRY_VERIFY(!QFile::exists(path + ".lock"));
        Result read;
        initial.read("one", read.readCompletion()); QTRY_VERIFY(read.called); QVERIFY(read.ok); QCOMPARE(read.email, "email");
    }
    void completionCanDestroyLocker()
    {
        QTemporaryDir dir;
        QPointer<SecureCredentialLocker> locker = new SecureCredentialLocker(dir.filePath("profiles.dat"), constantKey());
        bool called = false;
        locker->save("one", "email", "password", [&](bool ok, QString) {
            QVERIFY(ok); delete locker.data(); called = true;
        });
        QTRY_VERIFY(called); QVERIFY(locker.isNull());
    }
    void accountAndSerializedSizeLimitsPreserveExistingFile_data()
    {
        QTest::addColumn<bool>("accountLimit");
        QTest::newRow("account-limit") << true;
        QTest::newRow("serialized-size-limit") << false;
    }
    void accountAndSerializedSizeLimitsPreserveExistingFile()
    {
        QFETCH(bool, accountLimit);
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        QJsonObject accounts;
        const auto entry = accountLimit ? QJsonObject{{"email", "e"}, {"password", "p"}}
                                      : QJsonObject{{"email", QString(4096, 'e')}, {"password", QString(16384, 'p')}};
        if (accountLimit) {
            for (int i = 0; i < 1024; ++i) accounts.insert(QString::number(i), entry);
        } else {
            for (int i = 0; ; ++i) {
                auto next = accounts;
                next.insert(QString::number(i), entry);
                if (QJsonDocument(next).toJson(QJsonDocument::Compact).size() > 1024 * 1024) break;
                accounts = next;
            }
        }
        const QByteArray before = fixture(QJsonDocument(accounts).toJson(QJsonDocument::Compact));
        QVERIFY(!before.isEmpty()); QVERIFY(writeBytes(path, before));
        SecureCredentialLocker locker(path, constantKey());
        Result limit;
        locker.save("new", entry.value("email").toString(), entry.value("password").toString(), limit.completion());
        QTRY_VERIFY(limit.called); QVERIFY(!limit.ok); QVERIFY(!limit.error.isEmpty());
        QCOMPARE(bytes(path), before);
        Result update;
        locker.save("0", "updated-email", "updated-password", update.completion());
        QTRY_VERIFY(update.called); QVERIFY(update.ok);
    }
    void authenticatedInvalidJsonIsNotOverwritten_data()
    {
        QTest::addColumn<QByteArray>("plain");
        QTest::newRow("invalid-json") << QByteArray("{{");
        QTest::newRow("array-root") << QByteArray("[]");
        QTest::newRow("non-object-account") << QByteArray("{\"one\":false}");
        QTest::newRow("wrong-field-types") << QByteArray("{\"one\":{\"email\":7,\"password\":\"p\"}}");
        QTest::newRow("extra-field") << QByteArray("{\"one\":{\"email\":\"e\",\"password\":\"p\",\"extra\":true}}");
        QTest::newRow("empty-id") << QByteArray("{\"\":{\"email\":\"e\",\"password\":\"p\"}}");
        QTest::newRow("long-email") << QJsonDocument(QJsonObject{{"one", QJsonObject{{"email", QString(4097, 'e')}, {"password", "p"}}}}).toJson();
        QJsonObject many;
        for (int i = 0; i < 1025; ++i) many.insert(QString::number(i), QJsonObject{{"email", "e"}, {"password", "p"}});
        QTest::newRow("too-many-accounts") << QJsonDocument(many).toJson();
    }
    void authenticatedInvalidJsonIsNotOverwritten()
    {
        QFETCH(QByteArray, plain);
        QTemporaryDir dir;
        const QString path = dir.filePath("profiles.dat");
        const QByteArray encrypted = fixture(plain);
        QVERIFY(!encrypted.isEmpty()); QVERIFY(writeBytes(path, encrypted));
        SecureCredentialLocker locker(path, constantKey());
        Result saved, read;
        locker.read("one", read.readCompletion()); QTRY_VERIFY(read.called); QVERIFY(!read.ok);
        locker.save("two", "email", "password", saved.completion()); QTRY_VERIFY(saved.called); QVERIFY(!saved.ok);
        QCOMPARE(bytes(path), encrypted);
    }
    void persistsEncryptedMapAndFetchesKeyPerOperation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("profiles.dat");
        QByteArray master;
        QList<bool> requests;
        auto provider = [&master, &requests](bool create, SecureCredentialLocker::KeyCompletion done) {
            requests.append(create);
            if (master.isEmpty() && create) master = QByteArray(32, 'K');
            QTimer::singleShot(0, [done, master] { done(master.size() == 32, master, "missing key"); });
        };
        SecureCredentialLocker locker(path, provider);
        bool called = false;
        locker.save("one", "alice@example.test", "secret-password", [&](bool ok, QString error) {
            QVERIFY2(ok, qPrintable(error)); called = true;
        });
        QVERIFY(!called);
        QTRY_VERIFY(called);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray first = file.readAll();
        QVERIFY(!first.contains("alice"));
        QVERIFY(!first.contains("secret-password"));
        called = false;
        locker.save("two", "bob@example.test", "other", [&](bool ok, QString error) {
            QVERIFY2(ok, qPrintable(error)); called = true;
        });
        QTRY_VERIFY(called);
        SecureCredentialLocker reopened(path, provider);
        called = false;
        reopened.read("one", [&](bool ok, QString email, QString password, QString error) {
            QVERIFY2(ok, qPrintable(error));
            QCOMPARE(email, "alice@example.test"); QCOMPARE(password, "secret-password"); called = true;
        });
        QTRY_VERIFY(called);
        called = false;
        reopened.remove("one", [&](bool ok, QString error) { QVERIFY2(ok, qPrintable(error)); called = true; });
        QTRY_VERIFY(called);
        called = false;
        reopened.read("one", [&](bool ok, QString email, QString password, QString error) {
            QVERIFY(!ok); QVERIFY(email.isEmpty()); QVERIFY(password.isEmpty()); QVERIFY(!error.isEmpty()); called = true;
        });
        QTRY_VERIFY(called);
        called = false;
        reopened.read("two", [&](bool ok, QString email, QString password, QString error) {
            QVERIFY2(ok, qPrintable(error)); QCOMPARE(email, "bob@example.test"); QCOMPARE(password, "other"); called = true;
        });
        QTRY_VERIFY(called);
        QCOMPARE(requests, (QList<bool>{true, false, false, false, false, false}));
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments().at(1) == "--locker-child") {
        SecureCredentialLocker locker(app.arguments().at(2), [](bool create, SecureCredentialLocker::KeyCompletion done) {
            if (!create) { done(false, {}, "expected creation"); return; }
            std::puts("LOCKED"); std::fflush(stdout);
            QTimer::singleShot(500, [done] { done(true, QByteArray(32, 'k'), {}); });
        });
        locker.save("child", "email-child", "password", [&](bool ok, QString) { app.exit(ok ? 0 : 1); });
        QTimer::singleShot(5000, &app, [&] { app.exit(2); });
        return app.exec();
    }
    QStandardPaths::setTestModeEnabled(true);
    TestSecureCredentialLocker test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_securecredentiallocker.moc"
