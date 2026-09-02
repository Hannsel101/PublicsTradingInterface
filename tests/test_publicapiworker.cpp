#include <QtTest/QtTest>
#include <QSslSocket>

#include "publicapiworker.h"

class PublicApiWorkerTest : public QObject
{
    Q_OBJECT

private slots:
    void androidBuildProvidesFunctionalTlsBackend()
    {
#if defined(Q_OS_ANDROID)
        QVERIFY2(QSslSocket::supportsSsl(), "Android APK must package OpenSSL so HTTPS Public.com API calls can start sessions and load accounts.");
#else
        QSKIP("Android TLS packaging is only verified on Android.");
#endif
    }

    void displayLabelForAccountUsesFriendlyAccountTypes()
    {
        QCOMPARE(PublicApiWorker::displayLabelForAccount(QStringLiteral("ABC123"), QStringLiteral("BROKERAGE")),
                 QStringLiteral("Standard Brokerage: ABC123"));
        QCOMPARE(PublicApiWorker::displayLabelForAccount(QStringLiteral("IRA456"), QStringLiteral("TRADITIONAL_IRA")),
                 QStringLiteral("IRA: IRA456"));
        QCOMPARE(PublicApiWorker::displayLabelForAccount(QStringLiteral("ROTH789"), QStringLiteral("ROTH_IRA")),
                 QStringLiteral("ROTH IRA: ROTH789"));
    }

    void preflightWithoutLoadedAccountsShowsFinishedFailureResult()
    {
        PublicApiWorker worker;
        QSignalSpy visibleSpy(&worker, &PublicApiWorker::tradeResultsVisibleChanged);
        QSignalSpy resultsSpy(&worker, &PublicApiWorker::tradeResultsChanged);

        worker.executePreflight(QStringLiteral("aapl"), QStringLiteral("BUY"));

        QVERIFY(worker.tradeResultsVisible());
        QVERIFY(worker.tradeResultsComplete());
        QVERIFY(!worker.tradeResultsBusy());
        QCOMPARE(worker.currentTransactionTitle(), QStringLiteral("Preflight Buy of AAPL"));
        QCOMPARE(worker.tradeResults().size(), 1);
        QVERIFY(!visibleSpy.isEmpty());
        QVERIFY(!resultsSpy.isEmpty());

        const QVariantMap result = worker.tradeResults().first().toMap();
        QCOMPARE(result.value(QStringLiteral("accountId")).toString(), QStringLiteral("No loaded account"));
        QCOMPARE(result.value(QStringLiteral("accountLabel")).toString(), QStringLiteral("No loaded account"));
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(result.value(QStringLiteral("success")).toBool(), false);
        QVERIFY(result.value(QStringLiteral("message")).toString().contains(QStringLiteral("No eligible brokerage accounts")));

        worker.dismissTradeResults();
        QVERIFY(!worker.tradeResultsVisible());
    }
};

QTEST_MAIN(PublicApiWorkerTest)
#include "test_publicapiworker.moc"
