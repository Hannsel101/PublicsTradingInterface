# Robinhood account information
## Goal
Label each brokerage account Standard Brokerage, IRA or Roth IRA from structured metadata; persist stock/share snapshots per account and expose them through an accessible lowercase-i account-information action, including saved offline data.
## Next Step
Deliver the completed account-information feature; the separately specified all-accounts buy/sell workflow remains queued in next-task-stock-orders.md.
## Current Phase
Complete — account labels, persistent snapshots and information UI implemented and independently reviewed.
## Phases
### Phase 1: discovery
- [x] Inspect Robinhood models, persistence, account cards and network boundaries; preserve all pre-existing changes.
- **Status:** complete
### Phase 2: implementation
- [x] Implement exact account labels, safe holdings snapshots and read-only refresh; add i-button account information with cache freshness and empty/error states.
- **Status:** complete
### Phase 3: verification
- [x] Run C++ and QML regressions, desktop builds, Android compilation as appropriate and independent review; fix findings.
- **Status:** complete
### Phase 4: delivery
- [x] Document actual verification and live-provider/privacy limits; deliver implemented result.
- **Status:** complete
## Scope and decisions
- Robinhood only; retain Android launcher disablement and existing browser/token restrictions.
- Read-only account/position requests only; no live login, real keychain access, credentials or trades during development.
- Per-profile/per-brokerage-account snapshots, exact decimal quantity strings, timestamped cached/not-live UI, no invented holdings or fake empty results on failed refresh.
- Owner-private atomic local snapshot storage; no token/raw-response persistence in holdings data.
- Unknown structured account types must not be silently misclassified using nickname text.
