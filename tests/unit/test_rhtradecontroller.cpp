#include "rhtradecontroller.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QTimer>
#include <QtTest>

namespace {
struct Fixture : QObject {
    QTcpServer server;
    QTemporaryDir dir;
    RHNetworkEngine network{nullptr, true};
    RHTradeController::Session session;
    QList<int> delays;
    QList<std::function<void()>> timers;
    QList<QVariantMap> orders;
    QStringList requests;
    int releases = 0, acquisitions = 0;
    bool isCurrent = true;
    std::function<QJsonObject(QUrl)> overrideRead;
    std::function<QPair<int, QJsonObject>(QVariantMap)> overrideOrder;
    QString origin;
    int orderReadStatus = 200;
    QByteArray orderReadBytes; // Null means serialize the provider-shaped object.
    bool streamRead = false, streamOrder = false;
    QString streamPath, streamSymbol;
    int progressChunks = 0;
    explicit Fixture(int deadlineMs = 16000) : network(nullptr, true, deadlineMs) {
        if (!server.listen(QHostAddress::LocalHost)) qFatal("loopback listen failed");
        origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QFile file(dir.filePath("api.json")); if (!file.open(QIODevice::WriteOnly)) qFatal("fixture configuration open failed");
        QJsonObject ep{{"URL", origin + "/accounts/"}, {"Method", "GET"}, {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        file.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}}, {"Endpoints", QJsonObject{{"Accounts", ep}}}}).toJson()); file.close();
        if (!network.loadConfiguration(file.fileName())) qFatal("configuration failed");
        session.sessionIndex = 0; session.profileId = "p"; session.generation = 1; session.token = "secret-fixture";
        session.expiresAt = QDateTime::currentDateTimeUtc().addSecs(3600);
        for (int i = 1; i <= 3; ++i) session.accounts.append(QVariantMap{{"accountNumber", QString::number(i)}, {"displayName", QString("Account %1").arg(i)}, {"_url", origin + QString("/accounts/a%1/").arg(i)}});
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll(); const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int length = 0;
                    for (const auto &line : buffer->left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer->size() < headerEnd + 4 + length) return;
                    const auto line = buffer->split('\n').first().split(' ');
                    const QUrl url(origin + QString::fromUtf8(line.at(1)));
                    requests.append(QString::fromUtf8(line.first()) + " " + url.path());
                    QJsonObject response; int status = 200;
                    if (line.first() == "POST") {
                        QUrlQuery form(QString::fromUtf8(buffer->mid(headerEnd + 4, length)));
                        QVariantMap order; for (const auto &p : form.queryItems(QUrl::FullyDecoded)) order.insert(p.first, p.second);
                        orders.append(order);
                        if (overrideOrder) { const auto r = overrideOrder(order); status = r.first; response = r.second; }
                        else response = receipt(order, QString("order-%1").arg(orders.size()));
                    } else response = read(url);
                    const bool orderRead = line.first() == "GET" && url.path().startsWith("/orders/");
                    if (orderRead) status = orderReadStatus;
                    const QByteArray body = orderRead && !orderReadBytes.isNull() ? orderReadBytes : QJsonDocument(response).toJson(QJsonDocument::Compact);
                    const bool selectedRead = !streamPath.isEmpty() && url.path() == streamPath
                        && (streamSymbol.isEmpty() || QUrlQuery(url).queryItemValue("symbols") == streamSymbol);
                    if ((line.first() == "POST" && streamOrder) || (line.first() == "GET" && (streamRead || selectedRead))) {
                        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n" + body);
                        socket->flush(); buffer->clear();
                        auto *progress = new QTimer(socket); progress->setInterval(20);
                        connect(progress, &QTimer::timeout, socket, [this, socket] {
                            if (socket->state() == QAbstractSocket::ConnectedState) { socket->write(" "); socket->flush(); ++progressChunks; }
                        });
                        connect(socket, &QTcpSocket::disconnected, progress, &QTimer::stop);
                        progress->start(); return;
                    }
                    const QByteArray redirect = orderRead && status == 302 ? QByteArray("Location: /unexpected-order-redirect/\r\n") : QByteArray();
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\n" + redirect + "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost(); buffer->clear();
                });
            }
        });
    }
    static QJsonObject receipt(const QVariantMap &order, QString id, QString state = "queued") {
        return {{"id", id}, {"state", state}, {"account", order.value("account").toString()},
            {"instrument", order.value("instrument").toString()}, {"side", order.value("side").toString()},
            {"quantity", order.value("quantity").toString()}, {"ref_id", order.value("ref_id").toString()}};
    }
    QJsonObject read(QUrl url) {
        if (overrideRead) { const auto response = overrideRead(url); if (!response.isEmpty()) return response; }
        QJsonArray rows; QUrlQuery q(url);
        if (url.path() == "/accounts/") for (const auto &v : session.accounts) {
            const auto a = v.toMap(); rows.append(QJsonObject{{"account_number", a.value("accountNumber").toString()}, {"url", a.value("_url").toString()}, {"state", "active"}});
        }
        if (url.path() == "/instruments/") { const auto symbol = q.queryItemValue("symbols"); rows.append(QJsonObject{{"id", symbol}, {"symbol", symbol}, {"url", origin + "/instruments/" + symbol + "/"}, {"tradeable", true}, {"tradability", "tradable"}, {"state", "active"}}); }
        if (url.path() == "/quotes/") { const auto symbol = q.queryItemValue("symbols"); rows.append(QJsonObject{{"symbol", symbol}, {"instrument", origin + "/instruments/" + symbol + "/"}, {"last_trade_price", symbol == "MSFT" ? "0.9999" : "10.25"}, {"ask_price", symbol == "MSFT" ? "0.0001" : "10.25"}, {"bid_price", "0.0001"}}); }
        return {{"results", rows}, {"next", QJsonValue::Null}};
    }
    RHTradeController::Dependencies deps() {
        RHTradeController::Dependencies d;
        d.acquire = [this](auto callback) { ++acquisitions; callback(true, session, {}); };
        d.current = [this](const auto &s) { return isCurrent && s.generation == session.generation && s.profileId == session.profileId; };
        // This fixture has no disk-backed identity. Production uses the manager's
        // bounded worker; keep this explicitly cheap seam separate from current.
        d.currentLocal = [this](const auto &s) { return isCurrent && s.generation == session.generation && s.profileId == session.profileId; };
        d.currentAsync = [check = d.currentLocal](const auto &s, auto done) { done(check(s)); };
        d.release = [this] { ++releases; };
        d.availabilityReason = [] { return QString(); };
        d.schedule = [this](int delay, auto callback) { delays.append(delay); timers.append(std::move(callback)); };
        d.loopbackLiveOrdersForTests = true;
        return d;
    }
    void tick() { auto callback = timers.takeFirst(); callback(); }
};
}
class RHTradeControllerTest : public QObject {
    Q_OBJECT
private slots:
    void lateLookupAndCooldownCannotUnlockReplacementBatch_data() {
        QTest::addColumn<bool>("finishLookup");
        QTest::newRow("cancel-pending-get") << false;
        QTest::newRow("cancel-unknown-cooldown") << true;
    }
    void lateLookupAndCooldownCannotUnlockReplacementBatch() {
        QFETCH(bool, finishLookup); Fixture f;
        f.streamPath = finishLookup ? QString() : QString("/orders/broker-id/");
        f.overrideOrder = [](QVariantMap) { return qMakePair(201, QJsonObject{{"id", "broker-id"}}); };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_VERIFY(f.requests.contains("GET /orders/broker-id/"));
        std::function<void()> oldCooldown;
        if (finishLookup) { QTRY_COMPARE(f.timers.size(), 1); oldCooldown = f.timers.takeFirst(); }
        c.cancel(); QCOMPARE(f.releases, 1);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "unknown");
        f.streamPath.clear(); f.overrideOrder = {};
        QVERIFY(c.submit("MSFT", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(f.orders.size(), 2); QCOMPARE(f.releases, 1);
        if (oldCooldown) oldCooldown();
        QTest::qWait(100); QVERIFY(c.tradeResultsBusy()); QCOMPARE(f.orders.size(), 2); QCOMPARE(f.releases, 1);
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), i + 2); f.tick();
        }
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 2); QCOMPARE(f.acquisitions, 2);
        if (oldCooldown) oldCooldown(); QCoreApplication::processEvents(); QCOMPARE(f.releases, 2);
    }
    void repeatedBrokerOrClientIdentitiesCannotBePromoted_data() {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"broker-id", "prior-client-ref-as-id", "prior-client-ref"}) QTest::newRow(fault) << QString(fault);
    }
    void repeatedBrokerOrClientIdentitiesCannotBePromoted() {
        QFETCH(QString, fault); Fixture f;
        f.overrideOrder = [&](QVariantMap request) {
            auto o = Fixture::receipt(request, QString("broker-%1").arg(f.orders.size()));
            if (f.orders.size() > 1) {
                if (fault == "broker-id") o.insert("id", "broker-1");
                if (fault == "prior-client-ref-as-id") o.insert("id", f.orders.first().value("ref_id").toString());
                if (fault == "prior-client-ref") o.insert("ref_id", f.orders.first().value("ref_id").toString());
            }
            return qMakePair(201, o);
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); f.tick(); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(f.orders.size(), 2); QCOMPARE(c.tradeResults().at(1).toMap().value("status").toString(), "unknown");
        QVERIFY(c.tradeResults().at(1).toMap().value("orderId").toString().isEmpty());
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.orders.size(), 2); QCOMPARE(f.releases, 1);
    }
    void reconciliationKnownStatesNeverInventFill_data() {
        QTest::addColumn<QString>("state");
        for (const auto &state : {"queued", "unconfirmed", "confirmed", "partially_filled", "filled", "rejected", "cancelled", "failed"}) QTest::newRow(state) << QString(state);
    }
    void reconciliationKnownStatesNeverInventFill() {
        QFETCH(QString, state); Fixture f;
        f.overrideOrder = [&](QVariantMap) { return qMakePair(201, QJsonObject{{"id", QString("broker-%1").arg(f.orders.size())}}); };
        f.overrideRead = [&](QUrl u) -> QJsonObject {
            if (!u.path().startsWith("/orders/")) return {};
            return Fixture::receipt(f.orders.last(), QString("broker-%1").arg(f.orders.size()), state);
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), i + 1);
            const auto row = c.tradeResults().at(i).toMap();
            const bool accepted = !QStringList{"rejected", "cancelled", "failed"}.contains(state);
            QCOMPARE(row.value("status").toString(), state == "filled" ? QString("filled") : accepted ? QString("submitted") : QString("failed"));
            QCOMPARE(row.value("success").toBool(), accepted); f.tick();
        }
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
    }
    void unsafeOrMissingCandidateNeverLooksUp_data() {
        QTest::addColumn<QString>("candidate");
        for (const auto &id : {"", "../escape", "x?y", "x%2Fy", "secret-fixture", "x-secret-fixture-x", "client-ref"}) QTest::newRow(id[0] ? id : "missing") << QString(id);
    }
    void unsafeOrMissingCandidateNeverLooksUp() {
        QFETCH(QString, candidate); Fixture f;
        f.overrideOrder = [&](QVariantMap request) {
            QJsonObject o;
            if (!candidate.isEmpty()) o.insert("id", candidate == "client-ref" ? request.value("ref_id").toString() : candidate);
            return qMakePair(201, o);
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "unknown");
        QCOMPARE(f.requests.last(), QString("POST /orders/")); QCOMPARE(f.orders.size(), 1);
        QVERIFY(c.tradeResults().first().toMap().value("orderId").toString().isEmpty());
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1); QCOMPARE(f.orders.size(), 1);
    }
    void interruptedReconciliationKeepsAttemptUnknown_data() {
        QTest::addColumn<QString>("action");
        for (const auto &action : {"cancel", "expiry", "identity", "destroy"}) QTest::newRow(action) << QString(action);
    }
    void interruptedReconciliationKeepsAttemptUnknown() {
        QFETCH(QString, action); Fixture f;
        f.streamPath = "/orders/broker-id/";
        f.overrideOrder = [](QVariantMap) { return qMakePair(201, QJsonObject{{"id", "broker-id"}}); };
        if (action == "expiry") f.session.expiresAt = QDateTime::currentDateTimeUtc().addSecs(1);
        auto d = f.deps(); QString statusAtRelease;
        RHTradeController *c = nullptr;
        d.release = [&] {
            ++f.releases; const auto row = c->tradeResults().first().toMap(); statusAtRelease = row.value("status").toString();
            QVERIFY(row.value("message").toString().contains("post[http=201,qt=0,"));
            QVERIFY(row.value("message").toString().contains("lookup["));
        };
        c = new RHTradeController(f.network, d);
        QVERIFY(c->submit("AAPL", "buy")); QTRY_VERIFY(f.requests.contains("GET /orders/broker-id/"));
        QCOMPARE(f.orders.size(), 1); QVERIFY(f.timers.isEmpty()); QCOMPARE(f.releases, 0);
        QVERIFY(c->tradeResults().first().toMap().value("orderId").toString().isEmpty());
        if (action == "destroy") { delete c; c = nullptr; }
        else {
            if (action == "cancel") c->cancel();
            else if (action == "identity") f.isCurrent = false;
            else QTest::qWait(1100);
            if (action != "cancel") { QTRY_COMPARE(f.timers.size(), 1); f.tick(); }
            QTRY_VERIFY(c->tradeResultsComplete());
            QCOMPARE(c->tradeResults().first().toMap().value("status").toString(), "unknown");
        }
        QCOMPARE(statusAtRelease, "unknown"); QCOMPARE(f.releases, 1);
        QTest::qWait(100); QCOMPARE(f.orders.size(), 1); QCOMPARE(f.requests.count("GET /orders/broker-id/"), 1);
        if (c) { delete c; c = nullptr; } QCOMPARE(f.releases, 1);
    }
    void untrustedLookupHaltsWithoutRetry_data() {
        QTest::addColumn<QString>("fault");
        for (const auto &field : {"id", "ref_id", "account", "instrument", "side", "quantity", "state", "symbol"}) {
            QTest::newRow(field) << QString(field);
            if (QString(field) != "symbol") QTest::newRow(qPrintable(QString("missing-") + field)) << QString("missing-") + field;
        }
        for (const auto &fault : {"null-symbol", "numeric-quantity", "precision-quantity", "json", "array", "http", "redirect", "timeout", "oversize"})
            QTest::newRow(fault) << QString(fault);
    }
    void untrustedLookupHaltsWithoutRetry() {
        QFETCH(QString, fault); Fixture f(fault == "timeout" ? 400 : 16000);
        if (fault == "http") f.orderReadStatus = 503;
        if (fault == "redirect") f.orderReadStatus = 302;
        if (fault == "json") f.orderReadBytes = "{not json secret-fixture private@example.test";
        if (fault == "array") f.orderReadBytes = "[]";
        if (fault == "oversize") f.orderReadBytes = QByteArray(1024 * 1024 + 10, 'x');
        if (fault == "timeout") f.streamPath = "/orders/broker-id/";
        f.overrideOrder = [](QVariantMap) { return qMakePair(201, QJsonObject{{"id", "broker-id"}}); };
        f.overrideRead = [&](QUrl u) -> QJsonObject {
            if (!u.path().startsWith("/orders/")) return {};
            auto o = Fixture::receipt(f.orders.first(), "broker-id");
            if (fault.startsWith("missing-")) o.remove(fault.mid(8));
            else if (fault == "null-symbol") o.insert("symbol", QJsonValue::Null);
            else if (fault == "numeric-quantity") o.insert("quantity", 1);
            else if (fault == "precision-quantity") o.insert("quantity", "1.0000000000001");
            else o.insert(fault, "private@example.test secret-fixture account-private");
            return o;
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(f.orders.size(), 1); QCOMPARE(f.requests.count("GET /orders/broker-id/"), 1);
        QCOMPARE(f.requests.last(), QString("GET /orders/broker-id/"));
        const auto row = c.tradeResults().first().toMap();
        QCOMPARE(row.value("status").toString(), "unknown"); QVERIFY(row.value("orderId").toString().isEmpty());
        const auto message = row.value("message").toString();
        QVERIFY(message.contains("post[http=201,qt=0,")); QVERIFY(message.contains("lookup[http="));
        for (const auto &secret : {"secret-fixture", "private@example.test", "account-private"}) QVERIFY(!message.contains(secret));
        QVERIFY(!message.contains(f.origin)); QVERIFY(!message.contains(f.orders.first().value("ref_id").toString()));
        QCOMPARE(f.releases, 0); f.tick(); QTRY_VERIFY(c.tradeResultsComplete());
        QCOMPARE(f.orders.size(), 1); QCOMPARE(f.releases, 1); QCOMPARE(f.requests.count("GET /orders/broker-id/"), 1);
    }
    void ambiguousReceiptSingleReadConfirmsBeforeContinuing_data() {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"partial", "ref_id", "account", "instrument", "side", "quantity", "symbol", "null-symbol", "state"})
            QTest::newRow(fault) << QString(fault);
    }
    void ambiguousReceiptSingleReadConfirmsBeforeContinuing() {
        QFETCH(QString, fault); Fixture f;
        f.overrideOrder = [&](QVariantMap request) {
            auto o = Fixture::receipt(request, QString("broker-%1").arg(f.orders.size()));
            if (fault == "partial") o = {{"id", o.value("id")}};
            else if (fault == "null-symbol") o.insert("symbol", QJsonValue::Null);
            else o.insert(fault, "private-wrong-value");
            return qMakePair(201, o);
        };
        f.overrideRead = [&](QUrl u) -> QJsonObject {
            if (!u.path().startsWith("/orders/")) return {};
            return Fixture::receipt(f.orders.last(), QString("broker-%1").arg(f.orders.size()));
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(f.orders.size(), i + 1);
            QCOMPARE(f.requests.last(), QString("GET /orders/broker-%1/").arg(i + 1));
            QCOMPARE(c.tradeResults().at(i).toMap().value("status").toString(), "submitted");
            QCOMPARE(c.tradeResults().at(i).toMap().value("orderId").toString(), QString("broker-%1").arg(i + 1));
            QCOMPARE(f.releases, 0); f.tick();
        }
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
    }
    void acceptedReceiptWithoutDisplaySymbolContinuesAccounts_data() {
        QTest::addColumn<QString>("brokerState");
        QTest::newRow("accepted") << QString("queued");
        QTest::newRow("filled") << QString("filled");
    }
    void acceptedReceiptWithoutDisplaySymbolContinuesAccounts() {
        QFETCH(QString, brokerState);
        Fixture f;
        f.overrideOrder = [&](QVariantMap request) {
            // Deliberately no display-symbol echo; instrument is authoritative.
            return qMakePair(201, QJsonObject{{"id", QString("order-%1").arg(f.orders.size())},
                {"account", request.value("account").toString()},
                {"instrument", request.value("instrument").toString()},
                {"quantity", "1.00000000"}, {"side", "buy"},
                {"ref_id", request.value("ref_id").toString()}, {"state", brokerState}});
        };
        RHTradeController c(f.network, f.deps());
        QVERIFY(c.submit("AAPL", "buy"));
        for (int row = 0; row < 3; ++row) {
            QTRY_COMPARE(f.orders.size(), row + 1);
            QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(c.tradeResults().at(row).toMap().value("status").toString(),
                brokerState == "filled" ? QString("filled") : QString("submitted"));
            QVERIFY(c.tradeResultsBusy());
            f.tick();
        }
        QTRY_VERIFY(!c.tradeResultsBusy());
        QCOMPARE(f.orders.size(), 3);
        QCOMPARE(f.releases, 1);
    }
    void buysTickerMajorAcrossAllAccountsWithFinalCooldown()
    {
        Fixture f; RHTradeController c(f.network, f.deps());
        QVERIFY(c.submit(" aapl, MSFT, AAPL ", "buy")); QVERIFY(!c.submit("TSLA", "buy"));
        QSet<QString> refs;
        for (int i = 0; i < 6; ++i) {
            QTRY_COMPARE(f.orders.size(), i + 1); QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(c.tradeResults().size(), 6); QVERIFY(c.tradeResultsBusy());
            const auto order = f.orders.at(i);
            QCOMPARE(f.requests.mid(0, 6), (QStringList{"GET /accounts/", "GET /instruments/", "GET /quotes/", "GET /instruments/", "GET /quotes/", "POST /orders/"}));
            QCOMPARE(f.requests.count("GET /instruments/"), 2);
            QCOMPARE(f.requests.count("GET /quotes/"), 2);
            QCOMPARE(order.value("type").toString(), i < 3 ? "market" : "limit");
            QCOMPARE(order.value("price").toString(), i < 3 ? "10.25" : "0.0101");
            QVERIFY(!order.contains("preset_percent_limit"));
            QCOMPARE(order.value("bid_ask_timestamp"), f.orders.at(i < 3 ? 0 : 3).value("bid_ask_timestamp"));
            QCOMPARE(order.value("symbol").toString(), i < 3 ? "AAPL" : "MSFT");
            QCOMPARE(order.value("account").toString(), f.origin + QString("/accounts/a%1/").arg(i % 3 + 1));
            QCOMPARE(order.value("quantity").toString(), "1");
            QCOMPARE(c.tradeResults().at(i).toMap().value("status").toString(), "submitted");
            QVERIFY(!refs.contains(order.value("ref_id").toString())); refs.insert(order.value("ref_id").toString());
            QVERIFY(f.delays.last() >= 2000 && f.delays.last() <= 3000);
            QCOMPARE(f.releases, 0); f.tick();
        }
        QTRY_VERIFY(!c.tradeResultsBusy()); QVERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
        QCOMPARE(f.delays.size(), 6); QCOMPARE(f.acquisitions, 1);
    }

    void invalidSecondTickerAbortsEntireBuyPreflight_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"missing-last", "null-last", "number-last", "zero-last", "negative-last", "exponent-last", "overflow-last", "precision-last",
                "missing-ask", "null-ask", "zero-ask", "invalid-ask", "overflow-ask", "precision-ask", "zero-bid", "symbol", "instrument", "duplicate", "empty", "null-row", "paginated", "invalid-instrument", "limit-overflow"})
            QTest::newRow(fault) << QString(fault);
    }
    void invalidSecondTickerAbortsEntireBuyPreflight()
    {
        QFETCH(QString, fault); Fixture f;
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (QUrlQuery(url).queryItemValue("symbols") != "MSFT") return {};
            if (fault == "invalid-instrument" && url.path() == "/instruments/")
                return {{"results", QJsonArray{}}, {"next", QJsonValue::Null}};
            if (url.path() != "/quotes/") return {};
            QJsonObject q{{"symbol", "MSFT"}, {"instrument", f.origin + "/instruments/MSFT/"},
                {"last_trade_price", "0.5"}, {"ask_price", "0.51"}, {"bid_price", "0.49"}};
            if (fault == "missing-last") q.remove("last_trade_price");
            if (fault == "null-last") q.insert("last_trade_price", QJsonValue::Null);
            if (fault == "number-last") q.insert("last_trade_price", 0.5);
            if (fault == "zero-last") q.insert("last_trade_price", "0.000");
            if (fault == "negative-last") q.insert("last_trade_price", "-1");
            if (fault == "exponent-last") q.insert("last_trade_price", "1e-2");
            if (fault == "overflow-last") q.insert("last_trade_price", "1000000000000");
            if (fault == "precision-last") q.insert("last_trade_price", "0.1234567890123");
            if (fault == "missing-ask") q.remove("ask_price");
            if (fault == "null-ask") q.insert("ask_price", QJsonValue::Null);
            if (fault == "zero-ask") q.insert("ask_price", "0");
            if (fault == "invalid-ask") q.insert("ask_price", "NaN");
            if (fault == "overflow-ask") q.insert("ask_price", "1000000000000");
            if (fault == "precision-ask") q.insert("ask_price", "0.1234567890123");
            if (fault == "limit-overflow") q.insert("ask_price", "999999999999.999999999999");
            if (fault == "zero-bid") q.insert("bid_price", "0");
            if (fault == "symbol") q.insert("symbol", "AAPL");
            if (fault == "instrument") q.insert("instrument", f.origin + "/instruments/AAPL/");
            QJsonArray rows{q};
            if (fault == "duplicate") rows.append(q);
            if (fault == "empty") rows = {};
            if (fault == "null-row") rows = {QJsonValue::Null};
            return {{"results", rows}, {"next", fault == "paginated" ? QJsonValue(f.origin + "/quotes/?symbols=MSFT&cursor=x") : QJsonValue(QJsonValue::Null)}};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.timers.size(), 1);
        QVERIFY2(!c.error().isEmpty(), "Invalid preflight must halt the entire batch, not advance to an uncached row.");
        QVERIFY(c.error().contains("MSFT")); QVERIFY(c.error().contains("preflight", Qt::CaseInsensitive));
        QVERIFY(f.orders.isEmpty()); QCOMPARE(f.requests.count("GET /quotes/"), fault == "invalid-instrument" ? 1 : 2);
        QCOMPARE(f.releases, 0); const auto late = f.timers.first(); f.tick();
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
        late(); c.cancel(); QCoreApplication::processEvents(); QCOMPARE(f.releases, 1); QVERIFY(f.orders.isEmpty());
    }

    void exactBuyPolicyIsSharedByLiveAndPreview_data()
    {
        QTest::addColumn<QString>("last"); QTest::addColumn<QString>("ask");
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("price"); QTest::addColumn<bool>("live");
        for (const bool live : {false, true}) {
            const auto prefix = live ? "live-" : "preview-";
            QTest::newRow(qPrintable(QString(prefix) + "below-one")) << QString("0.9999") << QString("0.0001") << QString("limit") << QString("0.0101") << live;
            QTest::newRow(qPrintable(QString(prefix) + "one")) << QString("1") << QString("0.9999") << QString("market") << QString("0.9999") << live;
            QTest::newRow(qPrintable(QString(prefix) + "above-one")) << QString("1.0001") << QString("1.0002") << QString("market") << QString("1.0002") << live;
            QTest::newRow(qPrintable(QString(prefix) + "carry")) << QString("0.99") << QString("0.999999999999") << QString("limit") << QString("1.009999999999") << live;
            QTest::newRow(qPrintable(QString(prefix) + "normalize")) << QString("0.5") << QString("0.090000000000") << QString("limit") << QString("0.1") << live;
            QTest::newRow(qPrintable(QString(prefix) + "max-carry")) << QString("0.5") << QString("999999999998.999999999999") << QString("limit") << QString("999999999999.009999999999") << live;
        }
    }
    void exactBuyPolicyIsSharedByLiveAndPreview()
    {
        QFETCH(QString, last); QFETCH(QString, ask); QFETCH(QString, type); QFETCH(QString, price); QFETCH(bool, live);
        Fixture f; f.session.accounts = {f.session.accounts.first()}; auto d = f.deps(); d.loopbackLiveOrdersForTests = live;
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/quotes/") return {};
            return {{"results", QJsonArray{QJsonObject{{"symbol", "AAPL"}, {"instrument", f.origin + "/instruments/AAPL/"},
                {"last_trade_price", last}, {"ask_price", ask}, {"bid_price", "0.0001"}}}}, {"next", QJsonValue::Null}};
        };
        RHTradeController c(f.network, d); QVERIFY(c.submit("AAPL", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        const auto row = c.tradeResults().first().toMap();
        QCOMPARE(row.value("orderType").toString(), type); QCOMPARE(row.value("price").toString(), price);
        QCOMPARE(row.value("askPrice").toString(), ask);
        QVERIFY(QDateTime::fromString(row.value("quoteTimestamp").toString(), Qt::ISODateWithMs).isValid());
        QCOMPARE(f.orders.size(), c.liveOrdersEnabled() ? 1 : 0);
        if (c.liveOrdersEnabled()) {
            QCOMPARE(f.orders.first().value("type").toString(), type); QCOMPARE(f.orders.first().value("price").toString(), price);
            QCOMPARE(f.orders.first().value("bid_ask_timestamp"), row.value("quoteTimestamp"));
            QVERIFY(!f.orders.first().contains("preset_percent_limit"));
        } else QCOMPARE(row.value("status").toString(), "preview");
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
    }

    void invalidFirstQuoteStopsPreviewBeforeOtherTickers()
    {
        Fixture f; auto d = f.deps(); d.loopbackLiveOrdersForTests = false;
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/quotes/") return {};
            return {{"results", QJsonArray{QJsonObject{{"symbol", "AAPL"}, {"instrument", f.origin + "/instruments/AAPL/"},
                {"ask_price", "10.25"}, {"bid_price", "10.24"}}}}, {"next", QJsonValue::Null}};
        };
        RHTradeController c(f.network, d); QVERIFY(c.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); QVERIFY(c.error().contains("AAPL"));
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "failed");
        QCOMPARE(f.requests.count("GET /instruments/"), 1); QCOMPARE(f.requests.count("GET /quotes/"), 1);
        QVERIFY(f.orders.isEmpty()); QCOMPARE(f.releases, 0);
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1); QVERIFY(f.orders.isEmpty());
    }

    void replacementBatchAlwaysFetchesNewPlan_data()
    {
        QTest::addColumn<bool>("cancelFirst");
        QTest::newRow("after-finish") << false; QTest::newRow("after-cancel") << true;
    }
    void replacementBatchAlwaysFetchesNewPlan()
    {
        QFETCH(bool, cancelFirst); Fixture f; f.session.accounts = {f.session.accounts.first()};
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), 1);
        auto oldTimer = f.timers.takeFirst();
        if (cancelFirst) c.cancel(); else oldTimer();
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/quotes/") return {};
            return {{"results", QJsonArray{QJsonObject{{"symbol", "AAPL"}, {"instrument", f.origin + "/instruments/AAPL/"},
                {"last_trade_price", "0.1"}, {"ask_price", "0.1"}, {"bid_price", "0.09"}}}}, {"next", QJsonValue::Null}};
        };
        QVERIFY(c.submit("AAPL", "buy")); oldTimer();
        QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), 2);
        QCOMPARE(f.requests.count("GET /accounts/"), 2); QCOMPARE(f.requests.count("GET /instruments/"), 2); QCOMPARE(f.requests.count("GET /quotes/"), 2);
        QCOMPARE(f.orders.last().value("type").toString(), "limit"); QCOMPARE(f.orders.last().value("price").toString(), "0.11");
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 2);
    }

    void interruptedSecondTickerPreflightReleasesExactlyOnce_data()
    {
        QTest::addColumn<QString>("action");
        for (const auto &action : {"cancel", "destroy", "timeout"}) QTest::newRow(action) << QString(action);
    }
    void interruptedSecondTickerPreflightReleasesExactlyOnce()
    {
        QFETCH(QString, action); Fixture f(action == "timeout" ? 500 : 16000);
        f.streamPath = "/quotes/"; f.streamSymbol = "MSFT";
        auto c = std::make_unique<RHTradeController>(f.network, f.deps());
        QVERIFY(c->submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.requests.count("GET /quotes/"), 2); QVERIFY(f.orders.isEmpty()); QCOMPARE(f.releases, 0);
        if (action == "destroy") c.reset();
        else if (action == "cancel") { c->cancel(); QVERIFY(c->tradeResultsComplete()); }
        else {
            QTRY_COMPARE_WITH_TIMEOUT(f.timers.size(), 1, 2000);
            QVERIFY(c->error().contains("MSFT")); QCOMPARE(f.releases, 0); f.tick();
            QTRY_VERIFY(c->tradeResultsComplete());
        }
        QCOMPARE(f.releases, 1); c.reset(); QTest::qWait(80); QCOMPARE(f.releases, 1);
        QVERIFY(f.orders.isEmpty()); QCOMPARE(f.requests.count("GET /quotes/"), 2);
    }

    void preparedPlanNotificationMayDestroyOrCancelController_data()
    {
        QTest::addColumn<bool>("destroy"); QTest::newRow("destroy") << true; QTest::newRow("cancel") << false;
    }
    void preparedPlanNotificationMayDestroyOrCancelController()
    {
        QFETCH(bool, destroy); Fixture f;
        auto *c = new RHTradeController(f.network, f.deps()); QPointer<RHTradeController> guard(c);
        connect(c, &RHTradeController::tradeResultsChanged, &f, [&] {
            if (guard && !guard->tradeResults().isEmpty() && guard->tradeResults().first().toMap().contains("orderType")) {
                if (destroy) delete guard.data(); else guard->cancel();
            }
        });
        QVERIFY(c->submit("AAPL,MSFT", "buy")); QTRY_COMPARE(f.releases, 1);
        QVERIFY(f.orders.isEmpty()); QCOMPARE(f.requests.count("GET /quotes/"), 2);
        if (guard) { QVERIFY(guard->tradeResultsComplete()); delete guard.data(); }
        QCoreApplication::processEvents(); QCOMPARE(f.releases, 1);
    }

    void activeSubsetsIgnoreClosedRowsAndKeepDisplayedOrder_data()
    {
        QTest::addColumn<bool>("frozenIncludesClosed");
        QTest::newRow("frozen-active-and-closed") << true;
        QTest::newRow("frozen-active-only") << false;
    }
    void activeSubsetsIgnoreClosedRowsAndKeepDisplayedOrder()
    {
        QFETCH(bool, frozenIncludesClosed); Fixture f;
        const auto allAccounts = f.session.accounts;
        if (!frozenIncludesClosed) f.session.accounts.removeAt(1);
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/accounts/") return {};
            QJsonArray rows;
            for (const int i : {2, 1, 0}) {
                const auto a = allAccounts.at(i).toMap();
                rows.append(QJsonObject{{"account_number", a.value("accountNumber").toString()},
                    {"url", a.value("_url").toString()}, {"state", i == 1 ? "closed" : "active"}});
            }
            rows.append(QJsonObject{{"account_number", "old"}, {"url", f.origin + "/accounts/old/"}, {"state", "closed"}});
            return {{"results", rows}, {"next", QJsonValue::Null}};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.orders.size(), 1);
        QCOMPARE(c.tradeResults().size(), 4);
        for (int i = 0; i < 4; ++i) {
            QTRY_COMPARE(f.orders.size(), i + 1); QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(f.orders.at(i).value("account").toString(), f.origin + (i % 2 == 0 ? "/accounts/a1/" : "/accounts/a3/"));
            QCOMPARE(f.orders.at(i).value("symbol").toString(), i < 2 ? "AAPL" : "MSFT");
            f.tick();
        }
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
    }

    void freshScopeRejectsMissingNewDuplicateAndUnsafeIdentities_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"missing", "new-active", "duplicate-number", "duplicate-url", "changed-url", "unsafe-url", "missing-state", "all-closed", "frozen-duplicate"})
            QTest::newRow(fault) << QString(fault);
    }
    void freshScopeRejectsMissingNewDuplicateAndUnsafeIdentities()
    {
        QFETCH(QString, fault); Fixture f;
        if (fault == "frozen-duplicate") f.session.accounts.append(f.session.accounts.first());
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/accounts/") return {};
            QJsonArray rows;
            for (const auto &v : f.session.accounts) {
                const auto a = v.toMap();
                rows.append(QJsonObject{{"account_number", a.value("accountNumber").toString()},
                    {"url", a.value("_url").toString()}, {"state", fault == "all-closed" ? "closed" : "active"}});
            }
            if (fault == "missing") rows.removeLast();
            if (fault == "duplicate-number") rows.append(rows.first());
            if (fault == "new-active") rows.append(QJsonObject{{"account_number", "4"}, {"url", f.origin + "/accounts/a4/"}, {"state", "active"}});
            auto first = rows.first().toObject();
            if (fault == "duplicate-url") first.insert("url", f.origin + "/accounts/a2/");
            if (fault == "changed-url") first.insert("url", f.origin + "/accounts/changed/");
            if (fault == "unsafe-url") first.insert("url", "https://evil.example/accounts/a1/");
            if (fault == "missing-state") first.remove("state");
            rows[0] = first;
            return {{"results", rows}, {"next", QJsonValue::Null}};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_VERIFY(c.tradeResultsComplete()); QVERIFY(!c.error().isEmpty());
        QVERIFY(f.orders.isEmpty()); QCOMPARE(f.releases, 1);
    }

    void sellsFreshExactUnreservedSharesAcrossAccounts()
    {
        Fixture f;
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/positions/") return {};
            const auto n = QUrlQuery(url).queryItemValue("account_number");
            QJsonArray rows;
            if (n != "2") rows.append(QJsonObject{{"account", f.origin + "/accounts/a" + n + "/"},
                {"account_number", n}, {"instrument", f.origin + "/instruments/AAPL/"},
                {"quantity", "123456789012.123456789012"}, {"shares_held_for_sells", "0.100000000001"},
                {"shares_held_for_stock_grants", "0.000000000001"}, {"shares_held_for_options_collateral", "1"},
                {"shares_held_for_options_events", "0.000000000009"}});
            return {{"results", rows}, {"next", QJsonValue::Null}};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "sell"));
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(f.timers.size(), 1);
            const auto row = c.tradeResults().at(i).toMap();
            QCOMPARE(row.value("status").toString(), i == 1 ? "skipped" : "submitted");
            if (i != 1) {
                QCOMPARE(row.value("quantity").toString(), "123456789011.023456789001");
                QCOMPARE(f.orders.last().value("quantity").toString(), "123456789011.023456789001");
                QCOMPARE(f.orders.last().value("type").toString(), "market");
                QVERIFY(!f.orders.last().contains("price"));
                QVERIFY(f.orders.last().contains("bid_price"));
            }
            QVERIFY(f.delays.last() >= 2000 && f.delays.last() <= 3000); f.tick();
        }
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.orders.size(), 2); QCOMPARE(f.releases, 1);
    }

    void failedAcquisitionReleasesLeaseAndPreservesSafeReason()
    {
        Fixture f; auto d = f.deps();
        d.acquire = [&](auto callback) { ++f.acquisitions; callback(false, {}, "Could not read a usable trading token from the secure store. Reauthenticate and retry."); };
        RHTradeController c(f.network, d); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
        QCOMPARE(c.error(), "Could not read a usable trading token from the secure store. Reauthenticate and retry.");
        QVERIFY(f.requests.isEmpty());
    }

    void destructionAfterAcquisitionCallbackReleasesExactlyOnce()
    {
        Fixture f; auto *c = new RHTradeController(f.network, f.deps());
        QVERIFY(c->submit("AAPL", "buy")); delete c;
        QCoreApplication::processEvents(); QCOMPARE(f.releases, 1); QVERIFY(f.orders.isEmpty());
    }

    void staleAcquisitionCallbackCannotReleaseNewLease()
    {
        Fixture f; auto d = f.deps();
        std::function<void(bool, RHTradeController::Session, QString)> callback;
        d.acquire = [&](auto done) { ++f.acquisitions; callback = done; };
        auto *c = new RHTradeController(f.network, d); QVERIFY(c->submit("AAPL", "buy"));
        const auto stale = callback; delete c; QCOMPARE(f.releases, 1);
        RHTradeController next(f.network, d); QVERIFY(next.submit("MSFT", "buy"));
        stale(false, {}, "Old request"); QCoreApplication::processEvents(); QCOMPARE(f.releases, 1);
        callback(false, {}, "Secure store failed"); QTRY_VERIFY(next.tradeResultsComplete()); QCOMPARE(f.releases, 2);
        callback(false, {}, "Duplicate"); QCoreApplication::processEvents(); QCOMPARE(f.releases, 2);
    }

    void acceptsUnpaginatedAccountsWithAbsentNext()
    {
        Fixture f; auto d = f.deps(); d.loopbackLiveOrdersForTests = false;
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/accounts/") return {};
            QJsonArray rows; for (const auto &v : f.session.accounts) {
                const auto a = v.toMap(); rows.append(QJsonObject{{"account_number", a.value("accountNumber").toString()}, {"url", a.value("_url").toString()}, {"state", "active"}});
            }
            return {{"results", rows}};
        };
        RHTradeController c(f.network, d); QVERIFY(c.submit("AAPL", "buy"));
        for (int i = 0; i < 3; ++i) {
            QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(c.tradeResults().at(i).toMap().value("status").toString(), c.liveOrdersEnabled() ? "submitted" : "preview");
            f.tick();
        }
        QTRY_VERIFY(c.tradeResultsComplete());
        if (!c.liveOrdersEnabled()) QVERIFY(f.orders.isEmpty());
    }

    void brokerQuantityFormattingDoesNotTurnAcceptanceIntoUnknown()
    {
        Fixture f; f.session.accounts = {f.session.accounts.first()};
        f.overrideOrder = [](QVariantMap order) {
            auto response = QJsonObject::fromVariantMap(order); response.insert("quantity", "1.00000000");
            response.insert("id", "order-1"); response.insert("state", "confirmed");
            return QPair<int, QJsonObject>{201, response};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "submitted");
        QVERIFY(!c.tradeResultsComplete()); f.tick(); QTRY_VERIFY(c.tradeResultsComplete());
    }

    void absoluteDeadlineClassifiesOutcomeAndReleasesExactlyOnce_data()
    {
        QTest::addColumn<bool>("post");
        QTest::newRow("read-failed") << false;
        QTest::newRow("post-unknown-even-with-complete-rejection-prefix") << true;
    }
    void absoluteDeadlineClassifiesOutcomeAndReleasesExactlyOnce()
    {
        QFETCH(bool, post); Fixture f(1000);
        f.streamRead = !post; f.streamOrder = post;
        f.overrideOrder = [](QVariantMap) { return QPair<int, QJsonObject>{400, {{"detail", "Not accepted"}}}; };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE_WITH_TIMEOUT(f.timers.size(), 1, 3000);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), post ? "unknown" : "failed");
        QVERIFY(f.progressChunks > 3); QCOMPARE(f.orders.size(), post ? 1 : 0);
        QCOMPARE(f.releases, 0); QVERIFY(c.tradeResultsBusy());
        const auto late = f.timers.first();
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1);
        late(); c.cancel(); QCoreApplication::processEvents(); QCOMPARE(f.releases, 1);
        QCOMPARE(f.orders.size(), post ? 1 : 0);
        for (int i = 1; i < c.tradeResults().size(); ++i)
            QCOMPARE(c.tradeResults().at(i).toMap().value("status").toString(), "cancelled");
    }

    void rejectionPreservesLongReasonAndRedactsCredentialsInControllerResults_data()
    {
        QTest::addColumn<bool>("withSecrets");
        QTest::newRow("whole-long-rejection") << false;
        QTest::newRow("credentials-and-unicode-controls") << true;
    }
    void rejectionPreservesLongReasonAndRedactsCredentialsInControllerResults()
    {
        QFETCH(bool, withSecrets);
        Fixture f; f.session.accounts = {f.session.accounts.first()};
        const QString explanation = QString("The broker rejected this order because ") + QString(600, 'x')
            + " the buying power is insufficient. Refresh the account before placing a smaller order.";
        const QString secrets = " access_token=access-fixture refresh_token=refresh-fixture password=pass-fixture"
            " Authorization: Bearer bearer-fixture \"password\":\"quoted pass fixture\""
            " \"refresh_token\":\"quoted refresh fixture\" \"refresh\":\"standalone-refresh-fixture\" session_token=session-fixture " + QString::fromLatin1(f.session.token);
        f.overrideOrder = [&](QVariantMap) {
            const auto detail = withSecrets ? explanation + secrets + QChar(0x202e) + QChar(0x0085) + " END" : explanation;
            return QPair<int, QJsonObject>{400, {{"detail", detail}}};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1);
        const auto row = c.tradeResults().first().toMap();
        QCOMPARE(row.value("status").toString(), "failed");
        const auto message = row.value("message").toString();
        QVERIFY2(message.startsWith(explanation), qPrintable(message));
        if (withSecrets) QVERIFY(message.endsWith(" END")); else QCOMPARE(message, explanation);
        for (const auto &secret : {"access-fixture", "refresh-fixture", "pass-fixture", "bearer-fixture",
                "quoted pass fixture", "quoted refresh fixture", "standalone-refresh-fixture", "session-fixture", "secret-fixture"})
            QVERIFY2(!message.contains(secret), secret);
        QVERIFY(!message.contains(QChar(0x202e))); QVERIFY(!message.contains(QChar(0x0085)));
        QVERIFY(!QJsonDocument::fromVariant(c.tradeResults()).toJson().contains("secret-fixture"));
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.releases, 1); QCOMPARE(f.orders.size(), 1);
    }

    void credentialBearingOrderIdsAreUnknownAndNeverPublished_data()
    {
        QTest::addColumn<QString>("id");
        QTest::newRow("equal-to-bearer") << QString("secret-fixture");
        QTest::newRow("containing-bearer") << QString("order-secret-fixture-suffix");
    }
    void credentialBearingOrderIdsAreUnknownAndNeverPublished()
    {
        QFETCH(QString, id); Fixture f;
        f.overrideOrder = [id](QVariantMap order) {
            auto response = QJsonObject::fromVariantMap(order);
            response.insert("id", id); response.insert("state", "queued");
            return QPair<int, QJsonObject>{201, response};
        };
        RHTradeController c(f.network, f.deps());
        bool credentialPublished = false;
        connect(&c, &RHTradeController::tradeResultsChanged, &c, [&] {
            credentialPublished |= QJsonDocument::fromVariant(c.tradeResults()).toJson().contains(f.session.token);
        });
        QVERIFY(c.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), 1);
        const auto row = c.tradeResults().first().toMap();
        QCOMPARE(row.value("status").toString(), "unknown");
        QVERIFY(!row.value("success").toBool()); QVERIFY(row.value("orderId").toString().isEmpty());
        QVERIFY(row.value("message").toString().contains("Check the broker before retrying"));
        QVERIFY(!credentialPublished);
        QVERIFY(!QJsonDocument::fromVariant(c.tradeResults()).toJson().contains(f.session.token));
        QVERIFY(!c.error().contains(QString::fromLatin1(f.session.token)));
        QVERIFY(c.tradeResultsBusy()); QCOMPARE(f.releases, 0);
        QVERIFY(f.delays.first() >= 2000 && f.delays.first() <= 3000);
        const auto late = f.timers.first();
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QVERIFY(!c.tradeResultsBusy());
        QCOMPARE(c.tradeResults().size(), 6);
        for (int i = 1; i < c.tradeResults().size(); ++i)
            QCOMPARE(c.tradeResults().at(i).toMap().value("status").toString(), "cancelled");
        late(); QCoreApplication::processEvents();
        QCOMPARE(f.orders.size(), 1); QCOMPARE(f.releases, 1); QVERIFY(f.timers.isEmpty());
        QCOMPARE(f.requests, (QStringList{"GET /accounts/", "GET /instruments/", "GET /quotes/", "GET /instruments/", "GET /quotes/", "POST /orders/"}));
        QVERIFY(!credentialPublished);
        QVERIFY(!QJsonDocument::fromVariant(c.tradeResults()).toJson().contains(f.session.token));
        QVERIFY(!c.error().contains(QString::fromLatin1(f.session.token)));
    }

    void ambiguousSubmissionHaltsWithoutRetryAfterCooldown_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<QString>("fault");
        QTest::newRow("server-error") << 500 << "";
        QTest::newRow("timeout") << 408 << "";
        QTest::newRow("redirect") << 302 << "";
        QTest::newRow("missing-id") << 201 << "id";
        QTest::newRow("wrong-ref") << 201 << "ref_id";
        QTest::newRow("wrong-account") << 201 << "account";
        QTest::newRow("wrong-quantity") << 201 << "quantity";
        QTest::newRow("unknown-state") << 201 << "state";
    }
    void ambiguousSubmissionHaltsWithoutRetryAfterCooldown()
    {
        QFETCH(int, status); QFETCH(QString, fault); Fixture f;
        f.overrideOrder = [=](QVariantMap order) {
            auto response = QJsonObject::fromVariantMap(order); response.insert("id", "order-1"); response.insert("state", "queued");
            if (fault == "id") response.remove("id");
            else if (!fault.isEmpty()) response.insert(fault, "wrong");
            return QPair<int, QJsonObject>{status, response};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL,MSFT", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), 1);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "unknown");
        QVERIFY(c.tradeResultsBusy()); QCOMPARE(f.releases, 0);
        QVERIFY(f.delays.first() >= 2000 && f.delays.first() <= 3000);
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.orders.size(), 1); QCOMPARE(f.releases, 1);
        for (int i = 1; i < c.tradeResults().size(); ++i) QCOMPARE(c.tradeResults().at(i).toMap().value("status").toString(), "cancelled");
    }

    void cancellationOfInFlightOrderReportsUnknownAndNeverCancelsBrokerOrder()
    {
        Fixture f; RHTradeController c(f.network, f.deps());
        f.overrideOrder = [&](QVariantMap order) {
            c.cancel();
            auto response = QJsonObject::fromVariantMap(order); response.insert("id", "order-1"); response.insert("state", "queued");
            return QPair<int, QJsonObject>{201, response};
        };
        QVERIFY(c.submit("AAPL,MSFT", "buy")); QTRY_VERIFY(c.tradeResultsComplete());
        QCOMPARE(f.orders.size(), 1); QCOMPARE(f.releases, 1);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "unknown");
        for (const auto &request : f.requests) QVERIFY(!request.contains("cancel"));
        QCoreApplication::processEvents(); QCOMPARE(f.orders.size(), 1);
    }

    void cancellationDuringFinalCooldownKeepsSubmittedResultAndInvalidatesTimer()
    {
        Fixture f; RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1); c.cancel(); QVERIFY(c.tradeResultsComplete());
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "submitted");
        f.tick(); QCoreApplication::processEvents(); QCOMPARE(f.orders.size(), 1); QCOMPARE(f.releases, 1);
    }

    void sellPositionsFailClosedOnMissingInvalidOrAmbiguousReservations_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"missing", "negative", "numeric", "excessive", "duplicate", "wrong-account"}) QTest::newRow(fault) << QString(fault);
    }
    void sellPositionsFailClosedOnMissingInvalidOrAmbiguousReservations()
    {
        QFETCH(QString, fault); Fixture f; f.session.accounts = {f.session.accounts.first()};
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() != "/positions/") return {};
            QJsonObject p{{"account", f.origin + "/accounts/a1/"}, {"instrument", f.origin + "/instruments/AAPL/"},
                {"quantity", "1"}, {"shares_held_for_sells", "0"}, {"shares_held_for_stock_grants", "0"},
                {"shares_held_for_options_collateral", "0"}, {"shares_held_for_options_events", "0"}};
            if (fault == "missing") p.remove("shares_held_for_sells");
            if (fault == "negative") p.insert("shares_held_for_sells", "-1");
            if (fault == "numeric") p.insert("quantity", 1);
            if (fault == "excessive") p.insert("shares_held_for_sells", "2");
            if (fault == "wrong-account") p.insert("account", f.origin + "/accounts/a2/");
            QJsonArray rows{p}; if (fault == "duplicate") rows.append(p);
            return {{"results", rows}, {"next", QJsonValue::Null}};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "sell")); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(c.tradeResults().first().toMap().value("status").toString(), "failed");
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QVERIFY(f.orders.isEmpty()); QCOMPARE(f.releases, 1);
    }

    void malformedAndUnsafeAccountPaginationNeverPosts_data()
    {
        QTest::addColumn<QJsonValue>("next");
        QTest::newRow("boolean") << QJsonValue(false);
        QTest::newRow("number") << QJsonValue(1);
        QTest::newRow("empty-string") << QJsonValue("");
        QTest::newRow("foreign-origin") << QJsonValue("https://evil.example/accounts/?cursor=x");
        QTest::newRow("wrong-route") << QJsonValue("/orders/?cursor=x");
        QTest::newRow("cycle") << QJsonValue("/accounts/?cursor=x");
    }
    void malformedAndUnsafeAccountPaginationNeverPosts()
    {
        QFETCH(QJsonValue, next); Fixture f;
        f.overrideRead = [&](QUrl url) -> QJsonObject {
            if (url.path() == "/accounts/") return {{"results", QJsonArray{}}, {"next", next}};
            return {};
        };
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QVERIFY(f.orders.isEmpty()); QCOMPARE(f.releases, 1);
    }

    void changedSessionDuringCooldownPreventsFurtherPosts()
    {
        Fixture f; RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy")); QTRY_COMPARE(f.timers.size(), 1);
        f.isCurrent = false; f.tick(); QTRY_VERIFY(c.tradeResultsComplete()); QCOMPARE(f.orders.size(), 1); QCOMPARE(f.releases, 1);
    }

    void secondarySessionCannotReadOrPost()
    {
        Fixture f; f.session.authKind = RHNetworkEngine::AuthKind::WebSecondaryBearer;
        RHTradeController c(f.network, f.deps()); QVERIFY(c.submit("AAPL", "buy")); QTRY_VERIFY(c.tradeResultsComplete());
        QVERIFY(f.requests.isEmpty()); QCOMPARE(f.releases, 1);
    }

    void invalidInputNeverAcquiresOrDispatches()
    {
        RHNetworkEngine network;
        int acquisitions = 0;
        RHTradeController::Dependencies deps;
        deps.acquire = [&](auto) { ++acquisitions; };
        deps.current = [](const auto &) { return true; };
        deps.release = [] {};
        deps.availabilityReason = [] { return QString(); };
        RHTradeController controller(network, deps);
        for (const auto &text : QStringList{"", "AAPL,", "A APL", "AAPL/$", ",", "AAPL,,MSFT", QString::fromUtf8("ſPY"), QString::fromUtf8("ßPY")})
            QVERIFY(!controller.submit(text, "buy"));
        QVERIFY(!controller.submit("AAPL", "short"));
        QCOMPARE(acquisitions, 0);
        QVERIFY(!controller.tradeResultsBusy());
    }
};
QTEST_GUILESS_MAIN(RHTradeControllerTest)
#include "test_rhtradecontroller.moc"
