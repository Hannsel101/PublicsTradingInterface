# Robinhood sequential stock orders
## Goal
Implement Publics-style ticker Buy/Sell All and results for all brokerage accounts in the active Robinhood login, with a new random 2–3-second cooldown AFTER each account/ticker result, including the last. Buy one share; sell fresh sellable quantities. No live trades during development.
## Next Step
Delivery: implemented engine/UI are verified and independently reviewed. Remaining prerequisite is a verified primary-authorization acquisition contract; current browser login remains read-only. Do not mark the overall trading request complete.
## Current Phase
Partial delivery — implemented engine/UI verified; browser trading authorization blocked
## Phases
### Phase 1: discovery
- [x] Read queued requirements, actual Publics controls/order flow and upstream robin_stocks payload/header source; identify read-only token limitation.
- **Status:** complete
### Phase 2: implementation
- [x] Implement protected order transport/controller, verified-primary-session integration and ticker/results UI with regression tests.
- [ ] BLOCKED: browser-derived trading authorization needs authoritative primary-token provenance. Header/cookie inequality is unsafe; automatic promotion is disabled.
- **Status:** partial — engine/UI implemented; browser trading authorization blocked
### Phase 3: verification
- [x] Build/run Debug and Release loopback tests and QML, verify platform builds, independently review and fix findings.
- **Status:** complete for implemented engine/UI; live service acceptance untested
### Phase 4: delivery
- [x] Document executed checks, provider/auth limits and exact batch semantics; deliver implemented engine/UI without committing or placing live trades.
- **Status:** partial delivery; overall request blocked by browser primary authorization
## Scope and safety
- Robinhood only; preserve Publics behavior, existing holdings/cache/UI changes and Android launcher disablement.
- Tick-major/account-minor displayed order; all accounts in active login only, no auto-login of other saved profiles.
- First transaction immediate; each result gets a newly sampled 2000–3000ms asynchronous cooldown, including final cooldown. No concurrent orders or blocking sleeps.
- Fresh position quantities and validated instruments/accounts; exact decimal arithmetic, no cached sell sizes, no short/zero order or automatic retry of ambiguous submission.
- Explicit submitted versus filled versus failed/unknown results. Server failure reasons sanitized; tokens never enter QML, logs, URLs or results.
- Native read-header primary capture is disabled after a verified secondary-refresh race. Current browser sessions remain read-only; only existing verified primary OAuth sessions can use the order engine. Do not claim end-to-end browser trading is complete.
- Match Publics live-Release/Debug-preview distinction. Test live-dispatch branches only on explicitly allowed loopback endpoints; no real keychain/brokerage requests/trades.
