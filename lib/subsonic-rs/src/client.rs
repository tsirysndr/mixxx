use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use md5::{Digest, Md5};
use rand::distributions::Alphanumeric;
use rand::Rng;

use crate::error::{Error, Result};
use crate::model::{
    AlbumID3, AlbumWithSongs, Child, Envelope, Playlist, PlaylistWithSongs, ResponseBody,
};

pub const API_VERSION: &str = "1.16.1";
pub const CLIENT_NAME: &str = "mixxx";

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
/// Applied to metadata (JSON) requests only; downloads must not be
/// subject to a total-request timeout.
const JSON_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug, Clone)]
pub struct Config {
    /// Server base URL without the `/rest` suffix, e.g. `https://music.example.org`.
    pub base_url: String,
    pub username: String,
    pub password: String,
    pub verify_tls: bool,
}

pub struct SubsonicClient {
    http: reqwest::blocking::Client,
    base_url: String,
    username: String,
    password: String,
    /// Serializes concurrent downloads of the same track id.
    pub(crate) download_locks: Mutex<HashMap<String, Arc<Mutex<()>>>>,
}

/// Subsonic token auth: `t = hex(md5(password + salt))`.
pub fn auth_token(password: &str, salt: &str) -> String {
    let mut hasher = Md5::new();
    hasher.update(password.as_bytes());
    hasher.update(salt.as_bytes());
    hex::encode(hasher.finalize())
}

impl SubsonicClient {
    pub fn new(config: &Config) -> Result<Self> {
        let base_url = config.base_url.trim().trim_end_matches('/').to_string();
        if base_url.is_empty() {
            return Err(Error::Protocol("empty server URL".into()));
        }
        let http = reqwest::blocking::Client::builder()
            .connect_timeout(CONNECT_TIMEOUT)
            .timeout(None)
            .danger_accept_invalid_certs(!config.verify_tls)
            .build()?;
        Ok(SubsonicClient {
            http,
            base_url,
            username: config.username.clone(),
            password: config.password.clone(),
            download_locks: Mutex::new(HashMap::new()),
        })
    }

    fn auth_params(&self) -> Vec<(String, String)> {
        // Fresh random salt per request, per the Subsonic API spec.
        let salt: String = rand::thread_rng()
            .sample_iter(&Alphanumeric)
            .take(16)
            .map(char::from)
            .collect();
        vec![
            ("u".into(), self.username.clone()),
            ("t".into(), auth_token(&self.password, &salt)),
            ("s".into(), salt),
            ("v".into(), API_VERSION.into()),
            ("c".into(), CLIENT_NAME.into()),
            ("f".into(), "json".into()),
        ]
    }

    /// Raw GET without a total-request timeout (used for downloads).
    pub(crate) fn get(
        &self,
        endpoint: &str,
        params: &[(&str, &str)],
    ) -> Result<reqwest::blocking::Response> {
        let url = format!("{}/rest/{}", self.base_url, endpoint);
        let response = self
            .http
            .get(url)
            .query(&self.auth_params())
            .query(params)
            .send()?;
        Ok(response)
    }

    fn get_json(&self, endpoint: &str, params: &[(&str, &str)]) -> Result<ResponseBody> {
        let url = format!("{}/rest/{}", self.base_url, endpoint);
        let response = self
            .http
            .get(url)
            .query(&self.auth_params())
            .query(params)
            .timeout(JSON_TIMEOUT)
            .send()?
            .error_for_status()?;
        let envelope: Envelope = response
            .json()
            .map_err(|e| Error::Protocol(format!("invalid JSON from {endpoint}: {e}")))?;
        let body = envelope.inner;
        if let Some(err) = body.error {
            return Err(Error::from_api_error(err.code, err.message));
        }
        if body.status != "ok" {
            return Err(Error::Protocol(format!(
                "{endpoint} returned status {:?}",
                body.status
            )));
        }
        Ok(body)
    }

    /// Returns the server's reported version string.
    pub fn ping(&self) -> Result<String> {
        Ok(self.get_json("ping.view", &[])?.version)
    }

    pub fn get_album_list2(&self, offset: usize, size: usize) -> Result<Vec<AlbumID3>> {
        let size = size.to_string();
        let offset = offset.to_string();
        let body = self.get_json(
            "getAlbumList2.view",
            &[
                ("type", "alphabeticalByName"),
                ("size", &size),
                ("offset", &offset),
            ],
        )?;
        Ok(body.album_list2.map(|l| l.album).unwrap_or_default())
    }

    pub fn get_album(&self, id: &str) -> Result<AlbumWithSongs> {
        self.get_json("getAlbum.view", &[("id", id)])?
            .album
            .ok_or_else(|| Error::Protocol("missing album in getAlbum response".into()))
    }

    /// Bulk song paging via search3 with an empty query. Navidrome and
    /// other OpenSubsonic servers return the whole library this way;
    /// servers that don't support empty queries return nothing (callers
    /// fall back to the per-album sweep).
    pub fn search3_songs(&self, offset: usize, count: usize) -> Result<Vec<Child>> {
        let count = count.to_string();
        let offset = offset.to_string();
        let body = self.get_json(
            "search3.view",
            &[
                ("query", ""),
                ("artistCount", "0"),
                ("albumCount", "0"),
                ("songCount", &count),
                ("songOffset", &offset),
            ],
        )?;
        Ok(body.search_result3.map(|r| r.song).unwrap_or_default())
    }

    pub fn get_playlists(&self) -> Result<Vec<Playlist>> {
        let body = self.get_json("getPlaylists.view", &[])?;
        Ok(body.playlists.map(|p| p.playlist).unwrap_or_default())
    }

    pub fn get_playlist(&self, id: &str) -> Result<PlaylistWithSongs> {
        self.get_json("getPlaylist.view", &[("id", id)])?
            .playlist
            .ok_or_else(|| Error::Protocol("missing playlist in getPlaylist response".into()))
    }
}

#[cfg(test)]
mod tests {
    use super::auth_token;

    #[test]
    fn auth_token_matches_subsonic_api_docs_example() {
        // Documented example: password "sesame", salt "c19b2d".
        assert_eq!(
            auth_token("sesame", "c19b2d"),
            "26719a1196d2a940705a59634eb18eab"
        );
    }
}
