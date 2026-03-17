"""TCP client for communicating with the imgui_mcp C++ bridge."""

import json
import socket
import threading
from typing import Any


class BridgeClient:
    """Thread-safe TCP client for the imgui_mcp bridge.

    Sends newline-delimited JSON commands and reads JSON responses.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 8086, timeout: float = 30.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._lock = threading.Lock()
        self._next_id = 1
        self._recv_buf = ""

    def connect(self) -> None:
        """Connect to the bridge TCP server."""
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.settimeout(self.timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock.connect((self.host, self.port))
        self._recv_buf = ""

    def disconnect(self) -> None:
        """Disconnect from the bridge."""
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None

    def is_connected(self) -> bool:
        return self._sock is not None

    def send_command(self, cmd: str, **kwargs: Any) -> dict:
        """Send a command and wait for the response.

        Args:
            cmd: Command name (e.g., "ping", "click", "screenshot").
            **kwargs: Additional parameters (e.g., ref="Window/Button").

        Returns:
            Response dict from the bridge.

        Raises:
            ConnectionError: If not connected or connection lost.
        """
        with self._lock:
            if not self._sock:
                raise ConnectionError("Not connected to bridge")

            msg_id = self._next_id
            self._next_id += 1

            request = {"id": msg_id, "cmd": cmd, **kwargs}
            data = json.dumps(request) + "\n"

            try:
                self._sock.sendall(data.encode("utf-8"))
            except (OSError, BrokenPipeError) as e:
                self._sock = None
                raise ConnectionError(f"Failed to send: {e}") from e

            # Read response (newline-delimited JSON)
            while True:
                newline_pos = self._recv_buf.find("\n")
                if newline_pos >= 0:
                    line = self._recv_buf[:newline_pos]
                    self._recv_buf = self._recv_buf[newline_pos + 1 :]
                    try:
                        return json.loads(line)
                    except json.JSONDecodeError as e:
                        raise ConnectionError(f"Invalid JSON response: {e}") from e

                try:
                    chunk = self._sock.recv(65536)
                except (OSError, socket.timeout) as e:
                    self._sock = None
                    raise ConnectionError(f"Failed to recv: {e}") from e

                if not chunk:
                    self._sock = None
                    raise ConnectionError("Connection closed by bridge")

                self._recv_buf += chunk.decode("utf-8")
