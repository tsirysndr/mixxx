# mixxx-mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server that lets
an AI agent — Claude, Codex, Copilot, or anything else that speaks MCP — run
Mixxx: load decks, beatmatch, EQ, crossfade, search the library and drive
Auto DJ.

> This is part of the Mixxx fork at <https://github.com/tsirysndr/mixxx>.
> Upstream Mixxx has no MCP server; the agent-facing tools only exist in a
> build of that fork configured with `-DMCP=ON`.

## How the pieces fit

```
agent (Claude, …)  ──MCP over stdio──▶  mixxx-mcp  ──JSON-RPC/WebSocket──▶  Mixxx
                                          CLI            127.0.0.1              McpService
```

* **In Mixxx** (`src/mcp/`, cargo feature `server`): a `jsonrpsee` WebSocket
  server bound to loopback. Requests are marshalled onto Mixxx's main thread,
  where they act on control objects, the player manager, the track collection
  and the Auto DJ playlist.
* **The `mixxx-mcp` binary** (cargo feature `cli`): what an agent host spawns.
  It speaks MCP on stdio and forwards each tool call to that endpoint.

Both halves live in this one crate, so the tools an agent sees and the methods
Mixxx implements cannot drift apart — a test enforces the mapping.

Mixxx writes `mcp.json` into its settings directory (next to `mixxx.cfg`) with
the port and a bearer token; the CLI finds it with no configuration. The
listener is loopback-only *and* token-authenticated, because any web page can
open a WebSocket to localhost.

## Build

The server is built with Mixxx whenever a Rust toolchain is present
(`-DMCP=ON`, on by default; `-DMCP=OFF` to leave it out). The same build
produces the CLI at `build/mcp-cli/release/mixxx-mcp` and installs it
alongside the Mixxx binary.

To build just the CLI:

```sh
cargo build --release --manifest-path lib/mcp/Cargo.toml --bin mixxx-mcp
```

## Check it works

With Mixxx running:

```sh
mixxx-mcp --status          # endpoint, PID, and a full state snapshot
mixxx-mcp --list-tools      # the tools an agent will see
mixxx-mcp --call mixxx.search_library '{"query":"daft punk","limit":5}'
```

## Register it with an agent

Claude Code:

```sh
claude mcp add mixxx -- /path/to/mixxx-mcp
```

Or in `.mcp.json` / `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "mixxx": {
      "command": "/path/to/mixxx-mcp"
    }
  }
}
```

No arguments or environment are needed as long as Mixxx is running on the same
machine. `MIXXX_MCP_ENDPOINT` overrides the descriptor location if you run
Mixxx with a non-standard `--settings-path`.

## What it looks like in use

Once the server is registered, you talk to your agent in plain language and it
drives the decks. Real examples, with what the agent does behind them:

> **"What's playing right now?"**
>
> One `mixxx_get_state`.
>
> *Deck 1: Floating Points – Last Bloom, 4:12 elapsed of 7:31 (3:19 left),
> 124.0 BPM, 4A. Deck 2 is empty. Crossfader hard left, Auto DJ off.*

> **"Find me something that mixes out of this and get it ready on deck 2."**
>
> `mixxx_suggest_next {deck: 1}` → `mixxx_load_track {deck: 2, track_id: …}`
> → `mixxx_sync {deck: 2}` → `mixxx_headphone {deck: 2, enabled: true}`.
>
> *Cued Jayda G – Both of Us on deck 2: 123 BPM (needs +0.8 %), 4A — same key.
> Synced and in your headphones. Say when.*

> **"Take it away when the outro hits, nice long blend."**
>
> `mixxx_wait_until {deck: 1, remaining_seconds: 50}` — the agent sleeps, no
> polling — then `mixxx_crossfade {from_deck: 1, to_deck: 2,
> duration_seconds: 24}`, dropping deck 1's lows through the middle of the
> fade with `mixxx_set_eq`, and finally `mixxx_play {deck: 1, play: false}`.
>
> *Mixed at 6:41, 24-second blend. Deck 2 is out front; deck 1 stopped and
> ready to reload.*

> **"Build me an hour of deep house that starts mellow and climbs."**
>
> Several `mixxx_search_library` calls narrowing by genre and BPM band, then
> one `mixxx_autodj_add` with the ordered ids and `mixxx_autodj {action:
> "enable"}`.
>
> *Queued 14 tracks, 118 → 126 BPM, keys walking 5A → 6A → 7A. Auto DJ is on;
> "skip this one" will fade to the next.*

> **"Too much bass on the incoming track."**
>
> `mixxx_set_eq {deck: 2, low: 0.4}`.

> **"Loop the break on deck 1 for a bit."**
>
> `mixxx_set_loop {deck: 1, beats: 8}`, and `{enabled: false}` to let it run on.

> **"Open the mic for me."**
>
> No typed tool covers microphones, so the agent falls through to
> `mixxx_set_control {group: "[Microphone]", key: "talkover", value: 1}`.

The last one is the point of the escape hatch: anything Mixxx exposes as a
control object is reachable, so the agent is not boxed in by the tool list.

## The AI DJ skill

`skills/mixxx-ai-dj/SKILL.md` teaches an agent how to actually DJ with these
tools — harmonic and tempo matching, transition lengths, EQ during a blend,
when to use Auto DJ instead. Install it for Claude Code with:

```sh
ln -s "$PWD/lib/mcp/skills/mixxx-ai-dj" ~/.claude/skills/mixxx-ai-dj
```

## Tools

| Tool | Purpose |
|---|---|
| `mixxx_get_state`, `mixxx_get_deck` | Full mixer snapshot / one deck in detail |
| `mixxx_play`, `mixxx_cue`, `mixxx_seek`, `mixxx_beatjump` | Transport |
| `mixxx_load_track`, `mixxx_eject`, `mixxx_clone_deck` | Deck contents |
| `mixxx_set_volume`, `mixxx_set_gain`, `mixxx_set_crossfader`, `mixxx_set_eq` | Levels and tone |
| `mixxx_set_rate`, `mixxx_sync` | Tempo matching |
| `mixxx_set_loop`, `mixxx_hotcue`, `mixxx_headphone` | Loops, cues, monitoring |
| `mixxx_search_library`, `mixxx_get_track`, `mixxx_suggest_next` | Library, incl. harmonic/tempo matching |
| `mixxx_list_playlists`, `mixxx_get_playlist`, `mixxx_list_crates`, `mixxx_get_crate` | Collections |
| `mixxx_autodj`, `mixxx_autodj_queue`, `mixxx_autodj_add`, `mixxx_autodj_edit` | Auto DJ |
| `mixxx_get_control`, `mixxx_set_control` | Any Mixxx control object |
| `mixxx_wait_until`, `mixxx_wait_event`, `mixxx_crossfade` | Timing and timed fades |

`mixxx_wait_until`, `mixxx_wait_event` and `mixxx_crossfade` are executed by
the server process, not by Mixxx, so waiting and fading never hold up the
audio thread or the UI.

## Configuration

In `mixxx.cfg`:

```ini
[Mcp]
Enabled = 1     # 0 disables the server entirely
Port = 0        # 0 = OS-assigned; pin it if you prefer a fixed port
Token = ""      # empty = generated per run and written to mcp.json
```

## Layout

| File | Contents |
|---|---|
| `src/protocol.rs` | The method catalogue shared by both halves |
| `src/server.rs` | Embedded JSON-RPC server, `Backend` trait, timed helpers |
| `src/tools.rs` | MCP tool definitions and their JSON schemas |
| `src/client.rs` | WebSocket client used by the CLI |
| `src/mcp.rs` | MCP stdio protocol loop |
| `src/endpoint.rs` | The `mcp.json` descriptor and its discovery |
| `../mixxx-rust/src/mcp_bridge.rs` | cxx bridge into Mixxx |
| `../../src/mcp/` | The C++ service that executes requests |
