//! Rocksky (https://rocksky.app) integration for Mixxx: autoscrobbling
//! played tracks and acting as a remote-controllable player, built on the
//! official rocksky-sdk. The cxx bridge consumed by src/rocksky/ on the
//! Mixxx C++ side lives in the lib/mixxx-rust umbrella crate.

pub mod token;

// Re-exported for the bridge in lib/mixxx-rust.
pub use rocksky_sdk;
pub use tokio;
