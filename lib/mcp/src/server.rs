//! The JSON-RPC server embedded in Mixxx.
//!
//! Threading contract, which the C++ side relies on:
//!
//! * [`Server::start`] owns a small tokio runtime. Requests arrive on its
//!   worker threads and are handed to the [`Backend`] with
//!   [`Backend::dispatch`], which **must not block** — it is expected to
//!   post the request to Mixxx's main thread and return immediately.
//! * Mixxx answers later, from whichever thread it likes, by calling
//!   [`Server::respond`] / [`Server::respond_error`] with the same id.
//!   Both are non-blocking.
//! * Nothing here ever blocks Mixxx's event loop, so there is no way for
//!   the two sides to deadlock; a request Mixxx never answers is failed
//!   locally after [`REQUEST_TIMEOUT`].

use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use jsonrpsee_server::types::ErrorObjectOwned;
use jsonrpsee_server::{RpcModule, ServerBuilder, ServerHandle};
use serde_json::{json, Value};
use tokio::runtime::Runtime;
use tokio::sync::{oneshot, Notify};

use crate::endpoint::Endpoint;
use crate::protocol::{self, error_code};

/// How long Mixxx has to answer a forwarded request before the caller
/// gets a timeout error. Generous, because a request may be queued behind
/// a track load or a library scan on the main thread.
pub const REQUEST_TIMEOUT: Duration = Duration::from_secs(15);

/// Events kept for clients that poll `mixxx.wait_event` intermittently.
const EVENT_BACKLOG: usize = 256;

/// Receives requests that have to be executed by Mixxx itself.
///
/// Implemented on the C++ side of the cxx bridge; see
/// `lib/mixxx-rust/src/mcp_bridge.rs`.
pub trait Backend: Send + Sync + 'static {
    /// Deliver `params` (a JSON object, serialized) for `method`.
    ///
    /// Called from a runtime worker thread and must return promptly.
    /// The implementation eventually calls [`Server::respond`] or
    /// [`Server::respond_error`] with the same `id`, exactly once.
    fn dispatch(&self, id: u64, method: &str, params: &str);
}

/// How the server should listen and where to advertise itself.
pub struct ServerConfig {
    /// Loopback port; 0 lets the OS pick a free one.
    pub port: u16,
    /// Bearer token clients must present. Generated when `None`.
    pub token: Option<String>,
    /// Where to write the discovery descriptor, if anywhere.
    pub endpoint_file: Option<PathBuf>,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            port: 0,
            token: None,
            endpoint_file: None,
        }
    }
}

struct Shared {
    backend: Arc<dyn Backend>,
    pending: Mutex<HashMap<u64, oneshot::Sender<Result<Value, ErrorObjectOwned>>>>,
    next_id: AtomicU64,
    shutting_down: AtomicBool,
    events: EventLog,
    port: AtomicU64,
}

impl Shared {
    /// Round-trip one request through Mixxx.
    async fn call(&self, method: &str, params: Value) -> Result<Value, ErrorObjectOwned> {
        if self.shutting_down.load(Ordering::Acquire) {
            return Err(err(error_code::SHUTTING_DOWN, "Mixxx is shutting down"));
        }
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = oneshot::channel();
        self.pending.lock().unwrap().insert(id, tx);
        self.backend.dispatch(id, method, &params.to_string());

        match tokio::time::timeout(REQUEST_TIMEOUT, rx).await {
            Ok(Ok(result)) => result,
            // The sender was dropped: shutdown, or a handler that never answered.
            Ok(Err(_)) => Err(err(error_code::SHUTTING_DOWN, "Mixxx is shutting down")),
            Err(_) => {
                self.pending.lock().unwrap().remove(&id);
                Err(err(
                    error_code::TIMEOUT,
                    format!("Mixxx did not answer {method} within {REQUEST_TIMEOUT:?}"),
                ))
            }
        }
    }

    fn complete(&self, id: u64, result: Result<Value, ErrorObjectOwned>) {
        if let Some(tx) = self.pending.lock().unwrap().remove(&id) {
            // A dropped receiver just means the caller already timed out.
            let _ = tx.send(result);
        }
    }
}

/// Sequenced ring buffer of state-change notifications from Mixxx.
struct EventLog {
    entries: Mutex<VecDeque<(u64, Value)>>,
    next_seq: AtomicU64,
    notify: Notify,
}

impl EventLog {
    fn new() -> Self {
        Self {
            entries: Mutex::new(VecDeque::new()),
            next_seq: AtomicU64::new(1),
            notify: Notify::new(),
        }
    }

    fn push(&self, event: Value) {
        let seq = self.next_seq.fetch_add(1, Ordering::Relaxed);
        {
            let mut entries = self.entries.lock().unwrap();
            entries.push_back((seq, event));
            while entries.len() > EVENT_BACKLOG {
                entries.pop_front();
            }
        }
        self.notify.notify_waiters();
    }

    fn latest_seq(&self) -> u64 {
        self.next_seq.load(Ordering::Relaxed).saturating_sub(1)
    }

    fn since(&self, seq: u64) -> Vec<Value> {
        self.entries
            .lock()
            .unwrap()
            .iter()
            .filter(|(s, _)| *s > seq)
            .map(|(s, e)| {
                let mut e = e.clone();
                if let Some(obj) = e.as_object_mut() {
                    obj.insert("seq".into(), json!(s));
                }
                e
            })
            .collect()
    }
}

/// A running server. Dropping it stops listening.
pub struct Server {
    runtime: Runtime,
    handle: Option<ServerHandle>,
    shared: Arc<Shared>,
    endpoint_file: Option<PathBuf>,
    port: u16,
    token: String,
}

impl Server {
    /// Bind, start listening and (if configured) publish the descriptor.
    pub fn start(config: ServerConfig, backend: Arc<dyn Backend>) -> std::io::Result<Self> {
        let token = match config.token {
            Some(token) if !token.is_empty() => token,
            _ => random_token(),
        };

        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("mixxx-mcp")
            .enable_all()
            .build()?;

        let shared = Arc::new(Shared {
            backend,
            pending: Mutex::new(HashMap::new()),
            next_id: AtomicU64::new(1),
            shutting_down: AtomicBool::new(false),
            events: EventLog::new(),
            port: AtomicU64::new(0),
        });

        let module = build_module(shared.clone(), &token)
            .map_err(|e| std::io::Error::other(e.to_string()))?;

        let auth = tower_http::validate_request::ValidateRequestHeaderLayer::custom(BearerAuth {
            expected: format!("Bearer {token}"),
        });
        let addr = format!("127.0.0.1:{}", config.port);

        let (handle, port) = runtime.block_on(async move {
            let server = ServerBuilder::new()
                .set_http_middleware(tower::ServiceBuilder::new().layer(auth))
                .build(addr.as_str())
                .await?;
            let port = server.local_addr()?.port();
            Ok::<_, std::io::Error>((server.start(module), port))
        })?;
        shared.port.store(port as u64, Ordering::Relaxed);

        if let Some(path) = config.endpoint_file.as_ref() {
            let endpoint = Endpoint {
                port,
                token: token.clone(),
                pid: std::process::id(),
                rpc_version: protocol::RPC_VERSION,
            };
            // Not fatal: the CLI can still be pointed at the port by hand.
            if let Err(e) = endpoint.write_to(path) {
                eprintln!("mixxx-mcp: cannot write {}: {e}", path.display());
            }
        }

        Ok(Self {
            runtime,
            handle: Some(handle),
            shared,
            endpoint_file: config.endpoint_file,
            port,
            token,
        })
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn token(&self) -> &str {
        &self.token
    }

    /// Answer a dispatched request with a JSON result.
    ///
    /// `result_json` must be valid JSON; anything else is reported to the
    /// caller as an internal error rather than silently dropped.
    pub fn respond(&self, id: u64, result_json: &str) {
        let result = match serde_json::from_str::<Value>(result_json) {
            Ok(value) => Ok(value),
            Err(e) => Err(err(
                error_code::INTERNAL,
                format!("Mixxx produced invalid JSON: {e}"),
            )),
        };
        self.shared.complete(id, result);
    }

    /// Answer a dispatched request with an error.
    pub fn respond_error(&self, id: u64, code: i32, message: &str) {
        self.shared.complete(id, Err(err(code, message)));
    }

    /// Broadcast a state change to clients polling `mixxx.wait_event`.
    pub fn publish_event(&self, event_json: &str) {
        match serde_json::from_str::<Value>(event_json) {
            Ok(event) => self.shared.events.push(event),
            Err(e) => eprintln!("mixxx-mcp: dropping malformed event: {e}"),
        }
    }

    /// Stop listening and fail every in-flight request. Idempotent.
    pub fn stop(&mut self) {
        if self.shared.shutting_down.swap(true, Ordering::AcqRel) {
            return;
        }
        // Release in-flight callers before waiting on the server: their
        // handlers must finish for `stopped()` to resolve.
        self.shared.pending.lock().unwrap().clear();
        if let Some(handle) = self.handle.take() {
            let _ = handle.stop();
            // Bounded, so a wedged connection cannot delay Mixxx's exit.
            let _ = self.runtime.block_on(tokio::time::timeout(
                Duration::from_secs(2),
                handle.stopped(),
            ));
        }
        if let Some(path) = self.endpoint_file.take() {
            let _ = std::fs::remove_file(path);
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
    }
}

fn build_module(
    shared: Arc<Shared>,
    token: &str,
) -> Result<RpcModule<Arc<Shared>>, jsonrpsee_server::RegisterMethodError> {
    let mut module = RpcModule::new(shared);

    for &method in protocol::BACKEND_METHODS {
        module.register_async_method(method, move |params, ctx, _| async move {
            ctx.call(method, params_object(&params)).await
        })?;
    }

    let token = token.to_string();
    module.register_async_method("mixxx.server_info", move |_, ctx, _| {
        let token = token.clone();
        async move {
            Ok::<_, ErrorObjectOwned>(json!({
                "server": "mixxx-mcp",
                "rpc_version": protocol::RPC_VERSION,
                "port": ctx.port.load(Ordering::Relaxed),
                "token": token,
                "methods": protocol::BACKEND_METHODS
                    .iter()
                    .chain(protocol::LOCAL_METHODS.iter())
                    .collect::<Vec<_>>(),
            }))
        }
    })?;

    module.register_async_method("mixxx.wait_event", |params, ctx, _| async move {
        wait_event(&ctx, params_object(&params)).await
    })?;

    module.register_async_method("mixxx.wait_until", |params, ctx, _| async move {
        wait_until(&ctx, params_object(&params)).await
    })?;

    module.register_async_method("mixxx.crossfade", |params, ctx, _| async move {
        crossfade(&ctx, params_object(&params)).await
    })?;

    Ok(module)
}

// --- local method implementations ---------------------------------------

async fn wait_event(shared: &Shared, params: Value) -> Result<Value, ErrorObjectOwned> {
    let timeout = Duration::from_millis(number(&params, "timeout_ms").unwrap_or(30_000.0) as u64);
    // Without an explicit cursor, start from "now" and wait for the next
    // event rather than replaying the backlog.
    let since = number(&params, "since").map(|v| v as u64);
    let mut cursor = since.unwrap_or_else(|| shared.events.latest_seq());
    let deadline = Instant::now() + timeout;

    loop {
        let events = shared.events.since(cursor);
        if !events.is_empty() {
            cursor = shared.events.latest_seq();
            return Ok(json!({"seq": cursor, "events": events, "timed_out": false}));
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Ok(json!({"seq": cursor, "events": [], "timed_out": true}));
        }
        // notify_waiters() only wakes waiters registered *before* the
        // event, so re-check the log after the sleep in either case.
        let _ = tokio::time::timeout(remaining, shared.events.notify.notified()).await;
    }
}

async fn wait_until(shared: &Shared, params: Value) -> Result<Value, ErrorObjectOwned> {
    let deck = number(&params, "deck").ok_or_else(|| invalid("deck is required"))? as i64;
    let timeout = Duration::from_millis(number(&params, "timeout_ms").unwrap_or(300_000.0) as u64);
    let poll = Duration::from_millis(
        number(&params, "poll_ms")
            .unwrap_or(250.0)
            .clamp(50.0, 5_000.0) as u64,
    );
    let remaining_below = number(&params, "remaining_seconds");
    let position_above = number(&params, "position_seconds");
    let want_playing = params.get("playing").and_then(Value::as_bool);
    if remaining_below.is_none() && position_above.is_none() && want_playing.is_none() {
        return Err(invalid(
            "one of remaining_seconds, position_seconds or playing is required",
        ));
    }

    let started = Instant::now();
    loop {
        let state = shared
            .call("mixxx.get_deck", json!({ "deck": deck }))
            .await?;
        let satisfied = remaining_below
            .zip(number(&state, "remaining_seconds"))
            .map(|(want, have)| have <= want)
            .unwrap_or(false)
            || position_above
                .zip(number(&state, "position_seconds"))
                .map(|(want, have)| have >= want)
                .unwrap_or(false)
            || want_playing
                .zip(state.get("playing").and_then(Value::as_bool))
                .map(|(want, have)| want == have)
                .unwrap_or(false);
        let waited_ms = started.elapsed().as_millis() as u64;
        if satisfied {
            return Ok(json!({"satisfied": true, "waited_ms": waited_ms, "deck": state}));
        }
        if started.elapsed() >= timeout {
            return Ok(json!({"satisfied": false, "waited_ms": waited_ms, "deck": state}));
        }
        tokio::time::sleep(poll).await;
    }
}

/// A timed fade, run here rather than in Mixxx so the main thread stays
/// free while it plays out.
async fn crossfade(shared: &Shared, params: Value) -> Result<Value, ErrorObjectOwned> {
    let duration = number(&params, "duration_seconds")
        .unwrap_or(8.0)
        .clamp(0.0, 600.0);
    let smooth = params
        .get("curve")
        .and_then(Value::as_str)
        .map(|c| c.eq_ignore_ascii_case("smooth"))
        .unwrap_or(true);
    let from_deck = number(&params, "from_deck").map(|v| v as i64);
    let to_deck = number(&params, "to_deck").map(|v| v as i64);
    let volume_mode = params
        .get("mode")
        .and_then(Value::as_str)
        .map(|m| m.eq_ignore_ascii_case("volume"))
        .unwrap_or(false);

    if volume_mode && (from_deck.is_none() || to_deck.is_none()) {
        return Err(invalid("volume mode requires from_deck and to_deck"));
    }

    if params
        .get("start_playing")
        .and_then(Value::as_bool)
        .unwrap_or(true)
    {
        if let Some(deck) = to_deck {
            shared
                .call("mixxx.play", json!({"deck": deck, "play": true}))
                .await?;
        }
    }

    // 50 ms per step is smooth to the ear and cheap: a 30 s fade is 600
    // control writes.
    let steps = ((duration / 0.05).round() as u32).clamp(1, 600);
    let step_delay = Duration::from_secs_f64(duration as f64 / steps as f64);

    let (starts, targets): (Vec<(String, String)>, Vec<f64>) = if volume_mode {
        let from = channel_key(from_deck.unwrap());
        let to = channel_key(to_deck.unwrap());
        let to_volume = number(&params, "to_volume").unwrap_or(1.0).clamp(0.0, 1.0);
        (
            vec![(from, "volume".into()), (to, "volume".into())],
            vec![0.0, to_volume],
        )
    } else {
        let target = number(&params, "to").unwrap_or_else(|| {
            // Default to the side the incoming deck is on: odd decks are
            // left, even decks right in Mixxx's default orientation.
            match to_deck {
                Some(d) if d % 2 == 0 => 1.0,
                _ => -1.0,
            }
        });
        (
            vec![("[Master]".into(), "crossfader".into())],
            vec![target.clamp(-1.0, 1.0)],
        )
    };

    let mut from_values = Vec::with_capacity(starts.len());
    for (group, key) in &starts {
        let current = shared
            .call("mixxx.get_control", json!({"group": group, "key": key}))
            .await?;
        from_values.push(number(&current, "value").unwrap_or(0.0));
    }

    for step in 1..=steps {
        let linear = step as f64 / steps as f64;
        let t = if smooth {
            // Raised cosine: no audible jump at either end of the fade.
            0.5 - 0.5 * (linear * std::f64::consts::PI).cos()
        } else {
            linear
        };
        for (i, (group, key)) in starts.iter().enumerate() {
            let value = from_values[i] + (targets[i] - from_values[i]) * t;
            shared
                .call(
                    "mixxx.set_control",
                    json!({"group": group, "key": key, "value": value}),
                )
                .await?;
        }
        if step < steps {
            tokio::time::sleep(step_delay).await;
        }
    }

    if params
        .get("stop_after")
        .and_then(Value::as_bool)
        .unwrap_or(false)
    {
        if let Some(deck) = from_deck {
            shared
                .call("mixxx.play", json!({"deck": deck, "play": false}))
                .await?;
        }
    }

    Ok(json!({
        "ok": true,
        "duration_seconds": duration,
        "steps": steps,
        "mode": if volume_mode { "volume" } else { "crossfader" },
    }))
}

// --- transport auth ------------------------------------------------------

/// Rejects WebSocket handshakes that do not carry the endpoint token.
///
/// The listener is loopback-only, but any page in a browser can open a
/// WebSocket to localhost, so the token is what actually keeps a random
/// website from taking over the decks.
#[derive(Clone)]
struct BearerAuth {
    expected: String,
}

impl<B> tower_http::validate_request::ValidateRequest<B> for BearerAuth {
    type ResponseBody = jsonrpsee_server::HttpBody;

    fn validate(
        &mut self,
        request: &mut jsonrpsee_server::HttpRequest<B>,
    ) -> Result<(), jsonrpsee_server::HttpResponse<Self::ResponseBody>> {
        let presented = request
            .headers()
            .get(http::header::AUTHORIZATION)
            .and_then(|value| value.to_str().ok())
            .unwrap_or_default();
        if constant_time_eq(presented.as_bytes(), self.expected.as_bytes()) {
            return Ok(());
        }
        Err(jsonrpsee_server::HttpResponse::builder()
            .status(http::StatusCode::UNAUTHORIZED)
            .body(jsonrpsee_server::HttpBody::empty())
            .expect("static response is always valid"))
    }
}

/// Comparison whose timing does not depend on the position of the first
/// differing byte (it does reveal the length, which is not secret).
fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

// --- helpers -------------------------------------------------------------

fn channel_key(deck: i64) -> String {
    format!("[Channel{deck}]")
}

/// Params as a JSON object; positional or absent params become `{}` so
/// every handler can use the same lookup helpers.
fn params_object(params: &jsonrpsee_server::types::Params<'_>) -> Value {
    match params.as_str().map(serde_json::from_str::<Value>) {
        Some(Ok(value @ Value::Object(_))) => value,
        _ => json!({}),
    }
}

/// Numbers arrive as JSON numbers or, from sloppier clients, as strings.
fn number(value: &Value, key: &str) -> Option<f64> {
    match value.get(key)? {
        Value::Number(n) => n.as_f64(),
        Value::String(s) => s.parse().ok(),
        Value::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
        _ => None,
    }
}

fn err(code: i32, message: impl Into<String>) -> ErrorObjectOwned {
    ErrorObjectOwned::owned(code, message.into(), None::<()>)
}

fn invalid(message: &str) -> ErrorObjectOwned {
    err(error_code::INVALID_REQUEST, message)
}

fn random_token() -> String {
    let mut bytes = [0u8; 24];
    if getrandom::fill(&mut bytes).is_err() {
        // Only reachable if the OS entropy source is unavailable; a
        // predictable token is still better than no server at all, and
        // the listener is loopback-only.
        for (i, byte) in bytes.iter_mut().enumerate() {
            *byte = (i as u8).wrapping_mul(31).wrapping_add(7);
        }
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens_are_unique_and_long_enough() {
        let a = random_token();
        let b = random_token();
        assert_eq!(a.len(), 48);
        assert_ne!(a, b);
    }

    #[test]
    fn number_accepts_json_and_string_forms() {
        let v = json!({"a": 2.5, "b": "3.5", "c": true, "d": null});
        assert_eq!(number(&v, "a"), Some(2.5));
        assert_eq!(number(&v, "b"), Some(3.5));
        assert_eq!(number(&v, "c"), Some(1.0));
        assert_eq!(number(&v, "d"), None);
        assert_eq!(number(&v, "missing"), None);
    }

    #[test]
    fn event_log_is_sequenced_and_bounded() {
        let log = EventLog::new();
        for i in 0..(EVENT_BACKLOG + 10) {
            log.push(json!({"type": "tick", "i": i}));
        }
        assert_eq!(log.latest_seq(), (EVENT_BACKLOG + 10) as u64);
        assert_eq!(log.since(0).len(), EVENT_BACKLOG);
        let tail = log.since(log.latest_seq() - 3);
        assert_eq!(tail.len(), 3);
        assert_eq!(tail[0]["seq"], json!(log.latest_seq() - 2));
    }
}
