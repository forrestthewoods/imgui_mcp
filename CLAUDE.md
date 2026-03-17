# imgui_mcp

Playwright for Dear ImGui — an MCP-based library that lets Claude control native C++ Dear ImGui applications.

## Architecture

```
Claude <-MCP (stdio)-> Python MCP Server <-TCP/JSON-> C++ ImGui App (with embedded bridge library)
```

## Building

```
# From repo root, with VS 2022/18 Build Tools:
MSBuild.exe imgui_mcp.sln -p:Configuration=Debug -p:Platform=x64
```

Output: `x64/Debug/demo_app.exe`

## Running

1. Launch `demo_app.exe` (listens on TCP port 8086)
2. MCP server: `cd mcp_server && uv run server.py`
3. Test script: `cd mcp_server && uv run python test_bridge.py`

## Key knowledge: ImGui test engine ref paths

**Ref paths are FLAT within a window, not hierarchical by visual section.**

The Dear ImGui Demo window has visual section headers like "Widgets", "Layout & Scrolling", etc. These are collapsing headers, NOT parent containers in the ref path hierarchy.

- WRONG: `Dear ImGui Demo/Widgets/Basic` (treats "Widgets" as a parent)
- RIGHT: `Dear ImGui Demo/Basic` (flat — "Basic" is a direct child of the window)

The only hierarchy separator is the **window name** as root. All tree nodes within a window are direct children in ref path terms, regardless of how deeply nested they appear visually.

**Discovery flow:**
1. `list_windows` — get window names
2. `list_items(ref="WindowName", depth=N)` — enumerate children
3. Items come back with a `depth` field indicating visual nesting, but ref paths use the `label` directly under the window: `WindowName/label`

**Section headers vs tree nodes:**
- Section headers like "Widgets" are collapsing headers — `open`/`close` works on them
- Sub-items like "Basic", "Bullets" under "Widgets" are siblings at the ref path level, not children of "Widgets"
- `list_items(ref="Dear ImGui Demo/Widgets")` returns NOTHING because "Widgets" is not a container — use `list_items(ref="Dear ImGui Demo")` instead

## Debugging: timeouts are never the answer

We run on local TCP loopback. It is extremely fast and reliable. If a command
appears to "time out", something in the C++ bridge or test engine **hung** —
the response never came back. Increasing the timeout just delays discovering
the real bug. Diagnose what caused the hang instead:

- The test engine's `ItemOpen`/`ItemClick` can get stuck if the item requires
  navigation that never completes (e.g., scrolling to an item inside a
  complex table, or an item that disappears when the tree is manipulated).
- The TCP thread blocks on `future.get()` waiting for the test coroutine. If
  the coroutine never processes the command (because it exited, errored, or
  got stuck), the TCP thread hangs forever.
- Look at the C++ console output for test engine errors/warnings.

**Items with `/` in their label can't be used as ref paths:**
- Labels like "Color/Picker Widgets" or "Drag/Slider Flags" have `/` in them
- The test engine interprets `/` as a path separator, so `Dear ImGui Demo/Color/Picker Widgets` is parsed as window/Color/Picker...
- These will return "Item not found" — this is expected, not a bug
- Also, `DebugLabel` is truncated to 32 chars, so long labels may not match

## Bridge design notes

- The bridge uses **on-demand test queuing** — the test engine test only runs while processing commands, then exits to release input control back to the human user
- `ImGuiMcpBridge_Tick()` must be called every frame from the main loop to check for pending commands and detect hung operations
- All item operations pre-check with `ItemExists` before acting — this prevents hangs from navigating to non-existent items
- `ImGuiTestOpFlags_NoError` is used on all operations to prevent a bad ref path from killing the test coroutine
- The test engine auto-scrolls and auto-focuses windows when interacting with items

### Abort mechanism (critical)

The test engine's `ScrollTo` has a `while (!Abort)` loop that can hang forever. The built-in watchdog (`ConfigWatchdogKillTest`) does NOT set `ctx->Abort` — it only logs an error. The bridge implements its own timeout:

1. `Tick()` uses wall-clock time (`std::chrono::steady_clock`) to track how long a command has been active
2. After 10 seconds, it calls `ImGuiTestEngine_AbortCurrentTest()` which properly sets `ctx->Abort`
3. A `TeardownFunc` fulfills any pending promises so the TCP thread doesn't hang on `future.get()`
4. A stall detector re-queues tests if `g_TestRunning` is true but no commands are being processed

**Do NOT use `ImGui::GetTime()`** for timeout detection — the test engine can override `DeltaTime` in Fast mode, making ImGui time advance inconsistently.
