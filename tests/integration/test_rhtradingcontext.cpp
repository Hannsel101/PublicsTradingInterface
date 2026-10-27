#include "rhcredentialstore.h"
#include "rhbrowserlogin.h"
#include "rhsessionmanager.h"
#include "rhtradingcontext.h"
#include "rhoauthgrant.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
#include <QSemaphore>
#include <QThread>
#include <atomic>

namespace {
template<class Dependencies> bool hasAsyncOwnership(const Dependencies &dependencies) {
    if constexpr (requires { dependencies.currentAsync; dependencies.currentLocal; })
        return bool(dependencies.currentAsync) && bool(dependencies.currentLocal);
    return false;
}
class Store final : public RHCredentialStore {
public:
    QHash<int, QString> tokens;
    bool failRead = false;
    int reads = 0;
    void write(int index, const QString &token, Completion done) override {
        tokens.insert(index, token);
        QTimer::singleShot(0, this, [done = std::move(done)] { done(true, {}); });
    }
    void read(int index, ReadCompletion done) override {
        ++reads;
        QTimer::singleShot(0, this, [this, index, done = std::move(done)] {
            done(!failRead, failRead ? QString() : tokens.value(index), {});
        });
    }
    void remove(int index, Completion done) override {
        tokens.remove(index); done(true, {});
    }
};

struct WorkerGate {
    QSemaphore semaphore;
    std::atomic<int> calls{0};
    std::atomic<bool> offGui{false};
    std::atomic<int> exited{0};
};
struct GateRelease {
    std::shared_ptr<WorkerGate> gate;
    ~GateRelease() { gate->semaphore.release(100); }
};
struct Fixture : QObject {
    QTemporaryDir dir;
    QTcpServer server;
    RHNetworkEngine network;
    Store store;
    SecureCredentialLocker locker{dir.filePath("profiles.dat"),
        [](bool, SecureCredentialLocker::KeyCompletion done) { done(true, QByteArray(32, 'k'), {}); }};
    std::unique_ptr<RHSessionManager> sessions;
    QString origin;
    const QString brokerageOrigin = QStringLiteral("https://api.robinhood.com");
    QList<QVariantMap> orders;
    QList<QVariantMap> renewals;
    QList<QByteArray> orderHeaders;
    QStringList methods, requests;
    QList<int> delays;
    QList<std::function<void()>> timers;
    bool ready = false;
    bool partialOrderReceipts = false, holdOrderReads = false;
    QPointer<QTcpSocket> heldOrderRead;
    QByteArray heldOrderBody;
    static QJsonObject receipt(const QVariantMap &order, QString id) {
        return {{"id", id}, {"state", "queued"}, {"account", order.value("account").toString()},
            {"instrument", order.value("instrument").toString()}, {"side", order.value("side").toString()},
            {"quantity", order.value("quantity").toString()}, {"ref_id", order.value("ref_id").toString()}};
    }
    void releaseOrderRead() {
        if (!heldOrderRead) return;
        heldOrderRead->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(heldOrderBody.size()) + "\r\nConnection: close\r\n\r\n" + heldOrderBody);
        heldOrderRead->disconnectFromHost(); heldOrderRead.clear();
    }

    explicit Fixture(int networkDeadlineMs = 1800) : network(nullptr, true, networkDeadlineMs) {
        if (!server.listen(QHostAddress::LocalHost)) return;
        origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
                connect(peer, &QTcpSocket::readyRead, this, [this, peer, bytes] {
                    *bytes += peer->readAll();
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int length = 0;
                    for (const auto &line : bytes->first(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    const auto first = bytes->first(bytes->indexOf('\n')).split(' ');
                    const QUrl url(origin + QString::fromUtf8(first.value(1)));
                    methods.append(QString::fromLatin1(first.value(0)));
                    requests.append(QString::fromLatin1(first.value(0)) + " " + url.path());
                    QJsonObject response;
                    if (first.value(0) == "POST" && url.path() == "/oauth2/token/") {
                        QVariantMap fields;
                        const QUrlQuery query(QString::fromUtf8(bytes->mid(end + 4, length)));
                        for (const auto &item : query.queryItems(QUrl::FullyDecoded)) fields.insert(item.first, item.second);
                        renewals.append(fields);
                        response = {{"access_token", "renewed-primary-fixture"},
                            {"refresh_token", "refresh-rotated-fixture"}, {"token_type", "Bearer"},
                            {"scope", "internal"}, {"expires_in", 3600}};
                    } else if (first.value(0) == "POST") {
                        orderHeaders.append(bytes->first(end));
                        QVariantMap order;
                        const QUrlQuery query(QString::fromUtf8(bytes->mid(end + 4, length)));
                        for (const auto &item : query.queryItems(QUrl::FullyDecoded)) order.insert(item.first, item.second);
                        orders.append(order);
                        const auto id = QStringLiteral("integration-order-%1").arg(orders.size());
                        response = partialOrderReceipts ? QJsonObject{{"id", id}} : receipt(order, id);
                    } else if (url.path().startsWith("/orders/")) {
                        response = receipt(orders.last(), QStringLiteral("integration-order-%1").arg(orders.size()));
                    } else if (url.path() == "/user/") {
                        response = {{"id", "integration-owner"}, {"username", "owner"}};
                    } else if (url.path() == "/accounts/") {
                        QJsonArray rows;
                        for (int i = 1; i <= 3; ++i) rows.append(QJsonObject{
                            {"account_number", QString::number(i)}, {"url", brokerageOrigin + QStringLiteral("/accounts/a%1/").arg(i)},
                            {"brokerage_account_type", i == 1 ? "individual" : i == 2 ? "ira_traditional" : "ira_roth"},
                            {"state", "active"}});
                        response.insert("results", rows); // The production Accounts contract permits absent next.
                    } else if (url.path() == "/instruments/") {
                        const auto symbol = QUrlQuery(url).queryItemValue("symbols");
                        response = {{"results", QJsonArray{QJsonObject{{"id", symbol}, {"symbol", symbol},
                            {"url", brokerageOrigin + "/instruments/" + symbol + "/"}, {"tradeable", true},
                            {"tradability", "tradable"}, {"state", "active"}}}}, {"next", QJsonValue::Null}};
                    } else if (url.path() == "/quotes/") {
                        const auto symbol = QUrlQuery(url).queryItemValue("symbols");
                        response = {{"results", QJsonArray{QJsonObject{{"symbol", symbol},
                            {"instrument", brokerageOrigin + "/instruments/" + symbol + "/"},
                            {"last_trade_price", symbol == "MSFT" ? "0.9999" : "10.25"},
                            {"ask_price", symbol == "MSFT" ? "0.0001" : "10.25"}, {"bid_price", "0.0001"}}}}, {"next", QJsonValue::Null}};
                    }
                    const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    if (holdOrderReads && first.value(0) == "GET" && url.path().startsWith("/orders/")) {
                        heldOrderRead = peer; heldOrderBody = body; bytes->clear(); return;
                    }
                    peer->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    peer->disconnectFromHost(); bytes->clear();
                });
            }
        });
        const auto route = [this](QString path) {
            return QJsonObject{{"URL", origin + path}, {"Method", "GET"},
                {"BodyEncoding", "none"}, {"HeaderSets", QJsonArray{}}};
        };
        QFile config(dir.filePath("api.json"));
        if (!config.open(QIODevice::WriteOnly)) return;
        auto refresh = route("/oauth2/token/");
        refresh.insert("Method", "POST"); refresh.insert("BodyEncoding", "form");
        config.write(QJsonDocument(QJsonObject{{"Headers", QJsonObject{}},
            {"Endpoints", QJsonObject{{"Accounts", route("/accounts/")}, {"UserProfile", route("/user/")},
                {"TokenRefresh", refresh}}}}).toJson());
        config.close();
        if (!network.loadConfiguration(config.fileName())) return;
        sessions = std::make_unique<RHSessionManager>(network, dir.filePath("sessions.json"), &store,
            nullptr, 180000, 2500, &locker);
        ready = sessions->load();
    }
    RHTradeController::Dependencies dependencies() {
        auto result = makeRHTradingDependencies(*sessions);
        result.loopbackLiveOrdersForTests = true;
        result.schedule = [this](int delay, auto callback) { delays.append(delay); timers.append(std::move(callback)); };
        return result;
    }
};
}

class RHTradingContextTest : public QObject {
    Q_OBJECT
private slots:
    void productionAdapterProvidesNonblockingOwnership() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY2(hasAsyncOwnership(makeRHTradingDependencies(*f.sessions)),
            "Production reconciliation must provide asynchronous ownership and disk-free local checks.");
    }
    void reconciliationWatchNeverCallsLegacyCurrent() {
        Fixture f; QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0));
        QTRY_VERIFY(!f.sessions->busy());
        struct Gate { QSemaphore semaphore; std::atomic<int> calls{0}; std::atomic<bool> offGui{false}; };
        auto gate = std::make_shared<Gate>();
        struct Release { std::shared_ptr<Gate> gate; ~Release() { gate->semaphore.release(100); } } release{gate};
        const auto gui = QThread::currentThread();
        f.sessions->setTradingOwnershipReaderForTests([gate, gui] {
            gate->offGui = QThread::currentThread() != gui;
            ++gate->calls; gate->semaphore.acquire();
        });
        auto deps = f.dependencies(); int legacyWatchCalls = 0;
        auto legacy = deps.current;
        deps.current = [&](const auto &context) {
            if (f.heldOrderRead) ++legacyWatchCalls;
            return legacy(context);
        };
        RHTradeController trades(f.network, std::move(deps));
        QVERIFY(trades.submit("AAPL", "buy")); QTRY_VERIFY(f.heldOrderRead);
        int heartbeat = 0; QTimer timer; timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [&] { ++heartbeat; }); timer.start();
        QTest::qWait(220);
        QCOMPARE(legacyWatchCalls, 0);
        QCOMPARE(gate->calls.load(), 1); QVERIFY(gate->offGui.load()); QTRY_VERIFY(heartbeat >= 3);
        trades.cancel(); QVERIFY(trades.tradeResultsComplete()); QVERIFY(!f.sessions->busy());
        const auto rows = trades.tradeResults();
        f.releaseOrderRead(); gate->semaphore.release(); QTest::qWait(100);
        QCOMPARE(trades.tradeResults(), rows); QCOMPARE(f.orders.size(), 1); QVERIFY(f.timers.isEmpty());
    }
    void heldWorkerRemainsResponsive_data() {
        QTest::addColumn<QString>("action");
        for (const auto &action : {"cancel", "destroy-controller", "destroy-manager", "replacement", "local-expiry",
                                   "lookup-deadline", "verification-deadline"})
            QTest::newRow(action) << QString(action);
    }
    void heldWorkerRemainsResponsive() {
        QFETCH(QString, action);
        Fixture f(action == "local-expiry" || action == "verification-deadline" ? 6000 : 1800);
        QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser);
        if (action != "local-expiry") QVERIFY(f.sessions->beginBrowserLogin());
        if (action == "local-expiry") {
            QVERIFY(f.sessions->saveAuthenticatedSession("owner", "primary-integration-fixture", 3));
        } else f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        auto gate = std::make_shared<WorkerGate>(); GateRelease release{gate};
        const auto gui = QThread::currentThread();
        f.sessions->setTradingOwnershipReaderForTests([gate, gui] {
            gate->offGui = QThread::currentThread() != gui;
            ++gate->calls; gate->semaphore.acquire(); ++gate->exited;
        });
        auto deps = f.dependencies(); int legacyCalls = 0; bool audit = false;
        auto legacy = deps.current;
        deps.current = [&](const auto &s) { if (audit) ++legacyCalls; return legacy(s); };
        if (action == "verification-deadline") deps.ownershipDeadlineMsForTests = 2000;
        auto trades = std::make_unique<RHTradeController>(f.network, std::move(deps));
        QVERIFY(trades->submit("AAPL", "buy")); QTRY_VERIFY(f.heldOrderRead); audit = true;
        QTRY_COMPARE(gate->calls.load(), 1); QVERIFY(gate->offGui.load());
        int heartbeat = 0; QTimer timer; timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [&] { ++heartbeat; }); timer.start();
        QTRY_VERIFY(heartbeat >= 3);
        if (action == "verification-deadline") {
            f.releaseOrderRead(); // Even a complete good GET cannot bypass the held ownership worker.
            QVERIFY(f.timers.isEmpty());
        }
        if (action == "cancel" || action == "replacement") {
            QTimer::singleShot(0, trades.get(), &RHTradeController::cancel);
            QTRY_VERIFY(trades->tradeResultsComplete()); QVERIFY(!f.sessions->busy());
        } else if (action == "destroy-controller") {
            trades.reset(); QVERIFY(!f.sessions->busy());
        } else if (action == "destroy-manager") {
            f.sessions.reset();
            QTRY_COMPARE(f.timers.size(), 1);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(f.timers.size(), 1, 4500);
            QCOMPARE(trades->tradeResults().first().toMap().value("status").toString(), "unknown");
            QVERIFY(trades->tradeResults().first().toMap().value("orderId").toString().isEmpty());
            const auto message = trades->tradeResults().first().toMap().value("message").toString();
            QVERIFY(message.contains(action == "verification-deadline" ? "ownership_timeout"
                : action == "local-expiry" ? "session_changed" : "transport"));
        }
        QVERIFY(heartbeat >= 3);
        QCOMPARE(legacyCalls, 0); QCOMPARE(gate->calls.load(), 1);
        if (action == "replacement") {
            audit = false; f.partialOrderReceipts = false; f.holdOrderReads = false;
            QVERIFY(trades->submit("MSFT", "buy")); QTRY_COMPARE(f.timers.size(), 1);
            QVERIFY(f.sessions->busy()); QCOMPARE(f.orders.size(), 2);
        }
        const auto rows = trades ? trades->tradeResults() : QVariantList{};
        const auto orders = f.orders.size(); const auto timers = f.timers.size();
        f.releaseOrderRead(); gate->semaphore.release(); QTRY_COMPARE(gate->exited.load(), 1);
        QTest::qWait(80);
        QCOMPARE(f.orders.size(), orders); QCOMPARE(f.timers.size(), timers);
        if (trades) QCOMPARE(trades->tradeResults(), rows);
        if (action == "replacement") QVERIFY(f.sessions->busy());
        if (trades) trades->cancel();
    }
    void asynchronousOwnershipRejectsMetadataChanges_data() {
        QTest::addColumn<QString>("change");
        for (const auto &change : {"index", "account_name", "account_id", "created_at", "credential_revision",
                                   "credential_digest", "renewal_attempted_at", "renewal_pending", "deleted", "invalid", "oversize"})
            QTest::newRow(change) << QString(change);
    }
    void asynchronousOwnershipRejectsMetadataChanges() {
        QFETCH(QString, change);
        Fixture f; QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        auto gate = std::make_shared<WorkerGate>(); GateRelease release{gate};
        f.sessions->setTradingOwnershipReaderForTests([gate] { ++gate->calls; gate->semaphore.acquire(); ++gate->exited; },
            change == "oversize" ? 128 : 64 * 1024 * 1024);
        RHTradeController trades(f.network, f.dependencies());
        QVERIFY(trades.submit("AAPL", "buy")); QTRY_VERIFY(f.heldOrderRead); QTRY_COMPARE(gate->calls.load(), 1);
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        auto document = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
        auto rows = document.value("sessions").toArray(); auto row = rows.first().toObject();
        if (change == "index") row.insert(change, 1);
        else if (change == "renewal_pending") row.insert(change, true);
        else if (change == "created_at" || change == "renewal_attempted_at")
            row.insert(change, QDateTime::currentDateTimeUtc().addSecs(5).toString(Qt::ISODateWithMs));
        else if (change == "credential_revision") row.insert(change, "999999");
        else if (change == "credential_digest") row.insert(change, QString(64, 'a'));
        else row.insert(change, "other-profile");
        rows[0] = row; if (change == "deleted") rows = {};
        document.insert("sessions", rows);
        const auto bytes = change == "invalid" ? QByteArray("{invalid") : QJsonDocument(document).toJson();
        QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(metadata.write(bytes), bytes.size()); metadata.close();
        const auto tokens = f.store.tokens;
        f.releaseOrderRead(); gate->semaphore.release();
        QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(trades.tradeResults().first().toMap().value("status").toString(), "unknown");
        QVERIFY(trades.tradeResults().first().toMap().value("orderId").toString().isEmpty());
        QCOMPARE(f.orders.size(), 1); QCOMPARE(f.store.tokens, tokens);
        QVERIFY(metadata.open(QIODevice::ReadOnly)); QCOMPARE(metadata.readAll(), bytes);
        auto finish = f.timers.takeFirst(); finish(); QTRY_VERIFY(trades.tradeResultsComplete());
        QVERIFY(!f.sessions->busy());
    }
    void receiptRequiresProbeStartedAfterLookup_data() {
        QTest::addColumn<QString>("change");
        for (const auto &change : {"account_id", "credential_revision", "unchanged"})
            QTest::newRow(change) << QString(change);
    }
    void receiptRequiresProbeStartedAfterLookup() {
        QFETCH(QString, change);
        Fixture f(6000); QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        auto gate = std::make_shared<WorkerGate>(); GateRelease release{gate};
        const auto gui = QThread::currentThread();
        f.sessions->setTradingOwnershipReaderForTests({}, 64 * 1024 * 1024, [gate, gui] {
            gate->offGui = QThread::currentThread() != gui;
            // Hold the computed result, not the read: probe 1 already validated the old tuple.
            if (++gate->calls <= 2) gate->semaphore.acquire();
            ++gate->exited;
        });
        auto deps = f.dependencies(); int releases = 0, probeStarted = 0;
        QList<bool> results;
        const auto currentAsync = deps.currentAsync;
        deps.currentAsync = [&](const auto &s, auto done) {
            ++probeStarted;
            currentAsync(s, [&, done = std::move(done)](bool ok) {
                results.append(ok); done(ok);
            });
        };
        const auto releaseLease = deps.release;
        deps.release = [&] { ++releases; releaseLease(); };
        RHTradeController trades(f.network, std::move(deps));
        QVERIFY(trades.submit("AAPL,MSFT", "buy")); QTRY_VERIFY(f.heldOrderRead);
        QTRY_COMPARE(gate->calls.load(), 1); QVERIFY(gate->offGui.load());
        const auto tokens = f.store.tokens;
        if (change != "unchanged") {
            QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
            auto document = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
            auto rows = document.value("sessions").toArray(); auto row = rows.first().toObject();
            row.insert(change, change == "account_id" ? "replacement-owner" : "999999");
            rows[0] = row; document.insert("sessions", rows);
            QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const auto bytes = QJsonDocument(document).toJson(); QCOMPARE(metadata.write(bytes), bytes.size());
        }
        QSignalSpy disconnected(f.heldOrderRead, &QTcpSocket::disconnected);
        f.releaseOrderRead(); QTRY_COMPARE(disconnected.size(), 1);
        QTest::qWait(150); // Pump the GUI finished handler while the old successful result is held.
        QCOMPARE(probeStarted, 1); QVERIFY(f.timers.isEmpty());
        gate->semaphore.release(); QTRY_COMPARE(results.size(), 1); QVERIFY(results.first());
        // A periodic success must not promote even though its callback arrived after GET finished.
        QCOMPARE(trades.tradeResults().first().toMap().value("status").toString(), "pending");
        QVERIFY(f.timers.isEmpty());
        QTRY_COMPARE(gate->calls.load(), 2); QCOMPARE(probeStarted, 2);
        QVERIFY(trades.tradeResults().first().toMap().value("orderId").toString().isEmpty());
        QVERIFY(f.sessions->busy()); QCOMPARE(releases, 0); QCOMPARE(f.orders.size(), 1);
        gate->semaphore.release(); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(results.size(), 2); QCOMPARE(results.last(), change == "unchanged");
        const auto first = trades.tradeResults().first().toMap();
        QCOMPARE(first.value("status").toString(), change == "unchanged" ? "submitted" : "unknown");
        if (change != "unchanged") {
            QVERIFY(first.value("message").toString().contains("session_changed"));
            QVERIFY(!first.value("message").toString().contains("ownership_timeout"));
            QVERIFY(first.value("orderId").toString().isEmpty());
        }
        QCOMPARE(f.orders.size(), 1); QCOMPARE(f.requests.count("POST /orders/"), 1);
        QCOMPARE(f.requests.count("GET /orders/integration-order-1/"), 1);
        QCOMPARE(f.store.tokens, tokens); QCOMPARE(releases, 0);
        // The verified control continues serially through every account and the final cooldown.
        const int expectedOrders = change == "unchanged" ? 6 : 1;
        for (int i = 0; i < expectedOrders; ++i) {
            QTRY_COMPARE(f.timers.size(), 1); QCOMPARE(f.orders.size(), i + 1);
            QVERIFY(f.sessions->busy()); QVERIFY(trades.tradeResultsBusy()); QCOMPARE(releases, 0);
            QVERIFY(f.delays.at(i) >= 2000 && f.delays.at(i) <= 3000);
            auto cooldown = f.timers.takeFirst(); cooldown();
            if (i + 1 < expectedOrders) {
                QTRY_VERIFY(f.heldOrderRead); QCOMPARE(f.orders.size(), i + 2);
                QVERIFY(f.timers.isEmpty()); f.releaseOrderRead();
            }
        }
        QTRY_VERIFY(trades.tradeResultsComplete()); QVERIFY(!f.sessions->busy()); QCOMPARE(releases, 1);
        QCOMPARE(f.orders.size(), expectedOrders); QVERIFY(f.timers.isEmpty());
        const auto finalRows = trades.tradeResults(); QTest::qWait(100);
        QCOMPARE(trades.tradeResults(), finalRows); QCOMPARE(f.orders.size(), expectedOrders); QCOMPARE(releases, 1);
        QVERIFY(probeStarted >= 2);
    }
    void completedLookupWaitsForWorkerWithoutLegacyReceiptCheck() {
        Fixture f(6000); QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        auto gate = std::make_shared<WorkerGate>(); GateRelease release{gate};
        f.sessions->setTradingOwnershipReaderForTests([gate] { if (++gate->calls == 1) gate->semaphore.acquire(); });
        auto deps = f.dependencies(); bool audit = false; int legacyCalls = 0;
        const auto legacy = deps.current;
        deps.current = [&](const auto &s) { if (audit) ++legacyCalls; return legacy(s); };
        RHTradeController trades(f.network, std::move(deps));
        QVERIFY(trades.submit("AAPL", "buy")); QTRY_VERIFY(f.heldOrderRead); audit = true;
        QTRY_COMPARE(gate->calls.load(), 1); f.releaseOrderRead();
        int heartbeat = 0; QTimer timer; timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [&] { ++heartbeat; }); timer.start();
        QTRY_VERIFY(heartbeat >= 3); QVERIFY(f.timers.isEmpty());
        QVERIFY(trades.tradeResults().first().toMap().value("orderId").toString().isEmpty());
        gate->semaphore.release(); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(legacyCalls, 0); QCOMPARE(gate->calls.load(), 2);
        QCOMPARE(trades.tradeResults().first().toMap().value("status").toString(), "submitted");
        QCOMPARE(f.orders.size(), 1); trades.cancel();
    }
    void ownershipRefreshIsBoundedAndNeverPermanentlyCached() {
        Fixture f(6000); QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        auto calls = std::make_shared<std::atomic<int>>(0);
        f.sessions->setTradingOwnershipReaderForTests([calls] { ++*calls; });
        RHTradeController trades(f.network, f.dependencies());
        QVERIFY(trades.submit("AAPL", "buy")); QTRY_VERIFY(f.heldOrderRead);
        const int initial = calls->load(); QTest::qWait(1050);
        QVERIFY(calls->load() >= initial + 2); QVERIFY(calls->load() <= initial + 5);
        QFile metadata(f.dir.filePath("sessions.json")); QVERIFY(metadata.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
        auto rows = object.value("sessions").toArray(); auto row = rows.first().toObject();
        row.insert("account_id", "replacement-owner"); rows[0] = row; object.insert("sessions", rows);
        QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto bytes = QJsonDocument(object).toJson(); QCOMPARE(metadata.write(bytes), bytes.size()); metadata.close();
        // A prior successful check must not authorize this now-completed GET.
        f.releaseOrderRead(); QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(trades.tradeResults().first().toMap().value("status").toString(), "unknown");
        QCOMPARE(f.orders.size(), 1); trades.cancel();
    }
    void missingAsyncOwnershipFailsClosedWithoutLegacyPolling() {
        Fixture f; QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        auto deps = f.dependencies(); deps.currentAsync = {};
        RHTradeController trades(f.network, std::move(deps)); QVERIFY(trades.submit("AAPL", "buy"));
        QTRY_COMPARE(f.timers.size(), 1);
        QCOMPARE(trades.tradeResults().first().toMap().value("status").toString(), "unknown");
        QVERIFY(trades.tradeResults().first().toMap().value("message").toString().contains("ownership_unavailable"));
        QCOMPARE(f.orders.size(), 1); trades.cancel();
    }
    void verifiedManagerLeaseSurvivesReadOnlyReconciliation() {
        Fixture f; QVERIFY(f.ready); f.partialOrderReceipts = true; f.holdOrderReads = true;
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser); QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy()); QCOMPARE(f.sessions->rowCount(), 1);
        QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        RHTradeController trades(f.network, f.dependencies()); QVERIFY(trades.available());
        QVERIFY(trades.submit("AAPL", "buy"));
        for (int i = 0; i < 3; ++i) {
            QTRY_VERIFY(f.heldOrderRead); QCOMPARE(f.orders.size(), i + 1);
            QVERIFY(f.timers.isEmpty()); QVERIFY(f.sessions->busy()); QVERIFY(trades.tradeResultsBusy());
            QCOMPARE(f.requests.last(), QString("GET /orders/integration-order-%1/").arg(i + 1));
            QVERIFY(trades.tradeResults().at(i).toMap().value("orderId").toString().isEmpty());
            f.sessions->clearActiveSession(); QVERIFY(f.sessions->sessionActive());
            f.releaseOrderRead(); QTRY_COMPARE(f.timers.size(), 1);
            QCOMPARE(trades.tradeResults().at(i).toMap().value("status").toString(), "submitted");
            QVERIFY(f.sessions->busy()); QVERIFY(f.delays.at(i) >= 2000 && f.delays.at(i) <= 3000);
            auto callback = f.timers.takeFirst(); callback();
        }
        QTRY_VERIFY(trades.tradeResultsComplete()); QVERIFY(!f.sessions->busy()); QCOMPARE(f.orders.size(), 3);
        f.sessions->clearActiveSession(); QVERIFY(!f.sessions->sessionActive());
    }
    void renewedOAuthBundleEnablesOrdersWithFreshBearerOnly() {
        Fixture f; QVERIFY(f.ready);
        const QJsonObject initial{{"access_token", "initial-primary-fixture"},
            {"refresh_token", "refresh-original-fixture"}, {"token_type", "Bearer"},
            {"scope", "internal"}, {"expires_in", 3600}};
        const QString device = QStringLiteral("12345678-1234-4123-8123-123456789abc");
        QVERIFY(f.sessions->acceptOAuthGrant(initial, "integration-client", device));
        QTRY_VERIFY(!f.sessions->busy());
        QCOMPARE(f.sessions->rowCount(), 1);
        QVERIFY(f.sessions->data(f.sessions->index(0), RHSessionManager::CanRefreshRole).toBool());
        QVERIFY(f.sessions->initializeSession(0));
        QTRY_VERIFY(!f.sessions->busy());
        QVERIFY2(f.sessions->sessionActive(), qPrintable(f.sessions->error()));
        QCOMPARE(f.renewals.size(), 1);
        const auto renewal = f.renewals.first();
        QCOMPARE(renewal.value("grant_type").toString(), QStringLiteral("refresh_token"));
        QCOMPARE(renewal.value("refresh_token").toString(), QStringLiteral("refresh-original-fixture"));
        QCOMPARE(renewal.value("client_id").toString(), QStringLiteral("integration-client"));
        QCOMPARE(renewal.value("device_token").toString(), device);
        QVERIFY(!renewal.contains("password")); QVERIFY(!renewal.contains("mfa_code"));
        RHOAuthGrant saved;
        QVERIFY(RHOAuthGrant::deserialize(f.store.tokens.value(0), &saved));
        QCOMPARE(saved.accessToken(), QByteArray("renewed-primary-fixture"));
        QCOMPARE(saved.refreshToken(), QByteArray("refresh-rotated-fixture"));
        RHTradeController trades(f.network, f.dependencies());
        QVERIFY2(trades.available(), qPrintable(trades.unavailableReason()));
        const auto before = f.requests.size();
        QVERIFY(trades.submit("AAPL,MSFT,aapl", "buy"));
        for (int i = 0; i < 6; ++i) {
            QTRY_VERIFY2(f.timers.size() == 1, qPrintable(trades.error()));
            QCOMPARE(f.orders.size(), i + 1);
            QCOMPARE(f.requests.mid(before, 6), (QStringList{"GET /accounts/", "GET /instruments/", "GET /quotes/", "GET /instruments/", "GET /quotes/", "POST /orders/"}));
            QCOMPARE(f.orders.at(i).value("type").toString(), i < 3 ? "market" : "limit");
            QCOMPARE(f.orders.at(i).value("price").toString(), i < 3 ? "10.25" : "0.0101");
            QCOMPARE(f.orders.at(i).value("bid_ask_timestamp"), f.orders.at(i < 3 ? 0 : 3).value("bid_ask_timestamp"));
            QVERIFY(!f.orders.at(i).contains("preset_percent_limit"));
            QVERIFY(f.sessions->busy());
            QVERIFY(f.orderHeaders.at(i).contains("Authorization: Bearer renewed-primary-fixture\r\n"));
            QVERIFY(!f.orderHeaders.at(i).contains("initial-primary-fixture"));
            QVERIFY(!f.orderHeaders.at(i).contains("refresh-"));
            auto callback = f.timers.takeFirst(); callback();
        }
        QTRY_VERIFY(trades.tradeResultsComplete());
        const auto presentation = QJsonDocument::fromVariant(trades.tradeResults()).toJson();
        QVERIFY(!presentation.contains("renewed-primary-fixture"));
        QVERIFY(!presentation.contains("refresh-rotated-fixture"));
        QVERIFY(!f.sessions->busy()); QCOMPARE(f.renewals.size(), 1);
    }
    void actualSessionLeaseDrivesAllAccountsAndFinalCooldown() {
        Fixture f; QVERIFY(f.ready);
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser);
        QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptPrimaryBrowserToken("primary-integration-fixture");
        QTRY_VERIFY(!f.sessions->busy());
        QCOMPARE(f.sessions->rowCount(), 1);
        QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        RHTradeController trades(f.network, f.dependencies());
        connect(f.sessions.get(), &RHSessionManager::busyChanged, &trades, &RHTradeController::refreshAvailability);
        QVERIFY(trades.available());
        QVERIFY(trades.submit("AAPL", "buy"));
        for (int i = 0; i < 3; ++i) {
            QTRY_VERIFY2(f.timers.size() == 1, qPrintable(trades.error()));
            QCOMPARE(f.orders.size(), i + 1);
            QVERIFY(f.sessions->busy()); QVERIFY(trades.tradeResultsBusy());
            f.sessions->clearActiveSession(); QVERIFY(f.sessions->sessionActive());
            QCOMPARE(f.orders.at(i).value("account").toString(), f.brokerageOrigin + QStringLiteral("/accounts/a%1/").arg(i + 1));
            QCOMPARE(f.orders.at(i).value("quantity").toString(), QStringLiteral("1"));
            QVERIFY(f.delays.at(i) >= 2000 && f.delays.at(i) <= 3000);
            auto callback = f.timers.takeFirst(); callback();
        }
        QTRY_VERIFY(trades.tradeResultsComplete());
        QVERIFY(!f.sessions->busy()); QCOMPARE(f.orders.size(), 3);
        QVERIFY(trades.available());
        for (const auto &row : trades.tradeResults()) QCOMPARE(row.toMap().value("status").toString(), QStringLiteral("submitted"));
        f.sessions->clearActiveSession(); QVERIFY(!f.sessions->sessionActive());
    }
    void failedSecureStoreAcquisitionCannotLeakSessionLock() {
        Fixture f; QVERIFY(f.ready);
        QVERIFY(f.sessions->saveAuthenticatedSession("owner", "integration-fixture", 3600));
        QTRY_VERIFY(!f.sessions->busy());
        QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        f.store.failRead = true;
        RHTradeController trades(f.network, f.dependencies());
        QVERIFY(trades.submit("AAPL", "buy"));
        QTRY_VERIFY(trades.tradeResultsComplete());
        QVERIFY2(!f.sessions->busy(), "A failed token read must release the real session lease.");
        QVERIFY(f.orders.isEmpty()); QVERIFY(!trades.error().isEmpty());
        f.sessions->clearActiveSession(); QVERIFY(!f.sessions->sessionActive());
    }
    void secondarySessionNeverAcquiresTradingToken() {
        Fixture f; QVERIFY(f.ready);
        RHBrowserLogin browser({}, nullptr, RHBrowserLogin::BackendMode::Test);
        f.sessions->setBrowserLogin(&browser);
        QVERIFY(f.sessions->beginBrowserLogin());
        f.sessions->acceptBrowserToken("secondary-fixture", QDateTime::currentDateTimeUtc().addSecs(3600));
        QTRY_VERIFY(!f.sessions->busy());
        QCOMPARE(f.sessions->rowCount(), 1);
        QVERIFY(f.sessions->startSession(0)); QTRY_VERIFY(!f.sessions->busy());
        const int reads = f.store.reads;
        RHTradeController trades(f.network, f.dependencies());
        QVERIFY(!trades.available()); QVERIFY(trades.unavailableReason().contains("read-only"));
        QVERIFY(!trades.submit("AAPL", "buy"));
        QCOMPARE(f.store.reads, reads); QVERIFY(f.orders.isEmpty()); QVERIFY(!f.sessions->busy());
    }
};
QTEST_GUILESS_MAIN(RHTradingContextTest)
#include "test_rhtradingcontext.moc"
