"""Opt-in ROM-backed MCP stdio integration test. ROM never leaves this machine."""
import asyncio
import base64
import os
import sys
import tempfile
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main():
    rom = os.environ["CEMU_TEST_ROM"]
    transfer = os.environ["CEMU_TEST_FILE"]
    env = dict(os.environ)
    for key in ("DISPLAY", "QT_QPA_PLATFORM", "LD_LIBRARY_PATH"):
        env.pop(key, None)
    params = StdioServerParameters(
        command=sys.executable, args=[str(Path(__file__).with_name("server.py"))], env=env
    )
    with tempfile.TemporaryDirectory(prefix="cemu-mcp-test-") as folder:
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                assert [t.name for t in (await session.list_tools()).tools] == ["control"]

                async def call(**args):
                    result = await session.call_tool("control", args)
                    assert not result.isError, result
                    print("PASS", args["action"])
                    return result

                async def screen(filename):
                    result = await call(action="screenshot")
                    image = next(c for c in result.content if c.type == "image")
                    data = base64.b64decode(image.data)
                    assert data.startswith(b"\x89PNG\r\n\x1a\n")
                    if os.environ.get("CEMU_TEST_ARTIFACTS"):
                        out = Path(os.environ["CEMU_TEST_ARTIFACTS"])
                        out.mkdir(parents=True, exist_ok=True)
                        (out / filename).write_bytes(data)
                    return data

                try:
                    await call(action="attach", id="e2e", rom=rom)
                    await call(action="run_ms", ms=5000)
                    await call(action="key", key_name="clear")
                    await screen("ti-os.png")
                    await call(action="list_keys")
                    await call(action="send", files=[transfer])
                    await call(action="key", key_name="2")
                    state = str(Path(folder) / "roundtrip.ce")
                    await call(action="save_state", state=state)
                    assert Path(state).stat().st_size > 0
                    before = await screen("before.png")
                    await call(action="reset")
                    await call(action="load_state", state=state)
                    assert before == await screen("restored.png")
                    await call(action="key", key_name="2nd", key_action="down")
                    await call(action="key", key_name="2nd", key_action="up")
                    await call(action="key", key_name="clear")
                    await call(action="text", text="ABC123")
                    await call(action="keys", sequence="clear,2,add,3,enter")
                    await call(action="run_ms", ms=500)
                    await screen("arithmetic.png")
                    for args in [
                        dict(action="keys", sequence="clear\nquit"),
                        dict(action="key", key_name="bogus"),
                        dict(action="run_ms", ms=-1),
                        dict(action="launch", name="A"),
                        dict(action="send", files=["/no/such/file"]),
                        dict(action="text", text="!"),
                    ]:
                        result = await session.call_tool("control", args)
                        assert result.isError, args
                    await call(action="status")
                    await call(action="detach")
                    await call(action="attach", id="e2e")
                    await call(action="reload_rom")
                    await call(action="run_ms", ms=5000)
                    await call(action="key", key_name="clear")
                    await screen("reloaded.png")
                finally:
                    await call(action="detach", quit=True)


if __name__ == "__main__":
    asyncio.run(main())
