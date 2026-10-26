#include "rhnetworkengine.h"

#include <QFile>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>

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

RHNetworkEngine::RHNetworkEngine(QObject *parent, bool allowLoopbackForTests)
    : QObject(parent), m_allowLoopbackForTests(allowLoopbackForTests) {}

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
    const QByteArray &token, AuthKind kind, QString *error)
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
    const QUrl url(text, QUrl::StrictMode);
    const bool official = url.scheme() == QStringLiteral("https")
        && (url.host() == QStringLiteral("api.robinhood.com")
            || url.host() == QStringLiteral("phoenix.robinhood.com"))
        && (url.port(-1) == -1 || url.port() == 443);
    const bool loopback = m_allowLoopbackForTests && permittedUrl(url)
        && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1")
            || url.host() == QStringLiteral("localhost"));
    if (!permittedUrl(url) || (!official && !loopback) || text.contains('{')
        || text.contains(QString::fromLatin1(token))
        || url.query().contains(QStringLiteral("token"), Qt::CaseInsensitive))
        return fail();
    // The secondary cookie is NOT the web app's primary access token. Its
    // compatibility with brokerage/Phoenix is unverified. Permit only the two
    // configured identity-verification reads; never invent a Phoenix route.
    if (kind == AuthKind::WebSecondaryBearer) {
        const bool profile = endpoint == QStringLiteral("UserProfile")
            && url.path() == QStringLiteral("/user/") && !url.hasQuery();
        const bool accounts = endpoint == QStringLiteral("Accounts")
            && url.path() == QStringLiteral("/accounts/")
            && (!url.hasQuery() || url.query() == QStringLiteral("default_to_all_accounts=true"));
        if ((!profile && !accounts) || (!loopback && url.host() != QStringLiteral("api.robinhood.com")))
            return fail();
    }
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
    QVariantMap payload = entry.value(QStringLiteral("BodyDefaults")).toObject().toVariantMap();
    for (auto it = body.begin(); it != body.end(); ++it)
        payload.insert(it.key(), it.value());
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
