# Qt-free CEmu MCP

The Python stdio MCP server owns a `cemu-headless` subprocess and talks the
[headless protocol](../tests/autotester/headless_protocol.md). Neither the build
nor runtime needs Qt, a display server, or a Qt socket.

## Build and run

From the repository root, with GCC/G++ and make installed:

```sh
git submodule update --init
make -C core -j4
make -C tests/autotester cemu-headless -j4
cd mcp
uv run server.py
```

Configure your MCP host to run that command in the mcp directory. Alternatively
install the dependencies from pyproject.toml into a Python environment and run
`python /absolute/path/to/mcp/server.py`. Set `CEMU_MCP_BINARY` to override the
default `tests/autotester/cemu-headless` executable.

The existing `control` tool and Mac argument names are retained where supported.
Start with `action="attach", rom="/path/to/your.rom"`, then `run_ms, ms=5000`.
Screenshots are returned as PNG images; headless BMP output is converted with
Pillow. Emulated time does not advance automatically between calls.

Supported actions: attach, screenshot, key (tap/down/up), keys, text (A-Z/0-9),
send, reset, reload_rom, run_ms, status, list_keys, detach, save_state, load_state.
For save/load pass `state="/path/to/file.ce"`. State loading starts a replacement
process before closing the previous one. Reset is the core reset, not necessarily
a complete RAM clear; reload_rom is the fresh-ROM boot operation.
After loading a state without an associated ROM, reload_rom requires a ROM path.

IDs select instances owned by this server, not arbitrary external processes.
Detach leaves the instance alive for reattachment unless quit=True. Server
shutdown terminates all its owned instances. Calls are serialized; timeout or
cancellation kills a process to avoid reusing a desynchronized protocol stream.

Launch, read_memory, set_speed and throttle remain recognized but return explicit
unsupported errors. The current headless protocol has no corresponding commands.
Program launch can be performed manually with physical keys after send.
Mac send destinations file/ram/arch map to auto/ram/archive.

This is a trusted local automation server. It can read calculator files and write
screenshots/states on the host. Do not expose it to untrusted clients. Protocol
arguments reject newline/NUL injection and no shell is used for spawning.

## Real integration test

Use your own locally dumped ROM. No ROM is included or uploaded.

```sh
CEMU_TEST_ROM=/path/to/rom \
CEMU_TEST_FILE=/path/to/test.8xv \
CEMU_TEST_ARTIFACTS=/tmp/cemu-mcp-results \
uv run test_e2e.py
```

Tests actual MCP initialization/tool listing, ROM attach, PNG screenshots, keys,
file transfer, save/reset/load with identical restored framebuffer, text,
arithmetic input, errors, detach/reattach and fresh ROM reload. DISPLAY and
Qt-related environment variables are removed for this test.
