#include "telegramwebhookhost.h"
#include "telegraminspectorprocess.h"
#include <cstdio>
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QCoreApplication>
#include <QCryptographicHash>
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
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QUuid>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
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
    static QTemporaryDir ownership;
    TelegramWebhookHost::Options options;
    options.ownershipDirectory = ownership.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
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
// Cleanup is scoped to this test's exact child image; never a real ngrok PID.
struct OrphanFixture {
    QTemporaryDir directory;
    qint64 pid = 0;
#ifdef Q_OS_MACOS
    quint64 birthSeconds = 0, birthMicros = 0;
#elif defined(Q_OS_WIN)
    HANDLE process = nullptr;
#endif
    void trackPid(qint64 value) {
        pid = value;
#ifdef Q_OS_MACOS
        proc_bsdinfo info{};
        if (pid > 1 && proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info)) {
            birthSeconds = info.pbi_start_tvsec; birthMicros = info.pbi_start_tvusec;
        }
#elif defined(Q_OS_WIN)
        if (pid > 0) process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid));
#endif
    }
    QString readyPath() const { return directory.filePath("listening"); }
    QProcessEnvironment environment() const {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("PTI_FAKE_NGROK_READY", readyPath());
        return env;
    }
    ~OrphanFixture()
    {
#ifdef Q_OS_MACOS
        char image[PROC_PIDPATHINFO_MAXSIZE] = {};
        proc_bsdinfo info{};
        if (pid > 1 && birthSeconds && proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info)
            && info.pbi_start_tvsec == birthSeconds && info.pbi_start_tvusec == birthMicros
            && proc_pidpath(int(pid), image, sizeof(image)) > 0
            && QFileInfo(QString::fromLocal8Bit(image)).canonicalFilePath()
                == QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath())
            ::kill(pid_t(pid), SIGKILL);
#elif defined(Q_OS_WIN)
        if (process) {
            wchar_t image[32768] = {}; DWORD size = 32768;
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && QueryFullProcessImageNameW(process, 0, image, &size)
                && QFileInfo(QString::fromWCharArray(image, int(size))).canonicalFilePath().compare(
                    QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath(), Qt::CaseInsensitive) == 0)
                TerminateProcess(process, 1);
            CloseHandle(process);
        }
#endif
    }
};
bool inspectorConnects(quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    return socket.waitForConnected(50);
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
        if (!inspector.listen(mode == "wildcard" ? QHostAddress::AnyIPv4 : QHostAddress::LocalHost,
                args.value(2).toUShort())) { app.exit(9); return; }
        const auto readyPath = qEnvironmentVariable("PTI_FAKE_NGROK_READY");
        if (!readyPath.isEmpty()) {
            QFile ready(readyPath);
            if (!ready.open(QIODevice::WriteOnly)) { app.exit(10); return; }
            ready.write("listening");
        }
    });
    if (args.value(4).toInt() > 0) QTimer::singleShot(args.value(4).toInt(), &app, [&] { app.exit(7); });
    return app.exec();
}
}

class TestTelegramWebhookHost : public QObject
{
    Q_OBJECT
private slots:
    void synchronousAdmissionControlsAcknowledgement_data()
    {
        QTest::addColumn<bool>("accepted");
        QTest::newRow("admitted-or-ignored") << true;
        QTest::newRow("retryable-rejection") << false;
    }
    void synchronousAdmissionControlsAcknowledgement()
    {
        QFETCH(bool, accepted);
        TelegramWebhookHost host;
        const auto options = fakeOptions();
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), released(&host, &TelegramWebhookHost::released);
        QSignalSpy updates(&host, &TelegramWebhookHost::updateReceived);
        int admissions = 0;
        qint64 receivedId = 0;
        host.acquire("a", options, [&](const QJsonObject &update) {
            ++admissions;
            receivedId = update.value("update_id").toInteger();
            return accepted;
        });
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        auto url = ready.first()[1].toUrl();
        url.setScheme("http"); url.setHost("127.0.0.1"); url.setPort(options.port);
        QCOMPARE(requestStatus(url, "{\"update_id\":1}", ready.first()[2].toByteArray()), accepted ? 200 : 503);
        QCOMPARE(admissions, 1);
        QCOMPARE(receivedId, 1);
        QCOMPARE(updates.size(), accepted ? 1 : 0);
        host.release("a"); QTRY_COMPARE(released.size(), 1);
    }
    void deletingHostFromAdmissionCallbackIsSafe()
    {
        QPointer<TelegramWebhookHost> host = new TelegramWebhookHost;
        const auto options = fakeOptions();
        QSignalSpy ready(host, &TelegramWebhookHost::ready);
        host->acquire("a", options, [&](const QJsonObject &) { delete host.data(); return false; });
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
        QPointer<QProcess> child = host->findChild<QProcess *>();
        auto url = ready.first()[1].toUrl();
        url.setScheme("http"); url.setHost("127.0.0.1"); url.setPort(options.port);
        requestStatus(url, "{\"update_id\":1}", ready.first()[2].toByteArray());
        QVERIFY(!host);
        QTRY_VERIFY_WITH_TIMEOUT(!child, 3000);
    }
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
    void waitsForInspectorDecisionWithoutStartingProcess()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        QTcpServer foreign;
        QVERIFY(foreign.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), ready(&host, &TelegramWebhookHost::ready);
        options.startupTimeoutMs = 100;
        host.acquire("a", options);
        QTest::qWait(250);
        QVERIFY2(failed.isEmpty(), "Occupied inspector must await an explicit decision, not fail or time out");
        QVERIFY(ready.isEmpty());
        QVERIFY(host.findChildren<QProcess *>().isEmpty());
        QVERIFY(foreign.isListening());
        host.release("a");
    }
    void restartRefusesArbitraryListener()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        QTcpServer foreign;
        QVERIFY(foreign.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict);
        QSignalSpy failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        QTRY_COMPARE(conflict.size(), 1);
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Restart);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 2000);
        QVERIFY(foreign.isListening());
        QVERIFY(host.findChildren<QProcess *>().isEmpty());
    }
    void privateReceiptIsInstalledAndScopedCleanupPreservesOtherGenerations()
    {
        auto options = fakeOptions();
        TelegramWebhookHost host;
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options, {}, QString(64, 's'));
        QTRY_COMPARE(ready.size(), 1);
        const auto path = QDir(options.ownershipDirectory).filePath(
            QString("inspector-%1.json").arg(options.inspectorUrl.port()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        const auto object = QJsonDocument::fromJson(original).object();
        QCOMPARE(object.value("schema").toInt(), 1);
        QCOMPARE(object.value("pid").toString().toLongLong(), host.findChild<QProcess *>()->processId());
        QCOMPARE(object.value("creatorPid").toString().toLongLong(), QCoreApplication::applicationPid());
        QVERIFY(!object.contains("arguments"));
        QVERIFY(!original.contains(options.argumentPrefix[2].toUtf8())); // no provider-shaped fixture body
        QVERIFY(!original.contains(QByteArray(64, 's')));
#ifdef Q_OS_MACOS
        QCOMPARE(QFileInfo(options.ownershipDirectory).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther), QFileDevice::Permissions{});
        QCOMPARE(QFileInfo(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther), QFileDevice::Permissions{});
#endif
        // Even a recorded direct child is not an orphan while its creator lives.
        auto args = options.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        QVERIFY(!TelegramInspectorProcess::findOrphan(options.executable, args,
            quint16(options.inspectorUrl.port()), options.ownershipDirectory));
        const auto replacement = original + " ";
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(replacement), replacement.size()); file.close();
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), replacement); file.close();
        // A later explicit cleanup for the old generation cannot erase it either.
        TelegramInspectorProcess::forgetReceipt(quint16(options.inspectorUrl.port()), options.ownershipDirectory, original);
        QVERIFY(QFileInfo::exists(path));
    }
    void stopRemovesOnlyItsReceiptBeforeReleaseNotification()
    {
        auto options = fakeOptions();
        TelegramWebhookHost host;
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_COMPARE(ready.size(), 1);
        const auto path = QDir(options.ownershipDirectory).filePath(QString("inspector-%1.json").arg(options.inspectorUrl.port()));
        QVERIFY(QFileInfo::exists(path));
        bool removedAtRelease = false;
        connect(&host, &TelegramWebhookHost::released, &host, [&] { removedAtRelease = !QFileInfo::exists(path); });
        host.release("a");
        QTRY_COMPARE(released.size(), 1);
        QVERIFY(removedAtRelease);
        host.acquire("again", options);
        QTRY_COMPARE(ready.size(), 2);
        host.release("again");
        QTRY_COMPARE(released.size(), 2);
    }
    void exitedOwnedChildReceiptDoesNotBlockFreshStart()
    {
        auto options = fakeOptions();
        auto arguments = options.argumentPrefix;
        arguments << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        const auto port = quint16(options.inspectorUrl.port());
        QProcess old;
        old.start(options.executable, arguments);
        QTRY_COMPARE(old.state(), QProcess::Running);
        const auto previous = TelegramInspectorProcess::recordStarted(old, port, options.ownershipDirectory);
        old.kill();
        QTRY_COMPARE(old.state(), QProcess::NotRunning);
        QVERIFY(!previous.isEmpty());
        QProcess fresh;
        fresh.start(options.executable, arguments);
        QTRY_COMPARE(fresh.state(), QProcess::Running);
        const auto current = TelegramInspectorProcess::recordStarted(fresh, port, options.ownershipDirectory);
        fresh.kill();
        QTRY_COMPARE(fresh.state(), QProcess::NotRunning);
        QVERIFY(!current.isEmpty());
        QVERIFY(current != previous);
        TelegramInspectorProcess::forgetReceipt(port, options.ownershipDirectory, previous);
        QFile record(QDir(options.ownershipDirectory).filePath(QString("inspector-%1.json").arg(port)));
        QVERIFY(record.open(QIODevice::ReadOnly));
        QCOMPARE(record.readAll(), current);
        record.close();
        TelegramInspectorProcess::forgetReceipt(port, options.ownershipDirectory, current);
        QVERIFY(!record.exists());
    }
    void receiptCommitFailureStopsUntrackedChild()
    {
        auto options = fakeOptions();
        QTemporaryDir directory;
        options.ownershipDirectory = directory.filePath("not-a-directory");
        QFile block(options.ownershipDirectory);
        QVERIFY(block.open(QIODevice::WriteOnly)); block.write("blocked"); block.close();
        TelegramWebhookHost host;
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), ready(&host, &TelegramWebhookHost::ready);
        host.acquire("a", options);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(ready.isEmpty());
        QTRY_VERIFY(host.findChildren<QProcess *>().isEmpty());
        QTcpServer rebound;
        QVERIFY(rebound.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
    }
    void receiptMismatchRefusesRecovery_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"corrupt", "oversized", "schema", "pid", "birth", "birthFraction",
                 "creatorPid", "creatorBirth", "creatorBirthFraction", "user", "executable", "argvSha256", "port", "unknown", "missing", "cancel"})
            QTest::newRow(field) << QString::fromLatin1(field);
#ifdef Q_OS_MACOS
        QTest::newRow("public-file") << "public-file";
        QTest::newRow("symlink-file") << "symlink-file";
#endif
    }
    void receiptMismatchRefusesRecovery()
    {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        QSKIP("Native ownership proof is desktop-only");
#endif
        QFETCH(QString, field);
        auto options = fakeOptions();
        auto args = options.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        OrphanFixture orphan;
        QProcess launcher;
        launcher.setProcessEnvironment(orphan.environment());
        launcher.start(options.executable, QStringList{"--spawn-recorded-orphan", options.ownershipDirectory,
            QString::number(options.inspectorUrl.port())} + args);
        QTRY_COMPARE(launcher.state(), QProcess::NotRunning);
        QCOMPARE(launcher.exitCode(), 0);
        orphan.trackPid(launcher.readAllStandardOutput().trimmed().toLongLong());
        QVERIFY(orphan.pid > 1);
        QTRY_VERIFY(QFileInfo::exists(orphan.readyPath()));
        auto proof = TelegramInspectorProcess::findOrphan(options.executable, args,
            quint16(options.inspectorUrl.port()), options.ownershipDirectory);
        QVERIFY(proof);
        const auto path = QDir(options.ownershipDirectory).filePath(
            QString("inspector-%1.json").arg(options.inspectorUrl.port()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        auto object = QJsonDocument::fromJson(original).object();
        if (field == "missing") object.remove("birth");
        else if (field == "schema" || field == "port") object[field] = object[field].toInt() + 1;
        else if (field == "birth" || field == "birthFraction" || field == "creatorBirthFraction")
            object[field] = QString::number(object[field].toString().toULongLong() + 1);
        else if (field == "creatorBirth") object[field] = QString::number(object["birth"].toString().toULongLong() + 1);
        else if (field == "creatorPid" || field == "pid") object[field] = QString::number(QCoreApplication::applicationPid());
        else if (field != "cancel" && field != "public-file" && field != "symlink-file") object[field] = "mismatch";
        // Exercise native/schema matching independently of checksum rejection.
        // The sub-tick creator change separately tests accidental record corruption.
        if (field != "cancel" && field != "creatorBirthFraction") {
            object.remove("receiptSha256");
            object.insert("receiptSha256", QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        }
        auto bytes = field == "cancel" ? original : field == "corrupt" ? QByteArray("broken")
            : field == "oversized" ? QByteArray(9000, 'x') : QJsonDocument(object).toJson(QJsonDocument::Compact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), bytes.size()); file.close();
        if (field == "public-file")
            QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
        if (field == "symlink-file") {
            const auto target = orphan.directory.filePath("receipt-target.json");
            QVERIFY(QFile::copy(path, target));
            QVERIFY(QFile::remove(path));
            QVERIFY(QFile::link(target, path));
        }
        if (field != "cancel") {
            QVERIFY(!TelegramInspectorProcess::findOrphan(options.executable, args,
                quint16(options.inspectorUrl.port()), options.ownershipDirectory));
            QVERIFY(!proof->terminate(true)); // Proof retained before corruption must revalidate receipt too.
        }
        TelegramWebhookHost host;
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict), failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        QTRY_COMPARE(conflict.size(), 1);
        host.resolveInspectorConflict("a", field == "cancel" ? TelegramWebhookHost::InspectorConflictAction::Cancel
            : TelegramWebhookHost::InspectorConflictAction::Restart);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(inspectorConnects(quint16(options.inspectorUrl.port())));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
    void staleReceiptNeverAuthorizesReplacementListener()
    {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        QSKIP("Native ownership proof is desktop-only");
#endif
        auto options = fakeOptions();
        auto args = options.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        OrphanFixture orphan;
        QProcess launcher;
        launcher.setProcessEnvironment(orphan.environment());
        launcher.start(options.executable, QStringList{"--spawn-recorded-orphan", options.ownershipDirectory,
            QString::number(options.inspectorUrl.port())} + args);
        QTRY_COMPARE(launcher.state(), QProcess::NotRunning);
        QCOMPARE(launcher.exitCode(), 0);
        orphan.trackPid(launcher.readAllStandardOutput().trimmed().toLongLong());
        auto proof = TelegramInspectorProcess::findOrphan(options.executable, args,
            quint16(options.inspectorUrl.port()), options.ownershipDirectory);
        QVERIFY(proof);
        QVERIFY(proof->terminate(true));
        QTRY_VERIFY(!proof->alive());
        QVERIFY(QFile::remove(orphan.readyPath()));
        QProcess replacement;
        replacement.setProcessEnvironment(orphan.environment());
        replacement.start(options.executable, args);
        QTRY_COMPARE(replacement.state(), QProcess::Running);
        QTRY_VERIFY(QFileInfo::exists(orphan.readyPath()));
        QVERIFY(!TelegramInspectorProcess::findOrphan(options.executable, args,
            quint16(options.inspectorUrl.port()), options.ownershipDirectory));
        QVERIFY(!proof->terminate(true));
        TelegramWebhookHost host;
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict), failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        QTRY_COMPARE(conflict.size(), 1);
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Restart);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(replacement.state(), QProcess::Running);
        replacement.kill();
        QTRY_COMPARE(replacement.state(), QProcess::NotRunning);
    }
    void receiptStorageFailureIsClosed_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("full") << "full";
        QTest::newRow("existing-record") << "existing-record";
#ifdef Q_OS_MACOS
        QTest::newRow("public-directory") << "public-directory";
        QTest::newRow("symlink-directory") << "symlink-directory";
#endif
    }
    void receiptStorageFailureIsClosed()
    {
        QFETCH(QString, mode);
        auto options = fakeOptions();
        QTemporaryDir root;
        QVERIFY(root.isValid());
        options.ownershipDirectory = root.path();
        if (mode == "public-directory")
            QVERIFY(QFile::setPermissions(root.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner | QFileDevice::ReadOther | QFileDevice::ExeOther));
        else if (mode == "symlink-directory") {
            options.ownershipDirectory = root.filePath("link");
            QVERIFY(QFile::link(root.path(), options.ownershipDirectory));
        } else {
            for (int i = 0; i < (mode == "full" ? 64 : 1); ++i) {
                QFile record(root.filePath(QString("inspector-%1.json").arg(mode == "full" ? i : options.inspectorUrl.port())));
                QVERIFY(record.open(QIODevice::WriteOnly)); record.write("existing");
            }
        }
        TelegramWebhookHost host;
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), ready(&host, &TelegramWebhookHost::ready);
        host.acquire("a", options);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(ready.isEmpty());
        QTRY_VERIFY(host.findChildren<QProcess *>().isEmpty());
        if (mode == "full" || mode == "existing-record") {
            const auto files = QDir(root.path()).entryList({"inspector-*.json"}, QDir::Files);
            QCOMPARE(files.size(), mode == "full" ? 64 : 1);
            for (const auto &name : files) {
                QFile file(root.filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly));
                QCOMPARE(file.readAll(), QByteArray("existing"));
            }
        }
    }
    void unrecordedMatchingOrphanIsNeverOwned()
    {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        QSKIP("Native ownership proof is desktop-only");
#endif
        auto options = fakeOptions();
        auto args = options.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        OrphanFixture orphan;
        QProcess launcher;
        launcher.setProcessEnvironment(orphan.environment());
        launcher.start(options.executable, QStringList{"--spawn-fake-orphan"} + args);
        QTRY_COMPARE(launcher.state(), QProcess::NotRunning);
        QCOMPARE(launcher.exitCode(), 0);
        orphan.trackPid(launcher.readAllStandardOutput().trimmed().toLongLong());
        QVERIFY(orphan.pid > 1);
        QTRY_VERIFY(QFileInfo::exists(orphan.readyPath()));
        QVERIFY2(!TelegramInspectorProcess::findOrphan(options.executable, args,
                     quint16(options.inspectorUrl.port()), options.ownershipDirectory),
            "Same-user, exact argv and reparenting cannot establish app ownership");
        TelegramWebhookHost host;
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict), failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        QTRY_COMPARE(conflict.size(), 1);
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Restart);
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(inspectorConnects(quint16(options.inspectorUrl.port())));
    }
    void confirmRestartsOnlyMatchingOrphan_data()
    {
        QTest::addColumn<bool>("ignoreTerm");
        QTest::newRow("graceful") << false;
        QTest::newRow("bounded-force") << true;
    }
    void confirmRestartsOnlyMatchingOrphan()
    {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        QSKIP("Native orphan recovery is only supported on desktop macOS and Windows");
#endif
        QFETCH(bool, ignoreTerm);
        auto options = fakeOptions();
        if (ignoreTerm) options.argumentPrefix << "ignoreterm";
        QStringList args = options.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        OrphanFixture orphan;
        QProcess launcher;
        QVERIFY(orphan.directory.isValid());
        launcher.setProcessEnvironment(orphan.environment());
        launcher.start(options.executable, QStringList{"--spawn-recorded-orphan", options.ownershipDirectory,
            QString::number(options.inspectorUrl.port())} + args);
        QTRY_COMPARE(launcher.state(), QProcess::NotRunning);
        QCOMPARE(launcher.exitCode(), 0);
        orphan.trackPid(launcher.readAllStandardOutput().trimmed().toLongLong());
        QVERIFY(orphan.pid > 1);
        QTRY_VERIFY(QFileInfo::exists(orphan.readyPath()));
        QVERIFY(inspectorConnects(quint16(options.inspectorUrl.port())));
        auto proof = TelegramInspectorProcess::findOrphan(options.executable, args, quint16(options.inspectorUrl.port()), options.ownershipDirectory);
        QVERIFY2(proof, "Detached fake child must be verifiable as an orphan listener");
        auto wrongArgs = args; wrongArgs.last() = "http://127.0.0.1:1";
        QVERIFY(!TelegramInspectorProcess::findOrphan(options.executable, wrongArgs, quint16(options.inspectorUrl.port()), options.ownershipDirectory));
        QVERIFY(!TelegramInspectorProcess::findOrphan("/nonexistent/ngrok", args, quint16(options.inspectorUrl.port()), options.ownershipDirectory));
        QVERIFY(!TelegramInspectorProcess::findOrphan(options.executable, args, freePort(), options.ownershipDirectory));
        TelegramWebhookHost host;
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict);
        QSignalSpy ready(&host, &TelegramWebhookHost::ready), failed(&host, &TelegramWebhookHost::failed);
        QSignalSpy released(&host, &TelegramWebhookHost::released);
        const QString persistedSecret(64, 'a');
        host.acquire("a", options, {}, persistedSecret);
        host.acquire("b", options);
        QTRY_COMPARE(conflict.size(), 2);
        QVERIFY(proof->alive());
        QVERIFY(ready.isEmpty());
        QElapsedTimer elapsed; elapsed.start();
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Restart);
        QVERIFY(elapsed.elapsed() < 250);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 4000);
        QVERIFY2(failed.isEmpty(), "Confirmed matching orphan should be replaced by a fresh managed child");
        QVERIFY(!proof->alive());
        // Original proof cannot act on the new process holding the same port.
        QVERIFY(!proof->terminate(true));
        auto *managed = host.findChild<QProcess *>();
        QVERIFY(managed && managed->processId() != orphan.pid);
        QCOMPARE(ready.first()[2].toString(), persistedSecret);
        QVERIFY(ready.first()[1].toUrl().path() != ready.last()[1].toUrl().path());
        if (ignoreTerm) QVERIFY(elapsed.elapsed() >= 900);
        host.release("a");
        QCOMPARE(managed->state(), QProcess::Running);
        host.release("b");
        QTRY_COMPARE_WITH_TIMEOUT(released.size(), 2, 3000);
    }
    void refusesMismatchedOrCancelledOrphan_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"target", "executable", "wildcard", "cancel", "stop-during-restart"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void refusesMismatchedOrCancelledOrphan()
    {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
        QSKIP("Native orphan recovery is only supported on desktop macOS and Windows");
#endif
        QFETCH(QString, mode);
        auto original = fakeOptions();
        original.argumentPrefix << (mode == "wildcard" ? "wildcard" : "ignoreterm");
        auto args = original.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(original.port);
        OrphanFixture orphan;
        QProcess launcher;
        QVERIFY(orphan.directory.isValid());
        launcher.setProcessEnvironment(orphan.environment());
        launcher.start(original.executable, QStringList{"--spawn-recorded-orphan", original.ownershipDirectory,
            QString::number(original.inspectorUrl.port())} + args);
        QTRY_COMPARE(launcher.state(), QProcess::NotRunning);
        QCOMPARE(launcher.exitCode(), 0);
        orphan.trackPid(launcher.readAllStandardOutput().trimmed().toLongLong());
        QVERIFY(orphan.pid > 1);
        // TCP connect alone can report a transient pre-listen success on this
        // platform. The fixture publishes readiness only after listen succeeds.
        QTRY_VERIFY(QFileInfo::exists(orphan.readyPath()));
        QVERIFY(inspectorConnects(quint16(original.inspectorUrl.port())));
        auto options = original;
        if (mode == "target") options.port = freePort();
        if (mode == "executable") options.executable = "/no-such-directory/ngrok";
        TelegramWebhookHost host;
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict);
        QSignalSpy failed(&host, &TelegramWebhookHost::failed), released(&host, &TelegramWebhookHost::released);
        host.acquire("a", options);
        QTRY_VERIFY2_WITH_TIMEOUT(conflict.size() == 1,
            qPrintable(QStringLiteral("conflict absent; failures=%1 reason=%2 inspector-connects=%3")
                .arg(failed.size()).arg(failed.isEmpty() ? QString{} : failed.first()[1].toString())
                .arg(inspectorConnects(quint16(original.inspectorUrl.port())))), 2000);
        host.resolveInspectorConflict("a", mode == "cancel"
            ? TelegramWebhookHost::InspectorConflictAction::Cancel : TelegramWebhookHost::InspectorConflictAction::Restart);
        if (mode == "stop-during-restart") {
            host.release("a");
            QTRY_COMPARE(released.size(), 1);
            QTest::qWait(1200); // A retired restart must never escalate later.
            QVERIFY(failed.isEmpty());
        } else {
            QTRY_COMPARE(failed.size(), 1);
        }
        QVERIFY(host.findChildren<QProcess *>().isEmpty());
        QVERIFY(inspectorConnects(quint16(original.inspectorUrl.port())));
    }
    void deletingHostFromConflictObserverIsSafe()
    {
        auto options = fakeOptions();
        QTcpServer foreign;
        QVERIFY(foreign.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
        QPointer<TelegramWebhookHost> host = new TelegramWebhookHost;
        connect(host, &TelegramWebhookHost::inspectorConflict, this, [&] { delete host.data(); });
        host->acquire("a", options);
        host->acquire("b", options);
        QTRY_VERIFY(!host);
        QVERIFY(foreign.isListening());
    }
    void restartRefusesActiveMatchingChild()
    {
        auto options = fakeOptions();
        auto args = options.argumentPrefix;
        args << "http" << QString("http://127.0.0.1:%1").arg(options.port);
        OrphanFixture readiness;
        QVERIFY(readiness.directory.isValid());
        QProcess active;
        active.setProcessEnvironment(readiness.environment());
        active.start(options.executable, args);
        QTRY_COMPARE(active.state(), QProcess::Running);
        QTRY_VERIFY(QFileInfo::exists(readiness.readyPath()));
        QVERIFY(inspectorConnects(quint16(options.inspectorUrl.port())));
        TelegramWebhookHost host;
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict), failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        QTRY_COMPARE(conflict.size(), 1);
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Restart);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(active.state(), QProcess::Running);
        active.terminate();
        QTRY_COMPARE(active.state(), QProcess::NotRunning);
    }
    void cancelAbandonsOnlyItsPendingClient()
    {
        TelegramWebhookHost host;
        auto options = fakeOptions();
        QTcpServer foreign;
        QVERIFY(foreign.listen(QHostAddress::LocalHost, quint16(options.inspectorUrl.port())));
        QSignalSpy conflict(&host, &TelegramWebhookHost::inspectorConflict);
        QSignalSpy failed(&host, &TelegramWebhookHost::failed);
        host.acquire("a", options);
        QTRY_COMPARE(conflict.size(), 1);
        host.acquire("b", options);
        QCOMPARE(conflict.size(), 2);
        host.resolveInspectorConflict("unknown", TelegramWebhookHost::InspectorConflictAction::Cancel);
        QVERIFY(failed.isEmpty());
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Cancel);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first()[0].toString(), QString("a"));
        host.resolveInspectorConflict("a", TelegramWebhookHost::InspectorConflictAction::Restart);
        QCOMPARE(failed.size(), 1);
        QVERIFY(foreign.isListening());
        host.resolveInspectorConflict("b", TelegramWebhookHost::InspectorConflictAction::Cancel);
        QCOMPARE(failed.size(), 2);
        QVERIFY(foreign.isListening());
        QVERIFY(host.findChildren<QProcess *>().isEmpty());
        // A subsequent start remains possible, and must ask again.
        host.acquire("c", options);
        QTRY_COMPARE(conflict.size(), 3);
        host.release("c");
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
        QCOMPARE(requestStatus(a, update), 403);
        QCOMPARE(requestStatus(a, update, secretB), 403);
        QCOMPARE(requestStatus(b, update, secretA), 403);
        QVERIFY(updates.isEmpty());
        QCOMPARE(requestStatus(a, update, secretA), 200);
        QCOMPARE(updates.size(), 1);
        QCOMPARE(updates.first().at(0).toString(), QString("a"));
        QCOMPARE(updates.first().at(1).toJsonObject().value("update_id").toInt(), 42);
        QCOMPARE(requestStatus(b, update, secretB), 200);
        QCOMPARE(updates.size(), 2);
        for (const QByteArray &bad : {QByteArray("oops"), QByteArray("[]"), QByteArray("{}"),
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
    QStandardPaths::setTestModeEnabled(true);
    if (app.arguments().value(1) == "--spawn-recorded-orphan") {
        QProcess child;
        child.setProgram(app.applicationFilePath());
        child.setArguments(app.arguments().sliced(4));
        child.start();
        if (!child.waitForStarted(3000)) return 11;
        const auto receipt = TelegramInspectorProcess::recordStarted(child,
            app.arguments().value(3).toUShort(), app.arguments().value(2));
        if (receipt.isEmpty()) return 12;
        QElapsedTimer timer; timer.start();
        while (!QFileInfo::exists(qEnvironmentVariable("PTI_FAKE_NGROK_READY")) && timer.elapsed() < 3000)
            QThread::msleep(5);
        if (timer.elapsed() >= 3000) return 13;
        std::printf("%lld\n", static_cast<long long>(child.processId()));
        std::fflush(stdout);
        std::_Exit(0); // Simulate creator crash: no QProcess destructor/receipt cleanup.
    }
    if (app.arguments().value(1) == "--spawn-fake-orphan") {
        QProcess child;
        child.setProgram(app.applicationFilePath());
        child.setArguments(app.arguments().sliced(2));
        qint64 pid = 0;
        if (!child.startDetached(&pid)) return 8;
        std::printf("%lld\n", static_cast<long long>(pid));
        return 0;
    }
    if (app.arguments().contains("--fake-ngrok")) return fakeNgrok(app);
    TestTelegramWebhookHost test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_telegramwebhookhost.moc"
