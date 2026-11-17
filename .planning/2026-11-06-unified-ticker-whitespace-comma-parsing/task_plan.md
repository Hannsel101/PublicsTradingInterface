# Uniform ticker separators

## Goal
Accept runs of whitespace and/or commas as separators in every implemented applet ticker entry and all six Telegram trading commands; preserve symbol grammar, limits, normalization, order, security and execution policies.

## Current Phase
Complete: implementation, full-kit verification, review and delivery

## Phases
### Phase1: discover input paths and contract
- **Status:** complete
- Located PublicApiWorker::parseSymbols (manual/Publics Auto), RHTradeController and RHAutoTradeController, TelegramAgent and Debug whitelist, three applet QML ticker fields. No credential/live-provider access.
### Phase2: native and Telegram regressions/fixes
- **Status:** complete
- Parent owns Publics parser/QML and Release kit. deleg_4d7d4b7f/task-0 owns Telegram protocol/logging/tests plus Debug kit; task-1 owns Robinhood controllers/tests plus ASan kit. No concurrent kit writers; CMake parent-only.
- Split runs including tabs/newlines and Unicode whitespace, ignore empty delimiter segments, validate existing per-route ticker grammar before uppercase. Preserve Publics legacy nonseparator filtering rather than expand this change into a symbol-policy redesign.
### Phase3: integrated rebuild/test and review
- **Status:** complete
- Rebuild actual application and affected unit/QML/protocol/integration suites after owners return; verify polling/webhook, BUY/SELL, bounds, no-ticker no-op, invalid-token safety, native exact ordering, QML pass-through, no source changes during final sweeps. Independent bounded review; supported ASan.
- Final parent actual macOS whole Debug and Release application builds succeeded, CTest37/37 each. Affected ASan10/10 (detect_leaks=0). Exact JSON/JUnit inventories parsed and checked against identical unchanged source fingerprint across all kits. Independent bounded review deleg_802efd8b: no concrete introduced P1/P2. git diff --check clean.
### Phase4: delivery
- **Status:** complete
- Update relevant skill contracts and report actual tested results; Windows/live providers not verified. Preserve unrelated worktree, no commits/reset, no production keychain/Telegram/tunnel/broker operations.
