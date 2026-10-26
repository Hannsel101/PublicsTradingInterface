# Findings
- Existing application is QML/Quick; use QML WebEngineView (desktop equivalent of widget QWebEngineView).
- Qt6.11.2 macOS has WebEngineQuick/Core and WebView modules. Qt6.12 macOS lacks these installed packages. Android6.11.2 has native WebView modules.
- Official Qt QWebEngineCookieStore exposes cookieAdded with full QNetworkCookie and supports native HttpOnly observation per profile.
- QtWebView QML cookieAdded has only domain/name; on native backends it is only emitted for explicitly setCookie operations, not websites. Android needs native CookieManager rather than a fictitious QML cookie-value getter or document.cookie.
- Android CookieManager.getCookie(url) provides native cookie header pairs for a fixed URL; requires platform bridge and lifetime correlation.
- Exact secondary cookie name provided by user; its mapping to API/Phoenix bearer or cookie transport is not yet verified. Do not invent endpoints or treat secondary token as standard OAuth without explicit scoped transport/validation.
- Existing sessions store OAuth tokens in QtKeychain RobinHoodAuthN and metadata in sessions.json. Publics has separate targets/network and must stay unchanged.
## Primary Robinhood transport research
- A fresh unauthenticated fetch of the official homepage/login and public App/Vendor bundles succeeded; no user cookies or authenticated request was used.
- Official bundle separates `access_token` from `read_only_secondary_access_token`; secondary value is stored in the named cookie, while API auth agent uses primary `access_token` as Bearer. This does not prove secondary and primary tokens interchangeable.
- Official public code confirms read-only `https://api.robinhood.com/user/` and `/accounts/?default_to_all_accounts=true` paths, not secondary compatibility.
- Public Integuru implementation commit 0e5e76e190800f958a384df18a2b44ced14e1f1a uses secondary-cookie value as Bearer only for GET `https://bonfire.robinhood.com/tax_center_web/`; no Phoenix/profile/accounts implementation.
- Phoenix remains unconfigured; do not fabricate endpoints or claim working secondary-Phoenix auth. Candidate read-only UserProfile/Accounts verification is explicitly restricted and fails closed before cache on rejection.
- Official secondary cookie setter is JavaScript-driven with path `/`, Secure on HTTPS, no Domain; do not incorrectly require HttpOnly. Native capture supports HttpOnly as well without JS scraping.
- Public evidence snapshots (no credentials) at `/Users/hannsel101/.hermes/cache/scratch/rh-public-mu8t97a0/`.
- Official hashed bundles: `https://cdn.robinhood.com/assets/generated_assets/webapp/App-f3aefc7efb59c27b.js`, `https://cdn.robinhood.com/assets/generated_assets/webapp/Vendor-6759c1fbc4a6516e.js`.

## Implementation decisions
- Desktop native cookie store is attached to a fresh off-the-record QML WebEngineProfile. QML receives lifecycle/status only; C++ token handler is a std::function, not a token-valued Qt signal/property.
- Android starts at about:blank, clears only the named Robinhood secondary cookie natively, then loads the fixed official site after reset. Native cookie polling is generation-correlated; no global application cookie deletion or document.cookie scraping.
- Browser controller synthetic backend supports offline lifecycle/security tests without opening a site. UI tests load a local mock browser component.
- Session manager will disable headless signIn, authenticate the captured token against read-only UserProfile/Accounts before committing QtKeychain, and distinguish WebSecondaryBearer sessions from legacy OAuth Bearer sessions.
- Cancellable browser/verification stage is separate from the non-cancellable keychain/metadata commit stage.
- QtWebEngineQuick initialization is required before QGuiApplication; native QtWebView initialization follows it. Startup hooks live behind RobinHoodApplet methods; Publics code stays untouched.
- Existing Android KDAB integration already defines target-ABI OpenSSL::Crypto. Robinhood CMake now reuses an existing target rather than running host-oriented FindOpenSSL over it.

## Primary resources
- https://doc.qt.io/qt-6/qwebenginecookiestore.html
- https://doc.qt.io/qt-6/qml-qtwebview-webview.html
- https://developer.android.com/reference/android/webkit/CookieManager

## Independent native/runtime and security findings
- Native Chromium cookie store is lazy even after loadAllCookies(): attach an actual about:blank WebEngineView to initialize it. A standalone profile-only fixture queues synthetic cookie operations indefinitely. Integrated fixture now asserts real native HttpOnly capture and disconnection after finish without any brokerage navigation.
- QtWebView onUrlChanged is post-dispatch notification, unlike desktop WebEngine navigationRequested. Android must remain unavailable until a native pre-navigation interceptor is implemented and verified; retained JNI/WebView code alone is not sufficient.
- Cancellation cleanup must retain its generation across repeated inactive stop/destruction; only a genuinely newer start should invalidate an old deletion. Fix/regression delegated.
- Android native compilation succeeds but APK packaging is separately blocked by a missing installed Java runtime.
