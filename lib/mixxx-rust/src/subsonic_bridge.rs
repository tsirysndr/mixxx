//! cxx FFI bridge consumed by src/library/subsonic/ on the Mixxx C++ side.
//!
//! Threading contract: `Client` is Send + Sync; all `&Client` functions may
//! be called concurrently from any thread (the QtConcurrent import worker,
//! deck-load threads, ...). Errors cross the bridge as `rust::Error`
//! exceptions carrying the `Display` form of [`subsonic_rs::Error`].

use std::pin::Pin;

use subsonic_rs::client::{Config, SubsonicClient};
use subsonic_rs::error::Error;
use subsonic_rs::model::Child;

#[cxx::bridge(namespace = "subsonic")]
mod ffi {
    struct ConnectionConfig {
        /// Server base URL without the `/rest` suffix.
        base_url: String,
        username: String,
        password: String,
        verify_tls: bool,
    }

    struct FfiTrack {
        id: String,
        title: String,
        artist: String,
        album: String,
        album_artist: String,
        genre: String,
        year: i32,
        track_number: i32,
        duration_seconds: i32,
        bitrate_kbps: i32,
        suffix: String,
        cover_art_id: String,
        size_bytes: i64,
    }

    struct FfiPlaylist {
        id: String,
        name: String,
        track_count: i32,
    }

    unsafe extern "C++" {
        include!("library/subsonic/subsonicimportprogress.h");

        type ImportProgress;

        /// Returns false to cancel the running import.
        fn onProgress(
            self: Pin<&mut ImportProgress>,
            tracksLoaded: usize,
            albumsDone: usize,
            albumsTotal: usize,
        ) -> bool;
    }

    extern "Rust" {
        type Client;

        fn new_client(config: &ConnectionConfig) -> Result<Box<Client>>;
        fn ping(client: &Client) -> Result<String>;
        fn fetch_all_tracks(
            client: &Client,
            progress: Pin<&mut ImportProgress>,
        ) -> Result<Vec<FfiTrack>>;
        fn fetch_playlists(client: &Client) -> Result<Vec<FfiPlaylist>>;
        fn fetch_playlist_track_ids(client: &Client, playlist_id: &str) -> Result<Vec<String>>;
        fn download_track(
            client: &Client,
            track_id: &str,
            suffix: &str,
            cache_dir: &str,
        ) -> Result<String>;
        /// size > 0 requests a server-side scaled thumbnail; 0 = original.
        fn download_cover_art(
            client: &Client,
            cover_art_id: &str,
            cache_dir: &str,
            size: u32,
        ) -> Result<String>;
        fn cache_file_name(track_id: &str, suffix: &str) -> String;
    }
}

pub struct Client {
    inner: SubsonicClient,
}

fn new_client(config: &ffi::ConnectionConfig) -> Result<Box<Client>, Error> {
    let inner = SubsonicClient::new(&Config {
        base_url: config.base_url.clone(),
        username: config.username.clone(),
        password: config.password.clone(),
        verify_tls: config.verify_tls,
    })?;
    Ok(Box::new(Client { inner }))
}

fn ping(client: &Client) -> Result<String, Error> {
    client.inner.ping()
}

fn fetch_all_tracks(
    client: &Client,
    mut progress: Pin<&mut ffi::ImportProgress>,
) -> Result<Vec<ffi::FfiTrack>, Error> {
    let mut callback = |tracks_loaded: usize, albums_done: usize, albums_total: usize| {
        progress
            .as_mut()
            .onProgress(tracks_loaded, albums_done, albums_total)
    };
    let tracks = subsonic_rs::library::fetch_all_tracks(&client.inner, &mut callback)?;
    Ok(tracks.into_iter().map(to_ffi_track).collect())
}

fn fetch_playlists(client: &Client) -> Result<Vec<ffi::FfiPlaylist>, Error> {
    Ok(client
        .inner
        .get_playlists()?
        .into_iter()
        .map(|playlist| ffi::FfiPlaylist {
            id: playlist.id,
            name: playlist.name,
            track_count: playlist.song_count,
        })
        .collect())
}

fn fetch_playlist_track_ids(client: &Client, playlist_id: &str) -> Result<Vec<String>, Error> {
    subsonic_rs::library::fetch_playlist_track_ids(&client.inner, playlist_id)
}

fn download_track(
    client: &Client,
    track_id: &str,
    suffix: &str,
    cache_dir: &str,
) -> Result<String, Error> {
    subsonic_rs::download::download_track(&client.inner, track_id, suffix, cache_dir)
}

fn download_cover_art(
    client: &Client,
    cover_art_id: &str,
    cache_dir: &str,
    size: u32,
) -> Result<String, Error> {
    subsonic_rs::download::download_cover_art(&client.inner, cover_art_id, cache_dir, size)
}

fn cache_file_name(track_id: &str, suffix: &str) -> String {
    subsonic_rs::download::cache_file_name(track_id, suffix)
}

fn to_ffi_track(child: Child) -> ffi::FfiTrack {
    ffi::FfiTrack {
        id: child.id,
        title: child.title,
        artist: child.artist,
        album: child.album,
        album_artist: child.album_artist,
        genre: child.genre,
        year: child.year,
        track_number: child.track,
        duration_seconds: child.duration,
        bitrate_kbps: child.bit_rate,
        suffix: child.suffix,
        cover_art_id: child.cover_art,
        size_bytes: child.size,
    }
}
