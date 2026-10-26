# Robinhood browser token login
## Goal
Replace only the Robinhood applet's direct credential login with official-site embedded browser authentication, native cookie capture, scoped validated token use and QtKeychain sessions.
## Next Step
Deliver the verified source snapshot and explicit partial-support limits; no commit or live login performed.
## Current Phase
Delivery ready — verified desktop implementation; Android disabled and Phoenix compatibility unverified.
## Phases
### Phase 1: discovery
- [x] Verify installed modules, official browser APIs, existing applet and client boundaries.
- **Status:** complete
### Phase 2: browser and token boundary
- [x] Implement desktop WebEngine and Android WebView/native cookie bridge with origin/lifecycle checks.
- **Status:** complete
### Phase 3: session and UI integration
- [x] Validate captured token, preserve RobinHoodAuthX key labels, replace credential fields and reauthentication with browser flow.
- **Status:** complete
### Phase 4: verification
- [x] Build installed desktop/Android targets, run offline unit/QML regressions and independent review.
- **Status:** complete
### Phase 5: delivery
- [x] Record real verification and platform/live-site limits; supply implementation and hooks.
- **Status:** complete
## Delivered support boundary
- Desktop official-site browser flow, native capture, verified-token QtKeychain handoff and QML hooks implemented and tested offline.
- Android source scaffold compiled but production sign-in disabled pending a verified native pre-navigation interceptor. APK packaging additionally requires a missing JDK.
- Secondary-token UserProfile/Accounts mapping is provisional; rejected candidates are not saved. Phoenix routes remain disabled without a verified auth contract. No live provider/CAPTCHA/push success is claimed.
## Decisions
- Scope remains Robinhood only; preserve Publics and existing dirty work.
- No automated live brokerage login/keychain reads/trades; synthetic cookie and loopback tests only.
- Capture exact cookie natively, never through JavaScript/password interception, never log/expose its value to QML.
- Browser CAPTCHA/push and secondary-token API compatibility are not guaranteed without live user verification.
## Errors Encountered
| Error | Attempt | Resolution |
|---|---|---|
| Qt6.12 desktop WebEngine/WebView modules absent | 1 | Qt6.11.2 desktop modules are installed; test unsupported backend behavior separately. |
| Desktop optional Qt imports scoped below Launcher finalizers | 1 | Move optional discovery to common root scope, still link only Robinhood targets. |
| Desktop QML component not yet created during parallel work | 1 | UI agent owns file creation; do not repeat configure until interface sources are ready. |
| Android reset simulation stopped after token capture on late native failure | 1 | Ignore native poll completions after capture delivery; rebuilt unit suite 60/60 passes. |
| Native Chromium cookie integration hangs after synthetic cookie queued | 1 | Lazy Chromium backend needs an actual about:blank view; real capture now passes repeatedly with sandbox unchanged. |
| Android APK Gradle launch cannot find Java runtime | 1 | Native arm64 build passes; standard installed JDK locations absent. Packaging blocked pending JDK, no device deployment attempted. |
