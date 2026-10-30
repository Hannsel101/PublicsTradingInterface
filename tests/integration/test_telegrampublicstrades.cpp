#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QUuid>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextDocument>

// Synthetic provider-shaped responses; no credentials or provider traffic.
class LoopbackPublics : public QTcpServer
{
public:
    struct Post { QString account; QJsonObject body; QString path; };
    QList<Post> posts;
    QStringList paths;
    QStringList accounts{QStringLiteral("A"), QStringLiteral("B")};
    bool rejectAuth = false;
    QString rejectedSymbol;
    QByteArray orderResponse;
    int orderHttpStatus = 200;
    QString reconciliationStatus;
    LoopbackPublics()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto buffer = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", buffer);
                    const auto end = buffer.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    qsizetype length = 0;
                    for (const auto &line : buffer.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    if (buffer.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const QString path = QUrl::fromPercentEncoding(buffer.left(buffer.indexOf("\r\n")).split(' ').at(1));
                    paths.append(path);
                    const auto body = QJsonDocument::fromJson(buffer.mid(end + 4, length)).object();
                    if (path.endsWith("/access-tokens")) {
                        if (rejectAuth) respond(socket, R"({"message":"SECRET-RAW-ERROR"})", 401);
                        else respond(socket, QJsonDocument(QJsonObject{{"accessToken", body.value("secret").toString() + "-token"}}).toJson());
                    } else if (path == "/userapigateway/trading/account") {
                        QJsonArray rows;
                        const auto ids = buffer.contains("second-token") ? QStringList{"C"} : accounts;
                        for (const auto &id : ids) rows.append(QJsonObject{{"accountId", id}, {"accountType", "BROKERAGE"}});
                        respond(socket, QJsonDocument(QJsonObject{{"accounts", rows}}).toJson());
                    } else if (path.endsWith("/portfolio/v2")) {
                        respond(socket, path.contains("/A/")
                            ? R"({"positions":[{"instrument":{"type":"EQUITY","symbol":"AAPL"},"quantity":"2.5"}]})"
                            : R"({"positions":[]})");
                    } else if (path.contains("/order/")) {
                        if (reconciliationStatus.isEmpty()) respond(socket, "{}", 404);
                        else respond(socket, QJsonDocument(QJsonObject{{"orderId", path.section('/', -1)}, {"status", reconciliationStatus}}).toJson());
                    } else if (path.endsWith("/order") || path.endsWith("/preflight/single-leg")) {
                        posts.append({path.section('/', 3, 3), body, path});
                        if (body.value("instrument").toObject().value("symbol").toString() == rejectedSymbol)
                            respond(socket, R"({"message":"SECRET-RAW-ERROR"})", 400);
                        else if (orderHttpStatus != 200) respond(socket, "{}", orderHttpStatus);
                        else if (!orderResponse.isEmpty()) respond(socket, orderResponse);
                        else if (path.endsWith("/order"))
                            respond(socket, QJsonDocument(QJsonObject{{"orderId", body.value("orderId")}, {"status", "NEW"}}).toJson());
                        else respond(socket, R"({"orderValue":"100","estimatedExecutionFee":"0"})");
                    } else respond(socket, "{}", 404);
                });
            }
        });
    }
    void respond(QTcpSocket *socket, const QByteArray &body, int status = 200)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(serverPort())); }
};

#include "telegrampublicstradeservice.h"
#include "trading/autotradecontroller.h"
#include "auth/publicauthclient.h"
#include "api/publicapiworker.h"


class TelegramPublicsTradesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDir;
private slots:
    void initTestCase()
    {
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    }

    void waitsForAllKeysAndAccountsAndGroupsResults()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.rejectedSymbol = "MSFT";
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &label, const PublicAuthClient::CredentialCallback &done) {
                done(label == "PublicsApiKey0" ? "first" : "second", {});
            });
        auth.setSecretKeys({"PublicsApiKey0", "PublicsApiKey1"});
        QVERIFY(auth.setApiKeyNickname("PublicsApiKey0", "Family · A&B <main>"));
        QVERIFY(auth.setApiKeyNickname("PublicsApiKey1", "Retirement"));
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QSignalSpy busy(&service, &TelegramPublicsTradeService::busyChanged);
        QVERIFY(service.submit("first-request", {" aapl ", "msft", "AAPL"}, "buy"));
        QVERIFY(service.busy());
        QVERIFY(completed.isEmpty());
        QVERIFY(!service.submit("overlap", {"GM"}, "SELL"));
        QTRY_COMPARE(server.posts.size(), 4);
        QVERIFY(completed.isEmpty()); // first key finished; three-second inter-key cooldown still busy
        QVERIFY(service.busy());
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 8000);
        QVERIFY(!service.busy());
        QVERIFY(controller.tradeResultsComplete());
        QCOMPARE(completed.first().at(0).toString(), QStringLiteral("first-request"));
        QCOMPARE(server.posts.size(), 6);
        QStringList sequence;
        for (const auto &post : server.posts) {
            sequence << post.account + ':' + post.body.value("instrument").toObject().value("symbol").toString();
            QCOMPARE(post.body.value("quantity").toString(), QStringLiteral("1"));
            QCOMPARE(post.path.endsWith("/order"), bool(PUBLICS_TELEGRAM_EXPECT_LIVE));
        }
        QCOMPARE(sequence, QStringList({"A:AAPL", "A:MSFT", "B:AAPL", "B:MSFT", "C:AAPL", "C:MSFT"}));
        const QString html = completed.first().at(1).toStringList().join('\n');
        QVERIFY(html.startsWith("<b>Publics Auto Buy Results</b>"));
        QVERIFY(html.contains("🟢 Family · A&amp;B &lt;main&gt; Account#A Successful Trades: AAPL"));
        QVERIFY(html.contains("🔴 Family · A&amp;B &lt;main&gt; Account#A Failed Trades: MSFT"));
        QVERIFY(html.indexOf("Account#A Successful") < html.indexOf("Account#B Successful"));
        QVERIFY(html.indexOf("Account#B Successful") < html.indexOf("Retirement Account#C Successful"));
        QVERIFY(!html.contains("SECRET-RAW-ERROR"));
        QVERIFY(!html.contains("first-token"));
        QVERIFY(!html.contains("Standard Brokerage"));
        QVERIFY(html.contains(PUBLICS_TELEGRAM_EXPECT_LIVE ? "fills not confirmed" : "Preflight only"));
        QCOMPARE(busy.size(), 2);
    }

    void refreshesCommittedNicknamesFromAnotherAuthBeforeEachBatch()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.accounts = {"A"};
        const QString group = QUuid::createUuid().toString();
        PublicAuthClient ui({}, "TelegramTradeTest", group);
        ui.setSecretKeys({"PublicsApiKey0"});
        QVERIFY(ui.setApiKeyNickname("PublicsApiKey0", "Old name"));
        PublicAuthClient auth({}, "TelegramTradeTest", group, server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        auth.setSecretKeys({"PublicsApiKey0"});
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QVERIFY(ui.setApiKeyNickname("PublicsApiKey0", "New · <&> name"));
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QVERIFY(service.submit("renamed", {"AAPL"}, "BUY"));
        QTRY_COMPARE(completed.size(), 1);
        const auto html = completed.first().at(1).toStringList().join('\n');
        QVERIFY(html.contains("New · &lt;&amp;&gt; name Account#A Successful Trades: AAPL"));
        QVERIFY(!html.contains("Old name"));
        QVERIFY(!html.contains("PublicsApiKey0"));
        QVERIFY(ui.setApiKeyNickname("PublicsApiKey0", {}));
        QVERIFY(service.submit("cleared", {"MSFT"}, "BUY"));
        QTRY_COMPARE(completed.size(), 2);
        QVERIFY(completed.last().at(1).toStringList().join('\n').contains("PublicsApiKey0 Account#A Successful Trades: MSFT"));
    }

    void splitsEscapedHtmlWithoutDroppingAccountsOrTickers()
    {
        LoopbackPublics server;
        server.accounts.clear();
        for (int i = 0; i < 8; ++i)
            server.accounts.append(QString::number(i) + QString(650, QLatin1Char('X')) + "&<😀>");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        auth.setSecretKeys({"PublicsApiKey0"});
        QVERIFY(auth.setApiKeyNickname("PublicsApiKey0", "Nest · <&>😀"));
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QVERIFY(service.submit("long", {"AAPL", "MSFT"}, "BUY"));
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 15000);
        const auto messages = completed.first().at(1).toStringList();
        QVERIFY(messages.size() > 1);
        QString body;
        for (QString html : messages) {
            const QString header = "<b>Publics Auto Buy Results</b>\n\n";
            QVERIFY(html.startsWith(header));
            // The formatter only produces this fixed bold tag and escaped text.
            QString decoded = html;
            decoded.remove("<b>"); decoded.remove("</b>");
            decoded.replace("&lt;", "<"); decoded.replace("&gt;", ">");
            decoded.replace("&quot;", "\""); decoded.replace("&amp;", "&");
            QVERIFY2(decoded.toUcs4().size() < 4096, qPrintable(QString::number(decoded.toUcs4().size())));
            html.remove(0, header.size());
            QVERIFY(!html.contains('<'));
            QVERIFY(!html.contains('>'));
            // No incomplete HTML entities at message boundaries.
            QVERIFY(!QRegularExpression("&(?!amp;|lt;|gt;|quot;)").match(html).hasMatch());
            body += html;
        }
        for (const auto &account : server.accounts)
            QCOMPARE(body.count(("Account#" + account).toHtmlEscaped()), 1);
        QCOMPARE(body.count("Successful Trades: AAPL, MSFT"), 8);
    }

    void setupFailureReportsEveryRequestedTickerWithoutLeakingErrors_data()
    {
        QTest::addColumn<bool>("savedKey");
        QTest::newRow("no-keys") << false;
        QTest::newRow("credential-unavailable") << true;
    }

    void setupFailureReportsEveryRequestedTickerWithoutLeakingErrors()
    {
        QFETCH(bool, savedKey);
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done({}, "SECRET-RAW-ERROR"); });
        auth.setSecretKeys(savedKey ? QStringList{"PublicsApiKey0"} : QStringList{});
        PublicApiWorker worker(server.url());
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QVERIFY(service.submit("setup", {"AAPL", "MSFT"}, "BUY"));
        QVERIFY(completed.isEmpty());
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!service.busy());
        const auto html = completed.first().at(1).toStringList().join('\n');
        QVERIFY(html.contains("Failed Trades: AAPL, MSFT"));
        QVERIFY(html.contains("Account/key setup failed"));
        QVERIFY(!html.contains("SECRET-RAW-ERROR"));
        QVERIFY(!html.contains("Successful Trades:"));
        QVERIFY(server.paths.isEmpty());
    }

    void sellUsesEachAccountsHoldingsAndDoesNotCallSkipsSuccessful()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        auth.setSecretKeys({"PublicsApiKey0"});
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QVERIFY(service.submit("sell", {"AAPL", "GM"}, "SELL"));
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(server.posts.size(), 1);
        QCOMPARE(server.posts.first().account, QStringLiteral("A"));
        QCOMPARE(server.posts.first().body.value("quantity").toString(), QStringLiteral("2.5"));
        QCOMPARE(server.posts.first().body.value("orderSide").toString(), QStringLiteral("SELL"));
        const auto html = completed.first().at(1).toStringList().join('\n');
        QVERIFY(html.startsWith("<b>Publics Auto Sell Results</b>"));
        QVERIFY(html.contains("Account#A Successful Trades: AAPL"));
        QVERIFY(html.contains("Account#A Failed Trades: GM"));
        QVERIFY(html.contains("Account#B Failed Trades: AAPL, GM"));
        QVERIFY(html.contains("Skipped/no holdings; no sell order submitted: AAPL, GM"));
        QVERIFY(!html.contains("Account#B Successful Trades:"));
        QVERIFY(!html.contains("Unconfirmed/pending"));
    }

    void unconfirmedOrderNeverBecomesSuccessfulAndIsNeverRetried()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.accounts = {"A"};
        server.orderHttpStatus = 503;
        server.reconciliationStatus = "UNRECOGNIZED";
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        auth.setSecretKeys({"PublicsApiKey0"});
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = policy.gatewayCooldownMs = 0;
        policy.retryBaseMs = policy.retryCapMs = 1;
        policy.maxRetries = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QVERIFY(service.submit("uncertain", {"AAPL"}, "BUY"));
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(server.posts.size(), 1);
        const auto html = completed.first().at(1).toStringList().join('\n');
        QVERIFY(!html.contains("Successful Trades:"));
        if (PUBLICS_TELEGRAM_EXPECT_LIVE) {
            QVERIFY(html.contains("Unconfirmed/pending outcomes: AAPL"));
            QVERIFY(html.contains("Check Public before submitting again"));
            QCOMPARE(server.paths.filter("/order/").size(), 1);
        } else QVERIFY(html.contains("Failed Trades: AAPL"));
    }

    void pendingSnapshotAndUnknownStatusesCannotClaimSuccess()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        auth.setSecretKeys({"PublicsApiKey0"});
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        bool sawPending = false;
        connect(&controller, &AutoTradeController::tradeResultsChanged, &service, [&]() {
            const auto rows = service.snapshot();
            for (const auto &row : rows) {
                if (row.accountId.isEmpty() || row.outcome != TelegramPublicsTradeService::Outcome::Unconfirmed) continue;
                const auto text = service.formatResults({row}, "BUY").join('\n');
                sawPending = true;
                QVERIFY(text.contains("Unconfirmed/pending outcomes: AAPL"));
                QVERIFY(!text.contains("Successful Trades:"));
                QVERIFY(completed.isEmpty());
            }
        });
        QVERIFY(service.submit("pending", {"AAPL"}, "BUY"));
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(sawPending);
    }

    void synchronousControllerCompletionCanDestroyService()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        PublicApiWorker worker(server.url());
        AutoTradeController controller(&auth, &worker);
        auto *service = new TelegramPublicsTradeService(&controller, &auth);
        QPointer<TelegramPublicsTradeService> guard(service);
        connect(&controller, &AutoTradeController::tradeResultsCompleteChanged, &auth, [&] {
            if (controller.tradeResultsComplete() && guard) delete guard.data();
        });
        QVERIFY(service->submit("empty-keys", {"AAPL"}, "BUY"));
        QVERIFY(!guard);
        QVERIFY(server.paths.isEmpty());
    }

    void completionObserversCanReplaceJobOrDeleteService()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(), server.url(),
            [](const QString &, const PublicAuthClient::CredentialCallback &done) { done("first", {}); });
        PublicApiWorker worker(server.url());
        AutoTradeController controller(&auth, &worker);
        auto *service = new TelegramPublicsTradeService(&controller, &auth);
        QPointer<TelegramPublicsTradeService> guard(service);
        QStringList finished;
        bool replacementAdmitted = false;
        connect(service, &TelegramPublicsTradeService::busyChanged, &auth, [&]() {
            if (guard && !guard->busy() && !replacementAdmitted) {
                replacementAdmitted = true;
                QVERIFY(guard->submit("replacement", {"MSFT"}, "SELL"));
            }
        });
        connect(service, &TelegramPublicsTradeService::completed, &auth, [&](QString id, QStringList messages) {
            finished.append(id);
            if (id == "replacement") {
                QVERIFY(messages.join('\n').contains("Publics Auto Sell Results"));
                QVERIFY(messages.join('\n').contains("MSFT"));
                delete service;
            } else {
                QCOMPARE(id, QStringLiteral("original"));
                QVERIFY(messages.join('\n').contains("Publics Auto Buy Results"));
            }
        });
        QVERIFY(service->submit("original", {"AAPL"}, "BUY"));
        QTRY_VERIFY(!guard);
        QCOMPARE(finished, QStringList({"original", "replacement"}));
        QVERIFY(server.paths.isEmpty());
    }

    void authorizedAdmissionWaitsForAsynchronousKeyIndexBeforeLoadingAnyCredential()
    {
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        int loads = 0;
        int discoveries = 0;
        const QString group = QUuid::createUuid().toString();
        QSettings settings("PublicsTradingInterface", "PublicsTradingInterface");
        settings.beginGroup(group);
        settings.setValue("labels", QStringList{"PublicsApiKey0"});
        settings.sync();
        PublicAuthClient auth({}, "TelegramTradeTest", group, server.url(),
            [&loads](const QString &label, const PublicAuthClient::CredentialCallback &done) {
                ++loads;
                done(label == "PublicsApiKey0" ? "first" : "second", {});
            },
            [&](const PublicAuthClient::KeyIndexCallback &done) {
                ++discoveries;
                QTimer::singleShot(40, &server, [done] {
                    done(QKeychain::NoError, "[\"PublicsApiKey0\",\"PublicsApiKey1\"]");
                });
            });
        PublicApiWorker worker(server.url());
        PublicRequestPolicy policy;
        policy.portfolioPauseMs = policy.symbolPauseMs = policy.accountPauseMs = 0;
        worker.setRequestPolicy(policy);
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        // Replace only the keychain index boundary, not auth/controller/worker execution.
        service.m_indexLoader = [&]() { auth.listStoredApiKeys(); };
        QCOMPARE(discoveries, 0);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QSignalSpy busy(&service, &TelegramPublicsTradeService::busyChanged);
        QVERIFY(service.submit("index", {"AAPL"}, "BUY"));
        QCOMPARE(discoveries, 1);
        QCOMPARE(loads, 0);
        QVERIFY(service.busy());
        QVERIFY(completed.isEmpty());
        QVERIFY(!service.submit("duplicate", {"MSFT"}, "BUY"));
        QTest::qWait(10);
        QCOMPARE(loads, 0);
        QVERIFY(server.paths.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 8000);
        QCOMPARE(loads, 2);
        QCOMPARE(server.posts.size(), 3);
        QCOMPARE(auth.secretKeys(), QStringList({"PublicsApiKey0", "PublicsApiKey1"}));
        QVERIFY(!service.busy());
        QCOMPARE(busy.size(), 2);
    }

    void failedCompleteScopeDiscoveryNeverTradesLocalSubset_data()
    {
        QTest::addColumn<int>("error");
        QTest::addColumn<QString>("payload");
        QTest::addColumn<bool>("synchronous");
        QTest::newRow("unavailable-authoritative-index") << int(QKeychain::AccessDenied) << QString("RAW-SECRET") << false;
        QTest::newRow("malformed-authoritative-index") << int(QKeychain::NoError)
            << QString("[\"PublicsApiKey0\",\"PublicsApiKey1\",\"RAW-SECRET\"]") << false;
        QTest::newRow("synchronous-failure") << int(QKeychain::AccessDenied) << QString("RAW-SECRET") << true;
    }

    void failedCompleteScopeDiscoveryNeverTradesLocalSubset()
    {
        QFETCH(int, error);
        QFETCH(QString, payload);
        QFETCH(bool, synchronous);
        LoopbackPublics server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString group = QUuid::createUuid().toString();
        QSettings settings("PublicsTradingInterface", "PublicsTradingInterface");
        settings.beginGroup(group);
        settings.setValue("labels", QStringList{"PublicsApiKey0"}); // A only; secure index owns A+B.
        settings.sync();
        int loads = 0;
        int discoveries = 0;
        PublicAuthClient::KeyIndexCallback finishIndex;
        PublicAuthClient auth({}, "TelegramTradeTest", group, server.url(),
            [&](const QString &, const PublicAuthClient::CredentialCallback &done) { ++loads; done("first", {}); },
            [&](const PublicAuthClient::KeyIndexCallback &done) {
                ++discoveries;
                finishIndex = done;
                if (synchronous) done(QKeychain::Error(error), payload);
            });
        PublicApiWorker worker(server.url());
        AutoTradeController controller(&auth, &worker);
        connect(&auth, &PublicAuthClient::tokenReceived, &worker, &PublicApiWorker::processToken);
        TelegramPublicsTradeService service(&controller, &auth);
        service.m_indexLoader = [&] { auth.listStoredApiKeys(); };
        QCOMPARE(discoveries, 0);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QSignalSpy busy(&service, &TelegramPublicsTradeService::busyChanged);
        QVERIFY(service.submit("incomplete-scope", {"AAPL", "MSFT"}, "BUY"));
        QCOMPARE(auth.secretKeys(), QStringList{"PublicsApiKey0"});
        QCOMPARE(discoveries, 1);
        if (!synchronous) {
            QCoreApplication::processEvents();
            QCOMPARE(loads, 0);
            QVERIFY(service.busy());
            QVERIFY(completed.isEmpty());
            finishIndex(QKeychain::Error(error), payload);
        }
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(loads, 0);
        QVERIFY(controller.tradeResults().isEmpty());
        QVERIFY(server.paths.isEmpty());
        QVERIFY(server.posts.isEmpty());
        QVERIFY(!auth.sessionActive());
        QVERIFY(!service.busy());
        QCOMPARE(busy.size(), 2);
        QCOMPARE(completed.first().at(0).toString(), QStringLiteral("incomplete-scope"));
        const auto html = completed.first().at(1).toStringList().join('\n');
        QVERIFY(html.contains("Failed Trades: AAPL, MSFT"));
        QVERIFY(html.contains("discovery"));
        QVERIFY(html.contains("no trades submitted"));
        QVERIFY(!html.contains("Successful Trades:"));
        QVERIFY(!html.contains("RAW-SECRET"));
    }

    void productionConstructionIsLazyAndInjectedConstructionDoesNotDiscoverKeys()
    {
        TelegramPublicsTradeService production;
        auto *auth = production.findChild<PublicAuthClient *>();
        QVERIFY(auth);
        QVERIFY(!auth->apiKeyIndexLoading());
        QVERIFY(!auth->apiKeyLoading());
        QVERIFY(!auth->sessionActive());
        QVERIFY(!production.busy());
        QVERIFY(!production.submit("invalid", {"AAPL", "BRK.B"}, "BUY"));
        QTest::qWait(20);
        QVERIFY(!auth->apiKeyIndexLoading());
        QVERIFY(!auth->sessionActive());
        QVERIFY(auth->secretKeys().isEmpty());
    }

    void rejectsEntireMalformedInputWithoutLoadingCredentials()
    {
        int loads = 0;
        PublicAuthClient auth({}, "TelegramTradeTest", QUuid::createUuid().toString(),
                              QUrl("http://127.0.0.1:1/"),
                              [&loads](const QString &, const PublicAuthClient::CredentialCallback &) { ++loads; });
        auth.setSecretKeys({"PublicsApiKey0"});
        PublicApiWorker worker(QUrl("http://127.0.0.1:1/"));
        AutoTradeController controller(&auth, &worker);
        TelegramPublicsTradeService service(&controller, &auth);
        QSignalSpy completed(&service, &TelegramPublicsTradeService::completed);
        QVERIFY(!service.submit("request", {"AAPL", "bad$ticker"}, "BUY"));
        QVERIFY(!service.submit("request", {"AAPL", ""}, "BUY"));
        QVERIFY(!service.submit("request", {"AAPL,MSFT"}, "BUY"));
        QVERIFY(!service.submit("request", {}, "BUY"));
        QVERIFY(!service.submit("", {"AAPL"}, "BUY"));
        QVERIFY(!service.submit("request", {"AAPL"}, "HOLD"));
        QVERIFY(!service.submit("request", {QString::fromUtf8("ß")}, "BUY"));
        QVERIFY(!service.busy());
        QCOMPARE(loads, 0);
        QVERIFY(controller.tradeResults().isEmpty());
        QTest::qWait(10);
        QVERIFY(completed.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TelegramPublicsTradesTest)
#include "test_telegrampublicstrades.moc"
