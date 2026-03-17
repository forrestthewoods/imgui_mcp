"""FastMCP server for controlling Dear ImGui applications via the imgui_mcp bridge."""

import base64
import os
import subprocess
import time

from mcp.server.fastmcp import FastMCP, Image

from bridge_client import BridgeClient

mcp = FastMCP("imgui")

# Global bridge client
_client = BridgeClient()
_app_process: subprocess.Popen | None = None

# Path to demo app executable (relative to this script)
_DEFAULT_APP_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "x64", "Debug", "demo_app.exe",
)


def _ensure_connected() -> BridgeClient:
    """Ensure we have an active connection to the bridge."""
    if not _client.is_connected():
        raise RuntimeError(
            "Not connected to an ImGui app. Use launch_app() first, "
            "or connect manually if the app is already running."
        )
    return _client


@mcp.tool()
def launch_app(
    app_path: str = "",
    host: str = "127.0.0.1",
    port: int = 8086,
    timeout: float = 10.0,
) -> str:
    """Launch an ImGui application with the MCP bridge and connect to it.

    If no app_path is provided, launches the built-in demo app.
    The app must have the imgui_mcp bridge library embedded and listening on the specified port.

    Args:
        app_path: Path to the ImGui app executable. Empty string uses the default demo app.
        host: TCP host to connect to (default: 127.0.0.1).
        port: TCP port the bridge listens on (default: 8086).
        timeout: Max seconds to wait for connection.
    """
    global _app_process

    if not app_path:
        app_path = _DEFAULT_APP_PATH

    if not os.path.isfile(app_path):
        return f"Error: App not found at {app_path}. Build the demo_app project first."

    # Launch the app
    _app_process = subprocess.Popen([app_path])

    # Retry connecting until the bridge is ready
    _client.host = host
    _client.port = port
    _client.timeout = 30.0

    start = time.time()
    last_error = ""
    while time.time() - start < timeout:
        try:
            _client.connect()
            resp = _client.send_command("ping")
            if resp.get("ok"):
                return f"Connected to ImGui app (PID {_app_process.pid}) on {host}:{port}"
            last_error = str(resp)
        except (ConnectionError, OSError) as e:
            last_error = str(e)
        time.sleep(0.5)

    return f"Failed to connect after {timeout}s: {last_error}"


@mcp.tool()
def screenshot() -> Image:
    """Capture a screenshot of the ImGui application.

    Returns the current visual state of the application as a PNG image.
    Use this to verify the result of actions (clicks, input, etc.) or to
    understand the current UI state.
    """
    client = _ensure_connected()
    resp = client.send_command("screenshot")

    if not resp.get("ok"):
        raise RuntimeError(f"Screenshot failed: {resp.get('error', 'unknown error')}")

    image_b64 = resp["image"]
    image_bytes = base64.b64decode(image_b64)
    return Image(data=image_bytes, format="png")


@mcp.tool()
def click(ref: str) -> str:
    """Click a widget identified by its ref path.

    The ref path follows ImGui's test engine path format:
    - Window name is the root: "Dear ImGui Demo"
    - Children separated by '/': "Dear ImGui Demo/Widgets/Basic/Button"
    - '##' ID suffixes are part of the path: "Window##UniqueID/Widget"

    The test engine automatically handles:
    - Focusing the target window
    - Expanding tree nodes along the path
    - Scrolling to make the item visible
    - Moving the mouse and clicking

    Use list_windows() to discover window names, then list_items() to discover
    child widget ref paths.

    Args:
        ref: Widget ref path (e.g., "Hello, world!/Button").
    """
    client = _ensure_connected()
    resp = client.send_command("click", ref=ref)

    if not resp.get("ok"):
        return f"Click failed: {resp.get('error', 'unknown error')}"
    return f"Clicked: {ref}"


@mcp.tool()
def list_windows() -> str:
    """List all visible ImGui windows.

    Returns the names, positions, and sizes of all active, visible windows.
    Use the window names as the root of ref paths for other commands
    (click, list_items, item_info, etc.).

    This is typically the first step in discovering the UI structure:
    1. list_windows() -> get window names
    2. list_items(ref="WindowName") -> get child widgets
    3. click(ref="WindowName/ChildWidget") -> interact
    """
    client = _ensure_connected()
    resp = client.send_command("list_windows")

    if not resp.get("ok"):
        return f"Failed: {resp.get('error', 'unknown error')}"

    windows = resp.get("windows", [])
    if not windows:
        return "No visible windows found."

    lines = []
    for w in windows:
        lines.append(
            f"- \"{w['name']}\" at ({w['x']:.0f}, {w['y']:.0f}) "
            f"size {w['width']:.0f}x{w['height']:.0f}"
            f"{' [collapsed]' if w.get('collapsed') else ''}"
        )
    return "\n".join(lines)


@mcp.tool()
def list_items(ref: str, depth: int = 2) -> str:
    """List child widgets within a parent widget or window.

    Use this to discover the ref paths of clickable/interactable widgets.
    The returned labels can be appended to the parent ref to form full paths.

    Example flow:
        list_items(ref="Hello, world!") might return items like:
        - "Demo Window" (checkbox)
        - "Button"
        - "float" (slider)

        Then you can click: click(ref="Hello, world!/Button")

    Args:
        ref: Parent widget or window ref path (e.g., "Dear ImGui Demo").
        depth: How many levels deep to enumerate (default: 2, use -1 for all).
    """
    client = _ensure_connected()
    resp = client.send_command("list_items", ref=ref, depth=depth)

    if not resp.get("ok"):
        return f"Failed: {resp.get('error', 'unknown error')}"

    items = resp.get("items", [])
    if not items:
        return f"No items found under \"{ref}\"."

    lines = []
    for item in items:
        indent = "  " * item.get("depth", 0)
        label = item.get("label", "???")
        lines.append(f"{indent}- \"{label}\"")
    return "\n".join(lines)


@mcp.tool()
def item_info(ref: str) -> str:
    """Get detailed information about a specific widget.

    Returns the widget's ID, label, position, size, and status flags.
    Useful for checking widget state before interacting.

    Args:
        ref: Widget ref path (e.g., "Hello, world!/Button").
    """
    client = _ensure_connected()
    resp = client.send_command("item_info", ref=ref)

    if not resp.get("ok"):
        return f"Item not found: {resp.get('error', 'unknown error')}"

    return (
        f"Label: {resp.get('label', '???')}\n"
        f"ID: {resp.get('id', 0):#010x}\n"
        f"Position: ({resp.get('x', 0):.0f}, {resp.get('y', 0):.0f})\n"
        f"Size: {resp.get('width', 0):.0f}x{resp.get('height', 0):.0f}\n"
        f"Status flags: {resp.get('status_flags', 0):#010x}"
    )


@mcp.tool()
def item_exists(ref: str) -> str:
    """Check whether a widget exists at the given ref path.

    This is a non-destructive probe - it won't cause errors if the item
    doesn't exist. Use this to verify UI state before taking actions.

    Args:
        ref: Widget ref path to check.
    """
    client = _ensure_connected()
    resp = client.send_command("item_exists", ref=ref)

    if not resp.get("ok"):
        return f"Error: {resp.get('error', 'unknown error')}"

    exists = resp.get("exists", False)
    return f"{'Exists' if exists else 'Does not exist'}: {ref}"


@mcp.tool()
def close_app() -> str:
    """Shut down the connected ImGui application gracefully.

    Sends a shutdown command to the bridge, which will close the app.
    """
    global _app_process

    try:
        client = _ensure_connected()
        client.send_command("shutdown")
    except Exception:
        pass

    _client.disconnect()

    if _app_process:
        try:
            _app_process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _app_process.kill()
        _app_process = None

    return "App closed."


if __name__ == "__main__":
    mcp.run()
