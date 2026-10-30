#pragma once
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
#include <utility>

namespace TelegramRobinhoodFixture {
// Synthetic provider-shaped loopback fixtures; no production store/keychain I/O.
class Store final : public RHCredentialStore {
public:
    QHash<int, QString> tokens;
    bool failRead = false;
    int reads = 0;
    QList<int> readIndices;
    void write(int index, const QString &token, Completion done) override {
        tokens.insert(index, token);
        QTimer::singleShot(0, this, [done = std::move(done)] { done(true, {}); });
    }
    void read(int index, ReadCompletion done) override {
        ++reads; readIndices.append(index);
        QTimer::singleShot(0, this, [this, index, done = std::move(done)] {
            done(!failRead, failRead ? QString() : tokens.value(index), {});
        });
    }
    void remove(int index, Completion done) override {
        tokens.remove(index); done(true, {});
    }
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
    std::function<void()> onOrder;
    QList<QVariantMap> renewals;
    QList<QByteArray> orderHeaders;
    QStringList methods, requests;
    QList<int> delays;
    QList<std::function<void()>> timers;
    bool ready = false;
    int postStatus = 200;
    QString failedReadPath;
    int readStatus = 200;
    bool malformedPost = false, dropPost = false;
    int accountCount = 3;
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
                        if (onOrder) onOrder();
                        const auto id = QStringLiteral("integration-order-%1").arg(orders.size());
                        response = partialOrderReceipts ? QJsonObject{{"id", id}} : receipt(order, id);
                    } else if (url.path().startsWith("/orders/")) {
                        response = receipt(orders.last(), QStringLiteral("integration-order-%1").arg(orders.size()));
                    } else if (url.path() == "/user/") {
                        response = {{"id", "integration-owner"}, {"username", "owner"}};
                    } else if (url.path() == "/accounts/") {
                        QJsonArray rows;
                        for (int i = 1; i <= accountCount; ++i) rows.append(QJsonObject{
                            {"account_number", QString::number(i)}, {"url", brokerageOrigin + QStringLiteral("/accounts/a%1/").arg(i)},
                            {"brokerage_account_type", i == 1 ? "individual" : i == 2 ? "ira_traditional" : "ira_roth"},
                            {"state", "active"}});
                        response.insert("results", rows); // The production Accounts contract permits absent next.
                    } else if (url.path() == "/positions/") {
                        const auto number = QUrlQuery(url).queryItemValue("account_number");
                        QJsonArray positions;
                        if (number != "3") positions.append(QJsonObject{{"account", brokerageOrigin + "/accounts/a" + number + "/"},
                            {"instrument", brokerageOrigin + "/instruments/AAPL/"}, {"quantity", number == "1" ? "5.75" : "2.25"},
                            {"shares_held_for_sells", "0.25"}, {"shares_held_for_stock_grants", "0"},
                            {"shares_held_for_options_collateral", "0"}, {"shares_held_for_options_events", "0"}});
                        response = {{"results", positions}, {"next", QJsonValue::Null}};
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
                    const bool orderPost = first.value(0) == "POST" && url.path() == "/orders/";
                    if (orderPost && dropPost) { peer->disconnectFromHost(); bytes->clear(); return; }
                    const bool readFailure = first.value(0) == "GET" && url.path() == failedReadPath;
                    const int status = orderPost ? postStatus : readFailure ? readStatus : 200;
                    if (status >= 400) response = {{"detail", "PROVIDER-SECRET-DO-NOT-REPORT"}};
                    const auto body = orderPost && malformedPost ? QByteArray("{invalid") : QJsonDocument(response).toJson(QJsonDocument::Compact);
                    if (holdOrderReads && first.value(0) == "GET" && url.path().startsWith("/orders/")) {
                        heldOrderRead = peer; heldOrderBody = body; bytes->clear(); return;
                    }
                    peer->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
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


