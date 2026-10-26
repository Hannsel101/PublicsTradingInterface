# Development

## Source ownership

- `shared/`: `AppletInterface` QObject contract (`initialize`, `start`, `canStop`, `stop`) in the `applet_contract` library. Reusable non-domain facilities belong here, not in an applet.
- `launcher/`: executable entry point, native packaging/signing metadata, generic lifecycle/navigation controller, and a single-window `TradingLauncher` QML module. Its `Loader` instantiates the selected applet view only after activation.
- `launcher/qml/TradingLauncher/AppletChoice.qml`: common neutral choice button with a stripe at 70% of its width. Publics uses its dark-blue selected-color accent; Robin Hood is a disabled lime-green placeholder.
- `applets/publics/src/`: isolated auth/API/Auto Mode targets and `PublicsApplet`, which owns their original wiring and registers QML context on activation.
- `applets/publics/qml/PublicsTradingInterface/`: unchanged module URI; `CoreView.qml` is an embeddable `Item`, `Theme.qml` its applet palette, and `credentials/` and `trading/` contain the original UI and assets.
- `applets/robin_hood/`: reserved CMake subproject; no executable, view, brokerage logic, or active launcher route exists yet.
- `tests/unit/`, `tests/integration/`, `tests/qml/`: pure backend assertions, loopback HTTP fixtures, and Qt Quick tests.
- `assets/icons/app.svg`: the single editable application-icon source. Existing generated Android/iOS/desktop icons remain in their platform folders.

Ticker entry is deliberately manual, including comma-separated tickers. No autocomplete UI, instrument-search controller, search token wiring, or search cache is included.

To add an applet, create its own `applets/<name>/` CMake subproject with an `AppletInterface` implementation and QML view. Register the instance in `launcher/main.cpp` and a choice/loading rule in `TradingLauncher/LauncherWindow.qml`; do not link one applet against another. Match the launcher choice stripe to the applet's theme accent. The launcher owns the only `ApplicationWindow`; applet roots must be `Item`s. Keep active work non-interruptible with `canStop()` and clear applet-local sessions in `stop()`.

## Build and test

Requirements: CMake 3.21+, C++20 compiler, Qt 6.10+ Quick/Gui/Network/Concurrent, and QtKeychain. Tests additionally require Qt Test and QuickTest. If QtKeychain is not installed, CMake fetches the pinned upstream revision declared in the root CMake file.

```sh
cmake -S . -B build/dev -DCMAKE_PREFIX_PATH=/path/to/Qt/macos -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/dev --parallel 4
ctest --test-dir build/dev --output-on-failure
```

The macOS application bundle is under `build/dev/launcher/appPublicsTradingInterface.app`. CTest also registers launcher lifecycle/navigation, mock-only launcher stripes and view embedding, QML icon decoding, palette, desktop/compact module loading, session transitions, and manual multi-ticker input/button tests.

The shared backend libraries avoid compiling auth/account-discovery sources separately into every test. Auto Mode has a separately compiled preflight-only test library; production `publics_trading` and the app receive the `RELEASE` definition in standard Release builds or when `RELEASE_BUILD=ON` is explicitly selected. The test variant never receives that definition.

## Safety boundaries

- Release app builds submit live orders when the user initiates a trade. Debug uses preflight; never run a Release app for a smoke test.
- Backend network fixtures bind loopback only. QML tests link both UI modules and safe fake facades, not the production auth/trading backends.
- CTest unsets `PUBLICS_RUN_KEYCHAIN_TEST` for backend executables, so real-keychain tests remain skipped even if the calling shell exported it. To intentionally run those integration tests, invoke the auth test executable directly with the opt-in environment variable; do not do so during normal refactor verification.
- Preserve native bundle/package IDs, signing identities, and the existing credential-migration behavior when making future changes.
- `TradeResultScreen.resultController` remains the shared standard/Auto Mode presentation contract.
- Build, IDE, and `.codegraph/` state are local/generated; do not commit them. Follow `AGENTS.md` for explicit CodeGraph synchronization and its Qt/QML limitations.

## Mobile packaging

Android configuration is in `launcher/CMakeLists.txt`; the package template and signing instructions remain under `android/`. The generated Android deployment directory follows the executable's binary directory (`build/<configuration>/launcher/android-build`). iOS asset catalogs remain under `ios/`; regenerate platform icon artifacts from `assets/icons/app.svg` when changing the application icon.
