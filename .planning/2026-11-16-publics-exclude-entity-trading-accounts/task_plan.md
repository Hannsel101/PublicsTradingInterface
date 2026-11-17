# Exclude Publics ENTITY accounts

## Goal
Remove ENTITY from Publics account eligibility, retaining BROKERAGE, ROTH_IRA and TRADITIONAL_IRA; apply common backend policy to manual/preflight/Auto without changing Robinhood. No live provider/keychain/orders, preserve unrelated worktree.

## Current Phase
Complete: restricted eligibility, verified and reviewed

## Phases
### Phase1: discover, reproduce, restrict eligibility
- **Status:** complete
- Read AGENTS, refreshed CodeGraph, inspected shared backend. Existing positive ENTITY test explicitly superseded by user. Negative mixed-account and ENTITY-only regressions compiled/executed RED before the one-line allowlist change. Updated supported-type README only.
### Phase2: verify and review
- **Status:** complete
- Rebuild actual macOS Debug/Release applications and run six affected unit/integration/composition suites, supported ASan; independent bounded source review. Preserve existing ordering, sizing and pacing.
- Actual whole Debug/Release builds succeeded; all six affected suites passed in Debug, Release and ASan (detect_leaks=0). Exact JSON/JUnit parsed: same inventories and unchanged common source fingerprint. Bounded review deleg_3974e001 found no introduced P1/P2. git diff --check clean.
### Phase3: deliver
- **Status:** complete
- Record exact results and complete plan; no Windows/live-provider claim.
