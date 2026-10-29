#include "telegramwebhookhost.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QProcess>
#include <QPointer>
#include <csignal>
#include <QTimer>
#include <QtTest>

namespace {
quint16 freePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) return 0;
    return server.serverPort();
}
QByteArray tunnelPayload(quint16 port)
{
    return QJsonDocument(QJsonObject{{"tunnels", QJsonArray{QJsonObject{
        {"proto", "https"}, {"public_url", "https://offline.example.test"},
        {"config", QJsonObject{{"addr", QString("http://127.0.0.1:%1").arg(port)}}}}}}}).toJson();
}
TelegramWebhookHost::Options fakeOptions(int delay = 0)
{
    TelegramWebhookHost::Options options;
    options.port = freePort();
    options.inspectorUrl = QUrl(QString("http://127.0.0.1:%1/api/tunnels").arg(freePort()));
    options.executable = QCoreApplication::applicationFilePath();
    options.discoveryDelayMs = delay;
    options.startupTimeoutMs = 5000;
    options.argumentPrefix = {"--fake-ngrok", QString::number(options.inspectorUrl.port()),
                              QString::fromLatin1(tunnelPayload(options.port).toBase64()), "0", "0"};
    return options;
}
int requestStatus(const QUrl &url, const QByteArray &body = {}, const QByteArray &secret = {},
                  bool health = false)
{
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!secret.isEmpty()) request.setRawHeader(health ? "X-PTI-Health-Secret" : "X-Telegram-Bot-Api-Secret-Token", secret);
    auto *reply = health ? manager.get(request) : manager.post(request, body);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(3000);
    loop.exec();
    const int status = reply->isFinished() ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : -1;
    reply->abort();
    return status;
}
// Portable offline child: never invokes a shell, ngrok, or a public provider.
int fakeNgrok(QCoreApplication &app)
{
    const auto args = app.arguments();
    QTcpServer inspector;
    const auto payload = QByteArray::fromBase64(args.value(3).toLatin1());
    const auto mode = args.value(6);
    if (mode == "ignoreterm") std::signal(SIGTERM, SIG_IGN);
    int requests = 0;
    QObject::connect(&inspector, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = inspector.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, payload, mode, &requests] {
                socket->readAll();
                ++requests;
                if (mode == "hang") return;
                if (mode == "redirect") {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/redirected\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else if (mode == "error") {
                    socket->write("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    const auto body = mode == "oversized" ? QByteArray(70000, 'x')
                        : mode == "empty-first" && requests == 1 ? QByteArray("{\"tunnels\":[]}") : payload;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                }
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    QTimer::singleShot(args.value(5).toInt(), &app, [&] {
        if (!inspector.listen(QHostAddress::LocalHost, args.value(2).toUShort())) app.exit(9);
    });
    if (args.value(4).toInt() > 0) QTimer::singleShot(args.value(4).toInt(), &app, [&] { app.exit(7); });
    return app.exec();
}
}

class TestTelegramWebhookHost : public QObject
{
    Q_OBJECT
private slots:
    void executableDefaultsToEnvironmentOverride()
    {
        const bool existed = qEnvironmentVariableIsSet("NGROK_PATH");
        const auto previous = qgetenv("NGROK_PATH");
        qputenv("NGROK_PATH", QByteArray("/synthetic/offline-ngrok"));
        const auto configured = TelegramWebhookHost::Options{}.executable;
        if (existed) qputenv("NGROK_PATH", previous);
        else qunsetenv("NGROK_PATH");
        QCOMPARE(configured, QStringLiteral("/synthetic/offline-ngrok"));
    }
    void executableFallsBackToPathWhenEnvironmentUnset()
    {
        const bool existed = qEnvironmentVariableIsSet("NGROK_PATH");
        const auto previous = qgetenv("NGROK_PATH");
        qunsetenv("NGROK_PATH");
        const auto configured = TelegramWebhookHost::Options{}.executable;
        if (existed) qputenv("NGROK_PATH", previous);
        QCOMPARE(configured, QStringLiteral("ngrok"));
    }
    void rejectsUnsafeInspector_data()
    {
        QTest::addColumn<QUrl>("inspector");
        for (const auto &text : {"https://example.com/api/tunnels", "http://localhost:4040/api/tunnels",
                 "http://192.0.2.1:4040/api/tunnels", "http://user:password@127.0.0.1:4040/api/tunnels",
                 "http://127.0.0.1:4040/other", "http://127.0.0.1:4040/api/tunnels?x=1",
                 "http://127.0.0.1:4040/api/tunnels#fragment"})
            QTest::newRow(text) << QUrl(text);
    }
    void rejectsUnsafeInspector()
    {
        QFETCH(QUrl, inspector);
        TelegramWebhookHost host;
        QSignalSpy failed(&host, &TelegramWebhookHost::failed);
        TelegramWebhookHost::Options options;
        options.inspectorUrl = inspector;
        host.acquire("client", options);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().first().toString(), QString("client"));
        QVERIFY(host.healthSecret().isEmpty());
    }
    void rejectsUnsafeTunnelIdentity_data()
    {
        QTest::addColumn<QString>("publicUrl");
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("proto");
        QTest::addColumn<bool>("duplicate");
        QTest::newRow("http") << "http://offline.example.test" << "valid" << "https" << false;
        QTest::newRow("userinfo") << "https://name:password@offline.example.test" << "valid" << "https" << false;
        QTest::newRow("query") << "https://offline.example.test/?secret=x" << "valid" << "https" << false;
        QTest::newRow("fragment") << "https://offline.example.test/#x" << "valid" << "https" << false;
        QTest::newRow("path") << "https://offline.example.test/path" << "valid" << "https" << false;
        QTest::newRow("protocol") << "https://offline.example.test" << "valid" << "http" << false;
        QTest::newRow("foreign-upstream") << "https://offline.example.test" << "http://192.0.2.10:8080" << "https" << false;
        QTest::newRow("upstream-userinfo") << "https://offline.example.test" << "userinfo" << "https" << false;
        QTest::newRow("upstream-query") << "https://offline.example.test" << "query" << "https" << false;
        QTest::newRow("upstream-path") << "https://offline.example.test" << "path" << "https" << false;
        QTest::newRow("upstream-scheme") << "https://offline.example.test" << "scheme" << "https" << false;
        QTest::newRow("ambiguous") << "https://offline.example.test" << "valid" << "https" << true;
    }
    void rejectsUnsafeTunnelIdentity()
    {
        QFETCH(QString, publicUrl);
        QFETCH(QString, address);
        QFETCH(QString, proto);
        QFETCH(bool, duplicate);
        TelegramWebhookHost host;
        auto options = fakeOptions();
        QString upstream = QString("http://127.0.0.1:%1").arg(options.port);
        if (address == "userinfo") upstream = QString("http://user@127.0.0.1:%1").arg(options.port);
        else if (address == "query") upstream += "?x=1";
        else if (address == "path") upstream += "/foreign";
        else if (address == "scheme") upstream.replace("http:", "ftp:");
        else if (address != "valid") upstream = address;
        QJsonObject tunnel{{"proto", proto}, {"public_url", publicUrl}, {"config", QJsonObject{{"addr", upstream}}}};
        QJsonArray tunnels{tunnel};
        if (duplicate) tunnels.append(tunnel);
        options.argumentPrefix[2] = QString::fromLatin1(QJsonDocument(QJsonObject{{"tunnels", tunnels}}).toJson().toBase64());
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), failed(&host, &TelegramWebhookHost::failed);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000);
        QVERIFY(ready.isEmpty());
        QVERIFY(!failed.first().at(1).toString().contains("password"));
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
    }
    void receiverRejectsOversizedDeclaredBodyBeforeReadingIt()
    {
        TelegramWebhookHost host;
        const auto options = fakeOptions();
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, options.port);
        QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
        socket.write("POST " + ready.first().at(1).toUrl().path().toLatin1()
            + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: 300000\r\n\r\n");
        QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1500);
        QVERIFY(socket.readAll().startsWith("HTTP/1.1 413"));
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
    }
    void receiverHasAbsoluteIncompleteRequestDeadline()
    {
        TelegramWebhookHost host;
        const auto options = fakeOptions();
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, options.port);
        QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
        socket.write("POST /telegram/incomplete HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\n");
        QTimer drip;
        connect(&drip, &QTimer::timeout, &socket, [&] { socket.write(" "); });
        drip.start(100);
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::UnconnectedState, 4000);
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
    }
    void inspectorFailuresAreBounded_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QByteArray>("payload");
        QTest::newRow("redirect") << "redirect" << QByteArray();
        QTest::newRow("http-error") << "error" << QByteArray();
        QTest::newRow("hung") << "hang" << QByteArray();
        QTest::newRow("malformed") << "normal" << QByteArray("not-json");
        QTest::newRow("oversized") << "oversized" << QByteArray();
    }
    void inspectorFailuresAreBounded()
    {
        QFETCH(QString, mode);
        QFETCH(QByteArray, payload);
        TelegramWebhookHost host;
        auto options = fakeOptions();
        options.argumentPrefix << mode;
        if (!payload.isEmpty()) options.argumentPrefix[2] = QString::fromLatin1(payload.toBase64());
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), failed(&host, &TelegramWebhookHost::failed);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2200);
        QVERIFY(ready.isEmpty());
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
    }
    void sharedProcessRejectsMismatchedOptionsAndWaitsForLastClient()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        options.argumentPrefix << "ignoreterm";
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), failed(&host, &TelegramWebhookHost::failed);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QPointer<QProcess> child = host.findChild<QProcess *>();
        QVERIFY(child);
        const auto pid = child->processId();
        QCOMPARE(child->arguments().sliced(child->arguments().size() - 2),
            QStringList({"http", QString("http://127.0.0.1:%1").arg(options.port)}));
        host.acquire("b", options);
        QCOMPARE(ready.size(), 2);
        QCOMPARE(host.findChildren<QProcess *>().size(), 1);
        QCOMPARE(child->processId(), pid);
        auto different = options; different.port = freePort();
        host.acquire("wrong-port", different);
        different = options; different.executable = "other";
        host.acquire("wrong-executable", different);
        different = options; different.argumentPrefix << "other";
        host.acquire("wrong-arguments", different);
        different = options; different.inspectorUrl.setPort(freePort());
        host.acquire("wrong-inspector", different);
        QCOMPARE(failed.size(), 4);
        host.release("a");
        QCOMPARE(released.size(), 1);
        QCOMPARE(child->state(), QProcess::Running);
        bool finishedBeforeRelease = false;
        connect(&host, &TelegramWebhookHost::released, &host, [&](const QString &id) {
            if (id == "b") finishedBeforeRelease = !child || child->state() == QProcess::NotRunning;
        });
        QElapsedTimer elapsed; elapsed.start();
        host.release("b");
        QCOMPARE(released.size(), 1);
        host.acquire("retry", options);
        QCOMPARE(failed.size(), 5);
        QVERIFY(failed.last().at(1).toString().contains("retry later"));
        QTRY_COMPARE_WITH_TIMEOUT(released.size(), 2, 3000);
        QVERIFY(elapsed.elapsed() >= 900);
        QVERIFY(finishedBeforeRelease);
        QTcpServer rebound;
        QVERIFY(rebound.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
    }
    void refusesPreexistingInspectorWithoutStartingProcess()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        QTcpServer foreign;
        QVERIFY(foreign.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), ready(&host, &TelegramWebhookHost::ready);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QVERIFY(ready.isEmpty());
        QVERIFY(host.findChildren<QProcess *>().isEmpty());
        QVERIFY(foreign.isListening());
    }
    void retriesRefusedAndEmptyInspectorUntilReady()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        options.argumentPrefix[4] = "300";
        options.argumentPrefix << "empty-first";
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), failed(&host, &TelegramWebhookHost::failed);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QVERIFY(failed.isEmpty());
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
    }
    void missingExecutableFailsWithoutLeakingResources()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        options.executable = QStringLiteral("/no-such-directory/pti-fake-ngrok");
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000);
        QVERIFY(host.healthSecret().isEmpty());
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
        QTcpServer rebound;
        QVERIFY(rebound.listen(QHostAddress::LocalHost, options.port));
    }
    void unexpectedProcessExitFailsEveryClient()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        options.argumentPrefix[3] = "800";
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        host.acquire("b", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 2, 3000);
        QVERIFY(host.healthSecret().isEmpty());
        QTcpServer rebound;
        QVERIFY(rebound.listen(QHostAddress::LocalHost, options.port));
    }
    void startupDeadlineStopsAnEmptyInspector()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        options.startupTimeoutMs = 650;
        options.argumentPrefix[2] = QString::fromLatin1(QByteArray("{\"tunnels\":[]}").toBase64());
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), ready(&host, &TelegramWebhookHost::ready);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1500);
        QVERIFY(ready.isEmpty());
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
    }
    void destructionIsNonblockingAndReapsOnlyOwnedChild()
    {
        auto *host = new TelegramWebhookHost;
        auto options = fakeOptions(); options.argumentPrefix << "ignoreterm";
        QSignalSpy ready(host, &TelegramWebhookHost::ready);
        host->acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QPointer<QProcess> child = host->findChild<QProcess *>();
        QVERIFY(child);
        QElapsedTimer elapsed; elapsed.start();
        delete host;
        QVERIFY(elapsed.elapsed() < 250);
        QTRY_VERIFY_WITH_TIMEOUT(!child, 3000);
        QTcpServer rebound;
        QVERIFY(rebound.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
    }
    void deletingHostFromUpdateObserverIsSafe()
    {
        QPointer<TelegramWebhookHost> host = new TelegramWebhookHost;
        const auto options = fakeOptions();
        QSignalSpy ready(host, &TelegramWebhookHost::ready);
        host->acquire("a", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QPointer<QProcess> child = host->findChild<QProcess *>();
        auto url = ready.first().at(1).toUrl();
        url.setScheme("http"); url.setHost("127.0.0.1"); url.setPort(options.port);
        const auto secret = ready.first().at(2).toByteArray();
        connect(host, &TelegramWebhookHost::updateReceived, this, [&] { delete host.data(); });
        requestStatus(url, "{\"update_id\":1}", secret);
        QVERIFY(!host);
        QTRY_VERIFY_WITH_TIMEOUT(!child, 3000);
    }
    void protectedRoutesAreIsolatedAndHealthIsPrivate()
    {
        TelegramWebhookHost host;
        const auto options = fakeOptions();
        QSignalSpy ready(&host, &TelegramWebhookHost::ready);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        QSignalSpy updates(&host, &TelegramWebhookHost::updateReceived);
        host.acquire("a", options);
        host.acquire("b", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 5000);
        auto localUrl = [&](int index) {
            auto url = ready.at(index).at(1).toUrl();
            url.setScheme("http"); url.setHost("127.0.0.1"); url.setPort(options.port);
            return url;
        };
        const auto a = localUrl(0), b = localUrl(1);
        const auto secretA = ready.at(0).at(2).toByteArray(), secretB = ready.at(1).at(2).toByteArray();
        QVERIFY(a.path() != b.path());
        QVERIFY(secretA != secretB);
        const QByteArray update = "{\"update_id\":42,\"message\":{}}";
        QCOMPARE(requestStatus(a, update), 404);
        QCOMPARE(requestStatus(a, update, secretB), 404);
        QCOMPARE(requestStatus(b, update, secretA), 404);
        QVERIFY(updates.isEmpty());
        QCOMPARE(requestStatus(a, update, secretA), 200);
        QCOMPARE(updates.size(), 1);
        QCOMPARE(updates.first().at(0).toString(), QString("a"));
        QCOMPARE(updates.first().at(1).toJsonObject().value("update_id").toInt(), 42);
        QCOMPARE(requestStatus(b, update, secretB), 200);
        QCOMPARE(updates.size(), 2);
        for (const QByteArray bad : {QByteArray("oops"), QByteArray("[]"), QByteArray("{}"),
             QByteArray("{\"update_id\":0}"), QByteArray("{\"update_id\":1.5}"),
             QByteArray("{\"update_id\":\"42\"}"), QByteArray("{\"update_id\":4503599627370496}")})
            QCOMPARE(requestStatus(a, bad, secretA), 400);
        auto unknown = a; unknown.setPath("/telegram/unknown");
        QCOMPARE(requestStatus(unknown, update, secretA), 404);
        auto health = a; health.setPath("/health");
        QCOMPARE(requestStatus(health, {}, {}, true), 404);
        QCOMPARE(requestStatus(health, {}, secretA, true), 404);
        QCOMPARE(requestStatus(health, {}, host.healthSecret(), true), 200);
        QVERIFY(host.ownsHealthUrl(health));
        auto unsafeHealth = health; unsafeHealth.setHost("example.test");
        QVERIFY(!host.ownsHealthUrl(unsafeHealth));
        unsafeHealth = health; unsafeHealth.setQuery("x=1");
        QVERIFY(!host.ownsHealthUrl(unsafeHealth));
        host.release("a");
        QCOMPARE(released.size(), 1);
        QCOMPARE(requestStatus(a, update, secretA), 404);
        QCOMPARE(requestStatus(b, update, secretB), 200);
        host.release("b");
        QTRY_COMPARE(released.size(), 2);
    }
    void discoversAfterRealTwoSecondDelayAndReleases()
    {
        TelegramWebhookHost host;
        const auto options = fakeOptions(2000);
        QSignalSpy ready(&host, &TelegramWebhookHost::ready);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        QSignalSpy failed(&host, &TelegramWebhookHost::failed);
        QElapsedTimer elapsed;
        elapsed.start();
        host.acquire("first", options);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QVERIFY(elapsed.elapsed() >= 1950);
        QVERIFY(failed.isEmpty());
        const auto url = ready.first().at(1).toUrl();
        QCOMPARE(url.host(), QString("offline.example.test"));
        QVERIFY(url.path().startsWith("/telegram/"));
        QVERIFY(ready.first().at(2).toString().size() >= 32);
        QVERIFY(host.healthSecret().size() >= 32);
        QVERIFY(host.ownsHealthUrl(QUrl(QString("http://localhost:%1/health").arg(options.port))));
        host.release("first");
        QTRY_COMPARE(released.size(), 1);
        QVERIFY(host.healthSecret().isEmpty());
        QTcpServer rebound;
        QVERIFY(rebound.listen(QHostAddress::LocalHost, options.port));
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--fake-ngrok")) return fakeNgrok(app);
    TestTelegramWebhookHost test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_telegramwebhookhost.moc"
