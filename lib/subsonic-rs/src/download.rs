use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use crate::client::SubsonicClient;
use crate::error::{Error, Result};

/// Deterministic cache file name for a track. The C++ side obtains cache
/// paths exclusively through the FFI (`cache_file_name`), so this is the
/// single source of truth for the naming scheme.
pub fn cache_file_name(track_id: &str, suffix: &str) -> String {
    format!(
        "{}.{}",
        sanitize_component(track_id),
        sanitize_extension(suffix)
    )
}

fn sanitize_component(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'-' | b'_' => out.push(b as char),
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    if out.is_empty() {
        out.push('_');
    }
    out
}

fn sanitize_extension(suffix: &str) -> String {
    let ext: String = suffix
        .chars()
        .filter(char::is_ascii_alphanumeric)
        .map(|c| c.to_ascii_lowercase())
        .collect();
    if ext.is_empty() {
        "bin".to_string()
    } else {
        ext
    }
}

/// Downloads a track into `cache_dir` (original quality via `download`,
/// falling back to `stream?format=raw`) and returns the absolute path of
/// the cached file. Returns immediately if the file is already cached.
/// Safe to call concurrently, also for the same track id.
pub fn download_track(
    client: &SubsonicClient,
    track_id: &str,
    suffix: &str,
    cache_dir: &str,
) -> Result<String> {
    let file_name = cache_file_name(track_id, suffix);
    let final_path = Path::new(cache_dir).join(&file_name);
    if final_path.exists() {
        return path_to_string(final_path);
    }

    let lock = per_id_lock(client, track_id);
    let _guard = lock.lock().unwrap_or_else(|e| e.into_inner());
    if final_path.exists() {
        // Another thread finished the download while we waited.
        return path_to_string(final_path);
    }

    fs::create_dir_all(cache_dir)?;
    let response = fetch_audio(client, track_id)?;
    write_atomically(response, cache_dir, &file_name, &final_path)?;
    path_to_string(final_path)
}

/// Downloads cover art into `cache_dir` and returns the cached file path.
pub fn download_cover_art(
    client: &SubsonicClient,
    cover_art_id: &str,
    cache_dir: &str,
) -> Result<String> {
    let file_name = format!("cover-{}.jpg", sanitize_component(cover_art_id));
    let final_path = Path::new(cache_dir).join(&file_name);
    if final_path.exists() {
        return path_to_string(final_path);
    }
    fs::create_dir_all(cache_dir)?;
    let response = fetch_binary(client, "getCoverArt.view", &[("id", cover_art_id)])?;
    write_atomically(response, cache_dir, &file_name, &final_path)?;
    path_to_string(final_path)
}

fn fetch_audio(client: &SubsonicClient, track_id: &str) -> Result<reqwest::blocking::Response> {
    match fetch_binary(client, "download.view", &[("id", track_id)]) {
        Ok(response) => Ok(response),
        // Credentials won't get better on retry; everything else (download
        // disabled by the server admin, HTTP error) is worth the fallback.
        Err(Error::Auth(message)) => Err(Error::Auth(message)),
        Err(_) => fetch_binary(
            client,
            "stream.view",
            &[("id", track_id), ("format", "raw")],
        ),
    }
}

fn fetch_binary(
    client: &SubsonicClient,
    endpoint: &str,
    params: &[(&str, &str)],
) -> Result<reqwest::blocking::Response> {
    let response = client.get(endpoint, params)?;
    if !response.status().is_success() {
        return Err(Error::Protocol(format!(
            "{endpoint} returned HTTP {}",
            response.status()
        )));
    }
    // Subsonic servers report API errors as a JSON body with HTTP 200.
    let is_json = response
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
        .is_some_and(|ct| ct.contains("application/json"));
    if is_json {
        let envelope: crate::model::Envelope = response
            .json()
            .map_err(|e| Error::Protocol(format!("invalid JSON from {endpoint}: {e}")))?;
        if let Some(err) = envelope.inner.error {
            return Err(Error::from_api_error(err.code, err.message));
        }
        return Err(Error::Protocol(format!(
            "{endpoint} returned JSON instead of binary data"
        )));
    }
    Ok(response)
}

fn write_atomically(
    mut response: reqwest::blocking::Response,
    cache_dir: &str,
    file_name: &str,
    final_path: &Path,
) -> Result<()> {
    let partial_path = Path::new(cache_dir).join(format!(".partial-{file_name}"));
    let result = (|| -> Result<()> {
        let mut file = fs::File::create(&partial_path)?;
        io::copy(&mut response, &mut file)?;
        file.sync_all()?;
        fs::rename(&partial_path, final_path)?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&partial_path);
    }
    result
}

fn per_id_lock(client: &SubsonicClient, track_id: &str) -> Arc<Mutex<()>> {
    let mut locks = client
        .download_locks
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    locks.entry(track_id.to_string()).or_default().clone()
}

fn path_to_string(path: PathBuf) -> Result<String> {
    path.into_os_string()
        .into_string()
        .map_err(|_| Error::Protocol("cache path is not valid UTF-8".into()))
}

#[cfg(test)]
mod tests {
    use super::cache_file_name;

    #[test]
    fn cache_file_name_is_deterministic_and_safe() {
        assert_eq!(cache_file_name("abc123", "flac"), "abc123.flac");
        assert_eq!(cache_file_name("tr/17 x", "MP3"), "tr%2F17%20x.mp3");
        assert_eq!(cache_file_name("", ""), "_.bin");
    }
}
