#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QTimer>
#include <QUuid>

#include "publicauthclient.h"

class PublicAuthClientTest : public QObject
{
    Q_OBJECT

private slots:
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
