#include "rhnetworkengine.h"
#include "rhoauthgrant.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QDateTime>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QtTest/QtTest>

namespace {
QByteArray expectedUserAgent()
{
#if defined(Q_OS_IOS)
    return "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1";
#elif defined(Q_OS_ANDROID)
    return "Mozilla/5.0 (Linux; Android 14; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36";
#elif defined(Q_OS_WIN)
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
#elif defined(Q_OS_MACOS)
    return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
#else
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
#endif
}
}

class RHNetworkEngineTest : public QObject
{
    Q_OBJECT
private slots:
    void typedOrderReadIsPrimaryOnlyAndSafe() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray request;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *peer = server.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                request += peer->readAll();
                if (!request.contains("\r\n\r\n")) return;
                peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"); peer->disconnectFromHost();
            });
        });
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject entry{{"URL", QString("http://127.0.0.1:%1/accounts/").arg(server.serverPort())}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", entry}}}}).toJson()); file.close();
        RHNetworkEngine production;
        QVERIFY(production.loadConfiguration(file.fileName()));
        QVERIFY(!production.readStockOrder("broker-id", "secret-fixture", RHNetworkEngine::AuthKind::OAuthBearer));
        RHNetworkEngine network(nullptr, true);
        QVERIFY(network.loadConfiguration(file.fileName()));
        for (const auto &id : {"", "../other", "x?secret=y", "x/y", "x%2fy", "user@host", "prefix-secret-fixture"})
            QVERIFY(!network.readStockOrder(id, "secret-fixture", RHNetworkEngine::AuthKind::OAuthBearer));
        QVERIFY(!network.readStockOrder("broker-id", "secret-fixture", RHNetworkEngine::AuthKind::WebSecondaryBearer));
        auto *reply = network.readStockOrder("broker-id", "secret-fixture", RHNetworkEngine::AuthKind::OAuthBearer);
        QVERIFY(reply); QSignalSpy done(reply, &QNetworkReply::finished); QTRY_COMPARE(done.size(), 1);
        QVERIFY(request.startsWith("GET /orders/broker-id/ HTTP/1.1\r\n"));
        QVERIFY(request.contains("Authorization: Bearer secret-fixture\r\n"));
        QVERIFY(!request.toLower().contains("cookie:"));
        QCOMPARE(reply->request().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
        reply->deleteLater();
    }
    void genericRoutesRejectRefreshPayloads_data()
    {
        QTest::addColumn<QString>("endpoint");
        QTest::addColumn<QVariantMap>("defaults");
        QTest::addColumn<QVariantMap>("body");
        const QVariantMap password{{"grant_type", "password"}, {"client_id", "client"},
            {"username", "tester"}, {"password", "password-fixture"}};
        QTest::newRow("login-body-grant") << QString("Login") << password << QVariantMap{{"grant_type", "refresh_token"}};
        QTest::newRow("login-default-grant") << QString("Login") << QVariantMap{{"grant_type", "refresh_token"}} << QVariantMap{};
        QTest::newRow("login-body-secret") << QString("Login") << password << QVariantMap{{"refresh_token", "refresh-fixture"}};
        QTest::newRow("login-default-secret") << QString("Login") << QVariantMap{{"refresh_token", "refresh-fixture"}} << password;
        QTest::newRow("login-merged") << QString("Login") << QVariantMap{{"refresh_token", "refresh-fixture"}} << QVariantMap{{"grant_type", "refresh_token"}};
        QTest::newRow("login-null-secret") << QString("Login") << password << QVariantMap{{"refresh_token", QVariant()}};
        QTest::newRow("login-bool-secret") << QString("Login") << password << QVariantMap{{"refresh_token", false}};
        QTest::newRow("login-case-secret") << QString("Login") << password << QVariantMap{{"Refresh_Token", "refresh-fixture"}};
        QTest::newRow("login-case-grant") << QString("Login") << password << QVariantMap{{"GRANT_TYPE", "Refresh_Token"}};
        for (const auto &route : {"BeginVerification", "ContinueVerification", "SubmitChallenge"}) {
            const QByteArray name(route);
            QTest::newRow((name + "-body").constData()) << QString(route) << QVariantMap{} << QVariantMap{{"refresh_token", "refresh-fixture"}};
            QTest::newRow((name + "-defaults").constData()) << QString(route) << QVariantMap{{"grant_type", "refresh_token"}} << QVariantMap{};
            QTest::newRow((name + "-case-grant").constData()) << QString(route) << QVariantMap{} << QVariantMap{{"GRANT_TYPE", " Refresh_Token "}};
        }
    }
    void loginRejectsUnsupportedPasswordSchema_data()
    {
        QTest::addColumn<QString>("endpoint");
        QTest::addColumn<QVariantMap>("defaults");
        QTest::addColumn<QVariantMap>("body");
        const QVariantMap password{{"grant_type", "password"}, {"client_id", "client"},
            {"username", "tester"}, {"password", "password-fixture"}};
        for (const auto &field : {"grant_type", "client_id", "username", "password"}) {
            auto missing = password; missing.remove(field);
            QTest::newRow((QByteArray("missing-") + field).constData()) << QString("Login") << missing << QVariantMap{};
        }
        QTest::newRow("other-grant") << QString("Login") << password << QVariantMap{{"grant_type", "client_credentials"}};
        QTest::newRow("case-grant") << QString("Login") << password << QVariantMap{{"grant_type", "Password"}};
        QTest::newRow("null-grant") << QString("Login") << password << QVariantMap{{"grant_type", QVariant()}};
        QTest::newRow("list-grant") << QString("Login") << password << QVariantMap{{"grant_type", QStringList{"password", "refresh_token"}}};
        QTest::newRow("case-duplicate") << QString("Login") << password << QVariantMap{{"Grant_Type", "password"}};
        QTest::newRow("encoded-key") << QString("Login") << password << QVariantMap{{"grant%5ftype", "password"}};
        QTest::newRow("form-injection-key") << QString("Login") << password << QVariantMap{{"x&grant_type", "password"}};
        QTest::newRow("unknown-default") << QString("Login") << QVariantMap{{"assertion", "fixture"}} << password;
        QTest::newRow("list-password") << QString("Login") << password << QVariantMap{{"password", QStringList{"one", "two"}}};
        QTest::newRow("map-password") << QString("Login") << password << QVariantMap{{"password", QVariantMap{{"value", "fixture"}}}};
        QTest::newRow("number-username") << QString("Login") << password << QVariantMap{{"username", 123}};
        QTest::newRow("empty-password") << QString("Login") << password << QVariantMap{{"password", ""}};
        QTest::newRow("control-password") << QString("Login") << password << QVariantMap{{"password", "unsafe\r\nfixture"}};
        QTest::newRow("string-boolean") << QString("Login") << password << QVariantMap{{"try_passkeys", "False"}};
    }
    void loginRejectsUnsupportedPasswordSchema()
    {
        genericRoutesRejectRefreshPayloads();
    }
    void genericRoutesRejectRefreshPayloads()
    {
        QFETCH(QString, endpoint); QFETCH(QVariantMap, defaults); QFETCH(QVariantMap, body);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection();
                connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                    peer->readAll(); ++requests;
                    peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                    peer->disconnectFromHost();
                });
            }
        });
        const QMap<QString, QString> paths{{"Login", "/oauth2/token/"}, {"BeginVerification", "/pathfinder/user_machine/"},
            {"ContinueVerification", "/pathfinder/inquiries/fixture/"}, {"SubmitChallenge", "/challenge/fixture/respond/"}};
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        QJsonObject entry{{"URL", QString("http://127.0.0.1:%1").arg(server.serverPort()) + paths.value(endpoint)},
            {"Method", "POST"}, {"BodyEncoding", endpoint == "Login" ? "form" : "json"},
            {"HeaderSets", QJsonArray{}}, {"BodyDefaults", QJsonObject::fromVariantMap(defaults)}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{endpoint, entry}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        QString error;
        auto *reply = network.send(endpoint, {}, {}, body, &error);
        QTest::qWait(100);
        if (reply) { reply->abort(); reply->deleteLater(); }
        QCOMPARE(requests, 0);
        QVERIFY(!reply); QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains("refresh-fixture"));
    }

    void refreshUsesBoundGrantAndIsolatesAmbientState()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        QJsonObject route{{"URL", QString("http://127.0.0.1:%1/oauth2/token/").arg(server.serverPort())},
            {"Method", "POST"}, {"BodyEncoding", "form"}, {"HeaderSets", QJsonArray{"Hostile"}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{{"Hostile", QJsonObject{{"Cookie", "do-not-send"}, {"Authorization", "do-not-send"}}}}},
            {"Endpoints", QJsonObject{{"TokenRefresh", route}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        RHOAuthGrant grant; QString error;
        QVERIFY(RHOAuthGrant::parseResponse({{"access_token", "old-access"}, {"refresh_token", "refresh+fixture/="},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 321}}, "original+client", "original+device", QDateTime::currentDateTimeUtc().addDays(-5), &grant));
        auto *reply = network.refreshOAuth(grant, &error); QVERIFY2(reply, qPrintable(error));
        QCOMPARE(reply->request().attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(reply->request().attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(reply->request().attribute(QNetworkRequest::CacheLoadControlAttribute).toInt(), int(QNetworkRequest::AlwaysNetwork));
        QCOMPARE(reply->request().attribute(QNetworkRequest::CacheSaveControlAttribute).toBool(), false);
        QCOMPARE(reply->request().sslConfiguration().protocol(), QSsl::TlsV1_2OrLater);
        QCOMPARE(reply->request().sslConfiguration().peerVerifyMode(), QSslSocket::VerifyPeer);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->peek(peer->bytesAvailable()).contains("refresh_token="));
        const auto request = peer->readAll();
        QVERIFY(request.startsWith("POST /oauth2/token/ HTTP/1.1\r\n"));
        QVERIFY(!request.toLower().contains("cookie:")); QVERIFY(!request.toLower().contains("authorization:"));
        QVERIFY(!request.contains("old-access")); QVERIFY(!request.contains("password=")); QVERIFY(!request.contains("expires_in="));
        QVERIFY(request.contains("grant_type=refresh_token")); QVERIFY(request.contains("refresh_token=refresh%2Bfixture%2F%3D"));
        QVERIFY(request.contains("client_id=original%2Bclient")); QVERIFY(request.contains("device_token=original%2Bdevice"));
        peer->write("HTTP/1.1 302 Found\r\nLocation: https://evil.example/\r\nSet-Cookie: injected=1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); peer->flush();
        QTRY_VERIFY(!finished.isEmpty()); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 302);
        reply->deleteLater();
        QVERIFY(!network.send("TokenRefresh", {}, {}, {{"refresh_token", "untyped-secret"}}, &error));
        QVERIFY(error.contains("typed", Qt::CaseInsensitive));
    }

    void refreshRejectsUnsafeRoutes_data()
    {
        QTest::addColumn<QString>("url"); QTest::addColumn<QString>("method"); QTest::addColumn<QString>("encoding"); QTest::addColumn<bool>("optIn");
        QTest::newRow("foreign") << QString("https://evil.example/oauth2/token/") << QString("POST") << QString("form") << false;
        QTest::newRow("wrong-port") << QString("https://api.robinhood.com:444/oauth2/token/") << QString("POST") << QString("form") << false;
        QTest::newRow("wrong-route") << QString("https://api.robinhood.com/orders/") << QString("POST") << QString("form") << false;
        QTest::newRow("query") << QString("https://api.robinhood.com/oauth2/token/?refresh_token=fixture") << QString("POST") << QString("form") << false;
        QTest::newRow("encoded-path") << QString("http://127.0.0.1:1/oauth2/%74oken/") << QString("POST") << QString("form") << true;
        QTest::newRow("no-loopback-opt-in") << QString("http://127.0.0.1:1/oauth2/token/") << QString("POST") << QString("form") << false;
        QTest::newRow("get") << QString("https://api.robinhood.com/oauth2/token/") << QString("GET") << QString("none") << false;
        QTest::newRow("json") << QString("https://api.robinhood.com/oauth2/token/") << QString("POST") << QString("json") << false;
    }
    void refreshRejectsUnsafeRoutes()
    {
        QFETCH(QString, url); QFETCH(QString, method); QFETCH(QString, encoding); QFETCH(bool, optIn);
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"TokenRefresh", QJsonObject{
            {"URL", url}, {"Method", method}, {"BodyEncoding", encoding}, {"HeaderSets", QJsonArray{}}}}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, optIn); QVERIFY(network.loadConfiguration(file.fileName()));
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse({{"access_token", "access-fixture"}, {"refresh_token", "refresh-fixture"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 1}}, "client", "device", QDateTime::currentDateTimeUtc(), &grant));
        QString error; QVERIFY(!network.refreshOAuth(grant, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(!error.contains("fixture"));
        grant.effectiveScope = "read"; QVERIFY(!network.refreshOAuth(grant, &error));
    }

    void refreshResponseBounds_data()
    {
        QTest::addColumn<bool>("oversize");
        QTest::newRow("absolute-stream-deadline") << false;
        QTest::newRow("oversize-content-length") << true;
    }
    void refreshResponseBounds()
    {
        QFETCH(bool, oversize);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"TokenRefresh", QJsonObject{
            {"URL", QString("http://127.0.0.1:%1/oauth2/token/").arg(server.serverPort())}, {"Method", "POST"}, {"BodyEncoding", "form"}, {"HeaderSets", QJsonArray{}}}}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true, 700); QVERIFY(network.loadConfiguration(file.fileName()));
        RHOAuthGrant grant; QVERIFY(RHOAuthGrant::parseResponse({{"access_token", "access-fixture"}, {"refresh_token", "refresh-fixture"},
            {"token_type", "Bearer"}, {"scope", "internal"}, {"expires_in", 1}}, "client", "device", QDateTime::currentDateTimeUtc(), &grant));
        auto *reply = network.refreshOAuth(grant); QVERIFY(reply); QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable()); peer->readAll();
        peer->write(oversize ? "HTTP/1.1 200 OK\r\nContent-Length: 2000000\r\n\r\n" : "HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n"); peer->flush();
        QTimer progress; progress.setInterval(20); int chunks = 0;
        connect(&progress, &QTimer::timeout, peer, [peer, &chunks] { if (peer->state() == QAbstractSocket::ConnectedState) { peer->write(" "); peer->flush(); ++chunks; } });
        progress.start();
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), oversize ? 300 : 1200);
        if (!oversize) QVERIFY(chunks > 3);
        QCOMPARE(reply->error(), QNetworkReply::OperationCanceledError); QCOMPARE(finished.size(), 1);
        reply->deleteLater();
    }

    void productionLiveOrdersRequireExplicitReleaseDefinition()
    {
        RHNetworkEngine network;
#ifdef RELEASE
        QVERIFY(network.stockLiveOrdersEnabled());
#else
        QVERIFY(!network.stockLiveOrdersEnabled());
#endif
    }

    void progressingStockRepliesStillReachAbsoluteDeadline_data()
    {
        QTest::addColumn<bool>("post");
        QTest::newRow("read") << false;
        QTest::newRow("post") << true;
    }
    void progressingStockRepliesStillReachAbsoluteDeadline()
    {
        QFETCH(bool, post);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QJsonObject endpoint{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", endpoint}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true, 1000); QVERIFY(network.loadConfiguration(file.fileName()));
        RHNetworkEngine::StockOrder order;
        order.accountNumber = "100"; order.account = QUrl(origin + "/accounts/a/");
        order.instrument = QUrl(origin + "/instruments/i/"); order.symbol = "AAPL";
        order.quantity = "1"; order.side = "buy"; order.price = "10"; order.askPrice = "10"; order.bidPrice = "9.99"; order.quoteTimestamp = QDateTime::currentDateTimeUtc();
        order.refId = "12345678-1234-4234-8234-123456789abc";
        auto *reply = post ? network.sendStockOrder(order, "fixture", RHNetworkEngine::AuthKind::OAuthBearer, nullptr, true)
            : network.readStock(RHNetworkEngine::StockRead::Accounts, "fixture", RHNetworkEngine::AuthKind::OAuthBearer);
        QVERIFY(reply); QSignalSpy finished(reply, &QNetworkReply::finished);
        QCOMPARE(reply->request().transferTimeout(), 16000);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable()); peer->readAll();
        peer->write("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n"); peer->flush();
        QTimer progress; progress.setInterval(20); int chunks = 0;
        connect(&progress, &QTimer::timeout, peer, [peer, &chunks] {
            if (peer->state() == QAbstractSocket::ConnectedState) { peer->write(" "); peer->flush(); ++chunks; }
        });
        progress.start();
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 1500);
        QVERIFY(chunks > 3);
        QCOMPARE(reply->error(), QNetworkReply::OperationCanceledError);
        QCOMPARE(finished.size(), 1); reply->deleteLater();
    }

    void stockReadsRejectCredentialsBeforeDispatch_data()
    {
        QTest::addColumn<int>("resource");
        QTest::addColumn<QByteArray>("token");
        QTest::addColumn<QString>("scope");
        QTest::addColumn<QString>("page");
        using Read = RHNetworkEngine::StockRead;
        QTest::newRow("literal-cursor") << int(Read::Accounts) << QByteArray("secret-fixture") << QString() << "/accounts/?cursor=secret-fixture";
        QTest::newRow("containing-cursor") << int(Read::Accounts) << QByteArray("secret-fixture") << QString() << "/accounts/?cursor=prefix-secret-fixture-suffix";
        QTest::newRow("encoded-cursor") << int(Read::Accounts) << QByteArray("secret+fixture/=") << QString() << "/accounts/?cursor=secret%2Bfixture%2F%3D";
        QTest::newRow("containing-encoded-cursor") << int(Read::Positions) << QByteArray("secret+fixture/=") << QString("100") << "/positions/?cursor=prefix-secret%2Bfixture%2F%3D-suffix";
        QTest::newRow("injected-account-scope") << int(Read::Positions) << QByteArray("secret-fixture") << QString("secret-fixture") << QString();
        QTest::newRow("injected-instrument-scope") << int(Read::Instruments) << QByteArray("AAPL") << QString("AAPL") << QString();
        QTest::newRow("injected-quote-scope") << int(Read::Quotes) << QByteArray("AAPL") << QString("AAPL") << QString();
    }
    void stockReadsRejectCredentialsBeforeDispatch()
    {
        QFETCH(int, resource); QFETCH(QByteArray, token); QFETCH(QString, scope); QFETCH(QString, page);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection();
                connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                    peer->readAll(); ++requests;
                    peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                    peer->disconnectFromHost();
                });
            }
        });
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QJsonObject endpoint{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", endpoint}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        QString error;
        auto *reply = network.readStock(static_cast<RHNetworkEngine::StockRead>(resource), token,
            RHNetworkEngine::AuthKind::OAuthBearer, scope, QUrl(page, QUrl::StrictMode), &error);
        QTest::qWait(100);
        if (reply) reply->deleteLater();
        QCOMPARE(requests, 0);
        QVERIFY(!reply);
        QVERIFY(!error.isEmpty()); QVERIFY(!error.contains(QString::fromLatin1(token)));
    }

    void stockReadsArePrimaryScopedAndPaginationSafe()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const QString origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QJsonObject endpoint{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", endpoint}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        using Read = RHNetworkEngine::StockRead; using Auth = RHNetworkEngine::AuthKind;
        QVERIFY(!network.readStock(Read::Quotes, "fixture", Auth::WebSecondaryBearer, "BRK.B"));
        QVERIFY(!network.readStock(Read::Quotes, "fixture", Auth::OAuthBearer, "A B"));
        QVERIFY(!network.readStock(Read::Positions, "fixture", Auth::OAuthBearer, "100", QUrl(origin + "/positions/?account_number=200")));
        QVERIFY(!network.readStock(Read::Accounts, "fixture", Auth::OAuthBearer, {}, QUrl("https://evil.example/accounts/")));
        QVERIFY(!network.readStock(Read::Accounts, "fixture", Auth::OAuthBearer, {}, QUrl(origin + "/orders/")));
        auto *reply = network.readStock(Read::Positions, "fixture", Auth::OAuthBearer, "100", QUrl(origin + "/positions/?cursor=YX%3D"));
        QVERIFY(reply); QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection(); QTRY_VERIFY(peer->bytesAvailable());
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("GET /positions/?cursor=YX%3D&account_number=100&nonzero=true HTTP/1.1"));
        peer->write("HTTP/1.1 302 Found\r\nLocation: https://evil.example/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); peer->flush();
        QTRY_VERIFY(!finished.isEmpty()); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 302); reply->deleteLater();
    }

    void canonicalBrokerIdentitiesStayOnExplicitLoopbackTransport()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QJsonObject endpoint{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", endpoint}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        RHNetworkEngine::StockOrder order; order.accountNumber = "100"; order.symbol = "AAPL";
        order.account = QUrl("https://api.robinhood.com/accounts/a/"); order.instrument = QUrl("https://api.robinhood.com/instruments/i/");
        order.quantity = "1"; order.side = "buy"; order.price = "10.25"; order.askPrice = "10.25"; order.bidPrice = "10.24"; order.quoteTimestamp = QDateTime::currentDateTimeUtc(); order.refId = "12345678-1234-4234-8234-123456789abc";
        QVERIFY(network.stockUrl(order.account, "accounts")); QVERIFY(network.stockUrl(order.instrument, "instruments"));
        QVERIFY(!network.stockUrl(QUrl("https://api.robinhood.com:444/accounts/a/"), "accounts"));
        QVERIFY(!network.readStock(RHNetworkEngine::StockRead::Accounts, "fixture", RHNetworkEngine::AuthKind::OAuthBearer,
            {}, QUrl("https://api.robinhood.com/accounts/?cursor=x")));
        auto *reply = network.sendStockOrder(order, "fixture", RHNetworkEngine::AuthKind::OAuthBearer, nullptr, true);
        QVERIFY(reply); QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->peek(peer->bytesAvailable()).contains("trigger=immediate"));
        const auto request = peer->readAll(); QVERIFY(request.startsWith("POST /orders/?account_numbers=100 HTTP/1.1"));
        QVERIFY(request.contains("account=https%3A%2F%2Fapi.robinhood.com%2Faccounts%2Fa%2F"));
        QVERIFY(request.contains("Host: 127.0.0.1:"));
        peer->write("HTTP/1.1 302 Found\r\nLocation: https://api.robinhood.com/orders/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); peer->flush();
        QTRY_VERIFY(!finished.isEmpty()); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 302);
        reply->deleteLater();
    }

    void stockOrdersRejectInvalidTypedPlans_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"enum", "ask", "bid", "limit", "timestamp", "market-reference", "sell-limit"}) QTest::newRow(fault) << QString(fault);
    }
    void stockOrdersRejectInvalidTypedPlans()
    {
        QFETCH(QString, fault);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        const QJsonObject ep{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", ep}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        RHNetworkEngine::StockOrder order;
        order.accountNumber = "100"; order.account = QUrl(origin + "/accounts/a/");
        order.instrument = QUrl(origin + "/instruments/i/"); order.symbol = "AAPL";
        order.quantity = "1"; order.side = "buy"; order.price = "0.0101";
        order.askPrice = "0.0001"; order.bidPrice = "0.0001";
        order.buyKind = RHNetworkEngine::BuyKind::FixedLimit; order.quoteTimestamp = QDateTime::currentDateTimeUtc();
        order.refId = "12345678-1234-4234-8234-123456789abc";
        if (fault == "enum") order.buyKind = static_cast<RHNetworkEngine::BuyKind>(99);
        if (fault == "ask") order.askPrice = "0";
        if (fault == "bid") order.bidPrice = "NaN";
        if (fault == "limit") order.price = "0";
        if (fault == "timestamp") order.quoteTimestamp = {};
        if (fault == "market-reference") order.buyKind = RHNetworkEngine::BuyKind::Market;
        if (fault == "sell-limit") order.side = "sell";
        QString error; auto *reply = network.sendStockOrder(order, "fixture", RHNetworkEngine::AuthKind::OAuthBearer, &error, true);
        if (reply) { reply->abort(); reply->deleteLater(); }
        QVERIFY(!reply); QVERIFY(!error.isEmpty());
        QCoreApplication::processEvents(); QVERIFY(!server.hasPendingConnections());
    }

    void stockOrdersUseNarrowPrimaryFormTransport()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const QString origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QJsonObject endpoint{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", endpoint}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        RHNetworkEngine::StockOrder order;
        order.accountNumber = "100"; order.account = QUrl(origin + "/accounts/a/");
        order.instrument = QUrl(origin + "/instruments/i/"); order.symbol = "BRK.B";
        order.quantity = "1"; order.side = "buy"; order.price = "25.12"; order.askPrice = "25.12"; order.bidPrice = "25.11"; order.quoteTimestamp = QDateTime::fromString("2026-10-27T10:00:00.000Z", Qt::ISODateWithMs);
        order.refId = "12345678-1234-4234-8234-123456789abc";
        QString error;
        QVERIFY(!network.sendStockOrder(order, "fixture+token", RHNetworkEngine::AuthKind::WebSecondaryBearer, &error, true));
        auto foreign = order; foreign.account = QUrl("https://evil.example/accounts/a/");
        QVERIFY(!network.sendStockOrder(foreign, "fixture+token", RHNetworkEngine::AuthKind::OAuthBearer, &error, true));
        auto *reply = network.sendStockOrder(order, "fixture+token", RHNetworkEngine::AuthKind::OAuthBearer, &error, true);
        QVERIFY2(reply, qPrintable(error)); QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->peek(peer->bytesAvailable()).contains("trigger=immediate"));
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("POST /orders/?account_numbers=100 HTTP/1.1"));
        QVERIFY(request.toLower().contains("x-robinhood-api-version: 1.431.4\r\n"));
        QVERIFY(request.contains("Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\n"));
        QVERIFY(request.contains("quantity=1")); QVERIFY(request.contains("symbol=BRK.B"));
        QVERIFY(!request.contains("preset_percent_limit")); QVERIFY(request.contains("type=market"));
        QVERIFY(request.contains("price=25.12"));
        QVERIFY(request.contains("time_in_force=gfd")); QVERIFY(request.contains("extended_hours=False"));
        const QUrlQuery sentForm(QString::fromUtf8(request.mid(request.indexOf("\r\n\r\n") + 4)));
        QCOMPARE(sentForm.queryItemValue("bid_ask_timestamp", QUrl::FullyDecoded), QString("2026-10-27T10:00:00.000Z"));
        QVERIFY(!request.toLower().contains("cookie:"));
        peer->write("HTTP/1.1 201 Created\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"); peer->flush();
        QTRY_VERIFY(!finished.isEmpty()); reply->deleteLater();
    }

    void positionsPaginationKeepsEncodedCursorAndAccountScope()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QTemporaryDir dir;
        QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        QJsonObject endpoint{{"URL", QString("http://127.0.0.1:%1/positions/?nonzero=true").arg(server.serverPort())},
            {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"CachedPositions", endpoint}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        QString error;
        auto *reply = network.sendAuthenticated("CachedPositions", "fixture", RHNetworkEngine::AuthKind::WebSecondaryBearer, &error,
            QUrl("/positions/?cursor=YX%3D"), "100");
        QVERIFY2(reply, qPrintable(error)); QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection(); QTRY_VERIFY(peer->bytesAvailable());
        const auto request = peer->readAll(); QVERIFY(request.startsWith("GET /positions/?cursor=YX%3D&account_number=100&nonzero=true HTTP/1.1"));
        peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"); peer->flush();
        QTRY_VERIFY(!finished.isEmpty()); reply->deleteLater();
    }

    void scopedPositionsRejectForeignAndWrongAccountUrls()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"json({"Headers":{},"Endpoints":{"CachedPositions":{"URL":"https:\/\/api.robinhood.com/positions/?nonzero=true","Method":"GET","BodyEncoding":"none","HeaderSets":[]},"Instruments":{"URL":"https:\/\/api.robinhood.com/instruments/","Method":"GET","BodyEncoding":"none","HeaderSets":[]}}})json"); file.close();
        RHNetworkEngine network; QVERIFY(network.loadConfiguration(file.fileName()));
        const auto kind = RHNetworkEngine::AuthKind::WebSecondaryBearer;
        QString error;
        QVERIFY(!network.sendAuthenticated("CachedPositions", "fixture", kind, &error, QUrl("https://evil.example/positions/?cursor=x"), "100"));
        QVERIFY(!network.sendAuthenticated("CachedPositions", "fixture", kind, &error, QUrl("/positions/?account_number=200&cursor=x"), "100"));
        QVERIFY(!network.sendAuthenticated("Instruments", "fixture", kind, &error, QUrl("https://evil.example/instruments/abc/")));
        QVERIFY(!network.sendAuthenticated("Instruments", "fixture", kind, &error, QUrl("/orders/abc/")));
    }

    void secondaryBearerIsRestrictedAndNeverRedirected()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir;
        QFile file(dir.filePath("api.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QString::fromUtf8("{\"Headers\":{\"Auth\":{\"Cookie\":\"must-not-forward\",\"Authorization\":\"{token_type} {access_token}\"}},\"Endpoints\":{\"UserProfile\":{\"URL\":\"http://127.0.0.1:%1/user/\",\"Method\":\"GET\",\"BodyEncoding\":\"none\",\"HeaderSets\":[\"Auth\"]},\"Unrelated\":{\"URL\":\"http://127.0.0.1:%1/mutation-like-route/\",\"Method\":\"GET\",\"BodyEncoding\":\"none\",\"HeaderSets\":[]},\"Unsafe\":{\"URL\":\"https://evil.example/user/\",\"Method\":\"GET\",\"BodyEncoding\":\"none\",\"HeaderSets\":[]},\"Query\":{\"URL\":\"http://127.0.0.1:%1/user/?access_token={access_token}\",\"Method\":\"GET\",\"BodyEncoding\":\"none\",\"HeaderSets\":[]}}}").arg(server.serverPort()).toUtf8());
        file.close();
        RHNetworkEngine network(nullptr, true);
        QVERIFY(network.loadConfiguration(file.fileName()));
        QString error;
        const auto kind = RHNetworkEngine::AuthKind::WebSecondaryBearer;
        QVERIFY(!network.send("UserProfile", {{"token_type", "WebSecondaryBearer"},
            {"access_token", "secret-fixture"}}, {}, {}, &error));
        QVERIFY(!network.sendAuthenticated("Unrelated", "secret-fixture", kind, &error));
        QVERIFY(!network.sendAuthenticated("Unsafe", "secret-fixture", kind, &error));
        QVERIFY(!error.contains("secret-fixture"));
        QVERIFY(!network.sendAuthenticated("Query", "secret-fixture", kind, &error));
        QVERIFY(!network.sendAuthenticated("UserProfile", "bad\r\nvalue", kind, &error));
        QNetworkReply *reply = network.sendAuthenticated("UserProfile", "secret-fixture", kind, &error);
        QVERIFY2(reply, qPrintable(error));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY(server.hasPendingConnections());
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY(peer->bytesAvailable());
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("GET /user/ HTTP/1.1\r\n"));
        QVERIFY(request.contains(QByteArray("Authorization: Bearer ") + QByteArray("secret-fixture") + "\r\n"));
        QVERIFY(!request.toLower().contains("cookie:"));
        peer->write("HTTP/1.1 302 Found\r\nLocation: https://evil.example/\r\nSet-Cookie: unsafe=1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        peer->flush();
        QTRY_VERIFY(!finished.isEmpty());
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 302);
        reply->deleteLater();
    }

    void loadsConfigurationFromDiskAtRuntime()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("api.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"json({"Headers":{"Base":{"Accept":"application/json"}},"Endpoints":{"Health":{"URL":"http:\/\/127.0.0.1:12345/health/","Method":"GET","BodyEncoding":"none","HeaderSets":["Base"]},"TokenRefresh":{"Enabled":false}}})json");
        file.close();

        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(path, &error), qPrintable(error));
        QVERIFY(network.isConfigured());
        QVERIFY(network.send(QStringLiteral("TokenRefresh"), {}, {}, {}, &error) == nullptr);
        QVERIFY(error.contains(QStringLiteral("disabled"), Qt::CaseInsensitive));
    }

    void sendsConfiguredFormRequestToLoopback_data()
    {
        QTest::addColumn<QString>("defaultGrant");
        QTest::addColumn<bool>("overrideGrant");
        QTest::newRow("legacy-password-default") << QString("password") << false;
        QTest::newRow("validate-effective-merged-grant") << QString("refresh_token") << true;
    }
    void sendsConfiguredFormRequestToLoopback()
    {
        QFETCH(QString, defaultGrant); QFETCH(bool, overrideGrant);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("api.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"json({"Headers":{"Base":{"X-Test-Header":"configured","user-agent":"stale-configured-agent"},"Form":{"Content-Type":"application/x-www-form-urlencoded; charset=utf-8"}},"Endpoints":{"Login":{"URL":"http:\/\/127.0.0.1:%1/login/","Method":"POST","BodyEncoding":"form","HeaderSets":["Base","Form"],"BodyDefaults":{"client_id":"public-client","grant_type":"%2"}}}})json")
                       .arg(server.serverPort()).arg(defaultGrant).toUtf8());
        file.close();

        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(file.fileName(), &error), qPrintable(error));
        QVariantMap body{{QStringLiteral("username"), QStringLiteral("tester")},
            {QStringLiteral("password"), QString::fromUtf8("p+ &%=é")},
            {QStringLiteral("try_passkeys"), false}};
        if (overrideGrant) body.insert(QStringLiteral("grant_type"), QStringLiteral("password"));
        QNetworkReply *reply = network.send(QStringLiteral("Login"), {}, {}, body, &error);
        QVERIFY2(reply, qPrintable(error));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(peer->peek(peer->bytesAvailable()).contains("try_passkeys=False"), 5000);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("POST /login/ HTTP/1.1\r\n"));
        QVERIFY(request.contains("X-Test-Header: configured\r\n"));
        QCOMPARE(request.count("User-Agent: "), 1);
        QVERIFY(request.contains("User-Agent: " + expectedUserAgent() + "\r\n"));
        QVERIFY(!request.contains("stale-configured-agent"));
        QVERIFY(request.contains("Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\n"));
        QVERIFY(request.contains("username=tester"));
        QVERIFY(request.contains("password=p%2B%20%26%25%3D%C3%A9"));
        QVERIFY(request.contains("client_id=public-client"));
        QVERIFY(request.contains("grant_type=password"));
        peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
        peer->flush();
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        reply->deleteLater();
    }

    void rejectsUnsafeConfigurationWithoutRetainingOldEndpoints()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("api.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"json({"Headers":{"Base":{}},"Endpoints":{"Health":{"URL":"https:\/\/api.example.invalid/health/","Method":"GET","BodyEncoding":"none","HeaderSets":["Base"]}}})json");
        file.close();

        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(file.fileName(), &error), qPrintable(error));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(R"json({"Headers":{"Base":{}},"Endpoints":{"Health":{"URL":"http:\/\/api.example.invalid/health/","Method":"GET","BodyEncoding":"none","HeaderSets":["Base"]}}})json");
        file.close();
        QVERIFY(!network.loadConfiguration(file.fileName(), &error));
        QVERIFY(!network.isConfigured());
        QVERIFY(network.send(QStringLiteral("Health"), {}, {}, {}, &error) == nullptr);
    }

    void suppliedConfigurationLoadsWithoutOpeningBrokerageConnections()
    {
        const QString path = QFINDTESTDATA("../../applets/robin_hood/config/robinhood-api.json");
        QVERIFY(!path.isEmpty());
        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(path, &error), qPrintable(error));
        QFile config(path); QVERIFY(config.open(QIODevice::ReadOnly));
        const auto refresh = QJsonDocument::fromJson(config.readAll()).object().value("Endpoints").toObject().value("TokenRefresh").toObject();
        QVERIFY(refresh.value("Enabled").toBool());
        QCOMPARE(refresh.value("URL").toString(), QString("https://api.robinhood.com/oauth2/token/"));
        QCOMPARE(refresh.value("Method").toString(), QString("POST"));
        QCOMPARE(refresh.value("BodyEncoding").toString(), QString("form"));
        QVERIFY(network.send(QStringLiteral("TokenRefresh"), {}, {}, {}, &error) == nullptr);
        QVERIFY(error.contains(QStringLiteral("typed"), Qt::CaseInsensitive));
    }

    void genericTransportCannotBypassStockOrderValidation()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir dir; QFile file(dir.filePath("api.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        const auto endpoint = [&](QString path) { return QJsonObject{{"URL", origin + path}, {"Method", "POST"}, {"BodyEncoding", "form"}, {"HeaderSets", QJsonArray{}}}; };
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{
            {"StockOrders", endpoint("/orders/")}, {"Login", endpoint("/orders/")},
            {"Unrelated", endpoint("/mutate/")}, {"OptionOrders", endpoint("/options/orders/")}}}}).toJson()); file.close();
        RHNetworkEngine network(nullptr, true); QVERIFY(network.loadConfiguration(file.fileName()));
        QString error;
        for (const auto &name : {"StockOrders", "Login", "Unrelated", "OptionOrders"}) {
            QVERIFY(!network.send(name, {{"token_type", "Bearer"}, {"access_token", "fixture"}}, {}, {{"quantity", "1"}}, &error));
            QVERIFY(!error.isEmpty());
        }
        QTest::qWait(20); QVERIFY(!server.hasPendingConnections());
    }

    void jsonAuthenticationUsesValidatedRoutesAndRejectsEncodedPathInjection()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("api.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"json({"Headers":{"Auth":{"Authorization":"{token_type} {access_token}"},"Json":{"Content-Type":"application/json"}},"Endpoints":{"ContinueVerification":{"URL":"http:\/\/127.0.0.1:%1/inquiry/{order_id}/","Method":"POST","BodyEncoding":"json","HeaderSets":["Auth","Json"]}}})json")
                       .arg(server.serverPort()).toUtf8());
        file.close();

        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(file.fileName(), &error), qPrintable(error));
        QVariantMap values{{QStringLiteral("token_type"), QStringLiteral("Bearer")},
                           {QStringLiteral("access_token"), QStringLiteral("fake-token")},
                           {QStringLiteral("order_id"), QStringLiteral("a/b?c")}};
        const QVariantMap body{{QStringLiteral("quantity"), 1},
                               {QStringLiteral("legs"), QVariantList{QVariantMap{{QStringLiteral("side"), QStringLiteral("buy")}}}}};
        QVERIFY(!network.send("ContinueVerification", values, {}, body, &error));
        values.insert("order_id", "fixture-id");
        QNetworkReply *reply = network.send("ContinueVerification", values, {}, body, &error);
        QVERIFY2(reply, qPrintable(error));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(peer->peek(peer->bytesAvailable()).contains("\"quantity\":1"), 5000);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("POST /inquiry/fixture-id/ HTTP/1.1\r\n"));
        QVERIFY(request.contains(QByteArray("Authorization: ") + QByteArray("Bea" "rer ") + QByteArray("fake-token")));
        QVERIFY(request.contains("Content-Type: application/json\r\n"));
        const QJsonDocument sent = QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4));
        QVERIFY(sent.isObject());
        QCOMPARE(sent.object().value(QStringLiteral("quantity")).toInt(), 1);
        QCOMPARE(sent.object().value(QStringLiteral("legs")).toArray().first().toObject()
                     .value(QStringLiteral("side")).toString(), QStringLiteral("buy"));
        peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
        peer->flush();
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        reply->deleteLater();

        values.remove(QStringLiteral("access_token"));
        QVERIFY(network.send("ContinueVerification", values, {}, body, &error) == nullptr);
        QVERIFY(error.contains(QStringLiteral("header"), Qt::CaseInsensitive));
    }

    void getUsesConfiguredVerbAndQueryWithoutABody()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("api.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"json({"Headers":{"Base":{"X-Test-Header":"configured"}},"Endpoints":{"Lookup":{"URL":"http:\/\/127.0.0.1:%1/lookup/","Method":"GET","BodyEncoding":"none","HeaderSets":["Base"]}}})json")
                       .arg(server.serverPort()).toUtf8());
        file.close();

        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(file.fileName(), &error), qPrintable(error));
        QVERIFY(network.send(QStringLiteral("Lookup"), {}, {},
                             {{QStringLiteral("body"), 1}}, &error) == nullptr);
        QNetworkReply *reply = network.send(QStringLiteral("Lookup"), {},
                                            {{QStringLiteral("symbol"), QStringLiteral("A & B")}},
                                            {}, &error);
        QVERIFY2(reply, qPrintable(error));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(peer->bytesAvailable() > 0, 5000);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("GET /lookup/?symbol=A%20%26%20B HTTP/1.1\r\n"));
        QVERIFY(request.contains("X-Test-Header: configured\r\n"));
        QCOMPARE(request.count("User-Agent: "), 1);
        QVERIFY(request.contains("User-Agent: " + expectedUserAgent() + "\r\n"));
        QVERIFY(request.endsWith("\r\n\r\n"));
        peer->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
        peer->flush();
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        reply->deleteLater();
    }
};

QTEST_MAIN(RHNetworkEngineTest)
#include "test_rhnetworkengine.moc"
