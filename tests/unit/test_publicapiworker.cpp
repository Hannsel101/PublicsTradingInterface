#include <QtTest/QtTest>
#include <QSslSocket>

#include "api/publicapiworker.h"

class PublicApiWorkerTest : public QObject
{
    Q_OBJECT

private slots:
    void diagnosticsHaveLocalRequestIdsWithoutCredentials()
    {
        PublicApiWorker worker;
        worker.m_token = "offline-sensitive-placeholder";
        const auto first = worker.setPublicBrokerageHeaders("test");
        const auto second = worker.setPublicBrokerageHeaders("test");
        const auto id = first.attribute(QNetworkRequest::User).toString();
        QVERIFY(!QUuid(id).isNull());
        QVERIFY(id != second.attribute(QNetworkRequest::User).toString());
        QVERIFY(!id.contains(worker.m_token));
    }

    void parsesCommaSeparatedSymbolsWithoutLosingOrder()
    {
        QCOMPARE(PublicApiWorker::parseSymbols(QStringLiteral(" AAPL, TSLA, GM,")),
                 QStringList({QStringLiteral("AAPL"), QStringLiteral("TSLA"), QStringLiteral("GM")}));
        QCOMPARE(PublicApiWorker::parseSymbols(QStringLiteral("a a!p1l, t$sla,, gm")),
                 QStringList({QStringLiteral("AAPL"), QStringLiteral("TSLA"), QStringLiteral("GM")}));
        QCOMPARE(PublicApiWorker::parseSymbols(QStringLiteral(" , 123 ,!")), QStringList());
        QCOMPARE(PublicApiWorker::parseSymbols(QStringLiteral("aapl,aapl")),
                 QStringList({QStringLiteral("AAPL"), QStringLiteral("AAPL")}));
    }

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

    void portfolioResponseStoresStockShareBalances()
    {
        const QByteArray response = R"json(
            {
                "positions": [
                    {"instrument": {"symbol": "AAPL", "type": "EQUITY"}, "quantity": "12.5"},
                    {"instrument": {"symbol": "msft", "type": "EQUITY"}, "quantity": "3"},
                    {"instrument": {"symbol": "AAPL260116C00150000", "type": "OPTION"}, "quantity": "1"}
                ]
            }
        )json";

        const QHash<QString, float> balances = PublicApiWorker::stockShareBalancesFromPortfolio(response);

        QCOMPARE(balances.size(), 2);
        QCOMPARE(balances.value(QStringLiteral("AAPL")), 12.5F);
        QCOMPARE(balances.value(QStringLiteral("MSFT")), 3.0F);
        QVERIFY(!balances.contains(QStringLiteral("AAPL260116C00150000")));
        QVERIFY(!balances.contains(QStringLiteral("INVALID")));
    }

    void interruptedLiveBatchKeepsUncertainAndNotSubmittedOutcomes()
    {
        PublicApiWorker worker;
        worker.m_tradeResults = {
            QVariantMap{{"status", "pending"}, {"orderId", "original-order-id"}},
            QVariantMap{{"status", "pending"}}
        };
        worker.m_pendingTradeResults = 2;
        worker.setTradeResultsComplete(false);
        worker.setTradeResultsVisible(true);
        worker.clearSubAccountsList();
        QVERIFY(worker.tradeResultsComplete());
        QVERIFY(worker.tradeResultsVisible());
        QCOMPARE(worker.tradeResults().at(0).toMap().value("status").toString(), QStringLiteral("uncertain"));
        QCOMPARE(worker.tradeResults().at(0).toMap().value("orderId").toString(), QStringLiteral("original-order-id"));
        QCOMPARE(worker.tradeResults().at(1).toMap().value("status").toString(), QStringLiteral("not_submitted"));
    }

    void retryPolicyIsDeterministicWithInjectedClockAndJitter()
    {
        PublicRequestPolicy policy;
        QCOMPARE(policy.portfolioPauseMs, 500);
        QCOMPARE(policy.symbolPauseMs, 100);
        QCOMPARE(policy.accountPauseMs, 200);
        const auto now = QDateTime::fromString("2026-10-28T12:00:00Z", Qt::ISODate);
        QCOMPARE(policy.retryDelayMs(0, {}, now, 0), 1000);
        QCOMPARE(policy.retryDelayMs(0, {}, now, 250), 1250);
        QCOMPARE(policy.retryDelayMs(1, {}, now, 0), 2000);
        QCOMPARE(policy.retryDelayMs(5, {}, now, 9999), 30000);
        QCOMPARE(policy.retryDelayMs(0, "10", now, 0), 10000);
        QCOMPARE(policy.retryDelayMs(0, "3000000", now, 0), qint64(3000000000));
        QCOMPARE(policy.retryDelayMs(0, "Wed, 28 Oct 2026 12:00:10 GMT", now, 0), 10000);
        QCOMPARE(policy.retryDelayMs(0, "Wed, 28 Oct 2026 11:59:00 GMT", now, 0), 1000);
        QCOMPARE(policy.retryDelayMs(0, "invalid", now, 0), 1000);
    }

    void malformedHoldingsAreNeverValidEmptyPortfolios()
    {
        for (const QByteArray &body : {QByteArray("{}"), QByteArray("null"), QByteArray("<html>gateway</html>"),
             QByteArray(R"({"positions":[{}]})"),
             QByteArray(R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"A"},"quantity":"nan"}]})"),
             QByteArray(R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"A"},"quantity":true}]})")}) {
            bool valid = true;
            QVERIFY(PublicApiWorker::stockShareBalancesFromPortfolio(body, &valid).isEmpty());
            QVERIFY(!valid);
        }
        bool valid = false;
        QVERIFY(PublicApiWorker::stockShareBalancesFromPortfolio(R"({"positions":[]})", &valid).isEmpty());
        QVERIFY(valid);
    }

    void discoveredAccountsKeepStockShareBalances()
    {
        PublicApiWorker worker;
        worker.m_token = QStringLiteral("current-session-token");
        worker.m_accountLoadGeneration = 4;

        AccountData account{QStringLiteral("ACCOUNT-1"), QStringLiteral("BROKERAGE")};
        account.stockShareBalances.insert(QStringLiteral("AAPL"), 7.25F);

        worker.applyDiscoveredAccounts({account},
                                       worker.m_accountLoadGeneration,
                                       worker.m_token);

        QCOMPARE(worker.m_accountList.size(), 1);
        QCOMPARE(worker.m_accountList.first().stockShareBalances.value(QStringLiteral("AAPL")), 7.25F);
    }

    void sellAllPayloadUsesHeldShareQuantity()
    {
        const QJsonObject payload = PublicApiWorker::orderPayload(
            QStringLiteral("AAPL"), QStringLiteral("SELL"), 2.75F);

        QCOMPARE(payload.value(QStringLiteral("quantity")).toString(), QStringLiteral("2.75"));
    }

    void sellAllPayloadAvoidsBinaryFloatArtifacts()
    {
        const QJsonObject payload = PublicApiWorker::orderPayload(
            QStringLiteral("AAPL"), QStringLiteral("SELL"), 0.1F);

        QCOMPARE(payload.value(QStringLiteral("quantity")).toString(), QStringLiteral("0.1"));
    }

    void staleAccountDiscoveryCannotRepopulateAReplacedSession()
    {
        PublicApiWorker worker;
        worker.m_token = QStringLiteral("old-session-token");
        worker.m_accountLoadGeneration = 1;
        worker.addAccountToList(QStringLiteral("OLD-ACCOUNT"), QStringLiteral("BROKERAGE"));

        const quint64 oldAccountLoadGeneration = worker.m_accountLoadGeneration;
        worker.clearSubAccountsList();
        worker.m_token = QStringLiteral("new-session-token");

        const QList<AccountData> oldSessionAccounts = {
            {QStringLiteral("OLD-ACCOUNT"), QStringLiteral("BROKERAGE")}
        };
        worker.applyDiscoveredAccounts(oldSessionAccounts,
                                       oldAccountLoadGeneration,
                                       QStringLiteral("old-session-token"));

        QVERIFY(worker.m_accountList.isEmpty());

        const QList<AccountData> currentSessionAccounts = {
            {QStringLiteral("NEW-ACCOUNT"), QStringLiteral("BROKERAGE")}
        };
        worker.applyDiscoveredAccounts(currentSessionAccounts,
                                       worker.m_accountLoadGeneration,
                                       QStringLiteral("new-session-token"));

        QCOMPARE(worker.m_accountList.size(), 1);
        QCOMPARE(worker.m_accountList.first().accountId, QStringLiteral("NEW-ACCOUNT"));
    }
};

QTEST_MAIN(PublicApiWorkerTest)
#include "test_publicapiworker.moc"
