---
name: mixxx-ai-dj
description: DJ a live set with Mixxx through the mixxx MCP server — pick tracks that mix, beatmatch them, and run transitions on time, from the local library or a Subsonic/Navidrome server. Use whenever the user asks to DJ, play music, build or continue a set, take requests, control the decks, browse their Subsonic library, or manage the Auto DJ queue in Mixxx.
---

# DJing with Mixxx

> **Requires the Mixxx fork with MCP support:**
> <https://github.com/tsirysndr/mixxx>. Upstream Mixxx does not ship the
> `mixxx` MCP server, and none of these tools exist without it. If the tools
> are missing or `mixxx-mcp` reports that no endpoint was found, check that
> the running Mixxx is a build from that fork (built with `-DMCP=ON`) rather
> than a release from mixxx.org.

You are running a real mixer that someone is listening to. Two rules matter
more than anything else here:

1. **Never interrupt what is playing.** Do not load over, stop, eject or
   sharply refade a deck the audience is currently hearing. `mixxx_load_track`
   refuses to clobber a playing deck; if you find yourself reaching for
   `force: true`, you are about to make a mistake.
2. **Prepare early, act late.** Choose and load the next track while there is
   still a minute of the current one left. Then wait — don't fill the time
   with control changes nobody asked for.

## The loop

```
mixxx_get_state              → who is playing, how long is left, what is idle
mixxx_suggest_next           → candidates that match in tempo and key
mixxx_load_track (idle deck) → cue it up
mixxx_sync / mixxx_set_rate  → beatmatch
mixxx_wait_until             → sleep until the outro
mixxx_crossfade              → run the transition
```

Then repeat from the deck that is now playing. One pass per track; do not
re-plan the whole set every time.

### 1. Read the room

`mixxx_get_state` gives you every deck: `playing`, `remaining_seconds`,
`bpm`, `key_camelot`, the loaded `track`, and `outro_start_seconds` when the
track has been analysed. The idle deck is the one with `playing: false`.

If nothing is playing at all, load a track, set the crossfader to that deck's
side, and start it. If the user gave no direction, ask what they want before
opening a set — genre, energy, how long — but only once.

### 2. Choose the next track

`mixxx_suggest_next` with `deck` set to the playing deck does the work:
it returns library tracks inside a tempo window and, unless you pass
`harmonic_only: false`, in a compatible key. Each candidate carries
`tempo_change_percent`, i.e. how far its tempo has to bend to match.

Read the candidates and *pick*, don't just take the first row. Weigh:

- **Tempo.** Under ±3 % is inaudible; ±6 % is the practical ceiling for most
  material. Beyond that, tracks start sounding wrong even when they are in time.
- **Key.** Camelot numbers make this mechanical: from `8A` the safe moves are
  `8A` (same), `8B` (relative major), `7A` and `9A` (neighbouring fifths).
  A `+1` number step lifts energy slightly; `A`→`B` brightens the mood.
  Tracks with a null key were never analysed — usable, but listen first.
- **Energy and repetition.** Build over a set rather than sawing up and down,
  and don't replay an artist you played twenty minutes ago.
- **Not already played.** `exclude_played` defaults to true and covers the
  current session.

If nothing fits, widen it in this order: `bpm_tolerance` to 8, then
`harmonic_only: false`, then drop to `mixxx_search_library` with a genre or
free-text query. Say what you loosened and why.

### 3. Load and beatmatch

Load onto the idle deck, then match tempo. Two ways:

- `mixxx_sync` with `enabled: true` — Mixxx locks tempo and phase to the sync
  leader. Fast, reliable, and what you should reach for by default.
- `mixxx_set_rate` with a target `bpm` — manual matching, when you want the
  incoming track to keep its own feel or the two tracks disagree about where
  the beat is.

Cue it to your headphones with `mixxx_headphone` while you check it. Match
levels with `mixxx_set_gain` before the fade, not during it: a track that
jumps 6 dB louder mid-transition is the most audible mistake you can make.

### 4. Wait

```
mixxx_wait_until { deck: <playing>, remaining_seconds: 45 }
```

This blocks until the outro is close, then returns the deck state. Use the
track's `outro_start_seconds` when it has one — start the fade there — and
otherwise 30-60 seconds of remaining time for a long blend, 10-15 for a
quick cut. Never poll `mixxx_get_state` in a loop to do this.

### 5. Transition

`mixxx_crossfade` runs the fade for you and returns when it is done:

```
mixxx_crossfade {
  from_deck: 1, to_deck: 2,
  duration_seconds: 16,
  start_playing: true
}
```

Pick the length from the material:

| Situation | Length | Notes |
|---|---|---|
| Long intros/outros, steady genres (house, techno) | 16-32 s | Blend over a phrase or two |
| Vocal tracks, most pop | 6-12 s | Shorter — two vocals over each other is a mess |
| Energy change, or tracks that fight | 2-4 s | Effectively a cut, on the downbeat |

While a long blend runs, EQ is what keeps it clean: drop the outgoing track's
lows (`mixxx_set_eq` with `low: 0`) as the incoming one comes up, so two
kick drums never stack. Bring the outgoing highs down last.

After the fade, stop the deck you left behind (`mixxx_play` with
`play: false`) and reset its EQ to flat for next time.

## The Subsonic/Navidrome library

If the user's music lives on a Subsonic or Navidrome server, Mixxx browses it
as a second, separate collection. **Its tracks are not library tracks**: they
have server-side string ids (`subsonic_id`) instead of numeric `track_id`s,
and they are not on disk until they have been downloaded. So
`mixxx_search_library`, `mixxx_load_track`, `mixxx_suggest_next` and
`mixxx_autodj_add` never see them — use the `mixxx_subsonic_*` tools instead.

Start with `mixxx_subsonic_status`. It never fails, and tells you whether
there is a Subsonic library at all (`available`, `configured`), how much was
imported (`track_count`) and whether a refresh is running (`importing`). If
`available` is false, the local library is all you have; say so rather than
retrying.

Then navigate the same way the sidebar does:

```
mixxx_subsonic_browse {}                             → artists
mixxx_subsonic_browse {artist: "Burial"}             → that artist's albums
mixxx_subsonic_browse {artist: "Burial", album: "Untrue"}  → its tracks
mixxx_subsonic_browse {level: "genres"}              → genres, busiest first
mixxx_subsonic_search  {query: "dub techno", limit: 20}
mixxx_subsonic_playlists / mixxx_subsonic_playlist {name: "…"}
```

Browsing by artist and album is the right way to explore an unfamiliar
remote library; search is for when the user names something specific.

### Loading from the server costs time

Every row carries `cached`. A cached track loads instantly; an uncached one
has to be downloaded first:

```
mixxx_subsonic_load {deck: 2, subsonic_id: "…"}
→ {"cached": false, "status": "downloading"}
```

The call returns immediately and the deck fills when the download lands.
**Do not start the deck or fade into it until `mixxx_get_deck` shows the
track loaded** — check, and give it a few seconds if needed. This is why you
prepare early: budget a download on top of the usual lead time, and prefer
cueing up the next track a minute out rather than thirty seconds out.

Load one Subsonic track at a time. Only the most recently requested download
is loaded into a deck, so a second `mixxx_subsonic_load` issued while the
first is still downloading leaves that first deck empty (the file is still
fetched, so a repeat call lands instantly).

Subsonic tracks have no analysed BPM or key until Mixxx has loaded them, so
`mixxx_suggest_next` cannot plan with them. Pick by genre, album and your own
knowledge of the material, then beatmatch with `mixxx_sync` once the track is
on the deck and analysed.

For a hands-off set, `mixxx_subsonic_autodj_add` queues them directly —
downloads stream in the background and each track is appended in order as it
becomes ready, so the queue fills in over a few seconds:

```
mixxx_subsonic_autodj_add {subsonic_ids: ["…", "…", "…"], position: "bottom"}
```

`mixxx_subsonic_refresh` re-imports from the server; only reach for it if the
library looks stale or empty while `configured` is true. It runs in the
background — poll `mixxx_subsonic_status` until `importing` is false instead
of waiting blindly.

## Auto DJ

For a hands-off set, queue tracks and let Mixxx handle the transitions:
`mixxx_autodj_add` with the track ids, then `mixxx_autodj` with
`action: "enable"`. `mixxx_autodj_queue` shows what is coming and
`mixxx_autodj_edit` reorders or drops entries. `action: "fade_now"` moves to
the next track immediately — the right response to "skip this one".

Use Auto DJ when the user wants a playlist to run itself, and manual decks
when they want you actually mixing. Queueing ten well-chosen tracks is often
a better answer than mixing three.

## When something is not covered

`mixxx_get_control` and `mixxx_set_control` reach every control in Mixxx.
Deck controls live in `[Channel1]`, `[Channel2]`, …:

| Control | What it does |
|---|---|
| `rate` | Raw pitch slider, -1..1 |
| `quantize` | Snap cues and loops to the beat grid |
| `keylock` | Hold musical key while the tempo changes |
| `reverse`, `slip_enabled` | Reverse play, slip mode |
| `beatloop_2_activate` (4, 8, 16…) | Loop of that many beats |
| `filterHigh`, `filterLow` | Deprecated aliases; prefer `mixxx_set_eq` |

Master controls live in `[Master]` (`crossfader`, `gain`, `headMix`), Auto DJ
in `[AutoDJ]`, samplers in `[Sampler1]`… Effects use
`[EffectRack1_EffectUnit1]` and `[QuickEffectRack1_[Channel1]]`.

Turn keylock on for anything you pitch more than about 3 %, especially with
vocals.

## Reporting back

Say what you played and why it followed, in one line — "Next up: Burial –
Archangel, 8A into 8A at 138" — not a paragraph of tool narration. If you
had to compromise (out of key, tempo stretched, nothing left in the crate),
say so plainly.
