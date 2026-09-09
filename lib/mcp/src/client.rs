//! WebSocket client for the endpoint Mixxx exposes.

use std::time::Duration;

use jsonrpsee_core::client::ClientT;
use jsonrpsee_core::traits::ToRpcParams;
use jsonrpsee_ws_client::{WsClient, WsClientBuilder};
use serde_json::value::RawValue;
use serde_json::Value;

use crate::endpoint::{self, Endpoint};

/// Long enough for a `mixxx.crossfade` that runs for its maximum length.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(11 * 60);

pub struct Client {
    inner: WsClient,
    endpoint: Endpoint,
}

impl Client {
    /// Connect to the endpoint advertised by a running Mixxx.
    pub async fn discover() -> Result<Self, Error> {
        let (path, endpoint) = endpoint::discover().ok_or(Error::NotRunning)?;
        Self::connect(endpoint).await.map_err(|e| match e {
            // A stale descriptor from a crashed Mixxx is the common case;
            // say so rather than surfacing a bare connection refused.
            Error::Transport(_) => Error::StaleEndpoint(path.display().to_string()),
            other => other,
        })
    }

    pub async fn connect(endpoint: Endpoint) -> Result<Self, Error> {
        let mut headers = http::HeaderMap::new();
        let bearer = format!("Bearer {}", endpoint.token);
        headers.insert(
            http::header::AUTHORIZATION,
            http::HeaderValue::from_str(&bearer).map_err(|_| Error::BadToken)?,
        );
        let inner = WsClientBuilder::default()
            .set_headers(headers)
            .request_timeout(REQUEST_TIMEOUT)
            .connection_timeout(Duration::from_secs(5))
            .build(endpoint.url())
            .await
            .map_err(|e| Error::Transport(e.to_string()))?;
        Ok(Self { inner, endpoint })
    }

    pub fn endpoint(&self) -> &Endpoint {
        &self.endpoint
    }

    pub fn is_connected(&self) -> bool {
        self.inner.is_connected()
    }

    /// Call a `mixxx.*` method with an object of parameters.
    pub async fn call(&self, method: &str, params: Value) -> Result<Value, Error> {
        self.inner
            .request::<Value, _>(method, JsonParams(params))
            .await
            .map_err(|e| match e {
                jsonrpsee_core::ClientError::Call(err) => Error::Rpc {
                    code: err.code(),
                    message: err.message().to_string(),
                },
                other => Error::Transport(other.to_string()),
            })
    }
}

/// Passes a pre-built JSON object through as the `params` member.
struct JsonParams(Value);

impl ToRpcParams for JsonParams {
    fn to_rpc_params(self) -> Result<Option<Box<RawValue>>, serde_json::Error> {
        if self.0.is_null() {
            return Ok(None);
        }
        RawValue::from_string(serde_json::to_string(&self.0)?).map(Some)
    }
}

#[derive(Debug)]
pub enum Error {
    /// No endpoint descriptor was found in any of the usual places.
    NotRunning,
    /// A descriptor exists but nothing is listening on it.
    StaleEndpoint(String),
    BadToken,
    Transport(String),
    Rpc {
        code: i32,
        message: String,
    },
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::NotRunning => write!(
                f,
                "Mixxx does not appear to be running, or its MCP server is disabled \
                 (enable it in Preferences, or set [Mcp] Enabled to 1 in mixxx.cfg)"
            ),
            Error::StaleEndpoint(path) => write!(
                f,
                "found a Mixxx MCP endpoint at {path} but could not connect; \
                 Mixxx is probably not running any more"
            ),
            Error::BadToken => write!(f, "the endpoint descriptor contains an unusable token"),
            Error::Transport(message) => write!(f, "connection to Mixxx failed: {message}"),
            Error::Rpc { code, message } => write!(f, "Mixxx returned error {code}: {message}"),
        }
    }
}

impl std::error::Error for Error {}
