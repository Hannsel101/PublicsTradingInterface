# Robin Hood sessions applet

## Goal
Add a one-window Robin Hood applet with a lime-complementary UI, secure asynchronous keychain session storage, JSON metadata, an expiry-aware list, and a real keychain-backed Start Session boundary; keep Auto Mode disabled and omit a session timer.

## Next Step
Add a confirmed Delete action for a saved Robin Hood login (keychain token and local metadata), without affecting brokerage accounts.

## Current Phase
Phase 11: delete saved Robin Hood login

## Phases
### Phase 1: discovery
- [x] Trace existing applet composition, keychain APIs, QML, and tests; resolve token lifetime semantics.
- **Status:** complete
### Phase 2: architecture and tests
- [x] Establish isolated SessionManager persistence/read/delete contracts and offline tests.
- [x] Establish applet/QML navigation contracts and offline tests.
- **Status:** complete
### Phase 3: implementation
- [x] Implement validated JSON metadata plus async QtKeychain transactions and session model.
- [x] Wire Robin Hood applet to configured network boundary and implement themed picker/login/overview UI.
- **Status:** complete
### Phase 4: verification
- [x] Run offline tests, QML lint, macOS and available mobile builds, smoke launch, inspect UI sizing.
- **Status:** complete
### Phase 5: delivery
- [x] Review diff and safety boundary; report actual behavior and limits.
- **Status:** complete
### Phase 6: device approval handshake
- [x] Trace upstream workflow and observed response shapes; add offline red tests.
- [x] Add configured challenge endpoints, asynchronous polling, countdown UI and cancellation/timeout behavior.
- [x] Verify full desktop suite and available platform builds; document protocol limitations.
- **Status:** complete
### Phase 7: Publics viewport regression
- [x] Reproduce the truncated Publics view and isolate its cause (inactive Robin Hood Loader consumed half the content area).
- [x] Fix the composition with desktop/compact Publics and Robin Hood interaction/geometry regression tests.
- [x] Rebuild, run full offline suite, inspect an offscreen-rendered Publics view, and verify Android/iOS targets.
- **Status:** complete
### Phase 8: Robin Hood account labels and balances
- [x] Verify endpoint response shapes and write failing model/UI behavior tests.
- [x] Parse classification and balances into a typed per-account model; render human-readable labels and balances.
- [x] Run offline desktop tests, QML lint and available platform builds; document missing-balance handling.
- **Status:** complete
### Phase 9: forthcoming applet choices
- [x] Add a failing QML regression for all three disabled choices and verify existing navigation remains intact.
- [x] Add colored, visibly disabled Launcher choices without registering functional applets.
- [x] Build, run offline tests, and inspect compact and desktop layouts.
- **Status:** complete
### Phase 10: Robin Hood platform-specific User-Agent
- [x] Reproduce generic and absent User-Agent behavior in offline GET/POST tests.
- [x] Apply compile-time platform mapping in the request-header path; remove the generic configuration value.
- [x] Use the Windows string as the fallback, without changing Publics' already-correct request headers; other applets have no active network functionality.
- [x] Run the network tests and available desktop/mobile build and test checks.
- **Status:** complete
### Phase 11: delete saved Robin Hood login
- [ ] Test keychain/metadata removal, failure and gap reuse; prove the UI confirmation and selection reset.
- [ ] Implement a dedicated deletion operation and a confirmed saved-account control, preserving reauthentication routing.
- [ ] Run offline C++/QML tests, lint, desktop and available mobile app targets.
- **Status:** in progress

## Decisions
- Keep Publics sources untouched; resolve applet composition defects in the Launcher.
- Never make live brokerage requests during verification.
- Treat 30 days as the requested maximum; cap server-granted `expires_in` to 30 days and fail closed on invalid expiry.
- Expose metadata only in QAbstractListModel; use asynchronous QtKeychain jobs with a fake store for tests.
- Support password/MFA-code login when a token is granted and asynchronous device-approval workflows with an authentication-only 180-second countdown.
- Start Session verifies with the configured read-only Accounts endpoint before showing account overview; never place an order.

## Limitations
- Android compilation and iOS Simulator builds pass, but Android APK packaging lacks a usable Java runtime on this host.
- Live Robinhood authentication, platform keychain behavior on devices, and Windows builds have not been verified.