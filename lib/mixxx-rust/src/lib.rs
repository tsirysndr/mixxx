//! Umbrella staticlib for Mixxx's Rust integrations. Each cxx bridge is
//! gated by a cargo feature mirroring its CMake option; the protocol
//! logic lives in the per-integration library crates (subsonic-rs,
//! rocksky-rs), which stay bridge-free so their `cargo test` links
//! without any C++ side.

#[cfg(feature = "rocksky")]
mod rocksky_bridge;

#[cfg(feature = "subsonic")]
mod subsonic_bridge;
