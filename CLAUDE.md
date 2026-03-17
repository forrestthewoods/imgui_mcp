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

## Bridge design notes

- The bridge uses **on-demand test queuing** — the test engine test only runs while processing commands, then exits to release input control back to the human user
- `ImGuiMcpBridge_Tick()` must be called every frame from the main loop to check for pending commands
- All item operations use `ImGuiTestOpFlags_NoError` to prevent a bad ref path from killing the test coroutine
- The test engine auto-scrolls and auto-focuses windows when interacting with items
