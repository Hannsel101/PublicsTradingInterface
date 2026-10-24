# Development

## Source ownership

- `src/app/`: executable startup, QML context wiring, and native packaging/signing metadata.
- `src/auth/`: API-key persistence, existing credential migration, authorization, and the sole session-state owner (`PublicAuthClient`).
- `src/api/`: account discovery, portfolio balances, trade/preflight requests, and selected-key results.
- `src/trading/`: all-key Auto Mode orchestration.
- `qml/PublicsTradingInterface/`: one static QML module; `Main.qml` is its entry point and `Theme.qml` its shared palette singleton.
- `qml/PublicsTradingInterface/credentials/`: saved-key controls, switch-key button, session countdown, and action icons.
- `qml/PublicsTradingInterface/trading/`: manual ticker entry, standard/Auto Mode screens, and shared result presentation.
- `tests/unit/`, `tests/integration/`, `tests/qml/`: pure backend assertions, loopback HTTP fixtures, and Qt Quick tests.
- `assets/icons/app.svg`: the single editable application-icon source. Existing generated Android/iOS/desktop icons remain in their platform folders.

Ticker entry is deliberately manual, including comma-separated tickers. No autocomplete UI, instrument-search controller, search token wiring, or search cache is included.

## Build and test

Requirements: CMake 3.21+, C++20 compiler, Qt 6.10+ Quick/Gui/Network/Concurrent, and QtKeychain. Tests additionally require Qt Test and QuickTest. If QtKeychain is not installed, CMake fetches the pinned upstream revision declared in the root CMake file.

```sh
cmake -S . -B build/dev -DCMAKE_PREFIX_PATH=/path/to/Qt/macos -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/dev --parallel 4
ctest --test-dir build/dev --output-on-failure
```

The macOS application bundle is under `build/dev/src/app/appPublicsTradingInterface.app`. CTest also registers QML icon decoding, palette, desktop/compact module loading, session transitions, and manual multi-ticker input/button tests.

The shared backend libraries avoid compiling auth/account-discovery sources separately into every test. Auto Mode has a separately compiled preflight-only test library; production `publics_trading` and the app receive the `RELEASE` definition in standard Release builds or when `RELEASE_BUILD=ON` is explicitly selected. The test variant never receives that definition.

## Safety boundaries

- Release app builds submit live orders when the user initiates a trade. Debug uses preflight; never run a Release app for a smoke test.
- Backend network fixtures bind loopback only. QML tests link the UI module and safe fake facades, not the production auth/trading backends.
- CTest unsets `PUBLICS_RUN_KEYCHAIN_TEST` for backend executables, so real-keychain tests remain skipped even if the calling shell exported it. To intentionally run those integration tests, invoke the auth test executable directly with the opt-in environment variable; do not do so during normal refactor verification.
- Preserve native bundle/package IDs, signing identities, and the existing credential-migration behavior when making future changes.
- `TradeResultScreen.resultController` remains the shared standard/Auto Mode presentation contract.
- Build, IDE, and `.codegraph/` state are local/generated; do not commit them. Follow `AGENTS.md` for explicit CodeGraph synchronization and its Qt/QML limitations.

## Mobile packaging

Android configuration is in `src/app/CMakeLists.txt`; the package template and signing instructions remain under `android/`. The generated Android deployment directory now follows the executable's binary directory (`build/<configuration>/src/app/android-build`). iOS asset catalogs remain under `ios/`; regenerate platform icon artifacts from `assets/icons/app.svg` when changing the application icon.
