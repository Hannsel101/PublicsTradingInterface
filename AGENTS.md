# PublicsTradingInterface agent guidance

## CodeGraph navigation

This repository has a local CodeGraph index in `.codegraph/`. Use the installed `codegraph` CLI from the repository root to locate relevant code before reading large files or searching the entire tree.

1. At the start of a new task, run `codegraph sync` to refresh the index. Refresh it again after source edits before relying on graph results. No persistent watcher is configured by this repository setup.
2. Start with a bounded query such as `codegraph explore "PublicApiWorker orderPayload" --max-files 3`. This returns relevant source and relationships in one call.
3. For exact symbols, use `codegraph query "orderPayload" --json`, `codegraph node "PublicApiWorker::orderPayload"`, or `codegraph impact "PublicApiWorker::orderPayload" --depth 2 --json`.
4. Read the actual source around the returned locations before changing code. Verify callers and affected tests before treating an impact result as complete.
5. If the index is missing, run `codegraph init --yes`. Use `codegraph status` to check coverage and freshness.

## Coverage and safety

- C++ files and headers are indexed. This installation did not index the project's QML files. Search and read QML directly; do not interpret an empty graph result as proof that a QML reference, signal handler, or dependency does not exist.
- Qt signals, slots, runtime QML bindings, dynamic dispatch, and generated code can create relationships that static analysis misses. Confirm important cross-language relationships in source and tests.
- Keep queries scoped to the task. Do not dump the entire graph or open every source file when one symbol query will do.
- The graph is generated local state, not an authoritative specification. Do not commit `.codegraph/` or include its database in patches.
- CodeGraph telemetry is disabled on this machine. Do not enable it or upload source, index data, credentials, or query output to an external service without user approval.
- CodeGraph is connected here through CLI guidance, not an automatically registered MCP server. Do not assume MCP tools or automatic background synchronization are available.
