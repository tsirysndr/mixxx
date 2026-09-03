//! Subsonic/Navidrome protocol client for Mixxx.
//!
//! Pure-Rust implementation of the parts of the Subsonic REST API
//! (v1.16.1, JSON) that Mixxx needs to browse a remote music library and
//! download tracks into a local cache for deck playback.
//!
//! The cxx bridge consumed by the C++ side (`src/library/subsonic/` in
//! the Mixxx tree) lives in the lib/mixxx-rust umbrella crate.

pub mod client;
pub mod download;
pub mod error;
pub mod library;
pub mod model;

pub use client::{Config, SubsonicClient};
pub use download::{cache_file_name, download_cover_art, download_track};
pub use error::Error;
pub use library::fetch_all_tracks;
