#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QTimer>
#include <QUuid>

#include "publicauthclient.h"

class PublicAuthClientTest : public QObject
{
    Q_OBJECT

private slots:
    void clearingSelectionInvalidatesPendingKeyRead()
    {
        PublicAuthClient client;
        client.setSelectedApiKeyLabel(QStringLiteral("PublicsApiKey0"));
        client.m_apiKeyReadGeneration = 3;

        client.setSecretKey(QString());

        QVERIFY(client.selectedApiKeyLabel().isEmpty());
        QVERIFY(!client.applyLoadedApiKey(QStringLiteral("PublicsApiKey0"), 3,
                                         QStringLiteral("stale-test-secret")));
        QVERIFY(client.secretKey().isEmpty());
    }

    void normalizesOnlyPublicsApiKeyLabelsNumerically()
    {
        const QStringList labels = {
            QStringLiteral("PublicsApiKey10"),
            QStringLiteral("not-a-publics-key"),
            QStringLiteral("Publics API Key 1"),
            QStringLiteral("PublicsApiKey2"),
            QStringLiteral("PublicsApiKey2"),
            QStringLiteral("PublicsApiKey0"),
            QString()
        };

        QCOMPARE(PublicAuthClient::normalizedStoredApiKeyLabels(labels),
                 QStringList({
                     QStringLiteral("PublicsApiKey0"),
                     QStringLiteral("PublicsApiKey2"),
                     QStringLiteral("PublicsApiKey10")
                 }));
    }

    void nextApiKeyLabelUsesLowestAvailableNumber()
    {
        QCOMPARE(PublicAuthClient::nextApiKeyLabel({}), QStringLiteral("PublicsApiKey0"));
        QCOMPARE(PublicAuthClient::nextApiKeyLabel({QStringLiteral("PublicsApiKey0")}),
                 QStringLiteral("PublicsApiKey1"));
        QCOMPARE(PublicAuthClient::nextApiKeyLabel({
                     QStringLiteral("PublicsApiKey0"),
                     QStringLiteral("PublicsApiKey1"),
                     QStringLiteral("PublicsApiKey3")
                 }),
                 QStringLiteral("PublicsApiKey2"));
    }

    void nicknamesPersistWithoutChangingKeychainLabelsOrSelection()
    {
        const QString group = QStringLiteral("publicsApiKeysNicknameTest-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString first = QStringLiteral("PublicsApiKey0");
        const QString second = QStringLiteral("PublicsApiKey1");
        {
            PublicAuthClient client({}, QStringLiteral("TestService"), group,
                                    QUrl(QStringLiteral("http://127.0.0.1/")),
                                    [](const QString &, const PublicAuthClient::CredentialCallback &done) {
                done(QStringLiteral("test-only-secret"), {});
            });
            client.setSecretKeys({first, second});
            client.setSecretKey(first);
            QVERIFY(client.apiKeyReady());
            QSignalSpy labelsChanged(&client, &PublicAuthClient::secretKeysChanged);
            QVERIFY(client.setApiKeyNickname(first, QStringLiteral("  Retirement  ")));
            QCOMPARE(client.apiKeyDisplayName(first), QStringLiteral("Retirement"));
            QCOMPARE(client.apiKeyDisplayName(second), second);
            QCOMPARE(client.secretKeys(), QStringList({first, second}));
            QCOMPARE(client.selectedApiKeyLabel(), first);
            QCOMPARE(client.secretKey(), QStringLiteral("test-only-secret"));
            QCOMPARE(labelsChanged.count(), 0);
            QVERIFY(!client.setApiKeyNickname(QStringLiteral("PublicsApiKey2"), QStringLiteral("Other")));
        }
        QSettings settings;
        settings.beginGroup(group);
        QCOMPARE(settings.value(QStringLiteral("nicknames/PublicsApiKey0")).toString(), QStringLiteral("Retirement"));
        QVERIFY(!settings.contains(QStringLiteral("labels")));
        settings.endGroup();
        {
            PublicAuthClient restarted({}, QStringLiteral("TestService"), group);
            restarted.setSecretKeys({first, second});
            QCOMPARE(restarted.apiKeyDisplayName(first), QStringLiteral("Retirement"));
            QCOMPARE(restarted.apiKeyDisplayName(second), second);
            QCOMPARE(restarted.secretKeys(), QStringList({first, second}));
            QVERIFY(restarted.setApiKeyNickname(first, {}));
            QCOMPARE(restarted.apiKeyDisplayName(first), first);
        }
        PublicAuthClient cleared({}, QStringLiteral("TestService"), group);
        cleared.setSecretKeys({first, second});
        QCOMPARE(cleared.apiKeyDisplayName(first), first);
        settings.beginGroup(group);
        settings.remove(QString());
        settings.endGroup();
    }

    void rejectsMalformedKeychainLabelIndex()
    {
        QStringList labels;
        QVERIFY(!PublicAuthClient::deserializeApiKeyLabels(
            QStringLiteral("not-json"),
            &labels));
        QVERIFY(labels.isEmpty());

        QVERIFY(!PublicAuthClient::deserializeApiKeyLabels(
            QStringLiteral("[1]"),
            &labels));
        QVERIFY(labels.isEmpty());

        QVERIFY(!PublicAuthClient::deserializeApiKeyLabels(
            QStringLiteral("[\"PublicsApiKey0\",1]"),
            &labels));
        QVERIFY(labels.isEmpty());

        QVERIFY(!PublicAuthClient::deserializeApiKeyLabels(
            QStringLiteral("[\"not-a-publics-key\"]"),
            &labels));
        QVERIFY(labels.isEmpty());

        QVERIFY(PublicAuthClient::deserializeApiKeyLabels(
            QStringLiteral("[\"PublicsApiKey2\",\"PublicsApiKey0\"]"),
            &labels));
        QCOMPARE(labels, QStringList({
                     QStringLiteral("PublicsApiKey0"),
                     QStringLiteral("PublicsApiKey2")
                 }));
    }

    void staleKeychainReadCannotReplaceCurrentSelection()
    {
        PublicAuthClient client;
        client.setSelectedApiKeyLabel(QStringLiteral("PublicsApiKey1"));
        client.m_apiKeyReadGeneration = 2;

        QVERIFY(!client.applyLoadedApiKey(
            QStringLiteral("PublicsApiKey1"),
            1,
            QStringLiteral("stale-test-value")));
        QVERIFY(client.secretKey().isEmpty());
        QVERIFY(!client.apiKeyReady());

        QVERIFY(client.applyLoadedApiKey(
            QStringLiteral("PublicsApiKey1"),
            2,
            QStringLiteral("current-test-value")));
        QCOMPARE(client.secretKey(), QStringLiteral("current-test-value"));
        QVERIFY(client.apiKeyReady());
    }

    void staleTokenResponseCannotReplaceCurrentSession()
    {
        PublicAuthClient client;
        client.setSelectedApiKeyLabel(QStringLiteral("PublicsApiKey0"));
        client.m_tokenRequestGeneration = 2;
        QSignalSpy tokenSpy(&client, &PublicAuthClient::tokenReceived);

        QVERIFY(!client.applyAccessToken(
            QStringLiteral("PublicsApiKey0"),
            1,
            QStringLiteral("stale-token")));
        QVERIFY(!client.sessionActive());
        QCOMPARE(tokenSpy.count(), 0);

        QVERIFY(client.applyAccessToken(
            QStringLiteral("PublicsApiKey0"),
            2,
            QStringLiteral("current-token")));
        QVERIFY(client.sessionActive());
        QCOMPARE(tokenSpy.count(), 1);
        QCOMPARE(tokenSpy.takeFirst().at(0).toString(), QStringLiteral("current-token"));
    }

    void storesListsAndReadsApiKeysWithQtKeychain()
    {
        if (!qEnvironmentVariableIsSet("PUBLICS_RUN_KEYCHAIN_TEST")) {
            QSKIP("Set PUBLICS_RUN_KEYCHAIN_TEST=1 to run the OS keychain integration test.");
        }

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString service = QStringLiteral("PublicsTradingInterfaceTest-%1").arg(uniqueId);
        const QString settingsGroup = QStringLiteral("publicsApiKeysTest-%1").arg(uniqueId);
        const QString testSecret = QStringLiteral("test-publics-secret-value");

        PublicAuthClient writer(QString(), service, settingsGroup);
        QSignalSpy storedSpy(&writer, &PublicAuthClient::secretKeysChanged);
        QVERIFY(writer.storeNextApiKey(QString(), testSecret));
        QVERIFY(storedSpy.wait(5000));
        QCOMPARE(writer.secretKeys(), QStringList({QStringLiteral("PublicsApiKey0")}));

        PublicAuthClient reader(QString(), service, settingsGroup);
        QCOMPARE(reader.listStoredApiKeys(), QStringList({QStringLiteral("PublicsApiKey0")}));

        QSignalSpy readySpy(&reader, &PublicAuthClient::apiKeyReadyChanged);
        reader.setSecretKey(QStringLiteral("PublicsApiKey0"));
        if (!reader.apiKeyReady()) {
            QVERIFY(readySpy.wait(5000));
        }
        QVERIFY(reader.apiKeyReady());
        QCOMPARE(reader.selectedApiKeyLabel(), QStringLiteral("PublicsApiKey0"));
        QCOMPARE(reader.secretKey(), testSecret);

        QSettings settings;
        settings.beginGroup(settingsGroup);
        settings.remove(QString());
        settings.endGroup();

        for (const QString &key : {
                 QStringLiteral("PublicsApiKey0"),
                 QStringLiteral("PublicsApiKeyLabelIndexV1")
             }) {
            QKeychain::DeletePasswordJob deleteJob(service);
            deleteJob.setAutoDelete(false);
            deleteJob.setInsecureFallback(false);
            deleteJob.setKey(key);
            QSignalSpy deleteSpy(&deleteJob, &QKeychain::DeletePasswordJob::finished);
            deleteJob.start();
            QVERIFY(deleteSpy.wait(5000));
        }
    }

    void removesKeyAndNicknameFromKeychainAndSettings()
    {
        if (!qEnvironmentVariableIsSet("PUBLICS_RUN_KEYCHAIN_TEST")) {
            QSKIP("Set PUBLICS_RUN_KEYCHAIN_TEST=1 to run the OS keychain integration test.");
        }

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString service = QStringLiteral("PublicsTradingInterfaceTest-%1").arg(uniqueId);
        const QString group = QStringLiteral("publicsApiKeysTest-%1").arg(uniqueId);
        const QString first = QStringLiteral("PublicsApiKey0");
        const QString second = QStringLiteral("PublicsApiKey1");
        PublicAuthClient client({}, service, group);
        QSignalSpy labelsSpy(&client, &PublicAuthClient::secretKeysChanged);
        QVERIFY(client.storeNextApiKey({}, QStringLiteral("delete-test-one")));
        QVERIFY(labelsSpy.wait(5000));
        QVERIFY(client.storeNextApiKey({}, QStringLiteral("delete-test-two")));
        QVERIFY(labelsSpy.wait(5000));
        QVERIFY(client.setApiKeyNickname(first, QStringLiteral("Retirement")));
        QVERIFY(client.setApiKeyNickname(second, QStringLiteral("Savings")));
        client.setSecretKey(first);
        QTRY_VERIFY_WITH_TIMEOUT(client.apiKeyReady(), 5000);

        QSignalSpy removedSpy(&client, &PublicAuthClient::apiKeyRemovalFinished);
        QVERIFY(client.removeStoredApiKey(first));
        QVERIFY(client.apiKeyIndexLoading());
        QVERIFY(!client.removeStoredApiKey(second));
        QVERIFY(removedSpy.wait(5000));
        QCOMPARE(removedSpy.takeFirst().at(1).toBool(), true);
        QCOMPARE(client.secretKeys(), QStringList({second}));
        QCOMPARE(client.apiKeyDisplayName(first), first);
        QCOMPARE(client.apiKeyDisplayName(second), QStringLiteral("Savings"));
        QVERIFY(client.selectedApiKeyLabel().isEmpty());
        QVERIFY(client.secretKey().isEmpty());
        QVERIFY(!client.sessionActive());

        QSettings settings;
        settings.beginGroup(group);
        QCOMPARE(settings.value(QStringLiteral("labels")).toStringList(), QStringList({second}));
        QVERIFY(!settings.contains(QStringLiteral("nicknames/") + first));
        QCOMPARE(settings.value(QStringLiteral("nicknames/") + second).toString(), QStringLiteral("Savings"));
        settings.endGroup();

        PublicAuthClient restarted({}, service, group);
        QSignalSpy loadedSpy(&restarted, &PublicAuthClient::apiKeyIndexLoadingChanged);
        QCOMPARE(restarted.listStoredApiKeys(), QStringList({second}));
        if (restarted.apiKeyIndexLoading()) QVERIFY(loadedSpy.wait(5000));
        QCOMPARE(restarted.secretKeys(), QStringList({second}));
        QCOMPARE(restarted.apiKeyDisplayName(second), QStringLiteral("Savings"));
        restarted.setSecretKey(second);
        QTRY_VERIFY_WITH_TIMEOUT(restarted.apiKeyReady(), 5000);
        QCOMPARE(restarted.secretKey(), QStringLiteral("delete-test-two"));

        QSignalSpy readdedSpy(&restarted, &PublicAuthClient::secretKeysChanged);
        QVERIFY(restarted.storeNextApiKey({}, QStringLiteral("delete-test-reused")));
        QVERIFY(readdedSpy.wait(5000));
        QCOMPARE(restarted.secretKeys(), QStringList({first, second}));
        QCOMPARE(restarted.apiKeyDisplayName(first), first);

        settings.beginGroup(group);
        settings.remove(QString());
        settings.endGroup();
        for (const QString &key : {first, second, QStringLiteral("PublicsApiKeyLabelIndexV1")}) {
            QKeychain::DeletePasswordJob job(service);
            job.setAutoDelete(false);
            job.setInsecureFallback(false);
            job.setKey(key);
            QSignalSpy deletedSpy(&job, &QKeychain::DeletePasswordJob::finished);
            job.start();
            QVERIFY(deletedSpy.wait(5000));
        }
    }

    void recoversLabelsFromKeychainAfterSettingsAreRemoved()
    {
        if (!qEnvironmentVariableIsSet("PUBLICS_RUN_KEYCHAIN_TEST")) {
            QSKIP("Set PUBLICS_RUN_KEYCHAIN_TEST=1 to run the OS keychain integration test.");
        }

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString service = QStringLiteral("PublicsTradingInterfaceTest-%1").arg(uniqueId);
        const QString settingsGroup = QStringLiteral("publicsApiKeysTest-%1").arg(uniqueId);

        PublicAuthClient writer(QString(), service, settingsGroup);
        QSignalSpy storedSpy(&writer, &PublicAuthClient::secretKeysChanged);
        QVERIFY(writer.storeNextApiKey(QString(), QStringLiteral("test-publics-secret-value")));
        QVERIFY(storedSpy.wait(5000));

        QSettings settings;
        settings.beginGroup(settingsGroup);
        settings.remove(QString());
        settings.endGroup();

        PublicAuthClient reader(QString(), service, settingsGroup);
        QSignalSpy recoveredSpy(&reader, &PublicAuthClient::secretKeysChanged);
        QCOMPARE(reader.listStoredApiKeys(), QStringList());
        QVERIFY(reader.apiKeyIndexLoading());
        QVERIFY(recoveredSpy.wait(5000));
        QVERIFY(!reader.apiKeyIndexLoading());
        QCOMPARE(reader.secretKeys(), QStringList({QStringLiteral("PublicsApiKey0")}));

        QSignalSpy readySpy(&reader, &PublicAuthClient::apiKeyReadyChanged);
        reader.setSecretKey(QStringLiteral("PublicsApiKey0"));
        if (!reader.apiKeyReady()) {
            QVERIFY(readySpy.wait(5000));
        }
        QVERIFY(reader.apiKeyReady());
        QCOMPARE(reader.secretKey(), QStringLiteral("test-publics-secret-value"));

        for (const QString &key : {
                 QStringLiteral("PublicsApiKey0"),
                 QStringLiteral("PublicsApiKeyLabelIndexV1")
             }) {
            QKeychain::DeletePasswordJob deleteJob(service);
            deleteJob.setAutoDelete(false);
            deleteJob.setInsecureFallback(false);
            deleteJob.setKey(key);
            QSignalSpy deleteSpy(&deleteJob, &QKeychain::DeletePasswordJob::finished);
            deleteJob.start();
            QVERIFY(deleteSpy.wait(5000));
        }
    }

    void migratesExistingSettingsLabelsIntoKeychainIndex()
    {
        if (!qEnvironmentVariableIsSet("PUBLICS_RUN_KEYCHAIN_TEST")) {
            QSKIP("Set PUBLICS_RUN_KEYCHAIN_TEST=1 to run the OS keychain integration test.");
        }

        const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString service = QStringLiteral("PublicsTradingInterfaceTest-%1").arg(uniqueId);
        const QString settingsGroup = QStringLiteral("publicsApiKeysTest-%1").arg(uniqueId);

        QKeychain::WritePasswordJob legacySecretJob(service);
        legacySecretJob.setAutoDelete(false);
        legacySecretJob.setInsecureFallback(false);
        legacySecretJob.setKey(QStringLiteral("PublicsApiKey0"));
        legacySecretJob.setTextData(QStringLiteral("test-publics-secret-value"));
        QSignalSpy legacySecretSpy(&legacySecretJob, &QKeychain::WritePasswordJob::finished);
        legacySecretJob.start();
        QVERIFY(legacySecretSpy.wait(5000));
        QCOMPARE(legacySecretJob.error(), QKeychain::NoError);

        QSettings settings;
        settings.beginGroup(settingsGroup);
        settings.setValue("labels", QStringList({QStringLiteral("PublicsApiKey0")}));
        settings.endGroup();

        PublicAuthClient migrator(QString(), service, settingsGroup);
        QCOMPARE(migrator.listStoredApiKeys(),
                 QStringList({QStringLiteral("PublicsApiKey0")}));
        QTRY_VERIFY_WITH_TIMEOUT(!migrator.apiKeyIndexLoading(), 5000);

        settings.beginGroup(settingsGroup);
        settings.remove(QString());
        settings.endGroup();

        PublicAuthClient reader(QString(), service, settingsGroup);
        QSignalSpy recoveredSpy(&reader, &PublicAuthClient::secretKeysChanged);
        QCOMPARE(reader.listStoredApiKeys(), QStringList());
        QVERIFY(recoveredSpy.wait(5000));
        QCOMPARE(reader.secretKeys(), QStringList({QStringLiteral("PublicsApiKey0")}));

        for (const QString &key : {
                 QStringLiteral("PublicsApiKey0"),
                 QStringLiteral("PublicsApiKeyLabelIndexV1")
             }) {
            QKeychain::DeletePasswordJob deleteJob(service);
            deleteJob.setAutoDelete(false);
            deleteJob.setInsecureFallback(false);
            deleteJob.setKey(key);
            QSignalSpy deleteSpy(&deleteJob, &QKeychain::DeletePasswordJob::finished);
            deleteJob.start();
            QVERIFY(deleteSpy.wait(5000));
        }
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    PublicAuthClientTest test;
    int result = 0;
    QTimer::singleShot(0, &app, [&]() {
        result = QTest::qExec(&test, argc, argv);
        app.exit(result);
    });
    app.exec();
    return result;
}

#include "test_publicauthclient.moc"
