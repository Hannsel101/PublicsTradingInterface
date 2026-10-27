# Robinhood OAuth grant persistence and renewal
## Goal
Persist a genuine complete OAuth grant securely; initialize/renew selected sessions with refresh_token POST and automatically use validated renewed bearer credentials. Do not treat local state as authorization evidence or relabel secondary cookies.
## Next Step
Verification blocked after two fix/review cycles: resolve cancellation error clobber from a synchronous replacement save, add its exact regression and rerun independent review before approval. Current implementation/build evidence is preserved; no further automatic fix cycle.
## Current Phase
Integrated verification and independent review
## Phases
### Phase 1: discovery and contracts
- [x] Inspect existing C++ session/store/transport/build gates and verify OAuth refresh/rotation contract from primary sources.
- **Status:** complete
### Phase 2: implementation
- [x] Implement versioned QtKeychain full-grant storage, refresh transport, selected-profile initialization/idle renewal and renewed bearer use.
- [x] Add Renew Session UI for expired renewable entries; observed two-row RED then full QML GREEN.
- **Status:** complete
### Phase 3: verification
- [ ] Run renewal/rotation/scope/identity/cancellation/failure regressions, full Debug/Release builds and independent review.
- **Status:** blocked
### Phase 4: delivery
- [ ] Document tested behavior, initial-grant acquisition requirement and actual live-provider limits; no commits or live trades.
- **Status:** pending
## Boundaries
- Robinhood only. Preserve all earlier dirty changes, Android disablement and existing Release/Debug trading policy.
- QtKeychain rather than plaintext QSettings; device/client/identity context and complete response stored in one versioned secure record. Metadata is non-secret hints only.
- Successful standard refresh skips interactive password/MFA, but rejected/revoked grants require reauthentication; no guarantee of 30-day refresh or 24-hour access lifetimes.
- TLS/strict token endpoint, no ambient cookies/redirects, no secrets in URLs/QML/logs. Validate refreshed grant scope and account identity before activating primary headers.
- Never roll back a securely persisted rotated refresh grant to a potentially revoked old token merely because metadata hints failed to update.
- No actual brokerage, OS keychain, credentials, login or trades during development. Use synthetic loopback and injected stores.
