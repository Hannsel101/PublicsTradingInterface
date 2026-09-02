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

        QKeychain::DeletePasswordJob deleteJob(service);
        deleteJob.setAutoDelete(false);
        deleteJob.setInsecureFallback(false);
        deleteJob.setKey(QStringLiteral("PublicsApiKey0"));
        QSignalSpy deleteSpy(&deleteJob, &QKeychain::DeletePasswordJob::finished);
        deleteJob.start();
        QVERIFY(deleteSpy.wait(5000));
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
