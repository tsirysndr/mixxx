//! cxx FFI bridge consumed by src/rocksky/ on the Mixxx C++ side.
//!
//! Threading contract: all functions may be called from the Qt main
//! thread; nothing blocks on the network. Scrobbles are fire-and-forget
//! tasks on an internal tokio runtime (with one 30s retry). Remote-player
//! commands are delivered by calling the CommandHandler's methods from a
//! runtime worker thread — the C++ implementation must marshal to its own
//! thread (e.g. queued QMetaObject::invokeMethod). stop_remote_player()
//! blocks until the command loop has finished, after which no further
//! handler calls occur.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use cxx::UniquePtr;
use rocksky_rs::rocksky_sdk::appview::{AppView, ScrobbleInput};
use rocksky_rs::rocksky_sdk::remote_player::{
    RemoteCommand, RemoteNowPlaying, RemotePlayer, RemotePlayerConfig, RemoteStatus,
};
use rocksky_rs::rocksky_sdk::DEFAULT_APPVIEW;

#[cxx::bridge(namespace = "rocksky")]
mod ffi {
    struct ScrobbleData {
        title: String,
        artist: String,
        album: String,
        album_artist: String,
        duration_ms: u64,
        /// Unix seconds at play start.
        timestamp: i64,
        /// 0 = unknown.
        track_number: i32,
        /// 0 = unknown.
        year: i32,
    }

    struct NowPlayingData {
        title: String,
        artist: String,
        album: String,
        album_artist: String,
        duration_ms: u64,
        elapsed_ms: u64,
        is_playing: bool,
        /// Empty = unknown.
        codec: String,
        /// 0 = unknown.
        sample_rate: u32,
    }

    /// One entry of the playback queue: advertised outbound (ids empty)
    /// and received inbound via enqueue commands, where track_id is the
    /// Navidrome/Subsonic id a controller wants played.
    struct QueueItemData {
        track_id: String,
        title: String,
        artist: String,
        album: String,
        album_artist: String,
        duration_ms: u64,
        track_number: i32,
    }

    unsafe extern "C++" {
        include!("rocksky/rockskycommandhandler.h");

        type CommandHandler;

        fn onPlay(self: Pin<&mut CommandHandler>);
        fn onPause(self: Pin<&mut CommandHandler>);
        fn onNext(self: Pin<&mut CommandHandler>);
        fn onPrevious(self: Pin<&mut CommandHandler>);
        fn onSeek(self: Pin<&mut CommandHandler>, positionMs: u64);
        fn onQueueJump(self: Pin<&mut CommandHandler>, index: u32);
        fn onQueueRemove(self: Pin<&mut CommandHandler>, index: u32);
        fn onQueueMove(self: Pin<&mut CommandHandler>, from: u32, to: u32);
        /// mode: 0 = now, 1 = next, 2 = last.
        fn onEnqueue(self: Pin<&mut CommandHandler>, items: Vec<QueueItemData>, mode: u8);
    }

    extern "Rust" {
        type Client;

        /// ROCKSKY_TOKEN env var, then ~/.rocksky/token.json.
        fn resolve_token() -> Result<String>;
        fn new_client(token: &str) -> Result<Box<Client>>;
        /// Fire-and-forget; failures are retried once after 30s, then logged.
        fn submit_scrobble(client: &Client, data: &ScrobbleData);
        /// Connects and registers the device; reconnection is automatic.
        fn start_remote_player(
            client: &Client,
            device_name: &str,
            handler: UniquePtr<CommandHandler>,
        );
        fn update_now_playing(client: &Client, data: &NowPlayingData);
        /// Advertise the playback queue and the index of the current item.
        fn update_queue(client: &Client, items: Vec<QueueItemData>, index: u32);
        fn set_stopped(client: &Client);
        /// Blocks until the command loop has terminated (bounded, no network).
        fn stop_remote_player(client: &Client);
    }
}

/// The C++ handler is only ever *called* from the command-loop task; the
/// C++ side promises its methods are safe to call from any thread.
/// Access goes through pin_mut() so async blocks capture the whole
/// wrapper (and thus this Send impl) rather than the inner UniquePtr.
struct SendHandler(UniquePtr<ffi::CommandHandler>);
unsafe impl Send for SendHandler {}

impl SendHandler {
    fn pin_mut(&mut self) -> core::pin::Pin<&mut ffi::CommandHandler> {
        self.0.pin_mut()
    }
}

pub struct Client {
    runtime: rocksky_rs::tokio::runtime::Runtime,
    appview: AppView,
    token: String,
    player: Mutex<Option<Arc<RemotePlayer>>>,
    command_loop: Mutex<Option<rocksky_rs::tokio::task::JoinHandle<()>>>,
}

fn resolve_token() -> Result<String, rocksky_rs::token::TokenError> {
    rocksky_rs::token::resolve_token()
}

fn new_client(token: &str) -> Result<Box<Client>, std::io::Error> {
    let runtime = rocksky_rs::tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?;
    let appview = AppView::new(DEFAULT_APPVIEW).with_token(token);
    Ok(Box::new(Client {
        runtime,
        appview,
        token: token.to_string(),
        player: Mutex::new(None),
        command_loop: Mutex::new(None),
    }))
}

fn submit_scrobble(client: &Client, data: &ffi::ScrobbleData) {
    let input = ScrobbleInput {
        title: data.title.clone(),
        artist: data.artist.clone(),
        album_artist: if data.album_artist.is_empty() {
            data.artist.clone()
        } else {
            data.album_artist.clone()
        },
        album: Some(data.album.clone()),
        duration: Some(data.duration_ms),
        timestamp: Some(data.timestamp),
        track_number: (data.track_number > 0).then_some(data.track_number),
        year: (data.year > 0).then_some(data.year),
        ..Default::default()
    };
    let appview = client.appview.clone();
    client.runtime.spawn(async move {
        for attempt in 0..2 {
            match appview.create_scrobble(&input).await {
                Ok(_) => {
                    eprintln!("rocksky: scrobbled \"{} - {}\"", input.artist, input.title);
                    return;
                }
                Err(e) if attempt == 0 => {
                    eprintln!("rocksky: scrobble failed ({e}), retrying in 30s");
                    rocksky_rs::tokio::time::sleep(Duration::from_secs(30)).await;
                }
                Err(e) => {
                    eprintln!("rocksky: scrobble failed permanently: {e}");
                }
            }
        }
    });
}

fn start_remote_player(
    client: &Client,
    device_name: &str,
    handler: UniquePtr<ffi::CommandHandler>,
) {
    stop_remote_player(client);
    let _guard = client.runtime.enter();
    let player = Arc::new(RemotePlayer::connect(RemotePlayerConfig::new(
        client_token(client),
        device_name,
    )));
    *client.player.lock().unwrap() = Some(player.clone());

    let mut handler = SendHandler(handler);
    let handle = client.runtime.spawn(async move {
        while let Some(command) = player.next_command().await {
            let h = handler.pin_mut();
            match command {
                RemoteCommand::Play => h.onPlay(),
                RemoteCommand::Pause => h.onPause(),
                RemoteCommand::Next => h.onNext(),
                RemoteCommand::Previous => h.onPrevious(),
                RemoteCommand::Seek { position_ms } => h.onSeek(position_ms),
                RemoteCommand::QueueJump { index } => h.onQueueJump(index),
                RemoteCommand::QueueRemove { index } => h.onQueueRemove(index),
                RemoteCommand::QueueMove { from, to } => h.onQueueMove(from, to),
                RemoteCommand::Enqueue { tracks, mode, .. } => {
                    let items: Vec<ffi::QueueItemData> = tracks
                        .into_iter()
                        .map(|t| ffi::QueueItemData {
                            track_id: t.track_id,
                            title: t.title,
                            artist: t.artist,
                            album: t.album,
                            album_artist: t.album_artist,
                            duration_ms: t.duration_ms,
                            track_number: t.track_number,
                        })
                        .collect();
                    let mode_code = match mode.as_str() {
                        "now" => 0,
                        "next" => 1,
                        _ => 2,
                    };
                    h.onEnqueue(items.into(), mode_code);
                }
                // Shuffle/repeat/volume have no deck semantics a
                // controller should drive; ignoring is protocol-legal.
                _ => {}
            }
        }
    });
    *client.command_loop.lock().unwrap() = Some(handle);
}

fn client_token(client: &Client) -> String {
    client.token.clone()
}

fn update_now_playing(client: &Client, data: &ffi::NowPlayingData) {
    if let Some(player) = client.player.lock().unwrap().as_ref() {
        player.set_now_playing(RemoteNowPlaying {
            title: data.title.clone(),
            artist: data.artist.clone(),
            album: data.album.clone(),
            album_artist: data.album_artist.clone(),
            album_art: String::new(),
            duration_ms: data.duration_ms,
            elapsed_ms: data.elapsed_ms,
            is_playing: data.is_playing,
            codec: (!data.codec.is_empty()).then(|| data.codec.clone()),
            sample_rate: (data.sample_rate > 0).then_some(data.sample_rate),
            // Mixxx decks have none of these controls; None hides the
            // toggles in controller UIs instead of showing them wrong.
            shuffle: None,
            repeat: None,
            volume: None,
        });
    }
}

fn update_queue(client: &Client, items: Vec<ffi::QueueItemData>, index: u32) {
    use rocksky_rs::rocksky_sdk::remote_player::RemoteQueueItem;
    if let Some(player) = client.player.lock().unwrap().as_ref() {
        let queue = items
            .into_iter()
            .map(|item| RemoteQueueItem {
                track_id: item.track_id,
                title: item.title,
                artist: item.artist,
                album: item.album,
                album_artist: item.album_artist,
                duration_ms: item.duration_ms,
                track_number: item.track_number,
                ..Default::default()
            })
            .collect();
        player.set_queue(queue, index);
    }
}

fn set_stopped(client: &Client) {
    if let Some(player) = client.player.lock().unwrap().as_ref() {
        player.set_status(RemoteStatus::Stopped);
    }
}

fn stop_remote_player(client: &Client) {
    if let Some(player) = client.player.lock().unwrap().take() {
        player.disconnect();
    }
    if let Some(handle) = client.command_loop.lock().unwrap().take() {
        let _ = client.runtime.block_on(handle);
    }
}
