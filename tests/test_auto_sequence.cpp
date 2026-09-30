#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "autotradecontroller.h"
#include "publicauthclient.h"
#include "publicapiworker.h"

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
                paths << QString::fromUtf8(path);
                if (path == "/userapiauthservice/personal/access-tokens") {
                    authTimes << clock.elapsed();
                    const QString key = request.value(QStringLiteral("secret")).toString();
                    if (key == QStringLiteral("first") || key == QStringLiteral("second"))
                        response(socket, QJsonDocument(QJsonObject{{"accessToken", key + "-token"}}).toJson());
                    else response(socket, R"({"message":"bad key"})", 401);
                } else if (path == "/userapigateway/trading/account") {
                    response(socket, bearer == QStringLiteral("first")
                        ? R"({"accounts":[{"accountId":"A","accountType":"BROKERAGE"},{"accountId":"A","accountType":"BROKERAGE"},{"accountId":"B","accountType":"ROTH_IRA"}]})"
                        : R"({"accounts":[{"accountId":"C","accountType":"TRADITIONAL_IRA"}]})");
                } else if (path.contains("/portfolio/v2")) {
                    response(socket, path.contains("/A/")
                        ? R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"AAPL"},"quantity":"2.5"}]})"
                        : R"({"positions":[]})");
                } else if (path.contains("/preflight/single-leg")) {
                    orderResponseTimes << clock.elapsed();
                    quantities << request.value(QStringLiteral("quantity")).toString();
                    response(socket, R"({"orderValue":"100","estimatedExecutionFee":"0"})");
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
    QStringList paths;
    QStringList quantities;
};

class AutoSequenceTest : public QObject
{
    Q_OBJECT
private slots:
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
        PublicAuthClient auth({}, QStringLiteral("TestService"), QStringLiteral("TestGroup"), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done(QStringLiteral("first"), {}); });
        auth.setSecretKeys({QStringLiteral("PublicsApiKey0")});
        PublicApiWorker worker(server.url());
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        AutoTradeController controller(&auth, &worker);

        controller.submit(QStringLiteral("aapl"), QStringLiteral("SELL"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.tradeResultsComplete(), 5000);
        QCOMPARE(server.quantities, QStringList({QStringLiteral("2.5")}));
        QCOMPARE(controller.tradeResults().size(), 2);
        QCOMPARE(controller.tradeResults().at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        QCOMPARE(controller.tradeResults().at(1).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(!auth.sessionActive());
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
