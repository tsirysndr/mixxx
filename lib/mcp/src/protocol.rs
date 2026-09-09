//! The JSON-RPC surface shared by the in-Mixxx server and the CLI.
//!
//! Everything in [`BACKEND_METHODS`] is forwarded verbatim to the Mixxx
//! main thread, which owns all the state. The handful of methods in
//! [`LOCAL_METHODS`] are implemented inside the server itself because they
//! are compositions over time (waiting, ramping) that would otherwise
//! block Mixxx's event loop.

/// Bumped when the wire format changes incompatibly. Reported by
/// `mixxx.server_info` so a mismatched CLI can complain usefully.
pub const RPC_VERSION: u32 = 1;

/// Name of the endpoint descriptor Mixxx writes into its settings
/// directory so the CLI can find the running instance.
pub const ENDPOINT_FILE_NAME: &str = "mcp.json";

/// Methods dispatched to Mixxx. Registering them explicitly (rather than
/// accepting anything) means a typo from an agent comes back as a proper
/// "method not found" instead of a 15 second timeout.
pub const BACKEND_METHODS: &[&str] = &[
    // --- transport & deck state -------------------------------------
    "mixxx.get_state",
    "mixxx.get_deck",
    "mixxx.play",
    "mixxx.cue",
    "mixxx.seek",
    "mixxx.beatjump",
    "mixxx.load_track",
    "mixxx.eject",
    "mixxx.clone_deck",
    // --- mixer ------------------------------------------------------
    "mixxx.set_volume",
    "mixxx.set_gain",
    "mixxx.set_crossfader",
    "mixxx.set_eq",
    "mixxx.set_rate",
    "mixxx.sync",
    "mixxx.set_loop",
    "mixxx.hotcue",
    "mixxx.headphone",
    // --- library ----------------------------------------------------
    "mixxx.search_library",
    "mixxx.get_track",
    "mixxx.suggest_next",
    "mixxx.list_playlists",
    "mixxx.get_playlist",
    "mixxx.list_crates",
    "mixxx.get_crate",
    // --- auto dj ----------------------------------------------------
    "mixxx.autodj",
    "mixxx.autodj_queue",
    "mixxx.autodj_add",
    "mixxx.autodj_edit",
    // --- raw control surface ----------------------------------------
    "mixxx.get_control",
    "mixxx.set_control",
];

/// Methods answered by the server process itself.
pub const LOCAL_METHODS: &[&str] = &[
    "mixxx.server_info",
    "mixxx.wait_until",
    "mixxx.wait_event",
    "mixxx.crossfade",
];

/// True if `method` is part of the surface at all.
pub fn is_known_method(method: &str) -> bool {
    BACKEND_METHODS.contains(&method) || LOCAL_METHODS.contains(&method)
}

/// JSON-RPC error codes used across the boundary. The application range
/// starts below -32000 to stay clear of the reserved codes.
pub mod error_code {
    /// Mixxx rejected the request (bad deck number, unknown track, ...).
    pub const INVALID_REQUEST: i32 = -32602;
    /// Mixxx did not answer in time.
    pub const TIMEOUT: i32 = -32001;
    /// The server is shutting down (Mixxx is quitting).
    pub const SHUTTING_DOWN: i32 = -32002;
    /// Generic failure inside Mixxx.
    pub const INTERNAL: i32 = -32603;
}
