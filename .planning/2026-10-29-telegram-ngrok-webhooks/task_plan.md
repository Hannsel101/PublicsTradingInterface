# Task Plan: Telegram ngrok webhooks

## Goal
Automatically manage an owned ngrok tunnel and protected webhook receiver for each activated bot, preserve health status and safe asynchronous Stop, and deliver complete tested source.

## Current Phase
Verification and delivery

## Next Step
User must configure their ngrok account authtoken privately, then retry Start Bot. Read-only diagnosis found no default config and no agent/GUI NGROK_AUTHTOKEN; the generic process-exit log does not establish an exact ngrok error code. Original webhook review fixes and packaging remain pending.

## Phases
### Phase 1: Requirements and primary documentation
- User authorized adding a real receiver on8080 and necessary modifications.
- Correct API endpoints verified; webhook delivery replaces polling.
- **Status:** complete
### Phase 2: Shared ngrok host and protected receiver
- Owner deleg_d723b09e / sa-0-f61f5f18: new telegramwebhookhost.*, host tests and both CMake files.
- Shared managed process and route leases; private authenticated local health route.
- **Status:** complete
### Phase 3: Agent webhook protocol and cleanup
- Owner deleg_9dbd77c0 / sa-0-78868d5a: telegramagent.* and new test_telegramwebhookagent.cpp only.
- Production default webhook; explicit injected legacy constructors retain polling.
- Add stopping() for bounded asynchronous cleanup; active only after registration readback.
- **Status:** complete
### Phase 4: UI lifecycle integration
- Parent owns controller, QML exit dialog, QML test mock and UI/controller regressions.
- Hold navigation and Start until transport/process cleanup finishes.
- **Status:** complete
### Phase 5: Verification and delivery
- Register agent test target after child CMake ownership ends; run all tests, review and package all dependencies.
- No live tunnels, real Telegram writes, keychains or ngrok installation in tests.
- **Status:** in_progress

### Phase 6: User-directed Qt 6.12 desktop Release upgrade
- Activated the installed Qt 6.12.0 macOS kit in saved Qt Creator project settings after the user closed the IDE; kept existing desktop/mobile targets.
- Fresh Ninja Release configure/build succeeded; all 25 CTest targets passed. Verified SDK/module cache paths, linked Qt 6.12.0 frameworks, settings references and backups.
- **Status:** complete

### Phase 7: Managed ngrok process-exit diagnosis
- Located the generic exit/cleanup path and verified ngrok installation, missing default configuration and absent agent/GUI authentication environment.
- Diagnosis complete; live startup verification awaits private user account setup. No public tunnel or real Telegram mutation attempted.
- **Status:** blocked

## Decisions
- Managed QProcess, never detached; shared tunnel for multiple bots, close on last Stop.
- Dedicated protected POST route per bot; no public arbitrary application API.
- Keep local default /health through private header to owned receiver.
- Explicit Stop retires local intake immediately, then bounded remote cleanup and process termination; expose stopping().
- Cleanup preserves other webhook replacements and never drops pending updates.

## Errors
ngrok is installed; account authentication is not configured in the checked default locations/environments. Exact child stderr was not retained by the current app. Qt 6.12 build setup corrections are recorded in progress.md.
