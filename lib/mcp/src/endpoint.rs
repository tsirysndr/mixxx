//! Discovery of the running Mixxx instance.
//!
//! Mixxx writes a small descriptor into its settings directory when the
//! MCP server starts and removes it on shutdown. The CLI reads it so an
//! agent only has to run `mixxx-mcp` with no arguments.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::protocol::ENDPOINT_FILE_NAME;

/// Contents of `<settings dir>/mcp.json`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Endpoint {
    /// Loopback port the JSON-RPC/WebSocket server listens on.
    pub port: u16,
    /// Bearer token required on the WebSocket handshake.
    pub token: String,
    /// PID of the Mixxx process, for diagnostics.
    pub pid: u32,
    /// [`crate::protocol::RPC_VERSION`] of the running server.
    pub rpc_version: u32,
}

impl Endpoint {
    pub fn url(&self) -> String {
        format!("ws://127.0.0.1:{}", self.port)
    }

    pub fn write_to(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let json = serde_json::to_string_pretty(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
        std::fs::write(path, json)?;
        restrict_permissions(path);
        Ok(())
    }

    pub fn read_from(path: &Path) -> std::io::Result<Self> {
        let raw = std::fs::read_to_string(path)?;
        serde_json::from_str(&raw)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))
    }
}

/// The descriptor carries a credential, so keep it owner-readable.
#[cfg(unix)]
fn restrict_permissions(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
}

#[cfg(not(unix))]
fn restrict_permissions(_path: &Path) {}

/// Locations to probe, most specific first. Mirrors the settings paths
/// picked by `CmdlineArgs` on each platform (including the macOS App
/// Sandbox container used by App Store builds).
pub fn default_endpoint_paths() -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Ok(explicit) = std::env::var("MIXXX_MCP_ENDPOINT") {
        if !explicit.is_empty() {
            paths.push(PathBuf::from(explicit));
        }
    }
    if let Some(home) = home_dir() {
        if cfg!(target_os = "macos") {
            paths.push(
                home.join(
                    "Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx",
                )
                .join(ENDPOINT_FILE_NAME),
            );
            paths.push(
                home.join("Library/Application Support/Mixxx")
                    .join(ENDPOINT_FILE_NAME),
            );
        }
        paths.push(home.join(".mixxx").join(ENDPOINT_FILE_NAME));
    }
    if cfg!(windows) {
        if let Ok(local) = std::env::var("LOCALAPPDATA") {
            paths.push(PathBuf::from(local).join("Mixxx").join(ENDPOINT_FILE_NAME));
        }
    }
    paths
}

/// First existing, parsable descriptor from [`default_endpoint_paths`].
pub fn discover() -> Option<(PathBuf, Endpoint)> {
    for path in default_endpoint_paths() {
        if let Ok(endpoint) = Endpoint::read_from(&path) {
            return Some((path, endpoint));
        }
    }
    None
}

fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
}
