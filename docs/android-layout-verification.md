# Android layout verification

Verified October 24, 2026 with Qt 6.11.2 Android arm64 and a read-only
`Samsung_Galaxy_S24_Ultra_API_34` AOSP emulator. This is not physical Samsung
One UI verification.

## Fixes

- Session/key status and workspace descriptions wrap long unbroken nicknames.
- Auto Mode key rows elide long names inside their existing cards.
- Edit/delete dialogs stay inside the overlay; oversized content scrolls and
  action buttons remain reachable. Delete confirmation placeholders use chained
  QML `arg()` calls so the full prompt actually renders.
- Compact trading headers stack their text and Switch key action instead of
  overflowing horizontally. Desktop headers retain two columns.
- Trading panels derive their minimum height from content and padding rather
  than allowing Buy/Sell controls to escape a short card.

## Verification

`tests/qml/tst_mobile_layout.qml` uses the existing mock-only QML facades; it
cannot access production brokerage or keychain objects. Regressions were seen
failing before their corresponding fixes.

- Native Android: 19 test results passed, zero failed in each configuration:
  normal portrait, increased display density with 150% system font scale, and
  landscape-sized display override with the same increased font scale.
- Desktop fixtures cover 360–1000 logical-pixel widths, short viewports, long
  nicknames, key row/action containment, dialog scrolling, inactive credentials,
  Auto Mode, active trading, and session-timer text containment.
- All four CTest suites passed in Qt 6.11.2 Debug, Qt 6.12.0 Debug, and Qt 6.12.0
  Release on macOS. Application builds passed in those configurations.
- Production Android arm64 Release APK rebuilt successfully; ZIP integrity
  checked. The new APK is unsigned and was not installed or launched.
- Independent read-only layout audit completed. Its header/panel overflow
  findings were reproduced and fixed. The timer-size concern did not reproduce
  in the added containment checks, so its behavior was left unchanged.
- Captured emulator images were inspected. Emulator overrides were restored
  and the disposable emulator was stopped.

Local logs, JSON results, images, source/APK hashes, and the isolated probe setup
are under `build/android-layout-verification/` (ignored build output).
The APK is at
`build/android-s24-release/src/app/android-build/build/outputs/apk/release/android-build-release-unsigned.apk`.

Run desktop regressions with `ctest --test-dir build/architecture-debug --output-on-failure`.
For Android builds/signing, follow `android/README.md`; any older signed APK in
that build directory is not automatically refreshed by rebuilding the unsigned APK.

## Limits and safety

No physical S24 Ultra was connected. Check the rebuilt app on the actual device,
including its One UI font/display settings and keyboard behavior. iOS and Windows
were not rebuilt during this change. No production credentials, real keychain
operations, or live trading/preflight requests were used. Trading/authentication
logic and native signing identities were not modified. Changes remain uncommitted.
