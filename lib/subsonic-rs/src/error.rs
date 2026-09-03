/// Subsonic API error codes that indicate a credential/authentication
/// problem rather than a transient failure.
/// See http://www.subsonic.org/pages/api.jsp ("Error codes").
const AUTH_ERROR_CODES: &[i32] = &[40, 41, 42, 43, 44];

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("HTTP request failed: {0}")]
    Http(#[from] reqwest::Error),

    #[error("authentication failed: {0}")]
    Auth(String),

    #[error("server error {code}: {message}")]
    Api { code: i32, message: String },

    #[error("unexpected server response: {0}")]
    Protocol(String),

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("operation cancelled")]
    Cancelled,
}

impl Error {
    pub fn from_api_error(code: i32, message: String) -> Self {
        if AUTH_ERROR_CODES.contains(&code) {
            Error::Auth(message)
        } else {
            Error::Api { code, message }
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;
