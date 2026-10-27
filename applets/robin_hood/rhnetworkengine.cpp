#include "rhnetworkengine.h"
#include "rhoauthgrant.h"

#include <QFile>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QSet>
#include <QDateTime>
#include <QTimer>
#include <QSslConfiguration>
#include <QSslSocket>

namespace {
bool expand(const QString &source, const QVariantMap &values, bool path, QString *result)
{
    static const QRegularExpression placeholder(QStringLiteral(R"(\{([A-Za-z_][A-Za-z_0-9]*)\})"));
    QString output;
    qsizetype cursor = 0;
    auto matches = placeholder.globalMatch(source);
    while (matches.hasNext()) {
        const auto match = matches.next();
        output += source.mid(cursor, match.capturedStart() - cursor);
        const QString name = match.captured(1);
        if (!values.contains(name) || values.value(name).toString().isEmpty())
            return false;
        const QString value = values.value(name).toString();
        output += path ? QString::fromLatin1(QUrl::toPercentEncoding(value)) : value;
        cursor = match.capturedEnd();
    }
    output += source.mid(cursor);
    if (output.contains(QLatin1Char('\r')) || output.contains(QLatin1Char('\n')))
        return false;
    *result = output;
    return true;
}

bool permittedUrl(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userName().isEmpty()
        || !url.password().isEmpty() || url.hasFragment())
        return false;
    if (url.scheme() == QStringLiteral("https"))
        return true;
    return url.scheme() == QStringLiteral("http")
        && (url.host() == QStringLiteral("localhost")
            || url.host() == QStringLiteral("127.0.0.1")
            || url.host() == QStringLiteral("::1"));
}

QString formValue(const QVariant &value)
{
    if (value.metaType().id() == QMetaType::Bool)
        return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    return value.toString();
}
}

RHNetworkEngine::RHNetworkEngine(QObject *parent, bool allowLoopbackForTests, int loopbackStockDeadlineMsForTests)
    : QObject(parent), m_allowLoopbackForTests(allowLoopbackForTests),
      m_loopbackStockDeadlineMsForTests(allowLoopbackForTests && loopbackStockDeadlineMsForTests > 0
          && loopbackStockDeadlineMsForTests < 16000 ? loopbackStockDeadlineMsForTests : 16000) {}

bool RHNetworkEngine::safeBearer(const QByteArray &token)
{
    if (token.isEmpty() || token.size() > 16384)
        return false;
    for (const char byte : token) {
        const auto c = static_cast<unsigned char>(byte);
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9') && c != '-' && c != '.' && c != '_'
            && c != '~' && c != '+' && c != '/' && c != '=')
            return false;
    }
    return true;
}

QNetworkReply *RHNetworkEngine::sendAuthenticated(const QString &endpoint,
    const QByteArray &token, AuthKind kind, QString *error, const QUrl &location,
    const QString &accountNumber)
{
    if (error)
        error->clear();
    const auto fail = [error]() -> QNetworkReply * {
        if (error)
            *error = QStringLiteral("Authenticated read-only request is unsafe or unavailable.");
        return nullptr;
    };
    const QJsonObject entry = m_endpoints.value(endpoint).toObject();
    if (!m_configured || entry.isEmpty() || !entry.value("Enabled").toBool(true)
        || entry.value("Method").toString() != QStringLiteral("GET") || !safeBearer(token))
        return fail();
    const QString text = entry.value("URL").toString();
    const QUrl base(text, QUrl::StrictMode);
    QUrl url = location.isEmpty() ? base : base.resolved(location);
    if (url.scheme() != base.scheme() || url.host() != base.host() || url.port() != base.port()
        || !permittedUrl(url) || url.path(QUrl::FullyEncoded).contains('%'))
        return fail();
    const bool profile = endpoint == QStringLiteral("UserProfile")
        && url.path() == QStringLiteral("/user/") && !url.hasQuery();
    const bool accounts = endpoint == QStringLiteral("Accounts")
        && url.path() == QStringLiteral("/accounts/")
        && (!url.hasQuery() || url.query() == QStringLiteral("default_to_all_accounts=true"));
    const bool positions = endpoint == QStringLiteral("CachedPositions")
        && base.path() == QStringLiteral("/positions/") && url.path() == base.path()
        && QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z")).match(accountNumber).hasMatch();
    const bool instrument = endpoint == QStringLiteral("Instruments")
        && base.path() == QStringLiteral("/instruments/") && !url.hasQuery()
        && QRegularExpression(QStringLiteral("\\A/instruments/[A-Za-z0-9_-]{1,128}/\\z")).match(url.path()).hasMatch();
    if (!profile && !accounts && !positions && !instrument) return fail();
    if (positions) {
        QUrlQuery query(url);
        QSet<QString> keys;
        for (const auto &pair : query.queryItems()) {
            if (keys.contains(pair.first) || !QStringList{"nonzero", "account_number", "cursor"}.contains(pair.first)
                || (pair.first == "account_number" && pair.second != accountNumber)
                || (pair.first == "nonzero" && pair.second != "true")
                || pair.second.size() > 2048) return fail();
            keys.insert(pair.first);
        }
        query.removeAllQueryItems("account_number"); query.addQueryItem("account_number", accountNumber);
        query.removeAllQueryItems("nonzero"); query.addQueryItem("nonzero", "true");
        url.setQuery(query);
    }
    const bool official = url.scheme() == QStringLiteral("https")
        && (url.host() == QStringLiteral("api.robinhood.com")
            || url.host() == QStringLiteral("phoenix.robinhood.com"))
        && (url.port(-1) == -1 || url.port() == 443);
    const bool loopback = m_allowLoopbackForTests && permittedUrl(url)
        && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1")
            || url.host() == QStringLiteral("localhost"));
    if (!permittedUrl(url) || (!official && !loopback) || text.contains('{')
        || url.toString().contains(QString::fromLatin1(token))
        || url.query().contains(QStringLiteral("token"), Qt::CaseInsensitive))
        return fail();
    // The secondary cookie is NOT the web app's primary access token. Its
    // Compatibility remains provisional: configured identity and account-scoped
    // holdings/instrument GETs only. Never invent a Phoenix route.
    if (!loopback && url.host() != QStringLiteral("api.robinhood.com")) return fail();
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(16000);
    for (const auto &set : entry.value("HeaderSets").toArray()) {
        const QJsonObject headers = m_headerSets.value(set.toString()).toObject();
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            const QString name = it.key().toLower();
            if (name == "authorization" || name == "cookie" || name == "accept-encoding"
                || name == "host" || name == "proxy-authorization")
                continue;
            const QString value = it.value().toString();
            if (value.contains('{') || value.contains('\r') || value.contains('\n'))
                return fail();
            request.setRawHeader(it.key().toUtf8(), value.toUtf8());
        }
    }
    // Provisional cookie -> Bearer mapping: not an OAuth grant. The caller must
    // validate UserProfile and Accounts read-only before publishing any token.
    switch (kind) {
    case AuthKind::OAuthBearer:
    case AuthKind::WebSecondaryBearer:
        request.setRawHeader("Authorization", QByteArray("Bearer ") + token);
        break;
    default: return fail();
    }
    return m_network.get(request);
}

namespace {
bool stockMatch(const QString &text, const QString &pattern)
{
    return QRegularExpression("\\A(?:" + pattern + ")\\z").match(text).hasMatch();
}
QNetworkRequest stockRequest(const QUrl &url, const QByteArray &token)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(16000);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + token);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Robinhood-API-Version", "1.431.4");
    return request;
}
QNetworkReply *boundedStockReply(QNetworkReply *reply, int loopbackDeadlineMs)
{
    // The transfer timeout is inactivity-only. This reply-owned timer is never
    // restarted by download/upload progress and is stopped on any completion.
    const bool loopback = QStringList{"localhost", "127.0.0.1", "::1"}.contains(reply->url().host());
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    deadline->setTimerType(Qt::PreciseTimer);
    QObject::connect(deadline, &QTimer::timeout, reply, [reply] {
        if (!reply->isFinished()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, deadline, &QTimer::stop);
    deadline->start(loopback ? loopbackDeadlineMs : 16000);
    reply->setReadBufferSize(1024 * 1024 + 1);
    QObject::connect(reply, &QIODevice::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024) reply->abort();
    });
    return reply;
}
}

QNetworkReply *RHNetworkEngine::refreshOAuth(const RHOAuthGrant &grant, QString *error)
{
    if (error) error->clear();
    const auto fail = [error]() -> QNetworkReply * {
        if (error) *error = QStringLiteral("OAuth refresh was not dispatched: invalid grant or unavailable secure route.");
        return nullptr;
    };
    // Revalidate public value fields as well as retained renewal context. Expired
    // access tokens are intentionally allowed: refresh credentials are separate.
    RHOAuthGrant checked;
    if (!grant.primary() || !RHOAuthGrant::deserialize(grant.serialize(), &checked)) return fail();
    const auto entry = m_endpoints.value("TokenRefresh").toObject();
    const QString text = entry.value("URL").toString();
    const QUrl url(text, QUrl::StrictMode);
    const bool official = url.scheme() == "https" && url.host() == "api.robinhood.com" && url.port(443) == 443;
    const bool loopback = m_allowLoopbackForTests
        && QStringList{"127.0.0.1", "::1", "localhost"}.contains(url.host());
    if (!m_configured || !entry.value("Enabled").toBool(true) || entry.value("Method") != "POST"
        || entry.value("BodyEncoding") != "form" || !permittedUrl(url) || (!official && !loopback)
        || url.path(QUrl::FullyEncoded) != "/oauth2/token/" || url.hasQuery() || text.contains('{') || text.contains('%')) return fail();
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(16000);
    // Do not copy configured headers/defaults into this secret-bearing boundary.
    auto tls = QSslConfiguration::defaultConfiguration();
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(tls);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
    request.setRawHeader("X-Robinhood-API-Version", "1.431.4");
    // RFC 6749 section 6: form grant_type/refresh_token; preserve the issuing
    // client and never request broader scope. Robinhood's public webapp module
    // 468781 (postTokenRefreshRequest) + issueTokenRefreshRequest also supply
    // device_token and scope. No requested/fixed expires_in is sent on renewal.
    const QVariantMap fields{{"grant_type", "refresh_token"}, {"refresh_token", QString::fromLatin1(checked.refreshToken())},
        {"client_id", checked.clientId}, {"device_token", checked.deviceToken}, {"scope", checked.effectiveScope}};
    QByteArray form;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (!form.isEmpty()) form += '&';
        form += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value().toString());
    }
    auto *reply = boundedStockReply(m_network.post(request, form), m_loopbackStockDeadlineMsForTests);
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply] {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > 1024 * 1024 || total > 1024 * 1024) reply->abort();
    });
    return reply;
}

bool RHNetworkEngine::stockUrl(const QUrl &url, const QString &collection) const
{
    const auto entry = m_endpoints.value("Accounts").toObject();
    const QUrl base(entry.value("URL").toString(), QUrl::StrictMode);
    const bool official = base.scheme() == "https" && base.host() == "api.robinhood.com"
        && base.port(443) == 443;
    const bool loopback = m_allowLoopbackForTests && permittedUrl(base)
        && QStringList{"localhost", "127.0.0.1", "::1"}.contains(base.host());
    // Payload identity is distinct from transport origin in offline fixtures.
    // This never permits pagination or requests to leave the configured origin.
    const bool sameOrigin = url.scheme() == base.scheme() && url.host() == base.host() && url.port() == base.port();
    const bool canonicalFixtureIdentity = loopback && url.scheme() == "https" && url.host() == "api.robinhood.com" && url.port(443) == 443;
    return m_configured && entry.value("Enabled").toBool(true) && entry.value("Method") == "GET"
        && base.path() == "/accounts/" && permittedUrl(base) && (official || loopback)
        && permittedUrl(url) && (sameOrigin || canonicalFixtureIdentity) && !url.hasQuery() && !url.hasFragment()
        && !url.path(QUrl::FullyEncoded).contains('%')
        && stockMatch(url.path(), "/" + collection + "/[A-Za-z0-9_-]{1,128}/");
}

bool RHNetworkEngine::stockLiveOrdersEnabled(bool loopbackLiveForTests) const
{
#ifdef RELEASE
    Q_UNUSED(loopbackLiveForTests);
    return true;
#else
    const QUrl base(m_endpoints.value("Accounts").toObject().value("URL").toString());
    return loopbackLiveForTests && m_allowLoopbackForTests && permittedUrl(base)
        && QStringList{"localhost", "127.0.0.1", "::1"}.contains(base.host());
#endif
}

QNetworkReply *RHNetworkEngine::sendStockOrder(const StockOrder &order, const QByteArray &token,
    AuthKind kind, QString *error, bool loopbackLiveForTests)
{
    if (error) error->clear();
    const auto fail = [error]() -> QNetworkReply * {
        if (error) *error = "Stock order was not dispatched: unsafe request or unavailable live transport.";
        return nullptr;
    };
    const QString decimal = "(?:0|[1-9][0-9]{0,11})(?:\\.[0-9]{1,12})?";
    const auto positive = [&decimal](const QString &s) {
        return stockMatch(s, decimal) && s.contains(QRegularExpression("[1-9]"));
    };
    if (kind != AuthKind::OAuthBearer || !safeBearer(token) || !stockLiveOrdersEnabled(loopbackLiveForTests)
        || !stockUrl(order.account, "accounts") || !stockUrl(order.instrument, "instruments")
        || !stockMatch(order.accountNumber, "[A-Za-z0-9_-]{1,128}")
        || !stockMatch(order.symbol, "[A-Z][A-Z0-9]{0,9}(?:[.-][A-Z0-9]{1,4})?")
        || !positive(order.quantity) || (order.side != "buy" && order.side != "sell")
        || (order.buyKind != BuyKind::Market && order.buyKind != BuyKind::FixedLimit)
        || !positive(order.askPrice) || !positive(order.bidPrice)
        || (order.side == "sell" && order.buyKind != BuyKind::Market)
        || (order.side == "buy" && (order.quantity != "1" || !positive(order.price)
            || !order.quoteTimestamp.isValid()
            || (order.buyKind == BuyKind::Market && order.price != order.askPrice)))
        || !stockMatch(order.refId, "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-4[0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}"))
        return fail();
    // Always dispatch to validated configuration, never to a payload identity.
    QUrl url(m_endpoints.value("Accounts").toObject().value("URL").toString()); url.setPath("/orders/");
    QUrlQuery query; query.addQueryItem("account_numbers", order.accountNumber); url.setQuery(query);
    QVariantMap payload{{"account", order.account.toString()}, {"instrument", order.instrument.toString()},
        {"symbol", order.symbol}, {"quantity", order.quantity}, {"ref_id", order.refId},
        {"side", order.side}, {"type", order.side == "buy" && order.buyKind == BuyKind::FixedLimit ? "limit" : "market"},
        {"trigger", "immediate"}, {"time_in_force", "gfd"}, {"market_hours", "regular_hours"},
        {"extended_hours", false}, {"order_form_version", 4},
        {"bid_ask_timestamp", (order.quoteTimestamp.isValid() ? order.quoteTimestamp : QDateTime::currentDateTimeUtc()).toUTC().toString(Qt::ISODateWithMs)}};
    // Market buys retain the reference ask price from the provider contract;
    // type=market makes it a reference, not a limit. Never add a percent override.
    if (order.side == "buy") payload.insert("price", order.price);
    if (positive(order.askPrice)) payload.insert("ask_price", order.askPrice);
    if (positive(order.bidPrice)) payload.insert("bid_price", order.bidPrice);
    QByteArray form;
    for (auto it = payload.cbegin(); it != payload.cend(); ++it) {
        if (!form.isEmpty()) form += '&';
        form += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(formValue(it.value()));
    }
    auto request = stockRequest(url, token);
    request.setRawHeader("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
    return boundedStockReply(m_network.post(request, form), m_loopbackStockDeadlineMsForTests);
}

QNetworkReply *RHNetworkEngine::readStockOrder(const QString &orderId, const QByteArray &token,
    AuthKind kind, QString *error)
{
    if (error) error->clear();
    QUrl url(m_endpoints.value("Accounts").toObject().value("URL").toString(), QUrl::StrictMode);
    url.setPath("/orders/" + orderId + "/");
    if (kind != AuthKind::OAuthBearer || !safeBearer(token)
        || !stockMatch(orderId, "[A-Za-z0-9_-]{1,128}")
        || orderId.contains(QString::fromLatin1(token)) || !stockUrl(url, "orders")) {
        if (error) *error = "Unsafe or unavailable primary order read.";
        return nullptr;
    }
    auto request = stockRequest(url, token);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    return boundedStockReply(m_network.get(request), m_loopbackStockDeadlineMsForTests);
}

QNetworkReply *RHNetworkEngine::readStock(StockRead resource, const QByteArray &token,
    AuthKind kind, const QString &scope, const QUrl &page, QString *error)
{
    if (error) error->clear();
    const auto fail = [error]() -> QNetworkReply * {
        if (error) *error = "Unsafe or unavailable primary stock read.";
        return nullptr;
    };
    if (kind != AuthKind::OAuthBearer || !safeBearer(token)) return fail();
    const QUrl base(m_endpoints.value("Accounts").toObject().value("URL").toString(), QUrl::StrictMode);
    QUrl probe = base; probe.setPath("/accounts/probe/"); probe.setQuery(QString());
    if (!stockUrl(probe, "accounts")) return fail();
    QString path;
    QString scopeKey;
    switch (resource) {
    case StockRead::Accounts: path = "/accounts/"; break;
    case StockRead::Instruments: path = "/instruments/"; scopeKey = "symbols"; break;
    case StockRead::Quotes: path = "/quotes/"; scopeKey = "symbols"; break;
    case StockRead::Positions: path = "/positions/"; scopeKey = "account_number"; break;
    default: return fail();
    }
    if (!scopeKey.isEmpty() && !stockMatch(scope, resource == StockRead::Positions
        ? "[A-Za-z0-9_-]{1,128}" : "[A-Z][A-Z0-9]{0,9}(?:[.-][A-Z0-9]{1,4})?")) return fail();
    QUrl url = base; url.setPath(path); url.setQuery(QString());
    if (!page.isEmpty()) url = base.resolved(page);
    if (!permittedUrl(url) || url.scheme() != base.scheme() || url.host() != base.host()
        || url.port() != base.port() || url.path() != path || url.path(QUrl::FullyEncoded).contains('%')
        || url.toString().size() > 4096) return fail();
    QUrlQuery query(url); QSet<QString> keys;
    for (const auto &pair : query.queryItems(QUrl::FullyDecoded)) {
        if (keys.contains(pair.first) || pair.second.isEmpty() || pair.second.size() > 2048
            || (pair.first != "cursor" && pair.first != scopeKey
                && !(resource == StockRead::Accounts && pair.first == "default_to_all_accounts")
                && !(resource == StockRead::Positions && pair.first == "nonzero"))
            || (pair.first == scopeKey && pair.second != scope)
            || ((pair.first == "default_to_all_accounts" || pair.first == "nonzero") && pair.second != "true")) return fail();
        keys.insert(pair.first);
    }
    if (!scopeKey.isEmpty()) { query.removeAllQueryItems(scopeKey); query.addQueryItem(scopeKey, scope); }
    if (resource == StockRead::Positions) { query.removeAllQueryItems("nonzero"); query.addQueryItem("nonzero", "true"); }
    if (resource == StockRead::Accounts) { query.removeAllQueryItems("default_to_all_accounts"); query.addQueryItem("default_to_all_accounts", "true"); }
    url.setQuery(query);
    // Check the final URL, including injected scope, and decoded cursor values.
    // Credentials belong only in Authorization, never in a request target.
    const auto bearer = QString::fromLatin1(token);
    if (url.toString().contains(bearer) || url.query(QUrl::FullyDecoded).contains(bearer)) return fail();
    return boundedStockReply(m_network.get(stockRequest(url, token)), m_loopbackStockDeadlineMsForTests);
}

bool RHNetworkEngine::loadConfiguration(const QString &path, QString *error)
{
    m_configured = false;
    m_endpoints = {};
    m_headerSets = {};
    if (error)
        error->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot open network configuration");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid network configuration JSON");
        return false;
    }

    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("Headers")).isObject()
        || !root.value(QStringLiteral("Endpoints")).isObject()) {
        if (error)
            *error = QStringLiteral("Network configuration lacks Headers or Endpoints");
        return false;
    }

    const QJsonObject headers = root.value(QStringLiteral("Headers")).toObject();
    const QJsonObject endpoints = root.value(QStringLiteral("Endpoints")).toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (!it.value().isObject()) {
            if (error)
                *error = QStringLiteral("Invalid header set");
            return false;
        }
        const QJsonObject entries = it.value().toObject();
        for (auto header = entries.begin(); header != entries.end(); ++header) {
            if (header.key().isEmpty() || !header.value().isString()) {
                if (error)
                    *error = QStringLiteral("Invalid header definition");
                return false;
            }
        }
    }
    for (auto it = endpoints.begin(); it != endpoints.end(); ++it) {
        if (!it.value().isObject()) {
            if (error)
                *error = QStringLiteral("Invalid endpoint definition");
            return false;
        }
        const QJsonObject entry = it.value().toObject();
        if (entry.contains(QStringLiteral("Enabled"))
            && !entry.value(QStringLiteral("Enabled")).isBool()) {
            if (error)
                *error = QStringLiteral("Invalid endpoint availability");
            return false;
        }
        if (!entry.value(QStringLiteral("Enabled")).toBool(true))
            continue;

        const QString method = entry.value(QStringLiteral("Method")).toString();
        const QString encoding = entry.value(QStringLiteral("BodyEncoding")).toString();
        const QString url = entry.value(QStringLiteral("URL")).toString();
        const QJsonValue sets = entry.value(QStringLiteral("HeaderSets"));
        const QJsonValue defaults = entry.value(QStringLiteral("BodyDefaults"));
        if (url.isEmpty() || !sets.isArray()
            || (method != QStringLiteral("GET") && method != QStringLiteral("POST"))
            || (encoding != QStringLiteral("none") && encoding != QStringLiteral("json")
                && encoding != QStringLiteral("form"))
            || (method == QStringLiteral("GET") && encoding != QStringLiteral("none"))
            || (!defaults.isUndefined() && (!defaults.isObject() || encoding == QStringLiteral("none")))) {
            if (error)
                *error = QStringLiteral("Invalid endpoint transport definition");
            return false;
        }
        static const QRegularExpression placeholder(QStringLiteral(R"(\{[A-Za-z_][A-Za-z_0-9]*\})"));
        QString sampleUrl = url;
        sampleUrl.replace(placeholder, QStringLiteral("sample"));
        if (!permittedUrl(QUrl(sampleUrl, QUrl::StrictMode))) {
            if (error)
                *error = QStringLiteral("Unsafe endpoint URL");
            return false;
        }
        for (const QJsonValue &set : sets.toArray()) {
            if (!set.isString() || !headers.contains(set.toString())) {
                if (error)
                    *error = QStringLiteral("Unknown header set");
                return false;
            }
        }
        if (defaults.isObject()) {
            const QJsonObject object = defaults.toObject();
            for (auto item = object.begin(); item != object.end(); ++item) {
                if (item.key().isEmpty() || item.value().isArray() || item.value().isObject()) {
                    if (error)
                        *error = QStringLiteral("Invalid endpoint body defaults");
                    return false;
                }
            }
        }
    }
    m_headerSets = headers;
    m_endpoints = endpoints;
    m_configured = true;
    return true;
}

bool RHNetworkEngine::isConfigured() const
{
    return m_configured;
}

QNetworkReply *RHNetworkEngine::send(const QString &endpoint, const QVariantMap &values,
                                    const QVariantMap &query, const QVariantMap &body, QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &message) -> QNetworkReply * {
        if (error)
            *error = message;
        return nullptr;
    };
    if (values.value(QStringLiteral("token_type")).toString() == QStringLiteral("WebSecondaryBearer"))
        return fail(QStringLiteral("Secondary tokens require the explicit authenticated read-only transport."));
    if (!m_configured || !m_endpoints.value(endpoint).isObject()) {
        return fail(QStringLiteral("Unknown endpoint or missing network configuration"));
    }
    const QJsonObject entry = m_endpoints.value(endpoint).toObject();
    if (!entry.value(QStringLiteral("Enabled")).toBool(true))
        return fail(QStringLiteral("Endpoint is disabled"));
    if (endpoint == QStringLiteral("TokenRefresh"))
        return fail(QStringLiteral("OAuth refresh requires the typed refreshOAuth grant boundary."));

    // Validate the effective body on every generic route, not just TokenRefresh:
    // configuration defaults and caller fields must never create a refresh grant.
    QVariantMap payload = entry.value(QStringLiteral("BodyDefaults")).toObject().toVariantMap();
    for (auto it = body.cbegin(); it != body.cend(); ++it)
        payload.insert(it.key(), it.value());
    for (auto it = payload.cbegin(); it != payload.cend(); ++it) {
        const auto key = it.key().trimmed().toLower();
        if (key == QStringLiteral("refresh_token")
            || (key == QStringLiteral("grant_type")
                && it.value().toString().trimmed().compare(QStringLiteral("refresh_token"), Qt::CaseInsensitive) == 0))
            return fail(QStringLiteral("OAuth refresh requires the typed refreshOAuth grant boundary."));
    }

    if (endpoint == QStringLiteral("Login")) {
        // Legacy headless password login only. Exact map keys prevent aliases or
        // duplicate semantic form fields; never stringify containers or nulls.
        static const QSet<QString> textFields{QStringLiteral("grant_type"), QStringLiteral("client_id"),
            QStringLiteral("username"), QStringLiteral("password"), QStringLiteral("device_token"),
            QStringLiteral("scope"), QStringLiteral("challenge_type"), QStringLiteral("mfa_code")};
        const auto invalidLogin = [&fail]() {
            return fail(QStringLiteral("Login requires the supported password-grant form schema."));
        };
        for (const auto &key : {"grant_type", "client_id", "username", "password"}) {
            if (!payload.contains(QLatin1String(key))) return invalidLogin();
        }
        for (auto it = payload.cbegin(); it != payload.cend(); ++it) {
            if (textFields.contains(it.key())) {
                const auto value = it.value().toString();
                if (it.value().metaType().id() != QMetaType::QString || value.isEmpty()
                    || value.contains(QLatin1Char('\r')) || value.contains(QLatin1Char('\n'))
                    || value.contains(QChar::Null)) return invalidLogin();
            } else if (it.key() == QStringLiteral("try_passkeys")) {
                if (it.value().metaType().id() != QMetaType::Bool) return invalidLogin();
            } else if (it.key() == QStringLiteral("expires_in")) {
                if (!stockMatch(it.value().toString(), "[1-9][0-9]{0,9}")) return invalidLogin();
            } else {
                return invalidLogin();
            }
        }
        if (entry.value("Method") != "POST" || entry.value("BodyEncoding") != "form"
            || payload.value("grant_type").toString() != QStringLiteral("password")) return invalidLogin();
    }

    QString urlText;
    if (!expand(entry.value(QStringLiteral("URL")).toString(), values, true, &urlText))
        return fail(QStringLiteral("Missing or unsafe URL parameter"));
    QUrl url(urlText, QUrl::StrictMode);
    if (!permittedUrl(url))
        return fail(QStringLiteral("Unsafe endpoint URL"));
    if (!query.isEmpty()) {
        QUrlQuery parameters(url);
        for (auto it = query.begin(); it != query.end(); ++it)
            parameters.addQueryItem(it.key(), it.value().toString());
        url.setQuery(parameters);
    }

    // Generic configuration is not trading authorization. Brokerage mutations
    // must use the typed primary-only order boundary, never a JSON endpoint name.
    if (entry.value("Method").toString() == "POST") {
        const bool official = url.scheme() == "https" && url.host() == "api.robinhood.com" && url.port(443) == 443;
        const bool loopback = QStringList{"127.0.0.1", "::1", "localhost"}.contains(url.host());
        const auto path = url.path();
        const auto encoding = entry.value("BodyEncoding").toString();
        const bool login = endpoint == "Login" && encoding == "form"
            && (path == "/oauth2/token/" || (loopback && path == "/login/"));
        const bool begin = endpoint == "BeginVerification" && encoding == "json"
            && (path == "/pathfinder/user_machine/" || (loopback && path == "/machine/"));
        const bool continuation = endpoint == "ContinueVerification" && encoding == "json"
            && (stockMatch(path, "/pathfinder/inquiries/[A-Za-z0-9_-]{1,128}/")
                || (loopback && stockMatch(path, "/inquiry/[A-Za-z0-9_-]{1,128}/")));
        const bool challenge = endpoint == "SubmitChallenge" && encoding == "json"
            && stockMatch(path, "/challenge/[A-Za-z0-9_-]{1,128}/respond/");
        if ((!official && !loopback) || url.hasQuery() || url.path(QUrl::FullyEncoded).contains('%')
            || !(login || begin || continuation || challenge))
            return fail("POST endpoint is not in the validated authentication allowlist. Use the stock-order transport for trading.");
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(16000);
#if defined(Q_OS_IOS)
    const QByteArray userAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1";
#elif defined(Q_OS_ANDROID)
    const QByteArray userAgent = "Mozilla/5.0 (Linux; Android 14; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36";
#elif defined(Q_OS_WIN)
    const QByteArray userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
#elif defined(Q_OS_MACOS) // Q_OS_MAC also matches iOS and is deprecated in Qt 6.12.
    const QByteArray userAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
#else
    const QByteArray userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
#endif
    for (const QJsonValue &set : entry.value(QStringLiteral("HeaderSets")).toArray()) {
        const QJsonObject headers = m_headerSets.value(set.toString()).toObject();
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            // Qt must negotiate compression itself to enable reply decompression.
            if (it.key().compare(QStringLiteral("User-Agent"), Qt::CaseInsensitive) == 0
                || it.key().compare(QStringLiteral("Accept-Encoding"), Qt::CaseInsensitive) == 0)
                continue;
            QString value;
            if (!expand(it.value().toString(), values, false, &value))
                return fail(QStringLiteral("Missing or unsafe header parameter"));
            request.setRawHeader(it.key().toUtf8(), value.toUtf8());
        }
    }
    request.setRawHeader("User-Agent", userAgent);
#ifndef QT_NO_DEBUG
    if (endpoint == QStringLiteral("Login")
        && qEnvironmentVariableIntValue("PUBLICS_RH_RAW_LOGIN_DIAGNOSTICS") == 1) {
        // Log only these public configuration headers, never auth/cookies/body.
        for (const QByteArray &header : {QByteArray("Accept"), QByteArray("Content-Type"),
                                        QByteArray("X-Robinhood-API-Version"), QByteArray("User-Agent")})
            qDebug() << "LOGIN REQUEST HEADER:" << header << request.rawHeader(header);
    }
#endif

    if (entry.value(QStringLiteral("Method")).toString() == QStringLiteral("GET")) {
        if (!body.isEmpty())
            return fail(QStringLiteral("GET endpoint cannot send a body"));
        return m_network.get(request);
    }

    const QString encoding = entry.value(QStringLiteral("BodyEncoding")).toString();
    if (encoding == QStringLiteral("none")) {
        if (!body.isEmpty())
            return fail(QStringLiteral("Endpoint cannot send a body"));
        return m_network.post(request, QByteArray());
    }
    if (encoding == QStringLiteral("json"))
        return m_network.post(request, QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact));

    // QUrlQuery leaves '+' literal; form decoders would turn it into a space.
    QByteArray form;
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (it.value().isNull())
            continue;
        if (!form.isEmpty())
            form += '&';
        form += QUrl::toPercentEncoding(it.key());
        form += '=';
        form += QUrl::toPercentEncoding(formValue(it.value()));
    }
    return m_network.post(request, form);
}
