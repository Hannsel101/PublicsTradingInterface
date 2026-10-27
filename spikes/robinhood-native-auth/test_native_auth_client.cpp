#include "native_auth_client.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QPointer>

namespace {
struct Request { QByteArray method, path, headers, body; };
struct Response { int status = 200; QByteArray body; QByteArray extraHeaders; bool hold = false; bool drip = false; int claimedExtra = 0; };
class Fixture : public QObject {
public:
    QTcpServer server;
    QList<Request> requests;
    QList<Response> responses;
    Fixture() {
        if (!server.listen(QHostAddress::LocalHost)) qFatal("Cannot bind loopback fixture");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto headers = bytes->left(end);
                    qsizetype length = 0;
                    for (auto line : headers.split('\n'))
                        if (line.trimmed().toLower().startsWith("content-length:")) length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    if (bytes->size() < end + 4 + length) return;
                    const auto first = headers.split('\n').first().trimmed().split(' ');
                    requests.append({first.value(0), first.value(1), headers, bytes->mid(end + 4, length)});
                    socket->disconnect(this);
                    const auto response = responses.isEmpty() ? Response{500, "{}", {}, false} : responses.takeFirst();
                    if (response.hold) return;
                    if (response.drip) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100000\r\n\r\n");
                        auto *timer = new QTimer(socket);
                        connect(timer, &QTimer::timeout, socket, [socket] { socket->write(" "); socket->flush(); });
                        connect(socket, &QTcpSocket::disconnected, timer, &QTimer::stop);
                        timer->start(10); return;
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.body.size() + response.claimedExtra) + "\r\n" + response.extraHeaders + "\r\n" + response.body);
                    socket->disconnectFromHost();
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                });
            }
        });
    }
    NativeAuthClient::TestTransport transport(int timeout = 2000, int whole = 10000) const {
        return {QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())), timeout, whole, 1};
    }
    void json(const QJsonObject &object, int status = 200) { responses.append({status, QJsonDocument(object).toJson(QJsonDocument::Compact), {}, false}); }
};
QJsonObject fullGrant() {
    return {{"access_token", "access.synthetic"}, {"refresh_token", "refresh.synthetic"}, {"scope", "internal"},
            {"token_type", "Bearer"}, {"expires_in", 321}, {"read_only_secondary_token", "secondary.synthetic"}};
}
QMap<QString, QString> form(const QByteArray &body) {
    QMap<QString, QString> result;
    for (auto field : body.split('&')) {
        const auto equals = field.indexOf('=');
        auto name = field.left(equals); auto value = field.mid(equals + 1);
        name.replace('+', ' '); value.replace('+', ' ');
        result[QUrl::fromPercentEncoding(name)] = QUrl::fromPercentEncoding(value);
    }
    return result;
}
}

class TestNativeAuthClient : public QObject {
    Q_OBJECT
private slots:
    void referenceRequestHeaders_data() {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("accept") << QByteArray("accept") << QByteArray("*/*");
        QTest::newRow("language") << QByteArray("accept-language") << QByteArray("en-US,en;q=1");
        QTest::newRow("api-version") << QByteArray("x-robinhood-api-version") << QByteArray("1.431.4");
        QTest::newRow("user-agent") << QByteArray("user-agent") << QByteArray("*");
        QTest::newRow("form-type") << QByteArray("form") << QByteArray("application/x-www-form-urlencoded; charset=utf-8");
        QTest::newRow("json-type") << QByteArray("json") << QByteArray("application/json");
        QTest::newRow("get-no-type") << QByteArray("get") << QByteArray();
    }
    void referenceRequestHeaders() {
        QFETCH(QByteArray, name); QFETCH(QByteArray, expected);
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        fixture.json({{"id", "m"}});
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{
            {"type", "sms"}, {"status", "validated"}, {"id", "c"}}}}}});
        fixture.json({{"type_context", QJsonObject{{"result", "workflow_status_approved"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("fixture-user", "fixture-password", "fixture-device"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), NativeAuthClient::State::Succeeded, 3000);
        QCOMPARE(fixture.requests.size(), 5);
        int checked = 0;
        for (const auto &request : fixture.requests) {
            QByteArray key = name;
            if (name == "form" || name == "json" || name == "get") {
                if (name == "get" && request.method != "GET") continue;
                if (name == "form" && (request.method != "POST" || request.path != "/oauth2/token/")) continue;
                if (name == "json" && (request.method != "POST" || request.path == "/oauth2/token/")) continue;
                key = "content-type";
            }
            QByteArray actual;
            for (const auto &line : request.headers.split('\n')) {
                const auto colon = line.indexOf(':');
                if (colon >= 0 && line.first(colon).trimmed().toLower() == key)
                    actual = line.mid(colon + 1).trimmed();
            }
            QCOMPARE(actual, expected);
            ++checked;
        }
        QVERIFY(checked > 0);
    }
    void rejectedGrantDiagnostics_data() {
        QTest::addColumn<QJsonObject>("body");
        QTest::addColumn<QString>("expected");
        QTest::newRow("credentials-rejected")
            << QJsonObject{{"error", "invalid_grant"}, {"detail", "Unable to log in with provided credentials."}}
            << QString("error=invalid_grant; reason=credentials_rejected");
        QTest::newRow("required-client-field")
            << QJsonObject{{"error", "invalid_request"}, {"client_id", QJsonArray{"This field is required."}}}
            << QString("fields=client_id");
        QTest::newRow("scope-code-no-description-echo")
            << QJsonObject{{"error", "invalid_scope"}, {"error_description", "password-sensitive access.synthetic"}}
            << QString("error=invalid_scope; reason=unclassified");
        QTest::newRow("unknown-values-never-reflected")
            << QJsonObject{{"error", "password-sensitive"}, {"detail", "user-sensitive@example.test 654321 device-sensitive"},
                {"password", "password-sensitive"}, {"access_token", "access.synthetic"}, {"refresh_token", "refresh.synthetic"},
                {"untrusted-sensitive-key", "untrusted-sensitive-value"}}
            << QString("error=unrecognized; reason=unclassified");
        QTest::newRow("malformed-error-types")
            << QJsonObject{{"error", QJsonObject{{"secret", "password-sensitive"}}}, {"detail", QJsonArray{"access.synthetic"}}}
            << QString("error=unrecognized; reason=unclassified");
        QTest::newRow("known-list-message")
            << QJsonObject{{"non_field_errors", QJsonArray{"Unable to log in with provided credentials."}}}
            << QString("reason=credentials_rejected");
        QTest::newRow("recognized-field-name-only")
            << QJsonObject{{"gr_token", QJsonArray{"password-sensitive"}}}
            << QString("fields=gr_token");
        QTest::newRow("unknown-detail-not-printed")
            << QJsonObject{{"detail", "untrusted-sensitive-support-message"}}
            << QString("reason=unclassified");
    }
    void rejectedGrantDiagnostics() {
        QFETCH(QJsonObject, body); QFETCH(QString, expected);
        Fixture fixture; fixture.json(body, 400);
        NativeAuthClient client(fixture.transport());
        QStringList statuses; int grants = 0;
        connect(&client, &NativeAuthClient::changed, &client, [&] { statuses += client.statusText(); });
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("user-sensitive@example.test", "password-sensitive", "device-sensitive", "654321"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), NativeAuthClient::State::Failed, 3000);
        QVERIFY2(client.statusText().contains(expected), qPrintable(client.statusText()));
        QVERIFY(client.statusText().contains("HTTP 400"));
        for (const auto &status : statuses) {
            QVERIFY(!status.contains("sensitive"));
            QVERIFY(!status.contains("synthetic"));
            QVERIFY(!status.contains("654321"));
        }
        QCOMPARE(grants, 0); QCOMPARE(fixture.requests.size(), 1);
    }
    void truncatedWorkflowResponseDoesNotContinue() {
        Fixture fixture;
        const auto body = QJsonDocument(QJsonObject{{"verification_workflow", QJsonObject{{"id", "w"}}}}).toJson(QJsonDocument::Compact);
        fixture.responses.append({400, body, {}, false, false, 10});
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("u", "p", "d"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), NativeAuthClient::State::Failed, 3000);
        QCOMPARE(fixture.requests.size(), 1);
    }
    void absoluteDeadlineSurvivesContinuousProgress() {
        Fixture fixture; fixture.responses.append({200, {}, {}, false, true});
        NativeAuthClient client(fixture.transport(500, 3000));
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(client.start("u", "p", "d"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), NativeAuthClient::State::Failed, 2000);
        QVERIFY(elapsed.elapsed() < 1800);
        QVERIFY(client.statusText().contains("Request timed out"));
        QCOMPARE(fixture.requests.size(), 1);
    }
    void nonapprovalNeverRetriesLogin_data() {
        QTest::addColumn<QString>("phase");
        QTest::newRow("inquiry") << QString("inquiry");
        QTest::newRow("push") << QString("push");
        QTest::newRow("continuation") << QString("continuation");
    }
    void nonapprovalNeverRetriesLogin() {
        QFETCH(QString, phase);
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        fixture.json({{"id", "m"}});
        if (phase != "inquiry") fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", phase == "push" ? "prompt" : "sms"}, {"status", phase == "push" ? "issued" : "validated"}, {"id", "c"}}}}}});
        for (int i = 0; i < 100; ++i)
            fixture.json(phase == "push" ? QJsonObject{{"challenge_status", "pending"}}
                : phase == "continuation" ? QJsonObject{{"verification_workflow", QJsonObject{{"workflow_status", "workflow_status_internal_pending"}}}} : QJsonObject{});
        NativeAuthClient client(fixture.transport(2000, 2200)); int grants = 0;
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("u", "p", "d"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), NativeAuthClient::State::Failed, 4000);
        QCOMPARE(grants, 0);
        // A request dispatched before abort can reach the server after the terminal notification.
        // Drain that in-flight delivery before asserting that no new polls are scheduled.
        QTest::qWait(300);
        int tokenPosts = 0;
        for (const auto &request : fixture.requests) if (request.path == "/oauth2/token/") ++tokenPosts;
        QCOMPARE(tokenPosts, 1);
        const auto count = fixture.requests.size(); QTest::qWait(30); QCOMPARE(fixture.requests.size(), count);
    }
    void legacyMfaAlongsideWorkflowIsUnsupported() {
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}, {"mfa_required", true}}, 400);
        fixture.json({{"id", "m"}});
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("u", "p", "d"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 1500);
        QCOMPARE(client.state(), NativeAuthClient::State::Failed);
        QCOMPARE(fixture.requests.size(), 1);
    }
    void rejectsBadTransportAndInput() {
        Fixture fixture;
        auto config = fixture.transport(); config.origin.setPath("/unexpected");
        NativeAuthClient invalid(config);
        QVERIFY(!invalid.start("u", "p", "d"));
        QCOMPARE(invalid.state(), NativeAuthClient::State::Failed);
        NativeAuthClient client(fixture.transport());
        QVERIFY(!client.start({}, "p", "d"));
        QVERIFY(!client.start("u", {}, "d"));
        QVERIFY(!client.start("u", "p", {}));
        QVERIFY(!client.start("u", "p", "d", "abc"));
        client.submitCode("123456"); client.cancel();
        QTest::qWait(20); QCOMPARE(fixture.requests.size(), 0);
    }
    void invalidResponses_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<int>("http");
        QTest::newRow("malformed") << QByteArray("not json password-secret access.synthetic") << 200;
        QTest::newRow("array") << QByteArray("[]") << 200;
        QTest::newRow("oversized") << QByteArray(1024 * 1024 + 1, 'x') << 200;
        QTest::newRow("secret-error") << QByteArray("{\"error\":\"password-secret\",\"detail\":\"access.synthetic\",\"message\":\"refresh.synthetic\"}") << 401;
        QTest::newRow("http-failure-valid-grant") << QJsonDocument(fullGrant()).toJson() << 500;
    }
    void invalidResponses() {
        QFETCH(QByteArray, body); QFETCH(int, http);
        Fixture fixture; fixture.responses.append({http, body, {}, false});
        NativeAuthClient client(fixture.transport()); QStringList statuses;
        connect(&client, &NativeAuthClient::changed, &client, [&] { statuses += client.statusText(); });
        int grants = 0; client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("u", "password-secret", "d"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QCOMPARE(client.state(), NativeAuthClient::State::Failed); QCOMPARE(grants, 0);
        for (const auto &status : statuses) {
            QVERIFY(!status.contains("password-secret")); QVERIFY(!status.contains("synthetic"));
            QVERIFY(!status.contains(body));
        }
        QCOMPARE(fixture.requests.size(), 1);
    }
    void redirectNeverFollowedAndAmbientCookieNotReused() {
        Fixture fixture;
        fixture.responses.append({302, "{}", "Location: /forbidden/\r\nSet-Cookie: session=secret; Path=/\r\n", false});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("u", "p", "d"));
        QTRY_COMPARE(client.state(), NativeAuthClient::State::Failed);
        QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(client.start("u", "p", "d"));
        QTRY_COMPARE(client.state(), NativeAuthClient::State::Succeeded);
        QCOMPARE(fixture.requests.size(), 2);
        QVERIFY(!fixture.requests[1].headers.toLower().contains("cookie:"));
        QVERIFY(!fixture.requests[1].headers.toLower().contains("authorization:"));
        QCOMPARE(fixture.requests[1].path, "/oauth2/token/");
    }
    void emailIssuedCodeValidation_data() {
        QTest::addColumn<QString>("status");
        QTest::newRow("validated") << QString("validated");
        QTest::newRow("wrong-code") << QString("issued");
        QTest::newRow("rejected") << QString("rejected");
    }
    void emailIssuedCodeValidation() {
        QFETCH(QString, status);
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        fixture.json({{"id", "m"}});
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "email"}, {"status", "issued"}, {"id", "c"}}}}}});
        fixture.json({{"status", status}});
        fixture.json({{"type_context", QJsonObject{{"result", "workflow_status_approved"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("u", "p", "d"));
        client.submitCode("123456"); // Ignored outside WaitingForCode.
        QTRY_COMPARE(client.state(), NativeAuthClient::State::WaitingForCode);
        client.submitCode(QString(11, '1')); client.submitCode("１２３４５６");
        QCOMPARE(fixture.requests.size(), 3);
        client.submitCode("123456"); client.submitCode("654321");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QCOMPARE(client.state(), status == "validated" ? NativeAuthClient::State::Succeeded : NativeAuthClient::State::Failed);
        QCOMPARE(fixture.requests.size(), status == "validated" ? 6 : 4);
    }
    void workflowFailure_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto &name : {"unsafe-workflow-id", "unsafe-machine-id", "unknown-challenge", "unsafe-challenge-id", "push-denied", "continuation-denied", "ambiguous-challenge-post", "second-workflow"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void workflowFailure() {
        QFETCH(QString, scenario);
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", scenario == "unsafe-workflow-id" ? "../x" : "w"}}}}, 400);
        fixture.json({{"id", scenario == "unsafe-machine-id" ? "../x" : "m"}});
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", scenario == "unknown-challenge" ? "captcha" : scenario == "push-denied" ? "prompt" : "sms"}, {"status", "issued"}, {"id", scenario == "unsafe-challenge-id" ? "../x" : "c"}}}}}});
        if (scenario == "ambiguous-challenge-post") fixture.responses.append({200, {}, {}, true});
        else if (scenario == "push-denied") fixture.json({{"challenge_status", "denied"}});
        else fixture.json({{"status", "validated"}});
        fixture.json({{"type_context", QJsonObject{{"result", scenario == "continuation-denied" ? "workflow_status_denied" : "workflow_status_approved"}}}});
        fixture.json({{"verification_workflow", QJsonObject{{"id", "new-workflow"}}}}, 400);
        NativeAuthClient client(fixture.transport(1200)); int grants = 0;
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        connect(&client, &NativeAuthClient::changed, &client, [&] {
            if (client.state() == NativeAuthClient::State::WaitingForCode) client.submitCode("123456");
        });
        QVERIFY(client.start("u", "p", "d"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 7000);
        QCOMPARE(client.state(), NativeAuthClient::State::Failed); QCOMPARE(grants, 0);
        const auto count = fixture.requests.size(); QTest::qWait(30); QCOMPARE(fixture.requests.size(), count);
        if (scenario == "ambiguous-challenge-post") QCOMPARE(count, 4);
        if (scenario == "second-workflow") QCOMPARE(count, 6);
    }
    void deadlinesIncludeWaitingForCode() {
        Fixture fixture;
        fixture.responses.append({200, {}, {}, true});
        NativeAuthClient client(fixture.transport(100, 1000));
        QVERIFY(client.start("u", "p", "d"));
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), NativeAuthClient::State::Failed, 2000);
        QVERIFY(client.statusText().contains("timed out"));
        Fixture codes;
        codes.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        codes.json({{"id", "m"}});
        codes.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "sms"}, {"status", "issued"}, {"id", "c"}}}}}});
        NativeAuthClient waiting(codes.transport(2000, 3000));
        QVERIFY(waiting.start("u", "p", "d"));
        QTRY_COMPARE(waiting.state(), NativeAuthClient::State::WaitingForCode);
        QTRY_COMPARE_WITH_TIMEOUT(waiting.state(), NativeAuthClient::State::Failed, 4000);
        waiting.submitCode("123456"); QTest::qWait(20); QCOMPARE(codes.requests.size(), 3);
    }
    void cancelReentrancyAndStaleCompletion() {
        Fixture fixture; fixture.responses.append({200, {}, {}, true}); fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport()); int grants = 0;
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("old", "old-password", "old-device"));
        QTRY_COMPARE(fixture.requests.size(), 1);
        bool replacement = false;
        connect(&client, &NativeAuthClient::changed, &client, [&] {
            if (client.state() == NativeAuthClient::State::Cancelled && !replacement) {
                replacement = true; QVERIFY(client.start("new", "new-password", "new-device"));
            }
        });
        client.cancel(); client.submitCode("123456");
        QTRY_COMPARE(client.state(), NativeAuthClient::State::Succeeded);
        QCOMPARE(grants, 1); QCOMPARE(fixture.requests.size(), 2);
        QCOMPARE(form(fixture.requests[1].body).value("device_token"), "new-device");
        QTest::qWait(50); QCOMPARE(client.state(), NativeAuthClient::State::Succeeded);
        client.cancel(); QCOMPARE(client.state(), NativeAuthClient::State::Succeeded);
    }
    void directNotificationCanDestroyClient_data() {
        QTest::addColumn<int>("phase");
        QTest::newRow("requesting") << 0; QTest::newRow("success") << 1; QTest::newRow("failed") << 2; QTest::newRow("cancelled") << 3;
        QTest::newRow("approval") << 4; QTest::newRow("code") << 5;
    }
    void directNotificationCanDestroyClient() {
        QFETCH(int, phase);
        Fixture fixture;
        if (phase >= 4) {
            fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
            fixture.json({{"id", "m"}});
            fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "sms"}, {"status", "issued"}, {"id", "c"}}}}}});
        } else fixture.json(phase == 2 ? QJsonObject{} : fullGrant());
        QPointer<NativeAuthClient> client = new NativeAuthClient(fixture.transport());
        int grants = 0; client->setGrantHandler([&](RHOAuthGrant) { ++grants; });
        connect(client, &NativeAuthClient::changed, &fixture, [&] {
            if (!client) return;
            const auto state = client->state();
            if ((phase == 0 && state == NativeAuthClient::State::Requesting)
                || (phase == 1 && state == NativeAuthClient::State::Succeeded)
                || (phase == 2 && state == NativeAuthClient::State::Failed)
                || (phase == 3 && state == NativeAuthClient::State::Cancelled)
                || (phase == 4 && state == NativeAuthClient::State::WaitingForApproval)
                || (phase == 5 && state == NativeAuthClient::State::WaitingForCode)) delete client.data();
        });
        QVERIFY(client->start("u", "p", "d"));
        if (phase == 3) client->cancel();
        QTRY_VERIFY(!client);
        QTest::qWait(20); QCOMPARE(grants, 0);
        if (phase == 0 || phase == 3) QCOMPARE(fixture.requests.size(), 0);
    }
    void grantHandlerCanDestroyClient() {
        Fixture fixture; fixture.json(fullGrant());
        QPointer<NativeAuthClient> client = new NativeAuthClient(fixture.transport());
        int grants = 0;
        client->setGrantHandler([&](RHOAuthGrant) { ++grants; delete client.data(); });
        QVERIFY(client->start("u", "p", "d"));
        QTRY_VERIFY(!client); QCOMPARE(grants, 1);
    }
    void rejectsNonprimaryGrant_data() {
        QTest::addColumn<QJsonObject>("body");
        auto value = fullGrant(); value["read_only"] = true;
        QTest::newRow("read-only-main") << value;
        value = fullGrant(); value["scope"] = "read"; QTest::newRow("secondary-scope") << value;
        value = fullGrant(); value.remove("refresh_token"); QTest::newRow("missing-refresh") << value;
        value = fullGrant(); value.remove("access_token"); QTest::newRow("secondary-only") << value;
        value = fullGrant(); value["expires_in"] = "86400"; QTest::newRow("string-expiry") << value;
        value = fullGrant(); value["expires_in"] = 0; QTest::newRow("zero-expiry") << value;
        value = fullGrant(); value["client_id"] = "wrong"; QTest::newRow("wrong-client") << value;
        value = fullGrant(); value["device_token"] = "wrong"; QTest::newRow("wrong-device") << value;
        value = fullGrant(); value["mfa_required"] = true; QTest::newRow("mfa-required") << value;
        value = {{"mfa_required", true}, {"mfa_type", "totp"}}; QTest::newRow("no-unapproved-mfa-retry") << value;
    }
    void rejectsNonprimaryGrant() {
        QFETCH(QJsonObject, body);
        Fixture fixture; fixture.json(body);
        NativeAuthClient client(fixture.transport()); int grants = 0;
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("u", "password-secret", "device-1"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QCOMPARE(client.state(), NativeAuthClient::State::Failed); QCOMPARE(grants, 0);
        QTest::qWait(10); QCOMPARE(fixture.requests.size(), 1);
    }
    void prevalidatedEmailRequiresWorkflowApproval() {
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        fixture.json({{"id", "m"}});
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "email"}, {"status", "validated"}, {"id", "c"}}}}}});
        fixture.json({{"type_context", QJsonObject{{"result", "workflow_status_approved"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("u", "p", "d"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 10000);
        QVERIFY2(client.state() == NativeAuthClient::State::Succeeded, qPrintable(client.statusText()));
        QCOMPARE(fixture.requests.size(), 5);
    }
    void invalidPromptStatusStopsBeforePush_data() {
        QTest::addColumn<QJsonValue>("status");
        QTest::newRow("denied") << QJsonValue("denied");
        QTest::newRow("unknown") << QJsonValue("unknown");
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("nonstring") << QJsonValue(true);
    }
    void invalidPromptStatusStopsBeforePush() {
        QFETCH(QJsonValue, status);
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        fixture.json({{"id", "m"}});
        QJsonObject challenge{{"type", "prompt"}, {"id", "c"}};
        if (!status.isUndefined()) challenge.insert("status", status);
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", challenge}}}});
        // Later approval must never override an invalid inquiry status.
        fixture.json({{"challenge_status", "validated"}});
        fixture.json({{"type_context", QJsonObject{{"result", "workflow_status_approved"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport()); int grants = 0;
        QStringList statuses;
        connect(&client, &NativeAuthClient::changed, &client, [&] { statuses += client.statusText(); });
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("u", "p", "d"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3000);
        QCOMPARE(client.state(), NativeAuthClient::State::Failed);
        QTest::qWait(30);
        int pushes = 0, tokenPosts = 0;
        for (const auto &request : fixture.requests) {
            if (request.path.startsWith("/push/")) ++pushes;
            if (request.path == "/oauth2/token/" && request.method == "POST") ++tokenPosts;
        }
        QCOMPARE(pushes, 0);
        QCOMPARE(tokenPosts, 1); // Initial login only: zero retries.
        QCOMPARE(grants, 0);
        QCOMPARE(fixture.requests.size(), 3);
        QVERIFY(!statuses.contains("Approve verification in provider app"));
        QCOMPARE(client.state(), NativeAuthClient::State::Failed);
    }
    void allowlistedPromptStatusRequiresFinalApproval_data() {
        QTest::addColumn<QString>("status");
        QTest::addColumn<bool>("approved");
        for (const auto &status : {"issued", "pending", "validated"}) {
            QTest::newRow(qPrintable(QString(status) + "-approved")) << QString(status) << true;
            QTest::newRow(qPrintable(QString(status) + "-denied")) << QString(status) << false;
        }
    }
    void allowlistedPromptStatusRequiresFinalApproval() {
        QFETCH(QString, status); QFETCH(bool, approved);
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "w"}}}}, 400);
        fixture.json({{"id", "m"}});
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "prompt"}, {"status", status}, {"id", "c"}}}}}});
        fixture.json({{"challenge_status", "validated"}});
        fixture.json({{"type_context", QJsonObject{{"result", approved ? "workflow_status_approved" : "workflow_status_denied"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport()); int grants = 0;
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("u", "p", "d"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3000);
        QCOMPARE(client.state(), approved ? NativeAuthClient::State::Succeeded : NativeAuthClient::State::Failed);
        QCOMPARE(grants, approved ? 1 : 0);
        QCOMPARE(fixture.requests.size(), approved ? 6 : 5);
        QCOMPARE(fixture.requests[3].path, "/push/c/get_prompts_status/");
        QCOMPARE(fixture.requests[4].method, "POST");
        QCOMPARE(fixture.requests[4].path, "/pathfinder/inquiries/m/user_view/");
    }
    void pushWorkflowPollsUntilValidated() {
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "workflow-1"}}}}, 400);
        fixture.json({{"id", "machine-1"}});
        fixture.json({}); // Empty inquiry is not approval.
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "prompt"}, {"status", "issued"}, {"id", "push-1"}}}}}});
        fixture.json({{"challenge_status", "issued"}});
        fixture.json({{"challenge_status", "validated"}});
        fixture.json({{"verification_workflow", QJsonObject{{"workflow_status", "workflow_status_approved"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport());
        QVERIFY(client.start("user", "password-secret", "device-1"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 10000);
        QVERIFY2(client.state() == NativeAuthClient::State::Succeeded, qPrintable(client.statusText()));
        QCOMPARE(fixture.requests.size(), 8);
        QCOMPARE(fixture.requests[4].method, "GET");
        QCOMPARE(fixture.requests[4].path, "/push/push-1/get_prompts_status/");
        QCOMPARE(fixture.requests[5].path, fixture.requests[4].path);
        QCOMPARE(fixture.requests[7].body, fixture.requests[0].body);
    }
    void smsWorkflowRequiresFinalApproval() {
        Fixture fixture;
        fixture.json({{"verification_workflow", QJsonObject{{"id", "workflow-1"}}}}, 400);
        fixture.json({{"id", "machine-1"}});
        fixture.json({{"context", QJsonObject{{"sheriff_challenge", QJsonObject{{"type", "sms"}, {"status", "issued"}, {"id", "challenge-1"}}}}}});
        fixture.json({{"status", "validated"}});
        fixture.json({{"verification_workflow", QJsonObject{{"workflow_status", "workflow_status_internal_pending"}}}});
        fixture.json({{"type_context", QJsonObject{{"result", "workflow_status_approved"}}}});
        fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport()); int grants = 0;
        client.setGrantHandler([&](RHOAuthGrant) { ++grants; });
        QVERIFY(client.start("user", "password-secret", "device-1"));
        QTRY_COMPARE(client.state(), NativeAuthClient::State::WaitingForCode);
        QCOMPARE(fixture.requests.size(), 3); QCOMPARE(grants, 0);
        client.submitCode("bad code"); QTest::qWait(5); QCOMPARE(fixture.requests.size(), 3);
        client.submitCode("123456");
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 3000);
        QVERIFY2(client.state() == NativeAuthClient::State::Succeeded, qPrintable(client.statusText() + QString(" requests=%1").arg(fixture.requests.size())));
        QCOMPARE(grants, 1); QCOMPARE(fixture.requests.size(), 7);
        QCOMPARE(fixture.requests[1].method, "POST");
        QCOMPARE(fixture.requests[1].path, "/pathfinder/user_machine/");
        QCOMPARE(QJsonDocument::fromJson(fixture.requests[1].body).object(), (QJsonObject{{"device_id", "device-1"}, {"flow", "suv"}, {"input", QJsonObject{{"workflow_id", "workflow-1"}}}}));
        QCOMPARE(fixture.requests[2].method, "GET");
        QCOMPARE(fixture.requests[2].path, "/pathfinder/inquiries/machine-1/user_view/");
        QCOMPARE(fixture.requests[3].method, "POST");
        QCOMPARE(fixture.requests[3].path, "/challenge/challenge-1/respond/");
        QCOMPARE(form(fixture.requests[3].body).value("response"), "123456");
        QCOMPARE(QJsonDocument::fromJson(fixture.requests[4].body).object(), (QJsonObject{{"sequence", 0}, {"user_input", QJsonObject{{"status", "continue"}}}}));
        QCOMPARE(fixture.requests[6].body, fixture.requests[0].body);
    }
    void idleAndDirectGrant() {
        Fixture fixture; fixture.json(fullGrant());
        NativeAuthClient client(fixture.transport());
        QCOMPARE(client.state(), NativeAuthClient::State::Idle);
        QVERIFY(!client.busy());
        QTest::qWait(10); QCOMPARE(fixture.requests.size(), 0);
        int count = 0; RHOAuthGrant grant;
        client.setGrantHandler([&](RHOAuthGrant value) { ++count; grant = std::move(value); });
        QVERIFY(client.start("u+ser@example.test", "p+ a&ss=秘密", "device-1", "123456"));
        QVERIFY(client.busy());
        QVERIFY(!client.start("other", "other", "other"));
        QTRY_COMPARE(client.state(), NativeAuthClient::State::Succeeded);
        QCOMPARE(count, 1); QCOMPARE(fixture.requests.size(), 1);
        const auto &request = fixture.requests.first();
        QCOMPARE(request.method, "POST"); QCOMPARE(request.path, "/oauth2/token/");
        const auto fields = form(request.body);
        QCOMPARE(fields.value("username"), "u+ser@example.test");
        QCOMPARE(fields.value("password"), "p+ a&ss=秘密");
        QCOMPARE(fields.value("mfa_code"), "123456");
        QCOMPARE(fields.value("expires_in"), "86400");
        QCOMPARE(fields.value("scope"), "internal");
        QCOMPARE(fields.value("grant_type"), "password");
        QCOMPARE(fields.value("try_passkeys"), "False");
        QCOMPARE(fields.value("create_read_only_secondary_token"), "True");
        QCOMPARE(fields.value("token_request_path"), "/login");
        QVERIFY(!request.headers.toLower().contains("authorization:"));
        QVERIFY(!request.headers.toLower().contains("cookie:"));
        QVERIFY(request.headers.toLower().contains("application/x-www-form-urlencoded"));
        QCOMPARE(grant.response, fullGrant());
        QCOMPARE(grant.deviceToken, "device-1");
        QCOMPARE(grant.clientId, fields.value("client_id"));
        QCOMPARE(grant.refreshToken(), "refresh.synthetic");
        QCOMPARE(grant.obtainedAt.secsTo(grant.expiresAt), 321);
        QVERIFY(!client.statusText().contains("synthetic"));
        QVERIFY(!client.statusText().contains("p+"));
    }
};
QTEST_GUILESS_MAIN(TestNativeAuthClient)
#include "test_native_auth_client.moc"
