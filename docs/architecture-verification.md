# Architecture refactor verification

## Delivered

- Organized production code under `src/{app,auth,api,trading}` and QML under `qml/PublicsTradingInterface/{credentials,trading}`.
- Removed autocomplete entirely under the latest user request: UI, controller, token wiring, target, cache implementation, and dedicated tests. Manual comma-separated ticker input remains covered by Qt Quick tests.
- Added shared Theme singleton, reusable backend libraries, static UI module, safe QML test facades, and registered QML tests with CTest.
- Removed approved obsolete entry points/QRC, unused AccountFetcher, redundant includes/comments/IDs, duplicate editable application SVGs, and the test-only sell-quantity helper. Quantity coverage now verifies actual loopback preflight dispatch before helper removal.

## Executed build/test matrix

| Qt | Build | Host | CTest suites passing |
| --- | --- | --- | --- |
| 6.11.2 | Debug | macOS arm64 | 4/4 |
| 6.12.0 | Debug | macOS arm64 | 4/4 |
| 6.12.0 | Release | macOS arm64 | 4/4 |

The actual application was built in each configuration. Tests cover auth metadata/migration and stale requests, account/trade payloads, Auto Mode sequencing, fractional per-account sells, packaged icon decoding, Theme palette, desktop/compact module loading, session timeout/switching, and manual ticker aliases/Buy/Sell signals. Real-keychain tests remain skipped; all HTTP fixtures are local loopback and QML tests link mock-only facades.

The manual-input removal regression was observed failing before deleting the search dependency and passing afterward. Interactive tests wait for layout geometry and make their Qt Quick Test fixture visible.

## Safety/integrity assertions

- Actual compilation commands verified the live-order `RELEASE` definition only on production app/orchestration in Release. The separately compiled preflight test variant and test translation units have no `RELEASE` definition.
- Auth implementation and header are byte-identical to their original tracked sources; credential migration was not rewritten.
- 26 generated native artifacts are byte-identical. Existing bundle/package/version/SDK fields remain unchanged.
- `assets/icons/app.svg` exactly matches both removed editable source copies.
- Active source/CMake contains no autocomplete/controller, duplicate `tokenActive`, deleted helper, or old moved-path references.
- CodeGraph was refreshed. `git diff --check` passed. No commit, push, production app launch, live brokerage request, or real-keychain integration test was performed.

## Review and limitations

Independent read-only review (`deleg_a1def4b1`) passed: no security concerns, logic errors, or suggestions. Review included new/untracked moved files rather than relying on Git's tracked diff alone.

Qt 6.12 qmllint exited successfully but reported 90 `unqualified` access warnings; retained context-property/dynamic-scope bindings are not warning-free. Runtime component tests reject ReferenceError/TypeError/undefined/cannot-assign warnings.

Windows, Android, and iOS were not rebuilt here. Their generated native artifacts and packaging identities were checked by source comparison; mobile deployment documentation was updated for the executable's new binary directory.

## Artifacts

Debug macOS application: `build/architecture-debug/src/app/appPublicsTradingInterface.app`.

Additional builds: `build/architecture-qt612-debug/` and `build/architecture-qt612-release/` (Release app not launched).

Machine-readable matrix, JUnit XML, build logs, integrity assertions, reviewer verdict, and qmllint log: `/Users/hannsel101/.hermes/cache/scratch/publics-architecture/verification/`. Scratch logs may be pruned; this report remains in the repository.

Pre-refactor backup: `/Users/hannsel101/.hermes/backups/publics-architecture-20261024-112630`.
