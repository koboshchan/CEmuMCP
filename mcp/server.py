#!/usr/bin/env python3
"""MCP control of the Qt-free cemu-headless stdin/stdout protocol."""
import asyncio
import io
import os
import tempfile
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Literal

from PIL import Image as PILImage
from mcp.server.fastmcp import FastMCP, Image

ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("CEMU_MCP_BINARY", ROOT / "tests/autotester/cemu-headless"))
KEYS = "graph trace zoom window wind y= yequ 2nd mode del on sto ln log ^2 sq -1 inv math alpha 0 1 4 7 comma sin apps xton xtn dot 2 5 8 lpar cos prgm stat (-) neg 3 6 9 rpar tan vars enter + add - sub * mul / div ^ pow clear clr down left right up".split()
LETTERS = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "math apps prgm inv sin cos tan ^2 comma lpar rpar div log 7 8 9 mul ln 4 5 6 sub sto 1 2 3".split()))
lock = asyncio.Lock()
instances = {}
active = None


def clean(value: str) -> str:
    if any(c in value for c in "\r\n\0"):
        raise ValueError("Newlines and NUL are not allowed in protocol arguments")
    return value


def path(value: str | None, exists=False) -> str:
    if not value:
        raise ValueError("A file path is required")
    p = Path(clean(value)).expanduser().resolve()
    if exists and not p.is_file():
        raise ValueError(f"File not found: {p}")
    return str(p)


def number(value: int | None, label: str, maximum=600000) -> int:
    if value is None or not 0 <= value <= maximum:
        raise ValueError(f"{label} must be between 0 and {maximum}")
    return value


class Headless:
    def __init__(self, proc, rom):
        self.proc, self.rom = proc, rom

    @classmethod
    async def start(cls, rom=None, image=None):
        if not BINARY.is_file():
            raise RuntimeError(f"Build cemu-headless first or set CEMU_MCP_BINARY: {BINARY}")
        source = path(image or rom, exists=True)
        proc = await asyncio.create_subprocess_exec(
            str(BINARY), "--image" if image else "--rom", source,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        instance = cls(proc, rom)
        try:
            ready = await asyncio.wait_for(proc.stdout.readline(), 30)
            if not ready.startswith(b"CEMU_HEADLESS_READY"):
                raise RuntimeError(f"Headless failed to start (exit {proc.returncode}): {ready!r}")
        except BaseException:
            await instance.close()
            raise
        return instance

    async def call(self, command):
        if self.proc.returncode is not None:
            raise RuntimeError("Headless process exited; detach and attach again")
        self.proc.stdin.write((clean(command) + "\n").encode())
        try:
            await self.proc.stdin.drain()
            result = (await asyncio.wait_for(self.proc.stdout.readline(), 200)).decode().strip()
        except BaseException:
            # A timeout/cancellation leaves response framing uncertain. Never reuse it.
            await self.close()
            raise
        if not result.startswith("OK"):
            raise RuntimeError(result or "Headless closed its output")
        return {"ok": True, "response": result}

    async def close(self):
        if self.proc.returncode is None:
            self.proc.terminate()
        try:
            await asyncio.wait_for(self.proc.wait(), 5)
        except asyncio.TimeoutError:
            self.proc.kill()
            await self.proc.wait()


@asynccontextmanager
async def lifespan(app):
    try:
        yield {}
    finally:
        for instance in instances.values():
            await instance.close()
        instances.clear()


mcp = FastMCP("cemu", lifespan=lifespan)


@mcp.tool()
async def control(
    action: Literal["attach", "launch", "screenshot", "key", "keys", "text", "send",
                    "reset", "reload_rom", "run_ms", "read_memory", "set_speed",
                    "throttle", "status", "list_keys", "detach", "save_state", "load_state"],
    id: str = "default", rom: str | None = None, program: str | None = None,
    name: str | None = None, asm: bool = False, key_name: str | None = None,
    key_action: Literal["tap", "down", "up"] = "tap", duration_ms: int = 80,
    text: str | None = None, files: list[str] | None = None,
    location: Literal["file", "ram", "arch"] = "file", ms: int | None = None,
    address: str | None = None, size: int | None = None, percent: int | None = None,
    enabled: bool | None = None, quit: bool = False, state: str | None = None,
    sequence: str | None = None,
):
    """Drive Qt-free cemu-headless. One tool preserves the Mac control actions.

    attach boots rom or state, or selects an owned instance by id. No external
    Qt socket attachment. Emulation advances only through run_ms and key actions.
    screenshot returns a PNG. key taps or holds/releases a physical key.
    keys uses comma-separated names, delay:<ms>, down:<key>, up:<key>.
    text types A-Z and digits with calculator alpha keys.
    send transfers files to file/ram/arch (Mac destination names).
    save_state writes state; load_state restarts from state; reload_rom fresh-boots
    the ROM. reset resets the core. detach keeps an owned instance unless quit=True.
    All owned processes are closed when this MCP server exits.
    launch/read_memory/set_speed/throttle are explicitly unsupported by the current
    headless protocol. No silent approximation of those operations.
    Local file access is trusted, just like the original desktop MCP server.
    """
    global active
    async with lock:
        if action in {"launch", "read_memory", "set_speed", "throttle"}:
            raise ValueError(f"{action} is not supported by the cemu-headless protocol")
        if action == "attach":
            clean(id)
            if id not in instances:
                instances[id] = await Headless.start(rom=rom, image=state)
            active = id
            return await instances[id].call("status")
        if action == "list_keys":
            return {"ok": True, "keys": KEYS}
        if action == "detach":
            if active is not None and quit:
                await instances.pop(active).close()
            active = None
            return {"ok": True}
        if active is None:
            raise RuntimeError("Not attached. Call attach with rom or state first.")
        conn = instances[active]
        if action == "status":
            return await conn.call("status")
        if action == "run_ms":
            return await conn.call(f"run {number(ms, 'ms')}")
        if action == "key":
            if key_name not in KEYS:
                raise ValueError("Unknown key; use list_keys")
            if key_action == "tap":
                return await conn.call(f"key {key_name} {number(duration_ms, 'duration_ms')}")
            return await conn.call(f"keys {key_action}:{key_name}")
        if action == "keys":
            if not sequence:
                raise ValueError("keys requires sequence")
            return await conn.call("keys " + clean(sequence))
        if action == "text":
            if text is None or any(c not in LETTERS and c not in "0123456789" for c in text.upper()):
                raise ValueError("text supports only A-Z and 0-9")
            steps = []
            for c in text.upper():
                steps.extend(["alpha", LETTERS[c]] if c in LETTERS else [c])
            return await conn.call("keys " + ",".join(steps)) if steps else {"ok": True}
        if action == "send":
            if not files:
                raise ValueError("send requires files")
            resolved = [path(f, exists=True) for f in files]
            results = []
            for f in resolved:
                results.append(await conn.call(f"send-file {dict(file='auto',ram='ram',arch='archive')[location]} {f}"))
            return {"ok": True, "results": results}
        if action == "save_state":
            return await conn.call("save-state " + path(state))
        if action in {"load_state", "reload_rom"}:
            new_rom = path(rom, exists=True) if rom else conn.rom
            if action == "reload_rom" and not new_rom:
                raise ValueError("No ROM remembered; pass rom")
            replacement = await Headless.start(rom=new_rom, image=path(state, exists=True) if action == "load_state" else None)
            await conn.close()
            instances[active] = replacement
            return await replacement.call("status")
        if action == "reset":
            return await conn.call("reset")
        if action == "screenshot":
            with tempfile.TemporaryDirectory(prefix="cemu-mcp-") as folder:
                bmp = str(Path(folder) / "lcd.bmp")
                await conn.call("screenshot " + bmp)
                with PILImage.open(bmp) as image:
                    output = io.BytesIO()
                    image.save(output, format="PNG")
                return Image(data=output.getvalue(), format="png")
        raise ValueError(f"Unknown action: {action}")


if __name__ == "__main__":
    mcp.run()
