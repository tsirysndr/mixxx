//! Model Context Protocol bridge for Mixxx.
//!
//! The crate has two halves that share one definition of the control
//! surface, so the tools an agent sees can never drift from what Mixxx
//! implements:
//!
//! * [`server`] (feature `server`) is linked into Mixxx and answers
//!   JSON-RPC on a loopback WebSocket, delegating to the Qt main thread
//!   through the [`server::Backend`] trait.
//! * [`client`] and [`mcp`] (feature `cli`) build the `mixxx-mcp`
//!   executable an agent spawns; it speaks MCP on stdio and forwards
//!   every tool call to that endpoint.
//!
//! [`protocol`] lists the methods, [`tools`] describes them to the model
//! and [`endpoint`] is how the two halves find each other.

pub mod endpoint;
pub mod protocol;
pub mod tools;

#[cfg(feature = "server")]
pub mod server;

#[cfg(feature = "server")]
pub use server::{Backend, Server, ServerConfig};

#[cfg(feature = "cli")]
pub mod client;
#[cfg(feature = "cli")]
pub mod mcp;
