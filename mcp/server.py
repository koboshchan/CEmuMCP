#!/usr/bin/env python3
"""MCP server that drives a real, running CEmu (TI-84 Plus CE emulator) window.

Talks to the McpServer/McpDockWidget feature added to CEmu's Qt frontend
(gui/qt/mcpserver.{h,cpp}, gui/qt/mcpdock.{h,cpp}) over a local newline-delimited
JSON socket, and exposes it as a single MCP tool ("control") shaped like
mcp__Claude_Code_iOS_Simulator__control: one `action` enum, flat optional args.
"""

import base64
import json
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Literal

from mcp.server.fastmcp import FastMCP, Image

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CEMU_BINARY = REPO_ROOT / "build" / "CEmu.app" / "Contents" / "MacOS" / "CEmu"
CEMU_BINARY = Path(os.environ.get("CEMU_MCP_BINARY", str(DEFAULT_CEMU_BINARY)))

mcp = FastMCP("cemu")


def _socket_path(mcp_id: str) -> str:
    return os.path.join(tempfile.gettempdir(), f"cemu-mcp-{mcp_id}")


class CemuConnection:
    """One JSON request/response socket to a single running CEmu instance."""

    def __init__(self) -> None:
        self.sock: socket.socket | None = None
        self.buf = b""
        self.next_id = 1
        self.proc: subprocess.Popen | None = None
        self.mcp_id: str | None = None

    def is_connected(self) -> bool:
        return self.sock is not None

    def connect(self, mcp_id: str, timeout: float = 20.0) -> None:
        path = _socket_path(mcp_id)
        deadline = time.monotonic() + timeout
        last_err: Exception | None = None
        while time.monotonic() < deadline:
            if os.path.exists(path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(path)
                    self.sock = s
                    self.buf = b""
                    self.mcp_id = mcp_id
                    return
                except OSError as e:
                    last_err = e
            time.sleep(0.2)
        raise RuntimeError(
            f"Timed out waiting for CEmu's MCP socket at '{path}'"
            + (f" ({last_err})" if last_err else "")
        )

    def spawn_and_connect(self, mcp_id: str, rom: str) -> None:
        if not CEMU_BINARY.exists():
            raise RuntimeError(f"CEmu binary not found at '{CEMU_BINARY}' (set CEMU_MCP_BINARY)")
        rom_path = Path(rom).expanduser().resolve()
        if not rom_path.is_file():
            raise RuntimeError(f"ROM file not found: '{rom_path}'")

        self.proc = subprocess.Popen(
            [
                str(CEMU_BINARY),
                "--rom", str(rom_path),
                "--no-settings",
                "--id", f"cemu-mcp-{mcp_id}",
                "--mcp",
                "--mcp-id", mcp_id,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.connect(mcp_id)

    def call(self, cmd: str, args: dict | None = None, timeout: float = 60.0) -> dict:
        if not self.sock:
            raise RuntimeError("Not attached to CEmu. Call action='attach' or action='launch' first.")

        request_id = self.next_id
        self.next_id += 1
        payload = {"id": request_id, "cmd": cmd}
        if args:
            payload["args"] = args

        self.sock.settimeout(timeout)
        self.sock.sendall((json.dumps(payload) + "\n").encode())

        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                self.sock = None
                raise RuntimeError("CEmu's MCP connection closed unexpectedly")
            self.buf += chunk

        line, _, rest = self.buf.partition(b"\n")
        self.buf = rest
        response = json.loads(line)
        if not response.get("ok"):
            raise RuntimeError(response.get("error", "Unknown CEmu MCP error"))
        return response

    def close(self, quit_app: bool) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if quit_app and self.proc and self.proc.poll() is None:
            self.proc.terminate()
        self.proc = None
        self.mcp_id = None


_conn = CemuConnection()


def _require_connection() -> CemuConnection:
    if not _conn.is_connected():
        raise RuntimeError("Not attached to CEmu. Call action='attach' or action='launch' first.")
    return _conn


@mcp.tool()
def control(
    action: Literal[
        "attach", "launch", "screenshot", "key", "text", "send",
        "reset", "reload_rom", "run_ms", "read_memory", "set_speed", "throttle",
        "status", "list_keys", "detach",
    ],
    id: str = "default",
    rom: str | None = None,
    program: str | None = None,
    name: str | None = None,
    asm: bool = False,
    key_name: str | None = None,
    key_action: Literal["tap", "down", "up"] = "tap",
    duration_ms: int = 80,
    text: str | None = None,
    files: list[str] | None = None,
    location: Literal["file", "ram", "arch"] = "file",
    ms: int | None = None,
    address: str | None = None,
    size: int | None = None,
    percent: int | None = None,
    enabled: bool | None = None,
    quit: bool = False,
):
    """Drive a real, running CEmu (TI-84 Plus CE emulator) window.

    action:
      attach       - connect to (or boot, given `rom`) a CEmu instance identified by `id`.
      launch       - attach (booting with `rom` if needed), then optionally send `program`
                     (.8xp/.b84/...) and run it by `name` (defaults to the program's stem).
      screenshot   - return the current LCD framebuffer as a PNG image.
      key          - press/hold/release a named physical key (see action='list_keys').
                     `key_action`: tap (default, uses duration_ms), down, or up.
      text         - type a string via letter keys (0-9, A-Z, case-insensitive).
      send         - transfer `files` (.8xp/.8xv/.../.b84 bundle) to `location`
                     (file/ram/arch) without launching anything.
      reset        - reset the emulator (like the physical reset button).
      reload_rom   - fresh-boot from the ROM file, discarding all RAM/emulation
                     state (like the "Reload ROM" menu action). Pass `rom` to
                     switch to a different ROM file first; otherwise reloads
                     whichever ROM is currently loaded.
      run_ms       - just let `ms` milliseconds of emulated time pass.
      read_memory  - read `size` bytes at `address` (hex "0x..." or a named
                     constant like "ram_start"), returned as hex.
      set_speed    - set emulation speed percentage (0-500).
      throttle     - enable/disable real-time throttling (`enabled`).
      status       - report whether a client is connected and listening.
      list_keys    - list valid physical key names for `key`/`keys`.
      detach       - disconnect; pass quit=True to also close the CEmu window.
    """
    if action == "attach":
        if _conn.is_connected():
            return {"ok": True, "already_attached": True}
        path = _socket_path(id)
        if os.path.exists(path):
            _conn.connect(id)
        elif rom:
            _conn.spawn_and_connect(id, rom)
        else:
            raise RuntimeError(
                f"No running CEmu instance found for id='{id}' and no `rom` given to boot one."
            )
        return _conn.call("status")

    if action == "launch":
        if not _conn.is_connected():
            path = _socket_path(id)
            if os.path.exists(path):
                _conn.connect(id)
            elif rom:
                _conn.spawn_and_connect(id, rom)
            else:
                raise RuntimeError("launch requires `rom` the first time (to boot CEmu).")

        if program:
            program_path = Path(program).expanduser().resolve()
            if not program_path.is_file():
                raise RuntimeError(f"Program file not found: '{program_path}'")
            _conn.call("send", {"files": [str(program_path)], "location": location}, timeout=200.0)
            launch_name = name or program_path.stem.upper()
        elif name:
            launch_name = name
        else:
            raise RuntimeError("launch requires `program` and/or `name`.")

        return _conn.call("launch", {"name": launch_name, "asm": asm})

    if action == "screenshot":
        result = _require_connection().call("screenshot")
        png_bytes = base64.b64decode(result["png_base64"])
        return Image(data=png_bytes, format="png")

    if action == "key":
        if not key_name:
            raise RuntimeError("key requires `key_name`.")
        return _require_connection().call(
            "key", {"name": key_name, "action": key_action, "duration_ms": duration_ms}
        )

    if action == "text":
        if text is None:
            raise RuntimeError("text requires `text`.")
        return _require_connection().call("text", {"text": text})

    if action == "send":
        if not files:
            raise RuntimeError("send requires `files`.")
        resolved = [str(Path(f).expanduser().resolve()) for f in files]
        return _require_connection().call("send", {"files": resolved, "location": location}, timeout=200.0)

    if action == "reset":
        return _require_connection().call("reset")

    if action == "reload_rom":
        args: dict = {}
        if rom:
            rom_path = Path(rom).expanduser().resolve()
            if not rom_path.is_file():
                raise RuntimeError(f"ROM file not found: '{rom_path}'")
            args["path"] = str(rom_path)
        return _require_connection().call("reload_rom", args)

    if action == "run_ms":
        if not ms:
            raise RuntimeError("run_ms requires `ms`.")
        return _require_connection().call("run_ms", {"ms": ms})

    if action == "read_memory":
        if not address or not size:
            raise RuntimeError("read_memory requires `address` and `size`.")
        return _require_connection().call("read_memory", {"address": address, "size": size})

    if action == "set_speed":
        if percent is None:
            raise RuntimeError("set_speed requires `percent`.")
        return _require_connection().call("set_speed", {"percent": percent})

    if action == "throttle":
        if enabled is None:
            raise RuntimeError("throttle requires `enabled`.")
        return _require_connection().call("throttle", {"enabled": enabled})

    if action == "status":
        return _require_connection().call("status")

    if action == "list_keys":
        return _require_connection().call("list_keys")

    if action == "detach":
        _conn.close(quit_app=quit)
        return {"ok": True}

    raise RuntimeError(f"Unknown action: '{action}'")


if __name__ == "__main__":
    mcp.run()
