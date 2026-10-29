# Findings

## Primary sources, verified this task
- https://ngrok.com/docs/agent/api : local base http://127.0.0.1:4040/api; GET /api/tunnels returns tunnels/proto/public_url/config.addr. Tunnels API deprecated in favor of endpoints but currently documented.
- https://core.telegram.org/bots/api#setwebhook : HTTPS POST updates, secret_token header support; getUpdates unavailable while webhook registered; preserve pending updates explicitly.
- Correct provider origin is https://api.telegram.org/bot<TOKEN>/METHOD, not telegram.org<TOKEN>.
- ngrok forwards traffic; it does not create application HTTP server.

## Live environment
- ngrok was subsequently installed at `/opt/homebrew/bin/ngrok` (3.39.13). Latest read-only diagnosis: config check exits 1 for missing configuration; standard config files and agent/GUI NGROK_AUTHTOKEN are absent. Account authentication remains a user setup blocker; no live tunnel was opened.
- Qt6.12 macOS kit includes HttpServer and WebSockets.
- Existing class is TelegramAgent; TelegramBotRuntime is compatibility alias.
- Existing health UI active = runtime.active && runtime.healthy; navigation uses transport lifetime.
- Existing exit dialog immediately closes on accepted and only emits confirmed if stopAll returns true; async stop needs pending-exit coordination.
- New host code/CMake and agent code are separate delegated owners; parent owns UI. Do not run shared builds concurrently or count missing shared integration as RED.
