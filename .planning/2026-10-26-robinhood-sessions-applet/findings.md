# Findings

## Delete saved Robin Hood login
- The picker lists saved login sessions (`RHSessionManager`), whereas the account overview lists brokerage accounts (`RHAccountModel`); deletion should remove one saved login token under `RobinHoodAuth%1` and its `sessions.json` row, never a brokerage account.
- `reauthenticate(index)` already removes the exact keychain entry asynchronously, atomically persists the remaining session metadata, then removes the model row, but also routes to sign-in. A dedicated delete path should share that transaction without routing, and keep the selected item visible on failure.

## Upcoming Launcher choices
- Existing `AppletChoice` already appends “(coming soon)” to the accessible name for disabled choices; it needed darker background and muted label styling to look intentionally disabled while retaining each stripe's color.
- Offscreen compact QML image showed all five applet tiles inside the viewport, with Wells Fargo red, Fidelity dark green, and Chase blue stripes and dimmed text; mouse clicks on those disabled tiles did not change the active applet. No controller registration is needed until applets exist.

## Brokerage account labels and balances
- `load_phoenix_account()` calls a separate Phoenix endpoint and documents unified `total_equity`/`portfolio_equity`, whereas `load_account_profile()` calls `/accounts/` and documents `type`, `account_number`, `cash_balances`, and `margin_balances`. The existing session verification requests **only** `/accounts/` and must not assume Phoenix equity fields are present there. Preserve unknown balance explicitly instead of presenting zero or inventing an equity figure.
- `RHSessionManager` is already a QAbstractListModel of saved sessions. Its `accounts` property is a QVariantList used in a QML Repeater. A second account-specific QAbstractListModel is necessary for distinct displayName/accountNumber/currentBalance roles across three accounts in one saved session.
- A temporary QML mock screenshot of the overview (430×700) showed all three tiles with Individual Investing, Roth IRA, Traditional IRA, account numbers, and `Balance:` amounts/unavailable fully readable without overlap. The screenshot was a temporary local inspection artifact, not a committed test file.

## Publics display regression
- The new Robin Hood `Loader` was a second `Layout.fillHeight: true` child in the applet `ColumnLayout`. `Loader.active: false` destroys its item but does not remove the Loader itself from layout. Offscreen reproduction measured 324px each for active Publics and inactive Robin Hood below the 52px toolbar in a 700px window. This truncates the Publics applet. Binding `visible: active` on both Loaders excludes the unused applet from layout; the new regression checks the full 648px viewport and Publics was visually rendered in a Qt Quick offscreen smoke run.
- The visual check is a desktop/QML mock without stored account keys, not proof of actual Android/iOS rendering or a live brokerage operation.

## Device approval follow-up
- First-party `jmfernandes/robin_stocks` `authentication.py` (GitHub master `dfcf055`, matching local `1b4ef98` snapshot) uses `verification_workflow.id`, POST `/pathfinder/user_machine/`, GET inquiry `context.sheriff_challenge` with `type: prompt`, GET `/push/{challenge_id}/get_prompts_status/`, POST inquiry `{"sequence":0,"user_input":{"status":"continue"}}`, then retries the original OAuth POST. It polls for `workflow_status_approved` or `type_context.result` with that value. This is not a single generic `/challenge/status/` call.
- Existing runtime JSON already defines BeginVerification, ReadVerificationInquiry, CheckPrompt, ContinueVerification. Existing C++ rejects any `verification_workflow` at `rhsessionmanager.cpp:283` and lacks an auth-stage UI timer.
- Upstream has unbounded prompt polling and explicitly assumes approval after a timeout/error; **do not copy these fail-open behaviors**. Use one 180-second deadline, single in-flight request, explicit server approval, and no saved token until OAuth returns one.
- Treat the new 3-minute indicator as **authentication challenge countdown only**, not an OAuth-session validity timer (the earlier no-session-timer requirement remains).

- RHNetworkEngine, configuration, README and its test were already present from the previous task but still untracked; preserve them and include their integration in the final working tree.
- Launcher currently registers only Publics; `LauncherWindow.qml` contains disabled Robin Hood tile and a Publics-only Loader.
- `AppletInterface` provides initialize/start/canStop/stop; Publics QML uses a single embeddable CoreView.
- Current root CMake already links QtKeychain and sets up modular applet subprojects; Robin Hood target has Qt6::Network only.
- Publics styling is violet-accented; Robin Hood should use its own palette rather than import Publics Theme.
- Tests use `QmlTestSetup` mocks and Qt Test; make QML tests offline and avoid requiring real keychain state.
- Analyzed robin_stocks login defaults to 86400 seconds (one day), accepts an `expires_in` request, and uses server `expires_in` in the response; the UI must never claim 30 days were granted. Preserve the requested 30-day maximum but honor a shorter server grant.
- Source login includes device_token, public client_id, password grant fields, optional mfa_code, and verification_workflow; verification requires a separate multi-step flow. Do not claim that path is implemented unless it truly is.
- Publics uses asynchronous QtKeychain jobs with `setInsecureFallback(false)` and treats `EntryNotFound` as an idempotent delete. No brokerage network requests are required by the existing QML test harness.

## Open questions to verify
- Choose how to safely support login without falsely promising push/challenge verification.
- QtKeychain job async deletion and test-time injection seams.
- Best way to serve a runtime JSON config on desktop/mobile without leaking brokerage config into another applet.