# Robinhood native login production integration
## Goal
Replace the production Robinhood WebView sign-in route with the desktop-user-confirmed native flow and a matching native QML form. After approved grant and identity/account validation, persist email/password and full renewable grant securely across normal app updates; display verified email as session name.
## Scope and boundaries
- Robinhood only; preserve extensive prior dirty changes, Publics, stock-order behavior/build policy and Android launcher disablement. No commits.
- Promote the reviewed header-aligned native client and keep the spike as a historical standalone test. No real provider login, OS-keychain operations or trades during agent verification; inject loopback/stores.
- Versioned QtKeychain record at existing stable service/RobinHoodAuthX key stores full grant plus email/password together. No credentials in plaintext files/settings/logs or token/password QML roles. New native email labels must also remain encrypted at rest; hydrate display names asynchronously from secure records, with safe placeholder if unavailable. Existing legacy records remain readable and migrate on successful native reauthentication.
- Keep device/client context, server expiry, refresh rotation and principal validation. Persist nothing on rejection/cancellation/wrong identity. One commit gate/reservation through secure write/reconciliation/publication; fix known retired cancellation error mutation as part of lifecycle integration.
- Normal in-place updates retain OS credential storage using stable service/key/application identity; do not promise uninstall, keychain deletion, changed signing identity or OS-account recovery.
## Shared UI/backend API
RHSessionManager: setNativeAuthClient(RHNativeAuthClient*) C++ only; beginNativeLogin(accountId={}); existing signIn(email,password,mfaCode={}); submitNativeVerificationCode(code); cancelSignIn(); existing reauthenticate(index)/prepareReauthentication(accountId) select native when client injected.
Properties (all notify nativeLoginChanged): nativeLoginPending, nativeCodeRequired, nativeLoginCancellable, nativeLoginStatus, nativeLoginEmail. Pending spans form, request, approval, verification and secure commit; cancellable is false during commit. Emails alone may enter display properties; passwords/tokens never enter getters/models/signals.
RHNativeAuthClient: promoted spike class/API (renamed), with encrypted RAM PendingCredentials rather than long-lived plaintext password fields. Pure network target must not depend on session manager; shared secure-credentials library supplies staging/legacy locker.
## Ownership
- Backend workstream: new rhnativeauthclient.{h,cpp}, rhoauthgrant.{h,cpp}, rhsessionmanager*.{h,cpp}, session/codec/native-client tests. No CMake/QML/applet edits.
- UI workstream: CoreView.qml, new NativeLoginForm.qml, QML mocks/tests. No CMake/backend/applet edits.
- Parent: CMake integration, applet composition/runtime selection, docs, integrated builds/artifacts/reviews.
## Phases
1. Discover production/secure-store contracts and establish ownership. Status: complete.
2. Implement promoted native client/session credential transaction and QML form with regressions. Status: complete.
3. Verify whole desktop suites/Android package and independent reviews. Status: complete.
4. Deliver production app and exact persistence/verification limits. Status: complete.
