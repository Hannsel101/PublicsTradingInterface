# Findings
- RHAccountModel currently exposes displayName/accountNumber/currentBalance only; active accounts are cleared on switch and not persisted.
- Current display labels infer retirement from free-form description/nickname and emit labels outside requested Standard Brokerage/IRA/Roth IRA; replace with structured classification.
- Configured CachedPositions endpoint already exists at api.robinhood.com/positions/?nonzero=true, Instruments at /instruments/; browser secondary-token path currently permits only UserProfile/Accounts. Extension must remain bounded read-only, same-origin/path validated and fail-safe for unverified token compatibility.
- Saved profile/session metadata lives under AppData/robinhood/sessions.json with QSaveFile/QLockFile and QtKeychain tokens separate. Cache must remain associated with a specific saved profile and brokerage account.
- Current active-account cards have no information action; add small lowercase-i image, accessible label, on-demand details and offline saved-account access.
- C++ agent owns model/session/network/config and associated C++ tests; QML agent owns UI/components/resources/mocks/QML tests/module CMake and uses architecture-qt612-release. Parent owns integrated builds/docs/top-level wiring. C++ agent uses architecture-debug; parent uses architecture-qt612-debug and QtCreator build.
## Review-driven fixes
- Optional holdings work needs a visible cancellation path even while busy: keep Switch account enabled for an active session and use existing clearActiveSession to abort the generation-scoped pending request. Clear old model/session state before emitting idle so reentrant busyChanged handlers cannot lose a newly selected profile.
- Qt Quick Labels default to AutoText; render external symbol/account labels and error text as PlainText. A literal-markup regression observed AutoText before the fix and passes with explicit PlainText.
- Qt.DefaultLocaleShortDate is not exposed by the installed Qt6.12 QML type metadata; use a supported explicit date/time format and test the rendered numeric shape independently.
- Existing Accounts contract accepts a successful results array with omitted next as complete; unlike positions, a nonempty Accounts next is rejected. Document complete empty Accounts semantics explicitly.

## Primary-source protocol findings
- Official public App bundle AccountRecord contains brokerage_account_type, independently of type=cash/margin. Its enum maps individual, ira_roth, ira_traditional plus inherited/SEP/SIMPLE variants. Classification must prioritize brokerage_account_type; current parser omits that field and can mislabel a cash IRA as an ordinary cash account.
- Source: https://cdn.robinhood.com/assets/generated_assets/webapp/App-f3aefc7efb59c27b.js ; previously downloaded unauthenticated public cache at /Users/hannsel101/.hermes/cache/scratch/rh-public-mu8t97a0/App-f3aefc7efb59c27b.js . Only relevant public-code snippets inspected, no account data/authenticated request.
- Upstream robin_stocks account.py get_open_stock_positions passes account_number into positions_url and nonzero=true, and paginates. Query each verified brokerage account explicitly; a default unfiltered request can omit retirement holdings. Positions include account/account_number/instrument/quantity; instrument object provides symbol.
- Primary implementation URLs: https://raw.githubusercontent.com/jmfernandes/robin_stocks/master/robin_stocks/robinhood/account.py , urls.py and stocks.py in same directory. This is unofficial SDK evidence, not confirmation of live secondary-token permissions.
- Python urllib retrieval failed due local CA certificates; system curl with normal TLS verification successfully fetched the source. Did not disable TLS. Source-cache file-search helper errored; bounded pathlib/cached public-JS inspection succeeded.
- Both important protocol findings sent to C++ implementation agent before refresh/classification is finalized.

