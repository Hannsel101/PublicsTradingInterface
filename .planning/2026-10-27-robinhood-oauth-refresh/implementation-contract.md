# Shared OAuth refresh implementation contract

## Scope and trust
- Robinhood C++ only. No Python state exists to override. Local JSON/QSettings presence is never authorization evidence. Preserve secondary-cookie read-only classification, Release order policy, Android disablement and all pre-existing dirty changes.
- Store complete validated OAuth response and renewal context as one versioned QtKeychain secret at existing RobinHoodAuthX slots. No plaintext QSettings/token JSON on disk. Metadata contains only labels/IDs/expiry/refreshability hints.
- A genuine initial full grant must enter through a C++-only API; never fabricate refresh tokens or promote a secondary cookie. Refresh uses direct scoped OAuth endpoint POST, then identity/account validation before activation/persistence. No real credentials, OS keychain operations, brokerage requests or orders in tests.

## Agent A: value codec + network
Own new rhoauthgrant.{h,cpp}, rhnetworkengine.{h,cpp}, config/robinhood-api.json, new test_rhoauthgrant.cpp and network tests. Parent owns CMake.

Provide this C++ API (no QObject/metaobject token surface):
```cpp
struct RHOAuthGrant {
    QJsonObject response; // Complete actual response; derived context kept separately.
    QString clientId, deviceToken, accountId, effectiveScope;
    QDateTime obtainedAt, expiresAt;
    QByteArray accessToken() const;
    QByteArray refreshToken() const;
    bool primary() const; // Explicit effective internal scope + valid bearer grant, not cookie inequality.
    QString serialize() const;
    static bool parseResponse(const QJsonObject &response, const QString &clientId,
        const QString &deviceToken, QDateTime now, RHOAuthGrant *out,
        QString *error = nullptr, const RHOAuthGrant *previous = nullptr);
    static bool deserialize(const QString &secret, RHOAuthGrant *out, QString *error = nullptr);
    static bool extractAccessToken(const QString &secret, QByteArray *out, QString *error = nullptr);
};
```
- parseResponse validates complete successful bearer grant, expires_in, client/device binding, safe bounded access/refresh bytes, and supported explicit scope. New grants require refresh_token; renewal can retain previous refresh when omitted per OAuth standard. Reject errors, MFA/challenges, malformed or reduced/read-only scope for primary renewal; omission of scope may inherit the previous grant's effective scope.
- Preserve original response exactly in response, keeping inherited rotation/scope context separately if necessary; full response storage must not lose retained refresh token across another deserialize. Add private/internal fields if needed while preserving this interface.
- serialize/deserialize use strict versioned envelope and absolute expiry; malformed JSON never falls back to bearer. extractAccessToken supports valid envelope and legacy bare bearer, not arbitrary unversioned grant JSON.
- Network API: `QNetworkReply *refreshOAuth(const RHOAuthGrant &grant, QString *error = nullptr);` (forward declaration in header acceptable). Exact HTTPS api.robinhood.com /oauth2/token/ destination, or explicitly enabled test loopback only; configured TokenRefresh route must agree. Form grant_type=refresh_token, refresh_token, original client_id and device_token. Do not invent original client IDs or promise expiry durations. Disable ambient cookies/cache/redirects, bounded response, absolute deadline; no token URLs/logs. Source-verify provider details where possible; standard refresh fields use OAuth RFC6749 section6.

## Agent B: session lifecycle
Own rhsessionmanager.{h,cpp}, rhsessionmanager_auth.cpp, rhsessionmanager_trading.cpp, new rhsessionmanager_oauth.cpp, session unit tests and new OAuth session test file. Parent owns CMake/UI.

- Add C++-only `bool acceptOAuthGrant(const QJsonObject &response, const QString &clientId, const QString &deviceToken);` validating real grant using codec, UserProfile/Accounts and targeted identity before secure store commit. Native browser remains secondary-only until a real full grant acquisition integration exists; document this honestly.
- Add Q_INVOKABLE `bool initializeSession(int sessionIndex);`; startSession delegates to it. For selected profile: read secure secret even if access expired; if valid saved refresh bundle, immediately renew with network.refreshOAuth, validate returned scope/identity/accounts and rotated credentials, safely persist, then activate fresh bearer headers. No browser/password/MFA request on successful refresh. If no refresh token, retain existing legacy/session behavior. Never auto-authenticate other saved profiles.
- Versioned grant secrets coexist with legacy bare token slots. Every consumer (session verification, trading lease, reauthentication rollback/deletion) must understand bundle or preserve opaque secret exactly. Never send the JSON envelope as Authorization.
- Persist non-secret `canRefresh` flag (C++ model role CanRefreshRole, QML role name `canRefresh`) only after valid grant storage; this is a UI hint, not authentication evidence. Parent will expose Renew Session for expired renewable profiles.
- Add a guarded proactive idle renewal check for the active profile near expiry; do not refresh/mutate authorization during a trade lease or automatically retry ambiguous order POSTs. Single-flight, asynchronous, generation/lifetime guarded; cancellation must not overwrite a replacement session. Preserve dedicated commit/rollback gate before any observable notifications and balanced model publication. Failures preserve last usable secret where possible and surface sanitized reauthentication-needed errors; don't silently elevate read-only grants or retry rotated refresh indefinitely.

## Verification
Strict RED/GREEN. Inject fake stores and loopback HTTP; test restart/expired refresh, no MFA/password on renewal, new Authorization, rotation and omitted rotation, malformed grant, invalid_grant/401, reduced scope, wrong principal, redirect/cookie/URL token leakage, cancel/late callbacks, concurrent starts, store/metadata rollback, legacy bare-token and secondary behavior. Parent integrates/builds full Debug and Release suites plus independent review; no live brokerage operations or commits.
