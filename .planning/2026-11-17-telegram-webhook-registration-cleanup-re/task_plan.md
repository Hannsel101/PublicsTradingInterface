# Telegram webhook startup recovery

## Goal
For one password-authenticated Start, retry transient webhook registration failures only after confirmed owned remote cleanup and local host/process release. Maximum3 total attempts (initial+2retries), then manual Start. Never replay commands/messages or persist passwords; preserve authorization, immutable identity, replay protection and explicit Stop cancellation.

## Current Phase
Complete: recovery, controller composition, cross-kit verification and review

## Phases
### Phase1: source/data-flow investigation and reproduction
- **Status:** complete
- Read AGENTS, bridge/TDD/debug/Telegram guidance, refreshed CodeGraph. Existing startup setWebhook/readback failure goes through fail->stop and finishStop clears session credentials; no recovery path. Existing starting/stopping gate prevents manual Start during known cleanup. User live failure cause not proven from generic log; deterministic loopback committed503 reproduces fatal unlock flow.
### Phase2: bounded recovery and acceptance tests
- **Status:** complete
- deleg_3b4a6f40 owns Agent cpp/h, webhook/Agent tests and Debug kit. Parent owns controller test/server header, Release/ASan kits, shared CMake and plans. Typed transient-only startup classification, max3, cleanup-before-retry, single snapshot/password per Start; unknown cleanup/hard auth/config/429 stayterminal. No premature idle/active state, Stop/revocation cancel.
### Phase3: integrate, review, verify
- **Status:** complete
- After source ownership returns: actual whole Debug/Release application builds/full37CTest,11 relevant ASan suites, source fingerprints/JUnit and bounded independent lifecycle/security review. Verify controller authority/ledger after recovery, single activation snapshot, held cleanup, exhausted budget/manual reset and StopAll cancellation.
- Actual whole Debug/Release application builds and full CTest37/37 each passed. ASan10 suites passed initially; one synthetic host-fixture launcher exited9 without sanitizer diagnostics, then focused reproduction and full unchanged host suite passed. All11 ASan suite outcomes verified against the same source hash, with failed/rerun receipts preserved. Bounded review deleg_9718adf5: no introduced P1/P2. git diff --check clean.
### Phase4: delivery
- **Status:** complete
- Update workflow skills with verified lifecycle lessons, record exact results, complete plan. No Windows/live-provider/keychain/tunnel/broker mutations or claims. Preserve unrelated worktree.
