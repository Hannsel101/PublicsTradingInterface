# Robinhood Applet Auto Mode

## Goal
Implement native all-unexpired-session Auto Mode with session→account→ticker execution, buy-one/sell-all, exact2.5sec pacing, truthful sent outcomes and responsive session/status/input/results UI.

## Contract
- New Robinhood applet menu; session list displays only name and green Active/red Re-Authentication Required based on token expiry, reactive while open. Opening/selection alone must not authenticate.
- Explicit user clarification: SELL mirrors BUY session/account/ticker order and2.5sec delays, selling ALL freshly verified sellable shares.
- Snapshot unexpired saved profiles in model order, bind stable identities (sparse slot IDs), skip expired. For each allowed account complete all tickers before next account, and all accounts before next session.
- Fresh authoritative brokerage_account_type individual/ira_traditional/ira_roth only; never infer type from nicknames or cash/margin.
- Retain native credentials/lease/reconciliation/quote/live-preview protections. Submitted HTTP2xx/no-network-error orders count successful without claiming fills; rejected/not-sent/network errors fail; previews remain explicitly not sent and ambiguous effects never replay.
- Native child selected-account opt-in with final2500ms cooldown; orchestrator reuses one session lease across account/ticker chunks and releases before next login. Existing manual and Telegram defaults/scopes unchanged.
- Preserve legitimate existing changes; no production keychains/providers/tunnels/orders, no commits.

## Current Phase
Complete

## Next Step
None. Implementation, native/UI integration, independent review and regression verification are complete on macOS; live providers and other native platforms remain unverified.

## Phases
### Phase1: Requirements and source contracts
- **Status:** complete
- User clarified sell-all; interfaces/file ownership fixed; current disabled Auto Mode and native limits examined.
### Phase2: Native policy, orchestration and UI
- **Status:** complete
- Three isolated owners; parent owns robinhoodapplet.cpp, native/tests CMake and actual integration.
### Phase3: Actual composition and regression verification
- **Status:** complete
- Actual Debug/Release apps and full34/34 configured targets passed each; focused ASan5/5 passed, leaks disabled. Native CoreView-to-order BUY/SELL composition and precise final cooldown included.
- Rebuild real Debug/Release app and focused/full suites, native loopback all-session order/quantity/cooldown, QML compact/desktop, supported sanitizer.
### Phase4: Independent review and delivery
- **Status:** complete
- Native audit found no blockers. UI pending-row failure presentation finding reproduced RED at compact/desktop sizes, corrected to neutral Waiting / Submitting, and final rebuilt Debug/Release app plus4/4 focused targets each passed. Reviewed ASan UI/native composition2/2 passed; original full34/34 suites and ASan5/5 native baseline also passed.
- Resolve findings, final source/build checks and skill documentation. Distinguish tested fixtures from Windows/mobile/live provider checks.

## Errors
- QML Preview tool not connected; use actual rebuilt Qt Quick fixtures. No live-preview claims.
- Independent UI audit caught pending rows displayed as terminal failure; added pending regression, observed2RED rows, then fixed only the result presentation and reverified actual builds/fixtures.
