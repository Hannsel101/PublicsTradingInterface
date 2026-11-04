#pragma once
#include "rhcredentialstore.h"
#include "rhbrowserlogin.h"
#include "rhsessionmanager.h"
#include "rhtradingcontext.h"
#include "rhoauthgrant.h"
#include "rhnativeauthclient.h"
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
#include <utility>

namespace TelegramRobinhoodAllSessionsFixture {
// Synthetic provider-shaped loopback fixtures; no production store/keychain I/O.
// RAM-encrypted secrets exercise production envelope decoding without keychains.
class Store final : public RHCredentialStore {
public:
    QHash<int, std::shared_ptr<SecureCredentialLocker::PendingCredentials>> secrets;
    bool failRead = false, holdRead = false;
    QList<int> readIndices;
    QList<std::function<void()>> pendingReads;
    void write(int index, const QString &secret, Completion done) override {
        auto encrypted = std::make_shared<SecureCredentialLocker::PendingCredentials>();
        const bool ok = encrypted->set("fixture-store", secret);
        if (ok) secrets.insert(index, encrypted);
        QTimer::singleShot(0, this, [done = std::move(done), ok] { done(ok, {}); });
    }
    void read(int index, ReadCompletion done) override {
        readIndices.append(index);
        auto callback = [this, index, done = std::move(done)] {
            QString ignored, secret;
            const auto entry = secrets.value(index);
            const bool ok = !failRead && (!entry || entry->read(ignored, secret));
            done(ok, secret, {});
        };
        if (holdRead) pendingReads.append(std::move(callback));
        else QTimer::singleShot(0, this, std::move(callback));
    }
    void remove(int index, Completion done) override { secrets.remove(index); done(true, {}); }
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
    QString loginEmail, loginId;
    QHash<QString, QPair<QString, QString>> principals;
    std::unique_ptr<RHNativeAuthClient> auth;
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
                        const QString access = "native-fixture-" + loginId;
                        principals.insert(access, {loginId, loginEmail});
                        response = {{"access_token", access},
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
                        QString token;
                        for (const auto &line : bytes->first(end).split('\n'))
                            if (line.toLower().startsWith("authorization: bearer ")) token = QString::fromUtf8(line.mid(22).trimmed());
                        const auto identity = principals.value(token, {"integration-owner", "legacy@example.test"});
                        response = {{"id", identity.first}, {"email", identity.second}, {"username", "legacy-label"}};
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
        auth = std::make_unique<RHNativeAuthClient>(RHNativeAuthClient::TestTransport{QUrl(origin), 1000, 2000, 1});
        sessions->setNativeAuthClient(auth.get());
        ready = sessions->load();
    }
    void saveNative(const QString &email, const QString &id) {
        loginEmail = email; loginId = id;
        QVERIFY(sessions->beginNativeLogin());
        QVERIFY(sessions->signIn(email, "fixture-native-password"));
        QTRY_VERIFY(!sessions->busy());
        QVERIFY2(sessions->error().isEmpty(), qPrintable(sessions->error()));
    }
    bool editMetadata(std::function<void(QJsonArray &)> edit) {
        QFile file(dir.filePath("sessions.json")); if (!file.open(QIODevice::ReadOnly)) return false;
        auto object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto rows = object.value("sessions").toArray(); edit(rows); object.insert("sessions", rows);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(QJsonDocument(object).toJson()); file.close(); return sessions->load();
    }
    bool setExpiry(int slot, QDateTime expiry) {
        // Change only fixture time evidence, retaining the exact token, native owner,
        // password, opaque profile metadata and the current credential revision.
        const auto encrypted = store.secrets.value(slot);
        QString ignored, secret;
        RHOAuthGrant grant;
        if (!expiry.isValid() || !encrypted || !encrypted->read(ignored, secret)
            || !RHOAuthGrant::deserialize(secret, &grant)) return false;
        grant.expiresAt = expiry.toUTC();
        grant.obtainedAt = grant.expiresAt.addSecs(-grant.response.value("expires_in").toInteger());
        const auto updated = grant.serialize();
        if (updated.isEmpty()) return false;
        const auto replacement = std::make_shared<SecureCredentialLocker::PendingCredentials>();
        if (!replacement->set("fixture-store", updated)) return false;
        store.secrets.insert(slot, replacement);
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(updated.toUtf8(), QCryptographicHash::Sha256).toHex());
        bool found = false;
        const bool loaded = editMetadata([&](QJsonArray &rows) {
            for (int i = 0; i < rows.size(); ++i) {
                auto row = rows[i].toObject();
                if (row.value("index").toInt() != slot) continue;
                row.insert("created_at", grant.obtainedAt.toString(Qt::ISODateWithMs));
                row.insert("expires_at", grant.expiresAt.toString(Qt::ISODateWithMs));
                row.insert("credential_digest", digest);
                rows[i] = row;
                found = true;
            }
        });
        return loaded && found;
    }
    bool expire(int slot) {
        return setExpiry(slot, QDateTime::currentDateTimeUtc().addSecs(-60));
    }
    RHTradeController::Dependencies dependencies() {
        auto result = makeRHTradingDependencies(*sessions);
        result.loopbackLiveOrdersForTests = true;
        result.schedule = [this](int delay, auto callback) { delays.append(delay); timers.append(std::move(callback)); };
        return result;
    }
};
}


