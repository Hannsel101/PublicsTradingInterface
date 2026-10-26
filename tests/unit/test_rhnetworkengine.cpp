#include "rhnetworkengine.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
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

    void sendsConfiguredFormRequestToLoopback()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("api.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"json({"Headers":{"Base":{"X-Test-Header":"configured","user-agent":"stale-configured-agent"},"Form":{"Content-Type":"application/x-www-form-urlencoded; charset=utf-8"}},"Endpoints":{"Login":{"URL":"http:\/\/127.0.0.1:%1/login/","Method":"POST","BodyEncoding":"form","HeaderSets":["Base","Form"],"BodyDefaults":{"client_id":"public-client","grant_type":"password"}}}})json")
                       .arg(server.serverPort()).toUtf8());
        file.close();

        RHNetworkEngine network;
        QString error;
        QVERIFY2(network.loadConfiguration(file.fileName(), &error), qPrintable(error));
        QNetworkReply *reply = network.send(QStringLiteral("Login"), {}, {},
                                            {{QStringLiteral("username"), QStringLiteral("tester")},
                                             {QStringLiteral("password"), QString::fromUtf8("p+ &%=é")},
                                             {QStringLiteral("try_passkeys"), false}}, &error);
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
        QVERIFY(network.send(QStringLiteral("TokenRefresh"), {}, {}, {}, &error) == nullptr);
        QVERIFY(error.contains(QStringLiteral("disabled"), Qt::CaseInsensitive));
    }

    void jsonOrdersUseConfiguredHeadersAndEscapedRouteParameters()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("api.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral(R"json({"Headers":{"Auth":{"Authorization":"{token_type} {access_token}"},"Json":{"Content-Type":"application/json"}},"Endpoints":{"Order":{"URL":"http:\/\/127.0.0.1:%1/orders/{order_id}/","Method":"POST","BodyEncoding":"json","HeaderSets":["Auth","Json"]}}})json")
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
        QNetworkReply *reply = network.send(QStringLiteral("Order"), values,
                                            {{QStringLiteral("account_numbers"), QStringLiteral("A&B")}},
                                            body, &error);
        QVERIFY2(reply, qPrintable(error));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
        QTcpSocket *peer = server.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(peer->peek(peer->bytesAvailable()).contains("\"quantity\":1"), 5000);
        const QByteArray request = peer->readAll();
        QVERIFY(request.startsWith("POST /orders/a%2Fb%3Fc/?account_numbers=A%26B HTTP/1.1\r\n"));
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
        QVERIFY(network.send(QStringLiteral("Order"), values, {}, body, &error) == nullptr);
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
