# Progress
## Android launcher availability
- Added Android-only Robin Hood launcher disablement via Qt.platform.os, reusing existing muted background/text/stripe styling. Kept desktop and Publics selection enabled; disabled accessibility label explicitly states unavailable on Android.
- New regression first failed four data rows before implementation, then passed Android-unavailable/desktop-enabled states at 1000x700 and 360x620. Verifies viewport placement, grey pixel rendering, ignored disabled clicks, no Robinhood loader creation, and normal Publics navigation.
- Initial optional pixel probe sampled the wrong snapshot coordinates; corrected to a composed-window capture with mapped coordinates, rendering wait and physical-pixel scale. Focused and whole QML tests pass.
- Qt6.12 application rebuilt and full CTest13/13 passes; final QML suite passes after pixel coverage. QtCreator Qt6.11.2 app rebuilt and QML suite passes.
- Android arm64 debug APK rebuilt successfully with the updated launcher; apksigner verifies v2 signature. No device deployment/runtime claim.
- Independent read-only review PASSED with no concerns, logic errors or suggestions. Changes remain uncommitted. Updated Android APK is built and signature verified; no device runtime test performed.

## Discovery
- Read skills, repository guidance, CodeGraph, existing applet/session/network source and previous plan without changing prior task history.
- Inspected installed Qt modules; researched official Qt and Android cookie APIs.
- Created isolated plan and task checklist.
## Verification
- Dispatched independent primary-source protocol research; browser bridge; session/network; and QML workstreams with disjoint file ownership.
- Parent added conditional Qt browser discovery/QML packaging, bridge target/tests, applet context/startup/lifecycle wiring, and QtKeychain/runtime dependencies.
- First Qt6.11 desktop configure confirmed browser module availability and exposed imported-target scope warnings plus a not-yet-created parallel QML source. Moved optional module discovery to root scope; final dependencies remain linked only to Robinhood targets.
- Added native Chromium cookie-store integration test using a synthetic HttpOnly host-only cookie, with no page load or brokerage request.
- Existing Android OpenSSL target ABI is reused for the legacy locker dependency.
- Integrated Qt6.11.2 desktop app and all test targets built successfully with native WebEngine.
- First integrated CTest: 11/13 targets passed; Android reset simulation failed once, and real native cookie-store test timed out after 60 seconds. Investigating both before claiming completion; no live brokerage or real keychain access.
- Existing uncommitted work remains preserved.
- Fixed post-capture Android poll handling; browser unit suite now 60/60 passes on Qt6.11.2.
- Full Qt6.12 no-WebEngine configuration built and all 12 CTest targets passed, including unavailable-backend behavior.
- Android arm64-v8a application and JNI/native-WebView bridge compiled successfully with installed Qt6.11.2 and target-ABI OpenSSL. No Android device runtime/login exercised.
- Native WebEngine capture integration remains under investigation; independent origin/lifecycle/token-scope review dispatched.
- Android APK packaging reached androiddeployqt/Gradle but failed: no Java runtime available. Checked standard Android Studio, Homebrew and SDKMAN JDK locations; none installed. Native arm64 application build remains successful.
- Updated Robinhood README and marked locker documentation historical; documented secondary/primary distinction, fail-closed verification, unsupported Phoenix routing, Android shared-jar limitations and no live CAPTCHA/push claim.
- git diff --check passes; CodeGraph synced; Publics source diff remains empty.
- Independent native investigation resolved Chromium timeout: a standalone profile queues cookie operations until a view initializes it. Real about:blank WebEngineView now initializes the integration fixture; five isolated runs and Cocoa pass. Parent will verify integrated QtCreator suite.
- Security review found two Android blockers: post-dispatch URL notification is not pre-navigation enforcement; repeated inactive stop invalidates the only queued cookie cleanup. A separate fix agent owns fail-closed Android disabling and idempotent cleanup with RED/GREEN regressions.
- Parent observed QML unavailable-copy regression RED; UI copy now explicitly explains Android pre-navigation blocker rather than recommending module installation alone. Focused GREEN and complete QML CTest target pass.
- Parent rebuilt the real native integration target in the QtCreator build and ran it three times: all pass (0.84, 0.52, 0.54 seconds). Captures a synthetic HttpOnly cookie through Chromium, not a mock; no brokerage page loads.
- Saved reusable navigation-boundary and idempotent-cleanup lessons in the C++ safety skill.
- Separate fix agent disabled production Android sign-in pending native pre-navigation interception and made inactive stop idempotent. Tests observed policy/cleanup RED then 63/63 GREEN, including a host Android-policy probe (not device execution).
- Parent rebuilt all final C++ changes: QtCreator Qt6.11.2 full CTest 13/13 PASS, Qt6.12 no-browser full CTest 12/12 PASS; Android arm64 app and browser-unit native targets compile successfully. Native Android tests were not run on a device.
- Updated configure/unavailable UI copy so installing QtWebView alone is not represented as enabling safe Android sign-in. All three application targets rebuilt after the configure-copy changes.
- Independent read-only security-fix re-review PASSED: no security concerns or logic errors. Nonblocking stale session-manager unavailable copy was corrected with a new observed RED/GREEN regression. Parent rebuilt/reran full Qt6.11.2 CTest 13/13, Qt6.12 CTest 12/12 and Android app compile after that final change.
- APK follow-up complete: with installed Temurin21.0.13, the existing Qt6.11.2 arm64 appPublicsTradingInterface_make_apk target succeeds (Gradle BUILD SUCCESSFUL). ZIP integrity and required DEX/manifest/native app/WebView/OpenSSL entries checked; apksigner verifies APK Signature Scheme v2; aapt reports arm64-v8a, minSDK28 and targetSDK36. APK is 77,214,856 bytes at build/hermes-qt6112-android-arm64-s24-make/launcher/android-build/build/outputs/apk/debug/android-build-debug.apk. No device install/run and no change to Android login security gates.
- JDK follow-up: java_home detects Temurin21.0.13 arm64 at /Library/Java/JavaVirtualMachines/temurin-21.jdk/Contents/Home; java and javac both report21.0.13. Retrying existing Qt6.11.2 Android APK packaging with JAVA_HOME set for the build; not modifying global shell/QtCreator settings or deploying to a device.
- Follow-up verification complete: newly installed Qt6.12 desktop WebEngine backend builds, native synthetic cookie capture runs, and full CTest13/13 passes. JDK21 ARM64 identified and Homebrew Temurin21 availability checked; no JDK installed without user authorization. Android pre-navigation/Phoenix compatibility gates remain unchanged.
- Follow-up: user installed Qt6.12 packages. Live inspection confirms desktop WebEngineQuick/WebEngineCore/WebView/TaskTree and Android WebView modules now exist. JAVA_HOME is unset and java_home finds no JDK; Qt6.12 official prerequisites require JDK21 or newer. Revalidating desktop build with installed browser backend, not installing a system JDK without request.
- Prepared verified 37-entry source archive at /Users/hannsel101/.hermes/cache/scratch/rh-browser-source-20261026/robinhood-browser-refactor.zip with full current Robinhood classes, QML, integration hooks, regression tests and explicit compatibility/platform limits. Archive is a selected-file snapshot, not a standalone app or automatic patch.
