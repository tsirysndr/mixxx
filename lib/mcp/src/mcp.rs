//! The Model Context Protocol server that agents spawn.
//!
//! MCP is JSON-RPC 2.0 over newline-delimited stdio. This module speaks
//! the small subset a tool server needs (initialize, tools/list,
//! tools/call, ping) and forwards every tool call to the running Mixxx.
//!
//! Requests are handled concurrently: a `mixxx_crossfade` that runs for
//! thirty seconds must not stall the agent's other calls.

use std::sync::Arc;

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::sync::mpsc;

use crate::client::{Client, Error as ClientError};
use crate::tools;

/// Protocol revisions we can speak. The client's choice is echoed back
/// when we recognise it, otherwise we answer with our preferred one and
/// let the client decide whether it can live with that.
pub const SUPPORTED_PROTOCOL_VERSIONS: &[&str] =
    &["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];
pub const PREFERRED_PROTOCOL_VERSION: &str = "2025-06-18";

const INSTRUCTIONS: &str = "\
Controls a running Mixxx instance: decks, mixer, library and Auto DJ.

Typical flow for DJing a set:
  1. mixxx_get_state to see which decks are loaded and playing.
  2. mixxx_suggest_next (seeded from the playing deck) to pick a track
     that matches in tempo and key, then mixxx_load_track onto the idle
     deck and mixxx_set_rate or mixxx_sync to beatmatch it.
  3. mixxx_wait_until with remaining_seconds to sleep until the outro.
  4. mixxx_crossfade to perform the transition.

The local library (mixxx_search_library, track ids) and an attached
Subsonic/Navidrome server (mixxx_subsonic_*, server-side ids) are separate
collections: ids from one do not work with the other. mixxx_subsonic_status
says whether there is a Subsonic library at all; mixxx_subsonic_browse walks
it by artist/album, and mixxx_subsonic_load / mixxx_subsonic_autodj_add
download the tracks on the way to a deck or the queue.

Anything not covered by a typed tool is reachable through
mixxx_get_control / mixxx_set_control.";

pub async fn run() -> std::io::Result<()> {
    let server = Arc::new(Server::new());
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();

    // One writer, so concurrent handlers cannot interleave partial lines.
    let writer = tokio::spawn(async move {
        let mut stdout = tokio::io::stdout();
        while let Some(line) = rx.recv().await {
            if stdout.write_all(line.as_bytes()).await.is_err()
                || stdout.write_all(b"\n").await.is_err()
                || stdout.flush().await.is_err()
            {
                break;
            }
        }
    });

    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Some(line) = lines.next_line().await? {
        let line = line.trim().to_string();
        if line.is_empty() {
            continue;
        }
        let message: Value = match serde_json::from_str(&line) {
            Ok(value) => value,
            Err(e) => {
                let _ = tx.send(
                    error_response(Value::Null, -32700, &format!("invalid JSON: {e}")).to_string(),
                );
                continue;
            }
        };
        // Notifications and responses carry no id and expect no reply.
        let Some(id) = message.get("id").cloned() else {
            continue;
        };
        let server = server.clone();
        let tx = tx.clone();
        tokio::spawn(async move {
            let response = server.handle(id, &message).await;
            let _ = tx.send(response.to_string());
        });
    }

    drop(tx);
    let _ = writer.await;
    Ok(())
}

struct Server {
    /// Connected lazily so `mixxx-mcp` can be spawned before Mixxx is up,
    /// and reconnected transparently when Mixxx restarts.
    connection: tokio::sync::Mutex<Option<Arc<Client>>>,
}

impl Server {
    fn new() -> Self {
        Self {
            connection: tokio::sync::Mutex::new(None),
        }
    }

    async fn client(&self) -> Result<Arc<Client>, ClientError> {
        let mut guard = self.connection.lock().await;
        if let Some(client) = guard.as_ref() {
            if client.is_connected() {
                return Ok(client.clone());
            }
        }
        let client = Arc::new(Client::discover().await?);
        *guard = Some(client.clone());
        Ok(client)
    }

    async fn handle(&self, id: Value, message: &Value) -> Value {
        let method = message.get("method").and_then(Value::as_str).unwrap_or("");
        let params = message.get("params").cloned().unwrap_or_else(|| json!({}));

        match method {
            "initialize" => success(id, self.initialize(&params)),
            "ping" => success(id, json!({})),
            "tools/list" => success(
                id,
                json!({"tools": tools::catalog().iter().map(tools::Tool::to_json).collect::<Vec<_>>()}),
            ),
            "tools/call" => match self.call_tool(&params).await {
                Ok(result) => success(id, result),
                Err(response) => success(id, response),
            },
            // Answered so clients that probe every capability do not log
            // errors; this server only offers tools.
            "resources/list" => success(id, json!({"resources": []})),
            "resources/templates/list" => success(id, json!({"resourceTemplates": []})),
            "prompts/list" => success(id, json!({"prompts": []})),
            "logging/setLevel" => success(id, json!({})),
            other => error_response(id, -32601, &format!("unknown method: {other}")),
        }
    }

    fn initialize(&self, params: &Value) -> Value {
        let requested = params.get("protocolVersion").and_then(Value::as_str);
        let version = match requested {
            Some(version) if SUPPORTED_PROTOCOL_VERSIONS.contains(&version) => version,
            _ => PREFERRED_PROTOCOL_VERSION,
        };
        json!({
            "protocolVersion": version,
            "capabilities": {"tools": {"listChanged": false}},
            "serverInfo": {
                "name": "mixxx",
                "title": "Mixxx DJ",
                "version": env!("CARGO_PKG_VERSION"),
            },
            "instructions": INSTRUCTIONS,
        })
    }

    /// `Err` carries a tool-level failure, which MCP reports as a
    /// successful call with `isError: true` so the model can react to it.
    async fn call_tool(&self, params: &Value) -> Result<Value, Value> {
        let name = params
            .get("name")
            .and_then(Value::as_str)
            .ok_or_else(|| tool_error("tools/call requires a tool name"))?;
        let tool = tools::find(name).ok_or_else(|| tool_error(&format!("unknown tool: {name}")))?;
        let arguments = match params.get("arguments") {
            Some(Value::Object(map)) => Value::Object(map.clone()),
            Some(Value::Null) | None => json!({}),
            Some(_) => return Err(tool_error("arguments must be an object")),
        };

        let client = self
            .client()
            .await
            .map_err(|e| tool_error(&e.to_string()))?;
        match client.call(tool.method, arguments).await {
            Ok(result) => Ok(tool_result(result)),
            Err(e) => Err(tool_error(&e.to_string())),
        }
    }
}

fn tool_result(result: Value) -> Value {
    let text = serde_json::to_string_pretty(&result).unwrap_or_else(|_| result.to_string());
    let mut payload = json!({
        "content": [{"type": "text", "text": text}],
        "isError": false,
    });
    // structuredContent is specified to be an object, so only objects get it.
    if result.is_object() {
        payload["structuredContent"] = result;
    }
    payload
}

fn tool_error(message: &str) -> Value {
    json!({
        "content": [{"type": "text", "text": message}],
        "isError": true,
    })
}

fn success(id: Value, result: Value) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "result": result})
}

fn error_response(id: Value, code: i32, message: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initialize_echoes_a_supported_version() {
        let server = Server::new();
        let result = server.initialize(&json!({"protocolVersion": "2024-11-05"}));
        assert_eq!(result["protocolVersion"], json!("2024-11-05"));
        assert!(result["capabilities"]["tools"].is_object());
    }

    #[test]
    fn initialize_falls_back_for_unknown_versions() {
        let server = Server::new();
        let result = server.initialize(&json!({"protocolVersion": "1999-01-01"}));
        assert_eq!(result["protocolVersion"], json!(PREFERRED_PROTOCOL_VERSION));
    }

    #[test]
    fn object_results_are_also_returned_structured() {
        let payload = tool_result(json!({"bpm": 128}));
        assert_eq!(payload["structuredContent"]["bpm"], json!(128));
        assert_eq!(payload["isError"], json!(false));
        assert!(payload["content"][0]["text"]
            .as_str()
            .unwrap()
            .contains("128"));

        let scalar = tool_result(json!(3));
        assert!(scalar.get("structuredContent").is_none());
    }
}
