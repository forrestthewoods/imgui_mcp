"""Manual test script for the imgui_mcp bridge.

Run the demo_app first, then run this script to test TCP communication.
Usage: python test_bridge.py
"""

import base64
import sys
import time

from bridge_client import BridgeClient


def main():
    client = BridgeClient(port=8086, timeout=10.0)

    print("Connecting to bridge...")
    try:
        client.connect()
    except ConnectionError as e:
        print(f"Failed to connect: {e}")
        print("Make sure the demo_app is running first.")
        sys.exit(1)

    print("Connected!\n")

    # 1. Ping
    print("--- ping ---")
    resp = client.send_command("ping")
    print(f"  {resp}\n")

    # 2. List windows
    print("--- list_windows ---")
    resp = client.send_command("list_windows")
    if resp.get("ok"):
        for w in resp["windows"]:
            print(f"  {w['name']} at ({w['x']:.0f},{w['y']:.0f}) size {w['width']:.0f}x{w['height']:.0f}")
    print()

    # 3. List items in "Hello, world!" window
    print("--- list_items(Hello, world!, depth=2) ---")
    resp = client.send_command("list_items", ref="Hello, world!", depth=2)
    if resp.get("ok"):
        for item in resp["items"]:
            indent = "  " * item.get("depth", 0)
            print(f"  {indent}{item.get('label', '???')}")
    print()

    # 4. Screenshot
    print("--- screenshot ---")
    resp = client.send_command("screenshot")
    if resp.get("ok"):
        image_b64 = resp["image"]
        image_bytes = base64.b64decode(image_b64)
        with open("screenshot_before.png", "wb") as f:
            f.write(image_bytes)
        print(f"  Saved screenshot_before.png ({resp['width']}x{resp['height']}, {len(image_bytes)} bytes)\n")
    else:
        print(f"  Failed: {resp.get('error')}\n")

    # 5. Click the button
    print("--- click(Hello, world!/Button) ---")
    resp = client.send_command("click", ref="Hello, world!/Button")
    print(f"  {resp}\n")

    time.sleep(0.5)

    # 6. Screenshot after click
    print("--- screenshot (after click) ---")
    resp = client.send_command("screenshot")
    if resp.get("ok"):
        image_b64 = resp["image"]
        image_bytes = base64.b64decode(image_b64)
        with open("screenshot_after.png", "wb") as f:
            f.write(image_bytes)
        print(f"  Saved screenshot_after.png ({resp['width']}x{resp['height']}, {len(image_bytes)} bytes)\n")
    else:
        print(f"  Failed: {resp.get('error')}\n")

    # 7. Item exists
    print("--- item_exists(Hello, world!/Button) ---")
    resp = client.send_command("item_exists", ref="Hello, world!/Button")
    print(f"  {resp}\n")

    print("--- item_exists(Nonexistent/Widget) ---")
    resp = client.send_command("item_exists", ref="Nonexistent/Widget")
    print(f"  {resp}\n")

    client.disconnect()
    print("Done!")


if __name__ == "__main__":
    main()
