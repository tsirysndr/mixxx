//! The MCP tool catalogue.
//!
//! Every tool is a thin renaming of one JSON-RPC method: `tools/call`
//! forwards the arguments object unchanged. Descriptions are written for
//! an agent that is actually DJing, so they say what the control *does to
//! the mix*, not just which value it writes.

use serde_json::{json, Value};

pub struct Tool {
    /// MCP tool name, as the agent sees it.
    pub name: &'static str,
    pub description: &'static str,
    /// JSON-RPC method the arguments are forwarded to.
    pub method: &'static str,
    pub schema: Value,
}

impl Tool {
    pub fn to_json(&self) -> Value {
        json!({
            "name": self.name,
            "description": self.description,
            "inputSchema": self.schema,
        })
    }
}

/// Deck numbers are 1-based everywhere, matching Mixxx's `[ChannelN]`.
fn deck() -> Value {
    json!({"type": "integer", "minimum": 1, "description": "Deck number, 1-based ([Channel1] is deck 1)."})
}

fn num(description: &str) -> Value {
    json!({"type": "number", "description": description})
}

fn int(description: &str) -> Value {
    json!({"type": "integer", "description": description})
}

fn boolean(description: &str) -> Value {
    json!({"type": "boolean", "description": description})
}

fn text(description: &str) -> Value {
    json!({"type": "string", "description": description})
}

fn one_of(description: &str, values: &[&str]) -> Value {
    json!({"type": "string", "enum": values, "description": description})
}

fn object(properties: Value, required: &[&str]) -> Value {
    json!({
        "type": "object",
        "properties": properties,
        "required": required,
    })
}

fn no_args() -> Value {
    json!({"type": "object", "properties": {}})
}

pub fn catalog() -> Vec<Tool> {
    vec![
        // ---- state -----------------------------------------------------
        Tool {
            name: "mixxx_get_state",
            description: "Snapshot of the whole mixer: every deck (loaded track, play state, \
                          position, remaining time, BPM, key, volume, EQ), the crossfader, \
                          master/headphone levels and the Auto DJ status. Start here before \
                          any mixing decision.",
            method: "mixxx.get_state",
            schema: no_args(),
        },
        Tool {
            name: "mixxx_get_deck",
            description: "Detailed state of one deck, including position_seconds, \
                          remaining_seconds, intro/outro marker positions and the effective \
                          (rate-adjusted) BPM.",
            method: "mixxx.get_deck",
            schema: object(json!({"deck": deck()}), &["deck"]),
        },
        Tool {
            name: "mixxx_server_info",
            description: "Version of the Mixxx MCP bridge and the list of RPC methods it exposes.",
            method: "mixxx.server_info",
            schema: no_args(),
        },
        // ---- transport -------------------------------------------------
        Tool {
            name: "mixxx_play",
            description: "Start or stop playback on a deck.",
            method: "mixxx.play",
            schema: object(
                json!({
                    "deck": deck(),
                    "play": boolean("true to play, false to pause. Omit to toggle."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_cue",
            description: "Cue button actions: 'set' places the cue point at the current \
                          position, 'goto' jumps to it and stops, 'play' jumps to it and plays.",
            method: "mixxx.cue",
            schema: object(
                json!({
                    "deck": deck(),
                    "action": one_of("What to do with the cue point.", &["set", "goto", "play"]),
                }),
                &["deck", "action"],
            ),
        },
        Tool {
            name: "mixxx_seek",
            description: "Jump to an absolute position in the loaded track.",
            method: "mixxx.seek",
            schema: object(
                json!({
                    "deck": deck(),
                    "position_seconds": num("Absolute position in seconds."),
                    "fraction": num("Alternative to position_seconds: 0.0 = start, 1.0 = end."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_beatjump",
            description: "Jump forward (positive) or backward (negative) by a number of beats, \
                          staying in phase. Use this to line a track up on the bar.",
            method: "mixxx.beatjump",
            schema: object(
                json!({"deck": deck(), "beats": num("Beats to jump; may be fractional.")}),
                &["deck", "beats"],
            ),
        },
        Tool {
            name: "mixxx_load_track",
            description: "Load a library track into a deck. Identify the track by track_id from \
                          a search result, or by absolute file location. Refuses to clobber a \
                          deck that is currently playing unless force is true.",
            method: "mixxx.load_track",
            schema: object(
                json!({
                    "deck": deck(),
                    "track_id": int("Library track id, as returned by mixxx_search_library."),
                    "location": text("Absolute path of an audio file, as an alternative to track_id."),
                    "play": boolean("Start playing immediately after loading. Default false."),
                    "force": boolean("Allow replacing a track on a playing deck. Default false."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_eject",
            description: "Unload the track from a deck.",
            method: "mixxx.eject",
            schema: object(json!({"deck": deck()}), &["deck"]),
        },
        Tool {
            name: "mixxx_clone_deck",
            description: "Copy the track and playback position of one deck onto another \
                          (instant doubles).",
            method: "mixxx.clone_deck",
            schema: object(
                json!({"from_deck": deck(), "to_deck": deck()}),
                &["from_deck", "to_deck"],
            ),
        },
        // ---- mixer -----------------------------------------------------
        Tool {
            name: "mixxx_set_volume",
            description: "Set a deck's channel fader, 0.0 (silent) to 1.0 (full).",
            method: "mixxx.set_volume",
            schema: object(
                json!({"deck": deck(), "value": num("0.0 to 1.0.")}),
                &["deck", "value"],
            ),
        },
        Tool {
            name: "mixxx_set_gain",
            description: "Set a deck's pregain (trim) used for level matching. 1.0 is unity; \
                          the usable range is roughly 0.0 to 4.0.",
            method: "mixxx.set_gain",
            schema: object(
                json!({"deck": deck(), "value": num("Linear gain, 1.0 = unity.")}),
                &["deck", "value"],
            ),
        },
        Tool {
            name: "mixxx_set_crossfader",
            description: "Move the crossfader: -1.0 is hard left, 0.0 centre, 1.0 hard right. \
                          For a timed fade use mixxx_crossfade instead.",
            method: "mixxx.set_crossfader",
            schema: object(json!({"value": num("-1.0 to 1.0.")}), &["value"]),
        },
        Tool {
            name: "mixxx_set_eq",
            description: "Set the three-band EQ and/or the quick-effect filter knob of a deck. \
                          Each band is 0.0 (killed) to 4.0 (boosted), 1.0 is flat. The filter \
                          is 0.0 to 1.0 with 0.5 neutral. Only the values you pass are changed.",
            method: "mixxx.set_eq",
            schema: object(
                json!({
                    "deck": deck(),
                    "low": num("Low band, 0.0-4.0, 1.0 = flat."),
                    "mid": num("Mid band, 0.0-4.0, 1.0 = flat."),
                    "high": num("High band, 0.0-4.0, 1.0 = flat."),
                    "filter": num("Quick-effect (filter) knob, 0.0-1.0, 0.5 = neutral."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_set_rate",
            description: "Change a deck's tempo, either by target BPM or by a rate ratio \
                          (1.0 = original speed). Use this to beatmatch manually.",
            method: "mixxx.set_rate",
            schema: object(
                json!({
                    "deck": deck(),
                    "bpm": num("Desired playback BPM."),
                    "ratio": num("Alternative to bpm: playback rate ratio, 1.0 = original."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_sync",
            description: "Beat-sync a deck. Enable follower sync to lock its tempo and phase to \
                          the sync leader, or make it the leader.",
            method: "mixxx.sync",
            schema: object(
                json!({
                    "deck": deck(),
                    "enabled": boolean("Enable or disable sync on this deck. Default true."),
                    "leader": boolean("Make this deck the sync leader instead of a follower."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_set_loop",
            description: "Set and enable a beat loop of the given length at the current \
                          position, or disable the active loop.",
            method: "mixxx.set_loop",
            schema: object(
                json!({
                    "deck": deck(),
                    "beats": num("Loop length in beats (0.125 to 64). Required when enabling."),
                    "enabled": boolean("false disables the active loop. Default true."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_hotcue",
            description: "Set, jump to or clear one of a deck's hotcues.",
            method: "mixxx.hotcue",
            schema: object(
                json!({
                    "deck": deck(),
                    "number": int("Hotcue number, 1-based."),
                    "action": one_of(
                        "'set' stores the current position, 'goto' jumps there, 'play' jumps \
                         there and plays, 'clear' deletes it.",
                        &["set", "goto", "play", "clear"],
                    ),
                }),
                &["deck", "number", "action"],
            ),
        },
        Tool {
            name: "mixxx_headphone",
            description: "Cue a deck to the headphones and adjust the headphone mix/gain — how \
                          you preview the next track without the audience hearing it.",
            method: "mixxx.headphone",
            schema: object(
                json!({
                    "deck": deck(),
                    "enabled": boolean("Route this deck to the headphones."),
                    "mix": num("Headphone mix, -1.0 = cue only, 1.0 = master only."),
                    "gain": num("Headphone gain, 1.0 = unity."),
                }),
                &[],
            ),
        },
        // ---- library ---------------------------------------------------
        Tool {
            name: "mixxx_search_library",
            description: "Search the Mixxx library. Free text matches artist, title, album, \
                          genre and comment; the numeric filters narrow by tempo, key, rating \
                          and year. Returns track ids you can load or queue.",
            method: "mixxx.search_library",
            schema: object(
                json!({
                    "query": text("Free text; matches artist, title, album, genre and comment."),
                    "bpm_min": num("Lowest acceptable BPM."),
                    "bpm_max": num("Highest acceptable BPM."),
                    "key": text("Key filter in Camelot (e.g. 8A), Open Key or traditional (Am) notation."),
                    "genre": text("Genre substring."),
                    "min_rating": int("Minimum star rating, 0-5."),
                    "year_min": int("Earliest release year."),
                    "year_max": int("Latest release year."),
                    "limit": int("Maximum results, default 25, max 200."),
                    "offset": int("Result offset for paging."),
                    "sort": one_of(
                        "Result ordering. Default 'relevance'.",
                        &["relevance", "artist", "title", "bpm", "year", "rating", "played", "recent", "random"],
                    ),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_get_track",
            description: "Full metadata for one library track: BPM, key (Camelot and \
                          traditional), duration, rating, play count, comment, file location.",
            method: "mixxx.get_track",
            schema: object(json!({"track_id": int("Library track id.")}), &["track_id"]),
        },
        Tool {
            name: "mixxx_suggest_next",
            description: "Find mixable candidates for the next track: tracks within a tempo \
                          window and, by default, in a harmonically compatible key \
                          (same key, relative major/minor, or a fifth away). Seed it with a \
                          deck number, a track id, or an explicit bpm/key pair.",
            method: "mixxx.suggest_next",
            schema: object(
                json!({
                    "deck": int("Seed from the track loaded on this deck."),
                    "track_id": int("Seed from this library track instead."),
                    "bpm": num("Seed BPM, if not seeding from a track."),
                    "key": text("Seed key in Camelot, Open Key or traditional notation."),
                    "bpm_tolerance": num("Allowed tempo drift in percent. Default 6."),
                    "harmonic_only": boolean("Restrict to harmonically compatible keys. Default true."),
                    "genre": text("Restrict to a genre substring."),
                    "exclude_played": boolean("Skip tracks already played this session. Default true."),
                    "limit": int("Maximum results, default 20."),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_list_playlists",
            description: "List the library's playlists with their track counts.",
            method: "mixxx.list_playlists",
            schema: no_args(),
        },
        Tool {
            name: "mixxx_get_playlist",
            description: "List the tracks of a playlist, in order.",
            method: "mixxx.get_playlist",
            schema: object(
                json!({
                    "playlist_id": int("Playlist id from mixxx_list_playlists."),
                    "name": text("Playlist name, as an alternative to playlist_id."),
                    "limit": int("Maximum tracks, default 100."),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_list_crates",
            description: "List the library's crates with their track counts.",
            method: "mixxx.list_crates",
            schema: no_args(),
        },
        Tool {
            name: "mixxx_get_crate",
            description: "List the tracks in a crate.",
            method: "mixxx.get_crate",
            schema: object(
                json!({
                    "crate_id": int("Crate id from mixxx_list_crates."),
                    "name": text("Crate name, as an alternative to crate_id."),
                    "limit": int("Maximum tracks, default 100."),
                }),
                &[],
            ),
        },
        // ---- subsonic / navidrome browser ------------------------------
        Tool {
            name: "mixxx_subsonic_status",
            description: "Whether this Mixxx has a Subsonic/Navidrome library attached, which \
                          server it points at, how much of it has been imported and whether a \
                          refresh is running. Never fails — check it before the other \
                          mixxx_subsonic_* tools.",
            method: "mixxx.subsonic_status",
            schema: no_args(),
        },
        Tool {
            name: "mixxx_subsonic_browse",
            description: "Walk the Subsonic library the way the sidebar does: no arguments \
                          lists artists, an artist lists their albums, an album lists its \
                          tracks in order. 'level' overrides that inference and also reaches \
                          the genre list. Track rows carry 'cached', i.e. whether loading \
                          them is instant or has to wait for a download.",
            method: "mixxx.subsonic_browse",
            schema: object(
                json!({
                    "level": one_of(
                        "What to list. Inferred from the other arguments when omitted.",
                        &["genres", "artists", "albums", "tracks"],
                    ),
                    "artist": text("Artist (or album artist) to drill into; substring match."),
                    "album": text("Album to drill into; substring match."),
                    "genre": text("Restrict to a genre substring."),
                    "year_min": int("Earliest release year."),
                    "year_max": int("Latest release year."),
                    "limit": int("Maximum rows, default 100, max 500."),
                    "offset": int("Row offset for paging."),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_subsonic_search",
            description: "Search the imported Subsonic library. Free text matches artist, \
                          title, album and genre. Returns subsonic_id values for \
                          mixxx_subsonic_load and mixxx_subsonic_autodj_add — these are not \
                          Mixxx track ids and do not work with mixxx_load_track.",
            method: "mixxx.subsonic_search",
            schema: object(
                json!({
                    "query": text("Free text; matches artist, title, album and genre."),
                    "artist": text("Artist (or album artist) substring."),
                    "album": text("Album substring."),
                    "genre": text("Genre substring."),
                    "year_min": int("Earliest release year."),
                    "year_max": int("Latest release year."),
                    "limit": int("Maximum results, default 25, max 200."),
                    "offset": int("Result offset for paging."),
                    "sort": one_of(
                        "Result ordering. Default 'relevance' (artist, then album order).",
                        &["relevance", "title", "album", "year", "duration", "random"],
                    ),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_subsonic_playlists",
            description: "List the playlists that came from the Subsonic server, with their \
                          track counts.",
            method: "mixxx.subsonic_playlists",
            schema: no_args(),
        },
        Tool {
            name: "mixxx_subsonic_playlist",
            description: "List the tracks of a Subsonic playlist, in playlist order.",
            method: "mixxx.subsonic_playlist",
            schema: object(
                json!({
                    "playlist_id": int("Playlist id from mixxx_subsonic_playlists."),
                    "name": text("Playlist name, as an alternative to playlist_id."),
                    "limit": int("Maximum tracks, default 100, max 500."),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_subsonic_load",
            description: "Load a Subsonic track into a deck, downloading it first if it is not \
                          cached yet. Returns immediately: when 'cached' is false the deck is \
                          filled a moment later, once the download lands, so confirm with \
                          mixxx_get_deck before starting it. Refuses to clobber a playing deck \
                          unless force is true.",
            method: "mixxx.subsonic_load",
            schema: object(
                json!({
                    "deck": deck(),
                    "subsonic_id": text("Server-side track id from a browse/search result."),
                    "location": text("The track's subsonic:// location, as an alternative."),
                    "force": boolean("Allow replacing a track on a playing deck. Default false."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_subsonic_autodj_add",
            description: "Queue Subsonic tracks for Auto DJ. The downloads stream in the \
                          background and each track is appended as soon as it is ready, so the \
                          call returns before the queue is complete — the normal way to plan a \
                          set from a remote library.",
            method: "mixxx.subsonic_autodj_add",
            schema: object(
                json!({
                    "subsonic_ids": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Server-side track ids, in the order they should play.",
                    },
                    "locations": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "subsonic:// locations, as an alternative to subsonic_ids.",
                    },
                    "position": one_of(
                        "Where to insert. Default 'bottom'.",
                        &["top", "bottom", "replace"],
                    ),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_subsonic_refresh",
            description: "Re-import the Subsonic library from the server in the background. \
                          Returns at once; poll mixxx_subsonic_status until 'importing' is \
                          false. Only needed when the server's content changed during the \
                          session.",
            method: "mixxx.subsonic_refresh",
            schema: no_args(),
        },
        // ---- auto dj ---------------------------------------------------
        Tool {
            name: "mixxx_autodj",
            description:
                "Drive Auto DJ: check its status, enable/disable it, force the current \
                          transition ('fade_now'), skip the next track, shuffle or clear the queue.",
            method: "mixxx.autodj",
            schema: object(
                json!({
                    "action": one_of(
                        "Auto DJ action.",
                        &["status", "enable", "disable", "fade_now", "skip_next", "shuffle", "clear", "add_random"],
                    ),
                }),
                &["action"],
            ),
        },
        Tool {
            name: "mixxx_autodj_queue",
            description: "List the upcoming Auto DJ queue in play order.",
            method: "mixxx.autodj_queue",
            schema: object(json!({"limit": int("Maximum entries, default 50.")}), &[]),
        },
        Tool {
            name: "mixxx_autodj_add",
            description: "Add tracks to the Auto DJ queue — the normal way to plan a set ahead \
                          of time.",
            method: "mixxx.autodj_add",
            schema: object(
                json!({
                    "track_ids": {
                        "type": "array",
                        "items": {"type": "integer"},
                        "description": "Library track ids, in the order they should play.",
                    },
                    "position": one_of(
                        "Where to insert. Default 'bottom'.",
                        &["top", "bottom", "replace"],
                    ),
                }),
                &["track_ids"],
            ),
        },
        Tool {
            name: "mixxx_autodj_edit",
            description: "Reorder or remove an entry of the Auto DJ queue. Positions are \
                          1-based, matching mixxx_autodj_queue.",
            method: "mixxx.autodj_edit",
            schema: object(
                json!({
                    "action": one_of("Edit to perform.", &["remove", "move"]),
                    "position": int("1-based queue position to act on."),
                    "to": int("Destination position for 'move'."),
                }),
                &["action", "position"],
            ),
        },
        // ---- raw control surface ---------------------------------------
        Tool {
            name: "mixxx_get_control",
            description: "Read any Mixxx control object by group and key (e.g. group \
                          '[Channel1]', key 'rate'). The escape hatch for anything the typed \
                          tools do not cover, including effects and samplers.",
            method: "mixxx.get_control",
            schema: object(
                json!({
                    "group": text("Control group, e.g. '[Channel1]' or '[Master]'."),
                    "key": text("Control key, e.g. 'play' or 'crossfader'."),
                }),
                &["group", "key"],
            ),
        },
        Tool {
            name: "mixxx_set_control",
            description: "Write any Mixxx control object. Same addressing as mixxx_get_control. \
                          Values are the raw control values Mixxx uses.",
            method: "mixxx.set_control",
            schema: object(
                json!({
                    "group": text("Control group, e.g. '[Channel1]'."),
                    "key": text("Control key, e.g. 'rate'."),
                    "value": num("New value."),
                }),
                &["group", "key", "value"],
            ),
        },
        // ---- timed helpers ----------------------------------------------
        Tool {
            name: "mixxx_wait_until",
            description: "Block until a deck reaches a condition — typically 'the outro is \
                          near', i.e. remaining_seconds drops below a threshold. Returns as \
                          soon as it is satisfied, or when the timeout expires. Use this \
                          instead of polling in a loop.",
            method: "mixxx.wait_until",
            schema: object(
                json!({
                    "deck": deck(),
                    "remaining_seconds": num("Wake up when the deck has this much time left."),
                    "position_seconds": num("Wake up when playback passes this position."),
                    "playing": boolean("Wake up when the deck reaches this play state."),
                    "timeout_ms": int("Give up after this long. Default 300000 (5 min)."),
                    "poll_ms": int("Polling interval, 50-5000. Default 250."),
                }),
                &["deck"],
            ),
        },
        Tool {
            name: "mixxx_wait_event",
            description: "Wait for Mixxx to report state changes (track loaded, play/pause, \
                          Auto DJ queue edited). Pass back the returned 'seq' as 'since' on \
                          the next call so no event is missed.",
            method: "mixxx.wait_event",
            schema: object(
                json!({
                    "since": int("Sequence number returned by the previous call."),
                    "timeout_ms": int("Give up after this long. Default 30000."),
                }),
                &[],
            ),
        },
        Tool {
            name: "mixxx_crossfade",
            description: "Perform a timed transition. In 'crossfader' mode the crossfader is \
                          swept to the incoming deck's side; in 'volume' mode the outgoing \
                          deck's fader is taken down while the incoming one comes up. The call \
                          returns when the fade is finished, so pick the duration deliberately.",
            method: "mixxx.crossfade",
            schema: object(
                json!({
                    "from_deck": int("Deck being faded out."),
                    "to_deck": int("Deck being faded in."),
                    "duration_seconds": num("Fade length, default 8, max 600."),
                    "mode": one_of("Fade the crossfader or the channel faders.", &["crossfader", "volume"]),
                    "to": num("Crossfader target, -1.0 to 1.0. Defaults to the incoming deck's side."),
                    "to_volume": num("Volume mode: fader level the incoming deck ends at. Default 1.0."),
                    "curve": one_of("'smooth' (raised cosine) or 'linear'. Default 'smooth'.", &["smooth", "linear"]),
                    "start_playing": boolean("Start the incoming deck before fading. Default true."),
                    "stop_after": boolean("Stop the outgoing deck when the fade ends. Default false."),
                }),
                &[],
            ),
        },
    ]
}

/// The tool with this MCP name, if any.
pub fn find(name: &str) -> Option<Tool> {
    catalog().into_iter().find(|tool| tool.name == name)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol;

    #[test]
    fn every_tool_maps_to_a_known_method() {
        for tool in catalog() {
            assert!(
                protocol::is_known_method(tool.method),
                "{} points at unknown method {}",
                tool.name,
                tool.method
            );
        }
    }

    #[test]
    fn tool_names_are_unique_and_well_formed() {
        let mut names = std::collections::HashSet::new();
        for tool in catalog() {
            assert!(names.insert(tool.name), "duplicate tool {}", tool.name);
            assert!(tool.name.starts_with("mixxx_"), "{}", tool.name);
            assert!(!tool.description.is_empty());
            assert_eq!(tool.schema["type"], json!("object"));
        }
    }

    #[test]
    fn every_backend_method_is_reachable_from_some_tool() {
        let methods: std::collections::HashSet<_> =
            catalog().into_iter().map(|tool| tool.method).collect();
        for method in protocol::BACKEND_METHODS {
            assert!(methods.contains(method), "{method} has no tool");
        }
    }
}
