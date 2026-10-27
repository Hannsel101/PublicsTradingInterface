# Native Robinhood OAuth compatibility experiment

This standalone Qt Core/Network + Widgets app tests whether Robinhood accepts a native port of the `robin_stocks` password/verification flow. It is **not** the trading application and does not replace its browser login. There is no Python or WebView dependency.

## Safety and scope

- No request on startup. The user must enter credentials, consent and press **Test login**. Do not send credentials or verification codes in chat.
- Password/MFA/code values are transient; neither credentials nor OAuth tokens are saved. Only a non-secret device identifier is retained with the experiment's own QSettings identity. Memory clearing is best effort, not a secure-erasure guarantee.
- A complete successful token response is parsed with the existing `RHOAuthGrant` codec and delivered only to a C++ callback. The UI displays expiry, never token values. Closing the experiment discards its result.
- No identity/account/positions/order requests, no keychain operations and no trading activation. Verification may require the upstream inquiry/push GETs and challenge POSTs.
- Native password grants are an unofficial compatibility approach, not modern OAuth best practice (RFC 9700 §2.4). No provider approval, MFA, CAPTCHA or passkey requirement is bypassed; an unsupported challenge must fail explicitly.
- Production requests use the fixed verified-TLS Robinhood origin. Test transports are explicit loopback-only C++ fixtures, not user-editable URLs. No raw server response or credential-bearing diagnostics.

## Manual desktop test

Build/open the standalone project, not the main launcher:

```sh
/Users/hannsel101/Qt6/Tools/CMake/CMake.app/Contents/bin/cmake \
  -S spikes/robinhood-native-auth -B build/rh-native-auth-qt612 \
  -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/Users/hannsel101/Qt6/6.12.0/macos
/Users/hannsel101/Qt6/Tools/CMake/CMake.app/Contents/bin/cmake --build build/rh-native-auth-qt612 -j 4
/Users/hannsel101/Qt6/Tools/CMake/CMake.app/Contents/bin/ctest --test-dir build/rh-native-auth-qt612 --output-on-failure
open build/rh-native-auth-qt612/robinhood_native_auth_spike.app
```

Enter an authenticator code initially if required. For supported email/SMS challenges, use the verification-code field when enabled; for app approval, approve in the official Robinhood app. Cancel ends the local attempt, not a server-side action already received. Do not repeatedly retry a rejection or timeout.

A successful grant message is **not** evidence of verified identity, trading permission, durable storage or production integration. A failed attempt should be reported with the safe phase/status shown by this app—not a raw network dump, password, code or token.

## HTTP 400 diagnostics

The diagnostic update adds known error codes, fixed message categories and recognized response field names to a rejected-grant status. Unknown values and arbitrary field names are never printed. `fields=` means only that those recognized names occurred in the response; it does not establish which field is invalid or required. `credentials_rejected` classifies a provider message, not proof that a password is wrong. Report the status line only; no raw response or network dump. No authentication protocol or retry behavior is changed by this diagnostic update.

## Cross-platform boundary

The same C++ Core/Network client and native test UI are intended for desktop and Android. Android packaging includes its own Internet permission, TLS backend/OpenSSL libraries and distinct application identity `com.publicstradinginterface.robinhoodauthspike`; it must not overwrite the trading app. Production Robinhood remains disabled on Android.

Compilation/package/signature checks are separate from device TLS, background/resume behavior, secure-store integration and actual Robinhood acceptance. Windows/Linux/iOS are not claimed tested. This experiment deliberately does not test QtKeychain storage; production already has a separate storage abstraction that needs target runtime verification when integrated.

## Verdict: PARTIAL

Current header-parity revision:74 client cases and standalone CTest3/3 on Qt6.12 Debug and Release pass; the focused header/diagnostic ASan run reports17 passed with leak detection disabled. Android ARM64 Qt6.11.2 APK rebuilt/signature verified, with OpenSSL/TLS libraries and its separate identity. The self-contained ad-hoc-signed macOS Release bundle passed native Cocoa offline launch. Independent source reviews passed for the experiment and subsequent diagnostic/header changes. The user observed credential-rejection HTTP400 in the earlier native build despite a working fresh browser login with mobile approval; matching the selected reference headers does not prove that rejection is resolved. No live agent login, keychain operation, trade or Android device runtime test was performed. The user subsequently confirmed that the header-aligned desktop test displayed Full OAuth grant received after verification. This establishes user-reported acquisition for that desktop test only; individual header causality, Android device login, live identity/account validation, persistence, renewal and trading are not established. The experiment does not save the grant. Production remains unchanged.
