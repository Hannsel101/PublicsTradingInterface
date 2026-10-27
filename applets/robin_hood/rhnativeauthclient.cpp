#include "rhnativeauthclient.h"
#include "securecredentiallocker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <algorithm>

namespace {
constexpr qsizetype maxBody = 1024 * 1024;
// Wire compatibility reference (not provider acceptance evidence):
// robin_stocks/robinhood/authentication.py at immutable upstream commit
// 1b4ef98be36d1df127886889c843256799a1dfea (git ls-remote HEAD verified).
// Unlike upstream: bounded polls/deadlines, no timeout-as-approval or persistence.
const QString clientId = QStringLiteral("c82SH0WZOsabOXGP2sxqcj34FxkvfnWRZBKlBjFS");
void wipe(QString &value) { value.fill(QChar(0)); value.clear(); }
void wipe(QByteArray &value) { value.fill('\0'); value.clear(); }
QByteArray formBody(const QList<QPair<QString, QString>> &fields) {
    QByteArray body;
    for (const auto &field : fields) {
        if (!body.isEmpty()) body += '&';
        body += QUrl::toPercentEncoding(field.first) + '=' + QUrl::toPercentEncoding(field.second);
    }
    return body;
}
QString rejectionSummary(const QJsonObject &object, int http) {
    // Output is finite vocabulary only: never reflect arbitrary values or key names.
    const QStringList allowedErrors{"invalid_grant", "invalid_request", "invalid_client",
        "unauthorized_client", "unsupported_grant_type", "invalid_scope", "access_denied",
        "invalid_token", "temporarily_unavailable", "server_error", "verification_required",
        "mfa_required", "invalid_credentials", "rate_limited", "captcha_required", "invalid_device"};
    QString error = object.contains("error") ? "unrecognized" : "absent";
    const auto supplied = object.value("error");
    if (supplied.isString() && allowedErrors.contains(supplied.toString())) error = supplied.toString();
    QString reason = "unclassified";
    const auto classify = [&reason](const QJsonValue &value) {
        if (!value.isString() || value.toString().size() > 512 || reason != "unclassified") return;
        const auto message = value.toString().trimmed().toLower();
        if (message == "unable to log in with provided credentials."
            || message == "unable to log in with provided credentials"
            || message == "invalid username/password.") reason = "credentials_rejected";
        else if (message == "invalid client identifier.") reason = "client_rejected";
        else if (message == "unsupported grant type.") reason = "grant_type_rejected";
        else if (message.startsWith("request was throttled.")) reason = "rate_limited";
    };
    classify(object.value("detail"));
    classify(object.value("error_description"));
    const auto nonField = object.value("non_field_errors");
    if (nonField.isArray()) {
        const auto messages = nonField.toArray();
        for (qsizetype i = 0; i < qMin(messages.size(), qsizetype(16)); ++i) classify(messages.at(i));
    }
    QStringList fields;
    const QStringList knownFields{"username", "password", "client_id", "device_token", "scope",
        "grant_type", "mfa_code", "gr_token", "captcha", "challenge", "verification_workflow",
        "error", "detail", "error_description", "non_field_errors"};
    for (const auto &field : knownFields) if (object.contains(field)) fields.append(field);
    return QString("Authentication failed (HTTP %1); error=%2; reason=%3; fields=%4")
        .arg(http).arg(error, reason, fields.isEmpty() ? QString("none") : fields.join(','));
}
bool codeValid(const QString &code) {
    if (code.size() < 4 || code.size() > 10) return false;
    return std::all_of(code.begin(), code.end(), [](QChar c) { return c >= QChar('0') && c <= QChar('9'); });
}
bool validId(const QString &id) {
    if (id.isEmpty() || id.size() > 256) return false;
    return std::all_of(id.begin(), id.end(), [](QChar c) {
        return (c >= QChar('0') && c <= QChar('9')) || (c >= QChar('a') && c <= QChar('z'))
            || (c >= QChar('A') && c <= QChar('Z')) || c == QChar('-') || c == QChar('_');
    });
}
bool validOrigin(const QUrl &url) {
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https")
        && (url.host() == "127.0.0.1" || url.host() == "::1") && url.port() > 0
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && (url.path().isEmpty() || url.path() == "/");
}
}

struct RHNativeAuthClient::Impl {
    RHNativeAuthClient *q;
    QNetworkAccessManager network;
    TestTransport transport;
    bool valid = true;
    State state = State::Idle;
    QString status = QStringLiteral("Idle");
    GrantHandler handler;
    quint64 generation = 0;
    QPointer<QNetworkReply> reply;
    QTimer whole;
    QTimer poll;
    SecureCredentialLocker::PendingCredentials credentials;
    QString device, mfa;
    QString machine, challenge;
    bool retried = false;
    int polls = 0;
    QByteArray received;

    Impl(RHNativeAuthClient *owner, TestTransport config, bool test) : q(owner), transport(std::move(config)) {
        valid = !test || validOrigin(transport.origin);
        transport.requestTimeoutMs = std::clamp(transport.requestTimeoutMs, 1, 16000);
        transport.workflowTimeoutMs = std::clamp(transport.workflowTimeoutMs, 1, 120000);
        transport.pollIntervalMs = std::clamp(transport.pollIntervalMs, 1, 5000);
        network.setProxy(QNetworkProxy::NoProxy);
        whole.setSingleShot(true);
        poll.setSingleShot(true);
        whole.setTimerType(Qt::PreciseTimer);
        poll.setTimerType(Qt::PreciseTimer);
        QObject::connect(&whole, &QTimer::timeout, q, [this] { finish(State::Failed, "Authentication timed out"); });
    }
    bool busy() const { return state == State::Requesting || state == State::WaitingForCode || state == State::WaitingForApproval; }
    void clear() {
        ++generation;
        whole.stop(); poll.stop(); QObject::disconnect(&poll, nullptr, q, nullptr);
        machine.clear(); challenge.clear(); retried = false; polls = 0;
        if (reply) {
            auto *old = reply.data(); reply.clear();
            QObject::disconnect(old, nullptr, q, nullptr);
            old->abort(); old->deleteLater();
        }
        credentials.clear(); wipe(device); wipe(mfa); wipe(received);
    }
    ~Impl() { clear(); }
    void finish(State terminal, const QString &text) {
        clear(); state = terminal; status = text;
        emit q->changed(); // Last operation: observers may destroy or restart the client.
    }
    bool announce(State next, const QString &text) {
        state = next; status = text;
        const auto epoch = generation; QPointer<RHNativeAuthClient> guard(q);
        emit q->changed();
        return guard && generation == epoch && busy();
    }
    using Completion = std::function<void(QJsonObject, int)>;
    void request(const QByteArray &method, const QString &path, QByteArray body, bool json, Completion completion) {
        QNetworkRequest request(transport.origin.resolved(QUrl(path)));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        // Match the pinned reference client; Qt still negotiates compression/connection details.
        request.setRawHeader("Accept", "*/*");
        request.setRawHeader("Accept-Language", "en-US,en;q=1");
        request.setRawHeader("X-Robinhood-API-Version", "1.431.4");
        request.setRawHeader("User-Agent", "*");
        if (method == "POST") request.setHeader(QNetworkRequest::ContentTypeHeader,
            json ? "application/json" : "application/x-www-form-urlencoded; charset=utf-8");
        auto ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        ssl.setProtocol(QSsl::TlsV1_2OrLater); request.setSslConfiguration(ssl);
        const auto epoch = generation;
        auto *r = method == "GET" ? network.get(request) : network.post(request, body);
        wipe(body);
        reply = r; received.clear(); r->setReadBufferSize(maxBody + 1);
        auto *deadline = new QTimer(r); deadline->setSingleShot(true); deadline->setTimerType(Qt::PreciseTimer);
        QObject::connect(deadline, &QTimer::timeout, q, [this, epoch] {
            if (generation == epoch) finish(State::Failed, "Request timed out");
        });
        deadline->start(transport.requestTimeoutMs);
        QObject::connect(r, &QNetworkReply::readyRead, q, [this, epoch, r] {
            if (epoch != generation || reply != r) return;
            received += r->read(maxBody + 1 - received.size());
            if (received.size() > maxBody || r->bytesAvailable() > maxBody - received.size())
                finish(State::Failed, "Response too large");
        });
        QObject::connect(r, &QNetworkReply::finished, q, [this, epoch, r, deadline, completion = std::move(completion)] {
            deadline->stop();
            if (generation != epoch || reply != r) { r->deleteLater(); return; }
            received += r->read(maxBody + 1 - received.size());
            const int statusCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto error = r->error(); reply.clear(); r->deleteLater();
            if (received.size() > maxBody) { finish(State::Failed, "Response too large"); return; }
            if (statusCode >= 300 && statusCode < 400) { finish(State::Failed, "Redirect rejected"); return; }
            // Only these complete HTTP error classes can carry the supported login workflow.
            // A 400 status with RemoteHostClosedError is still a transport failure, never approval.
            const bool workflowHttpError = (statusCode == 400 && error == QNetworkReply::ProtocolInvalidOperationError)
                || (statusCode == 403 && error == QNetworkReply::ContentAccessDenied);
            if (!statusCode || (error != QNetworkReply::NoError && !workflowHttpError)) {
                finish(State::Failed, "Network request failed"); return;
            }
            QJsonParseError parse;
            auto document = QJsonDocument::fromJson(received, &parse); wipe(received);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                finish(State::Failed, "Invalid response"); return;
            }
            completion(document.object(), statusCode);
        });
    }
    void later(std::function<void()> action) {
        if (++polls > 64) { finish(State::Failed, "Verification poll limit reached"); return; }
        QObject::disconnect(&poll, nullptr, q, nullptr);
        const auto epoch = generation;
        QObject::connect(&poll, &QTimer::timeout, q, [this, epoch, action = std::move(action)] {
            if (generation == epoch && busy()) action();
        });
        poll.start(transport.pollIntervalMs);
    }
    void workflow(const QString &id) {
        if (!validId(id) || retried) { finish(State::Failed, "Unsupported verification workflow"); return; }
        if (!announce(State::WaitingForApproval, "Starting device verification")) return;
        const QJsonObject body{{"device_id", device}, {"flow", "suv"}, {"input", QJsonObject{{"workflow_id", id}}}};
        request("POST", "/pathfinder/user_machine/", QJsonDocument(body).toJson(QJsonDocument::Compact), true,
            [this](QJsonObject object, int http) {
                machine = object.value("id").toString();
                if (http < 200 || http >= 300 || !validId(machine)) { finish(State::Failed, "Invalid verification machine"); return; }
                later([this] { inquiry(); });
            });
    }
    void inquiry() {
        request("GET", "/pathfinder/inquiries/" + machine + "/user_view/", {}, false,
            [this](QJsonObject object, int http) {
                if (http < 200 || http >= 300) { finish(State::Failed, "Verification inquiry failed"); return; }
                const auto context = object.value("context").toObject();
                const auto sheriff = context.value("sheriff_challenge").toObject();
                if (sheriff.isEmpty()) { later([this] { inquiry(); }); return; }
                challenge = sheriff.value("id").toString();
                const auto type = sheriff.value("type").toString();
                const auto status = sheriff.value("status").toString();
                if (!validId(challenge)) { finish(State::Failed, "Invalid verification challenge"); return; }
                if ((type == "sms" || type == "email") && status == "validated") {
                    continueWorkflow(); return;
                }
                if ((type == "sms" || type == "email") && status == "issued") {
                    announce(State::WaitingForCode, type == "sms" ? "Enter SMS verification code" : "Enter email verification code");
                    return;
                }
                if (type == "prompt" && sheriff.value("status").isString()
                    && (status == "issued" || status == "pending" || status == "validated")) {
                    if (announce(State::WaitingForApproval, "Approve verification in provider app")) later([this] { push(); });
                    return;
                }
                finish(State::Failed, "Unsupported verification challenge");
            });
    }
    void push() {
        request("GET", "/push/" + challenge + "/get_prompts_status/", {}, false,
            [this](QJsonObject object, int http) {
                if (http < 200 || http >= 300) { finish(State::Failed, "Push verification failed"); return; }
                const auto status = object.value("challenge_status").toString();
                if (status == "validated") { continueWorkflow(); return; }
                if (status == "issued" || status == "pending") { later([this] { push(); }); return; }
                finish(State::Failed, "Push verification not approved");
            });
    }
    void continueWorkflow() {
        if (!announce(State::WaitingForApproval, "Waiting for final verification approval")) return;
        const QJsonObject body{{"sequence", 0}, {"user_input", QJsonObject{{"status", "continue"}}}};
        request("POST", "/pathfinder/inquiries/" + machine + "/user_view/", QJsonDocument(body).toJson(QJsonDocument::Compact), true,
            [this](QJsonObject object, int http) {
                if (http < 200 || http >= 300) { finish(State::Failed, "Verification continuation failed"); return; }
                const auto result = object.value("type_context").toObject().value("result").toString();
                const auto status = object.value("verification_workflow").toObject().value("workflow_status").toString();
                if (result == "workflow_status_approved" || status == "workflow_status_approved") {
                    if (retried) { finish(State::Failed, "Verification retry limit reached"); return; }
                    retried = true;
                    if (announce(State::Requesting, "Verification approved; requesting grant")) login();
                    return;
                }
                if (status == "workflow_status_internal_pending") { later([this] { continueWorkflow(); }); return; }
                finish(State::Failed, "Verification not approved");
            });
    }
    void submit(const QString &code) {
        if (state != State::WaitingForCode || !codeValid(code)) return;
        if (!announce(State::Requesting, "Submitting verification code")) return;
        request("POST", "/challenge/" + challenge + "/respond/", formBody({{"response", code}}), false,
            [this](QJsonObject object, int http) {
                if (http >= 200 && http < 300 && object.value("status") == "validated") {
                    continueWorkflow(); return;
                }
                finish(State::Failed, "Verification code not validated");
            });
    }
    void login() {
        QString username, password;
        if (!credentials.read(username, password)) {
            finish(State::Failed, "Staged authentication input is unavailable"); return;
        }
        QList<QPair<QString, QString>> fields{{"client_id", clientId}, {"expires_in", "86400"},
            {"grant_type", "password"}, {"scope", "internal"}, {"username", username}, {"password", password},
            {"device_token", device}, {"try_passkeys", "False"}, {"token_request_path", "/login"},
            {"create_read_only_secondary_token", "True"}};
        if (!mfa.isEmpty()) fields.append({"mfa_code", mfa});
        QByteArray body = formBody(fields);
        wipe(username); wipe(password);
        for (auto &field : fields) wipe(field.second);
        request("POST", "/oauth2/token/", std::move(body), false, [this](QJsonObject object, int http) {
            RHOAuthGrant grant;
            const bool full = (!object.contains("read_only") || object.value("read_only") == QJsonValue(false))
                && (!object.contains("is_read_only") || object.value("is_read_only") == QJsonValue(false));
            if (full && http >= 200 && http < 300 && RHOAuthGrant::parseResponse(object, clientId, device,
                    QDateTime::currentDateTimeUtc(), &grant) && grant.primary() && !grant.refreshToken().isEmpty()) {
                auto callback = handler;
                clear(); state = State::Succeeded; status = "Grant received; identity and permissions unverified";
                QPointer<RHNativeAuthClient> guard(q); const auto epoch = generation;
                emit q->changed();
                if (guard && generation == epoch && callback) callback(std::move(grant));
                return;
            }
            if (object.value("mfa_required") == QJsonValue(true) || object.contains("challenge")
                || object.contains("challenge_id") || object.contains("mfa_type")) {
                finish(State::Failed, "Unsupported legacy MFA or challenge continuation"); return;
            }
            if (object.contains("verification_workflow") && !object.contains("access_token")
                && !object.contains("refresh_token") && (http == 200 || http == 400 || http == 403)) {
                workflow(object.value("verification_workflow").toObject().value("id").toString()); return;
            }
            finish(State::Failed, rejectionSummary(object, http));
        });
    }
};

RHNativeAuthClient::RHNativeAuthClient(QObject *parent) : QObject(parent),
    d(std::make_unique<Impl>(this, TestTransport{QUrl("https://api.robinhood.com")}, false)) {}
RHNativeAuthClient::RHNativeAuthClient(TestTransport transport, QObject *parent) : QObject(parent),
    d(std::make_unique<Impl>(this, std::move(transport), true)) {}
RHNativeAuthClient::~RHNativeAuthClient() = default;
bool RHNativeAuthClient::start(const QString &username, const QString &password, const QString &deviceToken, const QString &mfaCode) {
    if (d->busy()) return false;
    if (!d->valid || username.isEmpty() || username.size() > 512 || password.isEmpty() || password.size() > 4096
        || deviceToken.isEmpty() || deviceToken.size() > 512 || (!mfaCode.isEmpty() && !codeValid(mfaCode))) {
        d->finish(State::Failed, "Invalid authentication input"); return false;
    }
    d->clear();
    if (!d->credentials.set(username, password)) {
        d->finish(State::Failed, "Could not stage authentication input securely"); return false;
    }
    d->device = deviceToken; d->mfa = mfaCode;
    d->whole.start(d->transport.workflowTimeoutMs);
    if (d->announce(State::Requesting, "Requesting password grant")) d->login();
    return true;
}
void RHNativeAuthClient::submitCode(const QString &code) { d->submit(code); }
void RHNativeAuthClient::cancel() { if (d->busy()) d->finish(State::Cancelled, "Cancelled"); }
RHNativeAuthClient::State RHNativeAuthClient::state() const { return d->state; }
bool RHNativeAuthClient::busy() const { return d->busy(); }
QString RHNativeAuthClient::statusText() const { return d->status; }
void RHNativeAuthClient::setGrantHandler(GrantHandler handler) { d->handler = std::move(handler); }
