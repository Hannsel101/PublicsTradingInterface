#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

#include "trading/autotradecontroller.h"
#include "auth/publicauthclient.h"
#include "api/publicapiworker.h"

class TestBrokerageServer : public QTcpServer
{
public:
    explicit TestBrokerageServer(QObject *parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            auto *socket = nextPendingConnection();
            auto *buffer = new QByteArray;
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                const QByteArray headers = buffer->left(headerEnd);
                qsizetype length = 0;
                for (const QByteArray &line : headers.split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                if (buffer->size() < headerEnd + 4 + length) return;
                const QByteArray path = headers.left(headers.indexOf("\r\n")).split(' ').at(1);
                const QJsonObject request = QJsonDocument::fromJson(buffer->mid(headerEnd + 4)).object();
                const QString bearer = QString::fromUtf8(headers).contains(QStringLiteral("first-token"))
                                           ? QStringLiteral("first") : QStringLiteral("second");
                if (path == "/userapiauthservice/personal/access-tokens") {
                    authTimes << clock.elapsed();
                    const QString key = request.value(QStringLiteral("secret")).toString();
                    if (key == QStringLiteral("first") || key == QStringLiteral("second"))
                        response(socket, QJsonDocument(QJsonObject{{"accessToken", key + "-token"}}).toJson());
                    else response(socket, R"({"message":"bad key"})", 401);
                } else if (path == "/userapigateway/trading/account") {
                    response(socket, bearer == QStringLiteral("first")
                        ? (includeEntity
                            ? R"({"accounts":[{"accountId":"A","accountType":"BROKERAGE"},{"accountId":"E","accountType":"ENTITY"},{"accountId":"B","accountType":"ROTH_IRA"}]})"
                            : R"({"accounts":[{"accountId":"A","accountType":"BROKERAGE"},{"accountId":"A","accountType":"BROKERAGE"},{"accountId":"B","accountType":"ROTH_IRA"}]})")
                        : R"({"accounts":[{"accountId":"C","accountType":"TRADITIONAL_IRA"}]})");
                } else if (path.contains("/portfolio/v2")) {
                    response(socket, portfolioResponses.value(path.split('/').at(3), path.contains("/A/")
                        ? R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"AAPL"},"quantity":"2.5"}]})"
                        : path.contains("/E/")
                            ? R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"AAPL"},"quantity":"4"}]})"
                            : R"({"positions":[]})"));
                } else if (path.contains("/preflight/single-leg") || path.endsWith("/order")) {
                    orderResponseTimes << clock.elapsed();
                    quantities << request.value(QStringLiteral("quantity")).toString();
                    const QString symbol = request.value(QStringLiteral("instrument")).toObject()
                                               .value(QStringLiteral("symbol")).toString();
                    orderedTrades << QStringLiteral("%1:%2").arg(QString::fromUtf8(path.split('/').at(3)), symbol);
                    if (failSymbol == symbol) response(socket, R"({"message":"rejected"})", 400);
                    else if (path.endsWith("/order")) response(socket, R"({"status":"ACCEPTED"})");
                    else response(socket, R"({"orderValue":"100","estimatedExecutionFee":"0"})");
                } else {
                    response(socket, R"({"message":"unexpected path"})", 404);
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QObject::destroyed, this, [buffer]() { delete buffer; });
        });
    }

    void response(QTcpSocket *socket, const QByteArray &body, int status = 200)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " OK\r\nContent-Type: application/json\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(serverPort())); }
    QElapsedTimer clock;
    QList<qint64> authTimes;
    QList<qint64> orderResponseTimes;
    QHash<QByteArray, QByteArray> portfolioResponses;
    QStringList quantities;
    QStringList orderedTrades;
    QString failSymbol;
    bool includeEntity = false;
};

class AutoSequenceTest : public QObject
{
    Q_OBJECT
private slots:
    void sellAllSelectsRequestedTickerBalancePerAccount()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.includeEntity = true;
        server.portfolioResponses = {
            {"A", R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"AAPL"},"quantity":"2.75"}]})"},
            {"B", R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"AAPL"},"quantity":"1.25"}]})"},
            {"E", R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"MSFT"},"quantity":"9"}]})"}
        };
        PublicApiWorker worker(server.url());
        QSignalSpy loaded(&worker, &PublicApiWorker::accountLoadFinished);
        worker.processToken(QStringLiteral("first-token"));
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 4000);
        QVERIFY(loaded.first().at(0).toBool());

        worker.executePreflight(QStringLiteral(" aapl "), QStringLiteral("SELL"));
        QTRY_VERIFY_WITH_TIMEOUT(worker.tradeResultsComplete(), 6000);
        QCOMPARE(server.orderedTrades, QStringList({"A:AAPL", "B:AAPL"}));
        QCOMPARE(server.quantities, QStringList({"2.75", "1.25"}));
        QCOMPARE(worker.tradeResults().size(), 3);
        QCOMPARE(worker.tradeResults().at(1).toMap().value("accountId").toString(), QStringLiteral("E"));
        QCOMPARE(worker.tradeResults().at(1).toMap().value("status").toString(), QStringLiteral("failed"));
    }

    void entityAccountTradesInSelectedKeyAndAutoMode()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.includeEntity = true;
        PublicApiWorker worker(server.url());
        QSignalSpy loaded(&worker, &PublicApiWorker::accountLoadFinished);
        worker.processToken(QStringLiteral("first-token"));
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 4000);
        QCOMPARE(loaded.first().at(0).toBool(), true);

        worker.executeTrade(QStringLiteral("AAPL, GM"), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(worker.tradeResultsComplete(), 6000);
        QVERIFY(server.orderedTrades.contains(QStringLiteral("E:AAPL")));
        QVERIFY(server.orderedTrades.contains(QStringLiteral("E:GM")));
        QCOMPARE(worker.tradeResults().size(), 6);
        QCOMPARE(worker.tradeResults().at(2).toMap().value(QStringLiteral("accountType")).toString(), QStringLiteral("ENTITY"));
        QCOMPARE(worker.tradeResults().at(2).toMap().value(QStringLiteral("accountLabel")).toString(), QStringLiteral("Business: E"));

        server.orderedTrades.clear();
        server.quantities.clear();
        worker.executePreflight(QStringLiteral("AAPL"), QStringLiteral("SELL"));
        QTRY_VERIFY_WITH_TIMEOUT(worker.tradeResultsComplete(), 6000);
        QCOMPARE(server.orderedTrades, QStringList({QStringLiteral("A:AAPL"), QStringLiteral("E:AAPL")}));
        QCOMPARE(server.quantities, QStringList({QStringLiteral("2.5"), QStringLiteral("4")}));

        worker.clearSubAccountsList();
        server.orderedTrades.clear();
        PublicAuthClient auth({}, QStringLiteral("TestService"), QStringLiteral("TestGroup"), server.url(),
            [](const QString &label, const PublicAuthClient::CredentialCallback &done) {
                done(label == QStringLiteral("PublicsApiKey0") ? QStringLiteral("first") : QStringLiteral("second"), {});
            });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0"), QStringLiteral("PublicsApiKey1")});
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);
        controller.submit(QStringLiteral("AAPL"), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 12000);
        QCOMPARE(server.orderedTrades, QStringList({QStringLiteral("A:AAPL"), QStringLiteral("E:AAPL"),
                                                   QStringLiteral("B:AAPL"), QStringLiteral("C:AAPL")}));
        QCOMPARE(controller.tradeResults().size(), 4);
    }

    void selectedKeyMarketRequestsUseSameAccountTickerSequenceOnLocalServer()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicApiWorker worker(server.url());
        QSignalSpy loaded(&worker, &PublicApiWorker::accountLoadFinished);
        worker.processToken(QStringLiteral("first-token"));
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 4000);
        QCOMPARE(loaded.first().at(0).toBool(), true);

        server.clock.start();
        worker.executeTrade(QStringLiteral(" AAPL, TSLA, "), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(worker.tradeResultsComplete(), 6000);
        QCOMPARE(server.orderedTrades, QStringList({QStringLiteral("A:AAPL"), QStringLiteral("A:TSLA"),
                                                   QStringLiteral("B:AAPL"), QStringLiteral("B:TSLA")}));
        QCOMPARE(server.quantities, QStringList(4, QStringLiteral("1")));
        for (const QVariant &row : worker.tradeResults())
            QCOMPARE(row.toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    }

    void selectedKeyPreflightContinuesAfterOneTickerFails()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.failSymbol = QStringLiteral("TSLA");
        PublicApiWorker worker(server.url());
        worker.processToken(QStringLiteral("first-token"));
        QSignalSpy loaded(&worker, &PublicApiWorker::accountLoadFinished);
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 4000);
        QCOMPARE(loaded.first().at(0).toBool(), true);

        server.clock.start();
        worker.executePreflight(QStringLiteral("AAPL, TSLA, GM,"), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(worker.tradeResultsComplete(), 6000);
        QCOMPARE(server.orderedTrades, QStringList({QStringLiteral("A:AAPL"), QStringLiteral("A:TSLA"), QStringLiteral("A:GM"),
                                                   QStringLiteral("B:AAPL"), QStringLiteral("B:TSLA"), QStringLiteral("B:GM")}));
        QCOMPARE(worker.tradeResults().size(), 6);
        for (int i = 0; i < 6; ++i) {
            const QVariantMap row = worker.tradeResults().at(i).toMap();
            QCOMPARE(row.value(QStringLiteral("status")).toString(),
                     i % 3 == 1 ? QStringLiteral("failed") : QStringLiteral("success"));
            QCOMPARE(row.value(QStringLiteral("symbol")).toString(),
                     QStringList({QStringLiteral("AAPL"), QStringLiteral("TSLA"), QStringLiteral("GM")}).at(i % 3));
        }
    }

    void multiTickerBuyRunsEachAccountBeforeMovingToNextKey()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, QStringLiteral("TestService"), QStringLiteral("TestGroup"), server.url(),
            [](const QString &label, const PublicAuthClient::CredentialCallback &done) {
                done(label == QStringLiteral("PublicsApiKey0") ? QStringLiteral("first") : QStringLiteral("second"), {});
            });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0"), QStringLiteral("PublicsApiKey1")});
        PublicApiWorker worker(server.url());
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);

        server.clock.start();
        controller.submit(QStringLiteral("aapl, t$sla, gm,"), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 16000);
        QCOMPARE(server.orderedTrades, QStringList({QStringLiteral("A:AAPL"), QStringLiteral("A:TSLA"), QStringLiteral("A:GM"),
                                                   QStringLiteral("B:AAPL"), QStringLiteral("B:TSLA"), QStringLiteral("B:GM"),
                                                   QStringLiteral("C:AAPL"), QStringLiteral("C:TSLA"), QStringLiteral("C:GM")}));
        QCOMPARE(server.quantities, QStringList(9, QStringLiteral("1")));
        QCOMPARE(controller.tradeResults().size(), 9);
        for (qsizetype i = 0; i < server.orderResponseTimes.size(); ++i) {
            if (i % 3 != 0)
                QVERIFY(server.orderResponseTimes.at(i) - server.orderResponseTimes.at(i - 1) >= 90);
        }
        for (const QVariant &result : controller.tradeResults()) {
            QVERIFY(result.toMap().contains(QStringLiteral("symbol")));
            QCOMPARE(result.toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        }
        QVERIFY(!auth.sessionActive());
    }

    void multiTickerSellUsesEachAccountsOwnBalancesAndSkipsUnheldSymbols()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, QStringLiteral("TestService"), QStringLiteral("TestGroup"), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done(QStringLiteral("first"), {}); });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0")});
        PublicApiWorker worker(server.url());
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);
        controller.submit(QStringLiteral("AAPL, GM,"), QStringLiteral("SELL"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 6000);
        QCOMPARE(server.orderedTrades, QStringList({QStringLiteral("A:AAPL")}));
        QCOMPARE(server.quantities, QStringList({QStringLiteral("2.5")}));
        QCOMPARE(controller.tradeResults().size(), 4);
        QCOMPARE(controller.tradeResults().at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        for (int i = 1; i < 4; ++i)
            QCOMPARE(controller.tradeResults().at(i).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
    }

    void buyFinishesFirstKeyClosesSessionWaitsThreeSecondsThenUsesSecondKey()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, QStringLiteral("TestService"), QStringLiteral("TestGroup"), server.url(),
            [](const QString &label, const PublicAuthClient::CredentialCallback &done) {
                done(label == QStringLiteral("PublicsApiKey0") ? QStringLiteral("first") : QStringLiteral("second"), {});
            });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0"), QStringLiteral("PublicsApiKey1")});
        PublicApiWorker worker(server.url());
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);
        QList<qint64> sessionClosedTimes;
        connect(&auth, &PublicAuthClient::sessionActiveChanged, &auth, [&]() {
            if (!auth.sessionActive()) sessionClosedTimes << server.clock.elapsed();
        });
        bool sawPendingAccount = false;
        connect(&controller, &AutoTradeController::tradeResultsChanged, &controller, [&]() {
            for (const QVariant &item : controller.tradeResults()) {
                const QVariantMap row = item.toMap();
                if (row.value(QStringLiteral("accountId")) == QStringLiteral("A")
                    && row.value(QStringLiteral("status")) == QStringLiteral("pending"))
                    sawPendingAccount = true;
            }
        });

        server.clock.start();
        controller.submit(QStringLiteral("aapl"), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 12000);
        QVERIFY(sawPendingAccount);
        QCOMPARE(server.authTimes.size(), 2);
        QVERIFY(!sessionClosedTimes.isEmpty());
        QCOMPARE(server.orderResponseTimes.size(), 3);
        QVERIFY(sessionClosedTimes.first() >= server.orderResponseTimes.at(1));
        // The next token request must start three seconds after session closure, not seven.
        const qint64 betweenKeysElapsed = server.authTimes.at(1) - sessionClosedTimes.first();
        QVERIFY(betweenKeysElapsed >= 3000);
        QVERIFY(betweenKeysElapsed < 5000);
        QVERIFY(server.orderResponseTimes.at(2) >= server.authTimes.at(1));
        QCOMPARE(server.quantities, QStringList({QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")}));
        QCOMPARE(controller.tradeResults().size(), 3);
        for (const QVariant &result : controller.tradeResults()) {
            QCOMPARE(result.toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
            QVERIFY(result.toMap().value(QStringLiteral("accountLabel")).toString().contains(QStringLiteral("PublicsApiKey")));
        }
        QVERIFY(!auth.sessionActive());
        QVERIFY(auth.selectedApiKeyLabel().isEmpty());
        QVERIFY(!controller.tradeResultsBusy());
    }

    void sellAllUsesExistingPortfolioAndOrderPath()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString settingsGroup = QStringLiteral("AutoNicknameTest-%1")
                                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        PublicAuthClient auth({}, QStringLiteral("TestService"), settingsGroup, server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done(QStringLiteral("first"), {}); });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0")});
        QVERIFY(auth.setApiKeyNickname(QStringLiteral("PublicsApiKey0"), QStringLiteral("Retirement")));
        PublicApiWorker worker(server.url());
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);

        controller.submit(QStringLiteral("aapl"), QStringLiteral("SELL"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 5000);
        QCOMPARE(server.quantities, QStringList({QStringLiteral("2.5")}));
        QCOMPARE(controller.tradeResults().size(), 2);
        QCOMPARE(controller.tradeResults().at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        QCOMPARE(controller.tradeResults().at(1).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(controller.tradeResults().at(0).toMap().value(QStringLiteral("keyLabel")).toString(), QStringLiteral("PublicsApiKey0"));
        QVERIFY(controller.tradeResults().at(0).toMap().value(QStringLiteral("accountLabel")).toString().startsWith(QStringLiteral("Retirement · ")));
        QVERIFY(controller.tradeResults().at(1).toMap().value(QStringLiteral("accountLabel")).toString().startsWith(QStringLiteral("Retirement · ")));
        QVERIFY(!auth.sessionActive());
        QSettings settings;
        settings.beginGroup(settingsGroup);
        settings.remove(QString());
        settings.endGroup();
    }

    void authorizationFailureClosesFirstSessionAndContinuesToNextKey()
    {
        TestBrokerageServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, QStringLiteral("TestService"), QStringLiteral("TestGroup"), server.url(),
            [](const QString &label, const PublicAuthClient::CredentialCallback &done) {
                done(label == QStringLiteral("PublicsApiKey0") ? QStringLiteral("invalid") : QStringLiteral("second"), {});
            });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0"), QStringLiteral("PublicsApiKey1")});
        PublicApiWorker worker(server.url());
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);

        server.clock.start();
        controller.submit(QStringLiteral("AAPL"), QStringLiteral("BUY"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 12000);
        QCOMPARE(server.authTimes.size(), 2);
        QCOMPARE(server.quantities, QStringList({QStringLiteral("1")}));
        QCOMPARE(controller.tradeResults().size(), 2);
        QCOMPARE(controller.tradeResults().at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(controller.tradeResults().at(1).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        QVERIFY(!auth.sessionActive());
    }
};

QTEST_GUILESS_MAIN(AutoSequenceTest)
#include "test_auto_sequence.moc"
