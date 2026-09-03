//! Smoke test against a live Subsonic/Navidrome server:
//!
//! ```sh
//! cargo run --example dump -- http://localhost:4533 <user> <password>
//! ```

use subsonic_rs::client::{Config, SubsonicClient};
use subsonic_rs::library::fetch_all_tracks;

fn main() {
    let mut args = std::env::args().skip(1);
    let (Some(base_url), Some(username), Some(password)) = (args.next(), args.next(), args.next())
    else {
        eprintln!("usage: dump <base_url> <username> <password>");
        std::process::exit(2);
    };

    let client = SubsonicClient::new(&Config {
        base_url,
        username,
        password,
        verify_tls: true,
    })
    .expect("failed to create client");

    println!("server version: {}", client.ping().expect("ping failed"));

    let tracks = fetch_all_tracks(&client, &mut |tracks, albums_done, albums_total| {
        eprint!("\rimporting: {tracks} tracks, album {albums_done}/{albums_total}");
        true
    })
    .expect("library fetch failed");
    eprintln!();

    for track in tracks.iter().take(20) {
        println!(
            "{} — {} [{}] ({}s, {}kbps, .{})",
            track.artist, track.title, track.album, track.duration, track.bit_rate, track.suffix
        );
    }
    println!("total: {} tracks", tracks.len());

    for playlist in client.get_playlists().expect("getPlaylists failed") {
        let ids = subsonic_rs::library::fetch_playlist_track_ids(&client, &playlist.id)
            .expect("getPlaylist failed");
        println!(
            "playlist: {} ({} tracks): {:?}",
            playlist.name, playlist.song_count, ids
        );
    }

    // Optional 4th argument: download the first track into this directory.
    if let Some(download_dir) = args.next() {
        if let Some(track) = tracks.first() {
            let path =
                subsonic_rs::download_track(&client, &track.id, &track.suffix, &download_dir)
                    .expect("download failed");
            let size = std::fs::metadata(&path).expect("stat failed").len();
            println!("downloaded: {path} ({size} bytes)");
        }
    }
}
