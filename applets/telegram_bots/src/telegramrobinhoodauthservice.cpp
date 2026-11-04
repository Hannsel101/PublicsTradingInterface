#include "telegramrobinhoodauthservice.h"
#include "rhsessionmanager.h"
#include "rhnetworkengine.h"
#include "rhnativeauthclient.h"
#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <utility>

static void initializeRobinhoodAuthResources() { Q_INIT_RESOURCE(rh_config); }
namespace {
using Result = RHSessionManager::StoredAuthenticationResult;
QString safeFailure(const QString &reason) {
    // Exact finite native vocabulary only; never reflect provider/store messages.
    static const QHash<QString, QString> known{
        {"stored_authentication_cancelled", "authentication cancelled"},
        {"Cancelled", "authentication cancelled"},
        {"Request timed out", "authentication request timed out"},
        {"Network request failed", "authentication network request failed"},
        {"Authentication request failed", "authentication network request failed"},
        {"Transport unavailable", "authentication transport unavailable"},
        {"Invalid response", "malformed authentication response"},
        {"Response too large", "authentication response too large"},
        {"Redirect rejected", "authentication redirect rejected"},
        {"unsupported_challenge_local_authentication_required", "unsupported challenge; complete authentication locally"},
        {"Push verification not approved", "verification not approved"},
        {"Verification not approved", "verification not approved"},
        {"Push verification failed", "push verification request failed"},
        {"Verification inquiry failed", "verification inquiry failed"},
        {"Verification continuation failed", "verification continuation failed"},
        {"Verification poll limit reached", "verification poll limit reached"},
        {"Verification retry limit reached", "verification retry limit reached"},
        {"Invalid verification machine", "invalid verification machine"},
        {"Invalid verification challenge", "invalid verification challenge"},
        {"Unsupported verification workflow", "unsupported verification workflow; complete authentication locally"},
        {"Unsupported verification challenge", "unsupported verification challenge; complete authentication locally"},
        {"Unsupported legacy MFA or challenge continuation", "unsupported challenge; complete authentication locally"},
        {"Verification code not validated", "unsupported challenge; complete authentication locally"},
        {"Could not stage authentication input securely", "secure authentication storage unavailable"},
        {"Staged authentication input is unavailable", "secure authentication input unavailable"},
        {"Invalid authentication input", "invalid saved authentication input"},
        {"stored_authentication_persistence_failed", "saved authentication could not be persisted"},
        {"stored_authentication_identity_mismatch", "saved account identity mismatch"},
        {"saved_account_email_ambiguous", "saved account email is ambiguous"},
        {"saved_accounts_credentials_unavailable", "saved secure credentials unavailable; complete authentication locally"},
        {"stored_credentials_unavailable_local_authentication_required", "saved secure credentials unavailable; complete authentication locally"},
        {"native_authentication_unavailable_local_authentication_required", "native authentication unavailable; complete authentication locally"},
        {"stored_authentication_failed_local_authentication_required", "saved authentication failed; complete authentication locally"},
        {"saved_accounts_metadata_unavailable", "saved account metadata unavailable"},
        {"saved_accounts_busy_or_unavailable", "saved account inventory busy or unavailable"},
        {"authentication_progress_callback_required", "authentication progress unavailable"},
        {"invalid_saved_account_email", "invalid saved account email"}
    };
    if (const auto found = known.constFind(reason); found != known.cend()) return *found;
    static const QRegularExpression rejection(QStringLiteral(
        "\\AAuthentication failed \\(HTTP ([1-5][0-9]{2})\\); "
        "error=(invalid_grant|invalid_request|invalid_client|unauthorized_client|unsupported_grant_type|invalid_scope|access_denied|invalid_token|temporarily_unavailable|server_error|verification_required|mfa_required|invalid_credentials|rate_limited|captcha_required|invalid_device|unrecognized|absent); "
        "reason=(unclassified|credentials_rejected|client_rejected|grant_type_rejected|rate_limited); fields="));
    if (reason.size() <= 2048) {
        const auto match = rejection.match(reason);
        if (match.hasMatch()) return QStringLiteral("provider rejected authentication (HTTP %1; error=%2; reason=%3)")
            .arg(match.captured(1), match.captured(2), match.captured(3));
    }
    return QStringLiteral("saved authentication unavailable");
}
bool validExpiry(const QString &value) {
    // Only typed NotNeeded carries a date, never a provider/freeform reason.
    static const QRegularExpression iso(QStringLiteral("\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]{1,3})?(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
    return value.size() <= 35 && iso.match(value).hasMatch()
        && QDateTime::fromString(value, Qt::ISODateWithMs).isValid();
}
bool validEmail(const QString &email) {
    static const QRegularExpression pattern(QStringLiteral("\\A[^\\s@]+@[^\\s@]+\\.[^\\s@]+\\z"));
    return email.size() <= 512 && pattern.match(email).hasMatch();
}
}
struct TelegramRobinhoodAuthService::State : QObject {
    TelegramRobinhoodAuthService *q;
    QPointer<RHSessionManager> sessions;
    std::unique_ptr<RHNetworkEngine> ownedNetwork;
    std::unique_ptr<RHNativeAuthClient> ownedClient;
    std::unique_ptr<RHSessionManager> ownedSessions;
    bool ownGraph = true, active = false, nativeStarted = false, terminalPending = false, notifiedBusy = false;
    int timeoutMs = 480000;
    quint64 generation = 0;
    QString id, email;
    std::function<void()> continuation;
    QTimer dependencyWatch;
    explicit State(TelegramRobinhoodAuthService *owner) : q(owner) {
        dependencyWatch.setInterval(100);
        connect(&dependencyWatch, &QTimer::timeout, this, [this] {
            if (active && !terminalPending && !sessions) { settle(QStringLiteral("Authentication Failed: saved authentication unavailable")); return; }
            refreshBusy();
        });
    }
    ~State() override {
        active = false; ++generation; continuation = {};
        if (nativeStarted && sessions) sessions->cancelSignIn();
    }
    bool busy() const { return active || (sessions && sessions->busy()); }
    void refreshBusy() {
        const bool value = busy();
        if (value) dependencyWatch.start(); else dependencyWatch.stop();
        if (notifiedBusy == value) return;
        notifiedBusy = value;
        emit q->busyChanged();
    }
    void watchManager() {
        if (sessions) connect(sessions, &RHSessionManager::busyChanged, this, [this] { refreshBusy(); });
    }
    template<class F> void later(F callback) {
        const QPointer<State> guard(this); const auto epoch = generation;
        QTimer::singleShot(0, this, [guard, epoch, callback = std::move(callback)]() mutable {
            if (guard && guard->active && guard->generation == epoch) callback(*guard);
        });
    }
    void settle(QString message) {
        if (!active) return;
        const auto request = id;
        active = false; nativeStarted = false; terminalPending = false; continuation = {}; ++generation;
        id.clear(); email.clear();
        const QPointer<TelegramRobinhoodAuthService> owner(q);
        refreshBusy();
        if (owner) emit owner->completed(request, {message.toHtmlEscaped()});
    }
    void begin() {
        const QPointer<State> guard(this); const auto epoch = generation;
        if (ownGraph && !sessions) {
            initializeRobinhoodAuthResources();
            ownedNetwork = std::make_unique<RHNetworkEngine>();
            if (!ownedNetwork->loadConfiguration(QStringLiteral(":/robinhood/config/robinhood-api.json"))) {
                settle(QStringLiteral("Authentication Failed: saved authentication unavailable")); return;
            }
            ownedClient = std::make_unique<RHNativeAuthClient>();
            ownedSessions = std::make_unique<RHSessionManager>(*ownedNetwork);
            sessions = ownedSessions.get();
            sessions->setNativeAuthClient(ownedClient.get());
            watchManager();
        }
        if (!sessions || sessions->busy()) { settle(QStringLiteral("Authentication Failed: saved authentication unavailable")); return; }
        if (ownGraph) {
            const bool loaded = sessions->load();
            if (!guard || !guard->active || guard->generation != epoch) return;
            if (!loaded) { settle(QStringLiteral("Authentication Failed: saved authentication unavailable")); return; }
        }
        nativeStarted = true;
        const bool admitted = sessions->authenticateStoredSession(email,
            [guard, epoch](QString canonicalEmail, std::function<void()> next) {
                if (!guard || !guard->active || guard->generation != epoch) return;
                guard->later([canonicalEmail = std::move(canonicalEmail), next = std::move(next)](State &s) mutable {
                    if (!s.nativeStarted || !validEmail(canonicalEmail) || s.continuation) return;
                    s.email = std::move(canonicalEmail);
                    s.continuation = std::move(next);
                    emit s.q->reportReady(s.id, {(QStringLiteral("Authentication for account: ") + s.email
                        + QStringLiteral(" in progress...")).toHtmlEscaped()});
                });
            },
            [guard, epoch](Result result, QString canonicalEmail, QString reason) {
                if (!guard || !guard->active || guard->terminalPending || guard->generation != epoch) return;
                guard->terminalPending = true;
                // The native job is terminal now, not when its queued report runs.
                // Never cancel a replacement manager job during this delivery gap.
                guard->nativeStarted = false;
                guard->continuation = {};
                guard->later([result, canonicalEmail = std::move(canonicalEmail), reason = std::move(reason)](State &s) {
                    if (validEmail(canonicalEmail)) s.email = canonicalEmail;
                    if (result == Result::Approved) s.settle(QStringLiteral("Authentication Successful for ") + s.email);
                    else if (result == Result::NotNeeded && validEmail(canonicalEmail) && validExpiry(reason))
                        s.settle(QStringLiteral("Authentication not performed for ") + s.email
                            + QStringLiteral(", reason: token expires at ") + reason);
                    else if (result == Result::Failed && reason == QStringLiteral("stored_authentication_timeout"))
                        s.settle(QStringLiteral("Authentication Failed for ") + s.email + QStringLiteral(", reason: 8 minute timeout reached"));
                    else s.settle(result == Result::NotFound ? QStringLiteral("Account Session Not Stored in List of Sessions")
                        : QStringLiteral("Authentication Failed: ") + safeFailure(reason));
                });
            }, timeoutMs);
        if (guard && guard->active && guard->generation == epoch && !admitted) {
            nativeStarted = false;
            settle(QStringLiteral("Authentication Failed: saved authentication unavailable"));
        }
    }
};
TelegramRobinhoodAuthService::TelegramRobinhoodAuthService(QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this)) {}
TelegramRobinhoodAuthService::TelegramRobinhoodAuthService(RHSessionManager *sessions, QObject *parent, int timeoutMs)
    : TelegramRobinhoodAuthService(parent) {
    d->ownGraph = false; d->sessions = sessions; d->timeoutMs = qBound(1, timeoutMs, 480000); d->watchManager();
    d->notifiedBusy = d->busy();
    if (d->notifiedBusy) d->dependencyWatch.start();
}
TelegramRobinhoodAuthService::~TelegramRobinhoodAuthService() = default;
bool TelegramRobinhoodAuthService::busy() const { return d->busy(); }
bool TelegramRobinhoodAuthService::submit(QString requestId, QString email) {
    email = email.trimmed();
    if (busy() || requestId.isEmpty() || requestId.size() > 512 || !validEmail(email)) return false;
    d->active = true; d->id = std::move(requestId); d->email = std::move(email);
    const QPointer<State> guard(d.get()); const auto epoch = ++d->generation;
    d->refreshBusy();
    if (guard && guard->active && guard->generation == epoch) guard->later([](State &s) { s.begin(); });
    return true;
}
void TelegramRobinhoodAuthService::continueAfterProgress(QString requestId) {
    if (!d->active || d->id != requestId || !d->continuation) return;
    auto next = std::exchange(d->continuation, {});
    d->later([next = std::move(next)](State &) mutable { next(); });
}
void TelegramRobinhoodAuthService::cancel() {
    if (!d->active || d->terminalPending) return;
    const QPointer<State> guard(d.get());
    if (d->nativeStarted && d->sessions) d->sessions->cancelSignIn();
    // Native cancellation may synchronously establish an elapsed-deadline result.
    if (guard && guard->active && !guard->terminalPending)
        guard->settle(QStringLiteral("Authentication Failed: authentication cancelled"));
}
