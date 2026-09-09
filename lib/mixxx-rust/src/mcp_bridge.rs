//! cxx FFI bridge consumed by src/mcp/ on the Mixxx C++ side.
//!
//! Threading contract: `start_server` spins up a tokio runtime that owns
//! the loopback JSON-RPC listener. Requests arrive on its worker threads
//! and are handed to the C++ `RequestHandler`, which must marshal them to
//! Mixxx's thread and answer later with `respond`/`respond_error`. No
//! function here blocks on the network, and none of them call back into
//! C++ synchronously, so the Qt event loop is never held up. Everything
//! except `stop_server` is safe to call from any thread.

use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use cxx::UniquePtr;
use mixxx_mcp::server::{Backend, ServerConfig};

#[cxx::bridge(namespace = "mixxxmcp")]
mod ffi {
    unsafe extern "C++" {
        include!("mcp/mcprequesthandler.h");

        type RequestHandler;

        /// Invoked from a runtime worker thread with a JSON object of
        /// parameters. The implementation must return promptly and later
        /// answer exactly once with `respond` or `respond_error`.
        fn onRequest(self: Pin<&mut RequestHandler>, id: u64, method: String, params: String);
    }

    extern "Rust" {
        type Server;

        /// Start listening on 127.0.0.1.
        ///
        /// `port` 0 picks a free port. An empty `token` generates one.
        /// `endpoint_file`, when not empty, receives the descriptor the
        /// `mixxx-mcp` CLI uses to find this instance; it is removed
        /// again by `stop_server`.
        fn start_server(
            port: u16,
            token: &str,
            endpoint_file: &str,
            handler: UniquePtr<RequestHandler>,
        ) -> Result<Box<Server>>;

        /// Answer a request with a JSON result.
        fn respond(server: &Server, id: u64, result_json: &str);
        /// Answer a request with a JSON-RPC error.
        fn respond_error(server: &Server, id: u64, code: i32, message: &str);
        /// Notify clients waiting in `mixxx.wait_event`.
        fn publish_event(server: &Server, event_json: &str);

        /// The port actually bound.
        fn server_port(server: &Server) -> u16;
        /// The token clients must present.
        fn server_token(server: &Server) -> String;

        /// Stop listening and fail every in-flight request. Bounded (at
        /// most two seconds) and idempotent; no handler calls happen
        /// after it returns.
        fn stop_server(server: &mut Server);
    }
}

/// The C++ handler is only ever *called* from runtime worker threads, and
/// the C++ side promises `onRequest` is safe to call from any thread. The
/// mutex is what makes `&mut` access sound across those threads; it is
/// never held across anything that blocks, because `onRequest` only posts
/// to Mixxx's event loop.
struct SendHandler(UniquePtr<ffi::RequestHandler>);
unsafe impl Send for SendHandler {}

struct HandlerBackend {
    handler: Mutex<SendHandler>,
}

impl Backend for HandlerBackend {
    fn dispatch(&self, id: u64, method: &str, params: &str) {
        let mut handler = self
            .handler
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        handler
            .0
            .pin_mut()
            .onRequest(id, method.to_string(), params.to_string());
    }
}

pub struct Server(mixxx_mcp::server::Server);

fn start_server(
    port: u16,
    token: &str,
    endpoint_file: &str,
    handler: UniquePtr<ffi::RequestHandler>,
) -> Result<Box<Server>, std::io::Error> {
    if handler.is_null() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "a request handler is required",
        ));
    }
    let backend = Arc::new(HandlerBackend {
        handler: Mutex::new(SendHandler(handler)),
    });
    let config = ServerConfig {
        port,
        token: (!token.is_empty()).then(|| token.to_string()),
        endpoint_file: (!endpoint_file.is_empty()).then(|| PathBuf::from(endpoint_file)),
    };
    Ok(Box::new(Server(mixxx_mcp::server::Server::start(
        config, backend,
    )?)))
}

fn respond(server: &Server, id: u64, result_json: &str) {
    server.0.respond(id, result_json);
}

fn respond_error(server: &Server, id: u64, code: i32, message: &str) {
    server.0.respond_error(id, code, message);
}

fn publish_event(server: &Server, event_json: &str) {
    server.0.publish_event(event_json);
}

fn server_port(server: &Server) -> u16 {
    server.0.port()
}

fn server_token(server: &Server) -> String {
    server.0.token().to_string()
}

fn stop_server(server: &mut Server) {
    server.0.stop();
}
