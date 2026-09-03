use std::path::{Path, PathBuf};

#[derive(Debug, thiserror::Error)]
pub enum TokenError {
    #[error("no Rocksky token: set ROCKSKY_TOKEN or run `rocksky login` (missing {0})")]
    Missing(String),
    #[error("failed to read {0}: {1}")]
    Io(String, std::io::Error),
    #[error("invalid token file {0}: {1}")]
    Invalid(String, String),
}

#[derive(serde::Deserialize)]
struct TokenFile {
    token: String,
}

fn default_token_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .unwrap_or_default();
    Path::new(&home).join(".rocksky").join("token.json")
}

pub fn read_token_from(path: &Path) -> Result<String, TokenError> {
    let display = path.display().to_string();
    if !path.exists() {
        return Err(TokenError::Missing(display));
    }
    let contents = std::fs::read_to_string(path).map_err(|e| TokenError::Io(display.clone(), e))?;
    let parsed: TokenFile = serde_json::from_str(&contents)
        .map_err(|e| TokenError::Invalid(display.clone(), e.to_string()))?;
    let token = parsed.token.trim().to_string();
    if token.is_empty() {
        return Err(TokenError::Invalid(display, "empty token".into()));
    }
    Ok(token)
}

/// Token resolution order (mirrors playerd): ROCKSKY_TOKEN env var, then
/// ~/.rocksky/token.json.
pub fn resolve_token() -> Result<String, TokenError> {
    if let Ok(token) = std::env::var("ROCKSKY_TOKEN") {
        let token = token.trim().to_string();
        if !token.is_empty() {
            return Ok(token);
        }
    }
    read_token_from(&default_token_path())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_cli_token_file_format() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("token.json");
        std::fs::write(&path, r#"{ "token": "eyJhbGciOi.test.jwt" }"#).unwrap();
        assert_eq!(read_token_from(&path).unwrap(), "eyJhbGciOi.test.jwt");
    }

    #[test]
    fn missing_and_invalid_files_error() {
        let dir = tempfile::tempdir().unwrap();
        assert!(matches!(
            read_token_from(&dir.path().join("nope.json")),
            Err(TokenError::Missing(_))
        ));
        let path = dir.path().join("bad.json");
        std::fs::write(&path, r#"{ "token": "" }"#).unwrap();
        assert!(matches!(
            read_token_from(&path),
            Err(TokenError::Invalid(..))
        ));
    }
}
