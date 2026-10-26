# Progress

## Platform-specific Robin Hood User-Agent
- Follow-up scope: apply a platform-specific browser User-Agent to applets that need it. Robin Hood is the only active applet with a generic header today; Publics explicitly retains its existing `public-dev-docs` headers and its request code is untouched. Wells Fargo, Fidelity, and Chase are disabled placeholders without network engines. The Robin Hood fallback and its network test now use the exact Windows browser User-Agent instead of `PublicsTradingInterface/1.0`.
- The Common header set supplied `User-Agent: *`; `RHNetworkEngine::send` passed it unchanged to every endpoint. Added loopback GET/POST regressions for a missing header and a stale lowercase config override; both failed against the original engine.
- `send` now selects the requested fixed UA by compiled Qt platform macro, applies it once after the configured headers, and ignores case-insensitive config UA overrides. Removed the generic JSON header. Use `Q_OS_MACOS` instead of deprecated `Q_OS_MAC`, which also matches iOS. Other platforms temporarily use the Windows User-Agent fallback; no device model or browser version is detected.
- Targeted loopback test passed; the macOS application and QML test built, and all 9/9 CTest suites passed. Android arm64 and iOS Simulator app targets built. `git diff --check` passed. No live Robinhood login or push-device label was verified; Windows remains unbuilt here.

## Forthcoming applet choices
- Existing Launcher home hosts two enabled `AppletChoice` controls in a scrollable `ColumnLayout`. A new desktop/compact QML regression failed while Wells Fargo was absent, then passed after adding the three disabled choices with red (`#c5303e`), dark-green (`#14633e`), and blue (`#236dc2`) stripes. Shared `AppletChoice` now gives disabled buttons a darker background, muted text, and softened stripe; its “(coming soon)” accessible name remains. Disabled clicks do not activate an applet. No controller registration was added.
- Desktop and compact QML tests assert all three buttons are within the viewport, noninteractive, distinctly styled, and properly named for accessibility. An isolated 360px-wide Qt Quick rendering visually confirmed all five choices fit without clipping. macOS app/QML lint built and 9/9 CTest suites passed. Android arm64 and iOS Simulator app targets built; `git diff --check` passed. No device install or brokerage requests.

## Brokerage account overview follow-up
- User's authenticated session returned one margin and two cash accounts; existing `verifySession` extracts only `account_number`/`type` into a `QVariantList`, and QML renders raw type. The existing QAbstractListModel represents saved sessions (not brokerage accounts). Upstream `load_phoenix_account` uses a distinct endpoint; `/accounts/` can carry `cash_balances` but does not promise portfolio equity. No live brokerage requests or credentials involved.
- Red `/accounts/` loopback test failed because margin/cash account maps lacked `displayName`; after `RHAccountModel` replaced the variant list it passed with 3 separate role-mapped accounts. Red QML test failed before the cards displayed account names/balances; it passed after the delegate change. Follow-up red tests caught missing nested retirement `account_type` and fallback `rhs_account_number`; both now pass.
- `RHAccountModel` owns typed displayName/accountNumber/currentBalance entries. Numeric formatting groups US dollars and rounds to cents; valid zero is `$0.00`, while invalid/absent balances are “Unavailable.” Margin-only `cash_balances` do not masquerade as total equity. Model reset updates the QML Repeater atomically on verification and clears on session switch. README documents snapshot-only limits.
- A mock-backed QML overview screenshot showed all three named account tiles, numbers, and balance labels readable at 430×700. Final macOS app/test runner built, RH QML lint target completed (pre-existing context-property warnings), and 9/9 CTest suites passed. Android arm64 and iOS Simulator app targets built; `git diff --check` passed. No live brokerage calls.
- First iOS Simulator target build after adding the new translation unit linked without its symbols (exit 65); an immediate rebuild compiled the regenerated Xcode graph and succeeded. Do not infer the host/device behavior from that intermediate failure. No device install or Android APK packaging was attempted.

## Publics viewport follow-up
- User reports selecting Publics produces no visible applet. Existing Launcher QML test checked only non-null loader item and window identity; a new desktop geometry assertion failed: Publics and inactive Robin Hood Loaders each occupied 324px of the 648px applet area. Both were still visible layout children despite `Loader.active: false`.
- Bound each Loader's visibility to its active state so the inactive applet is excluded from ColumnLayout. Extended the QML interaction test for Publics desktop/compact and Robin Hood to require the active loader and its item to occupy the full viewport, and the other loader to be invisible. Targeted tests passed; an offscreen Qt Quick render showed the Publics heading and key-management panel in the full viewport.
- Full macOS app rebuilt; all 9 CTest suites passed. `launcher_ui_qmllint` target completed (pre-existing unqualified-context-property warnings). Android arm64 app-library and iOS Simulator app-target builds succeeded. `git diff --check` passed. No live brokerage calls or device installation were performed.
- Desktop computer-use backend was unavailable; visual inspection was via an isolated Qt Quick offscreen image. Exact behavior on the user's device remains unverified.

## Device approval follow-up
- Reopened the plan at phase 6 for the user's push challenge and authentication-only 180-second countdown request. The existing applet currently rejects `verification_workflow` before any polling.
- Reviewing the upstream robin_stocks workflow; only offline loopback verification is permitted here.
- Added a loopback reproduction: OAuth 403 with `verification_workflow.id` previously stopped after the first request; after the fix, Pathfinder, inquiry, prompt, continue, OAuth retry, keychain store and Accounts verified end-to-end.
- Added direct `challenge_id` push, 180-second deadline with test-time shorter policy, cancel, decline and expired-deadline tests; targeted C++ test class passed 14 cases. Offscreen QML countdown test was red for the missing UI, then passed with approval/cancel controls.
- Implemented non-blocking 2.5-second polling, a monotonic deadline, serialized requests, a 3-minute authentication-only countdown, safe cancellation when leaving the applet, and no token persistence before final OAuth success.
- Full macOS CTest run: 9/9 suites passed. RH/Launcher QML lint targets built (existing context-property unqualified warnings). Android arm64 native app built; iOS Simulator app link initially returned 65 on a parallel build, then completed successfully on retry. `git diff --check` passed.

## 2026-10-26
- Read repo guidance, status, root/launcher/QML/test wiring and prior RHNetworkEngine.
- Synced CodeGraph. No source changes for this request yet.
- Added `RHSessionManager` model, QtKeychain adapter, runtime Login body defaults and loopback tests; applet and QML picker/login/overview registered in the single-window Launcher.
- CMake desktop app and targeted C++ unit tests build and pass. Fixed QML test host visibility (`TestCase` defaults hidden), with user-like mouse selection now passing.

## Test results
- macOS app and nine CTest suites passed; RH and Launcher QML lint targets exited successfully.
- Android arm64 app-library and iOS Simulator app targets built successfully after the final production edits.
- Expired, active, empty, and compact/portrait/desktop picker cases passed in offscreen QML tests.
- Offline keychain, metadata, authentication expiry/challenge, and loopback network tests passed; no live brokerage requests were sent.

## Errors
- Fetched QtKeychain did not export include paths to the new target; mirrored existing `publics_auth` include directories.
- `ctest` is not on PATH; use Qt installation's absolute ctest path.
- Android APK packaging was blocked by missing Java runtime; Android C++/QML compilation was successful.
- An initial red-loop run was terminated while the old login handler rejected the challenge; a short diagnostic established only the first HTTP request had completed. After the implementation and targeted test, the diagnostic was removed.
- Qt Quick Test filter requires `RobinHoodSessions::test_deviceApprovalCountdownAndCancellation` rather than the bare function name; the qualified test first failed on missing UI and later passed.
- iOS Simulator Xcode link returned exit 65 once without a retained diagnostic; an immediate repeat exited 0. Treat as an uncharacterized transient, not a code fix.