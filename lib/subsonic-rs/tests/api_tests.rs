use httpmock::prelude::*;
use serde_json::json;

use subsonic_rs::client::{Config, SubsonicClient};
use subsonic_rs::library::{fetch_all_tracks_paged, fetch_playlist_track_ids};
use subsonic_rs::{download_track, Error};

fn make_client(server: &MockServer) -> SubsonicClient {
    SubsonicClient::new(&Config {
        base_url: server.base_url(),
        username: "alice".into(),
        password: "sesame".into(),
        verify_tls: true,
    })
    .unwrap()
}

fn ok_body(extra: serde_json::Value) -> serde_json::Value {
    let mut body = json!({
        "status": "ok",
        "version": "1.16.1",
    });
    body.as_object_mut()
        .unwrap()
        .extend(extra.as_object().unwrap().clone());
    json!({ "subsonic-response": body })
}

fn album(id: &str, name: &str) -> serde_json::Value {
    json!({"id": id, "name": name, "artist": "Artist", "year": 2020, "songCount": 2})
}

fn song(id: &str, title: &str) -> serde_json::Value {
    json!({
        "id": id, "title": title, "artist": "Artist", "album": "Album",
        "albumArtist": "Artist", "genre": "House", "year": 2020, "track": 1,
        "duration": 180, "bitRate": 1024, "suffix": "flac",
        "coverArt": "c1", "size": 12345678
    })
}

#[test]
fn ping_sends_required_auth_params() {
    let server = MockServer::start();
    let mock = server.mock(|when, then| {
        when.method(GET)
            .path("/rest/ping.view")
            .query_param("u", "alice")
            .query_param("v", "1.16.1")
            .query_param("c", "mixxx")
            .query_param("f", "json")
            .query_param_exists("t")
            .query_param_exists("s");
        then.status(200).json_body(ok_body(json!({})));
    });

    let version = make_client(&server).ping().unwrap();
    assert_eq!(version, "1.16.1");
    mock.assert();
}

#[test]
fn ping_maps_auth_failure() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET).path("/rest/ping.view");
        then.status(200).json_body(json!({
            "subsonic-response": {
                "status": "failed",
                "version": "1.16.1",
                "error": {"code": 40, "message": "Wrong username or password"}
            }
        }));
    });

    let err = make_client(&server).ping().unwrap_err();
    assert!(matches!(err, Error::Auth(_)), "got {err:?}");
}

#[test]
fn fetch_all_tracks_uses_search3_fast_path() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET)
            .path("/rest/search3.view")
            .query_param("query", "")
            .query_param("songCount", "2")
            .query_param("songOffset", "0");
        then.status(200).json_body(ok_body(
            json!({"searchResult3": {"song": [song("s1", "T1"), song("s2", "T2")]}}),
        ));
    });
    server.mock(|when, then| {
        when.method(GET)
            .path("/rest/search3.view")
            .query_param("songOffset", "2");
        then.status(200).json_body(ok_body(
            json!({"searchResult3": {"song": [song("s3", "T3")]}}),
        ));
    });

    let client = make_client(&server);
    let tracks = fetch_all_tracks_paged(&client, 2, &mut |_, _, _| true).unwrap();
    assert_eq!(tracks.len(), 3);
    assert_eq!(tracks[0].id, "s1");
    assert_eq!(tracks[2].id, "s3");
}

#[test]
fn fetch_all_tracks_search3_cancels_via_callback() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET).path("/rest/search3.view");
        then.status(200).json_body(ok_body(
            json!({"searchResult3": {"song": [song("s1", "T1")]}}),
        ));
    });

    let client = make_client(&server);
    let err = fetch_all_tracks_paged(&client, 500, &mut |_, _, _| false).unwrap_err();
    assert!(matches!(err, Error::Cancelled), "got {err:?}");
}

// The album-sweep tests below have no search3 mock: the fast path gets a
// 404 and the implementation must fall back to getAlbumList2/getAlbum.
#[test]
fn fetch_all_tracks_pages_albums_and_collects_songs() {
    let server = MockServer::start();
    // Page 1 (offset 0, full page of 2) and page 2 (offset 2, short page of 1).
    server.mock(|when, then| {
        when.method(GET)
            .path("/rest/getAlbumList2.view")
            .query_param("offset", "0")
            .query_param("size", "2");
        then.status(200).json_body(ok_body(
            json!({"albumList2": {"album": [album("a1", "One"), album("a2", "Two")]}}),
        ));
    });
    server.mock(|when, then| {
        when.method(GET)
            .path("/rest/getAlbumList2.view")
            .query_param("offset", "2")
            .query_param("size", "2");
        then.status(200).json_body(ok_body(
            json!({"albumList2": {"album": [album("a3", "Three")]}}),
        ));
    });
    for (id, songs) in [
        ("a1", vec![song("s1", "T1"), song("s2", "T2")]),
        ("a2", vec![song("s3", "T3")]),
        ("a3", vec![song("s4", "T4")]),
    ] {
        server.mock(move |when, then| {
            when.method(GET)
                .path("/rest/getAlbum.view")
                .query_param("id", id);
            then.status(200).json_body(ok_body(
                json!({"album": {"id": id, "name": "Album", "song": songs}}),
            ));
        });
    }

    let client = make_client(&server);
    let mut progress_calls = Vec::new();
    let tracks = fetch_all_tracks_paged(&client, 2, &mut |tracks, done, total| {
        progress_calls.push((tracks, done, total));
        true
    })
    .unwrap();

    assert_eq!(tracks.len(), 4);
    assert_eq!(tracks[0].id, "s1");
    assert_eq!(tracks[0].suffix, "flac");
    assert_eq!(tracks[0].album_artist, "Artist");
    // Two paging calls + three album calls.
    assert_eq!(progress_calls.len(), 5);
    assert_eq!(*progress_calls.last().unwrap(), (4, 3, 3));
}

#[test]
fn fetch_all_tracks_cancels_via_callback() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET).path("/rest/getAlbumList2.view");
        then.status(200).json_body(ok_body(
            json!({"albumList2": {"album": [album("a1", "One")]}}),
        ));
    });
    let album_mock = server.mock(|when, then| {
        when.method(GET).path("/rest/getAlbum.view");
        then.status(200).json_body(ok_body(
            json!({"album": {"id": "a1", "name": "One", "song": [song("s1", "T1")]}}),
        ));
    });

    let client = make_client(&server);
    // Cancel as soon as the first album has been imported.
    let err =
        fetch_all_tracks_paged(&client, 500, &mut |_, albums_done, _| albums_done < 1).unwrap_err();
    assert!(matches!(err, Error::Cancelled), "got {err:?}");
    album_mock.assert_hits(1);
}

#[test]
fn fetch_playlist_track_ids_returns_entries_in_order() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET)
            .path("/rest/getPlaylist.view")
            .query_param("id", "p1");
        then.status(200).json_body(ok_body(json!({
            "playlist": {
                "id": "p1", "name": "Warmup", "songCount": 2,
                "entry": [song("s9", "A"), song("s3", "B")]
            }
        })));
    });

    let ids = fetch_playlist_track_ids(&make_client(&server), "p1").unwrap();
    assert_eq!(ids, vec!["s9".to_string(), "s3".to_string()]);
}

#[test]
fn download_track_writes_file_and_caches() {
    let server = MockServer::start();
    let mock = server.mock(|when, then| {
        when.method(GET)
            .path("/rest/download.view")
            .query_param("id", "s1");
        then.status(200)
            .header("content-type", "audio/flac")
            .body("FLACDATA");
    });

    let client = make_client(&server);
    let cache_dir = tempdir("download_track");

    let path = download_track(&client, "s1", "flac", &cache_dir).unwrap();
    assert!(path.ends_with("s1.flac"));
    assert_eq!(std::fs::read_to_string(&path).unwrap(), "FLACDATA");

    // Second call must be a cache hit: no additional HTTP request.
    let path2 = download_track(&client, "s1", "flac", &cache_dir).unwrap();
    assert_eq!(path, path2);
    mock.assert_hits(1);
    // No partial file left behind.
    assert_eq!(std::fs::read_dir(&cache_dir).unwrap().count(), 1);
}

#[test]
fn download_track_falls_back_to_raw_stream() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET).path("/rest/download.view");
        then.status(200)
            .header("content-type", "application/json")
            .json_body(json!({
                "subsonic-response": {
                    "status": "failed",
                    "version": "1.16.1",
                    "error": {"code": 70, "message": "Downloads disabled"}
                }
            }));
    });
    server.mock(|when, then| {
        when.method(GET)
            .path("/rest/stream.view")
            .query_param("id", "s2")
            .query_param("format", "raw");
        then.status(200)
            .header("content-type", "audio/mpeg")
            .body("MP3DATA");
    });

    let client = make_client(&server);
    let cache_dir = tempdir("download_fallback");
    let path = download_track(&client, "s2", "mp3", &cache_dir).unwrap();
    assert_eq!(std::fs::read_to_string(path).unwrap(), "MP3DATA");
}

#[test]
fn download_track_surfaces_auth_error_without_fallback() {
    let server = MockServer::start();
    server.mock(|when, then| {
        when.method(GET).path("/rest/download.view");
        then.status(200)
            .header("content-type", "application/json")
            .json_body(json!({
                "subsonic-response": {
                    "status": "failed",
                    "version": "1.16.1",
                    "error": {"code": 40, "message": "Wrong username or password"}
                }
            }));
    });
    let stream_mock = server.mock(|when, then| {
        when.method(GET).path("/rest/stream.view");
        then.status(200).body("SHOULD NOT BE REACHED");
    });

    let client = make_client(&server);
    let cache_dir = tempdir("download_auth");
    let err = download_track(&client, "s3", "mp3", &cache_dir).unwrap_err();
    assert!(matches!(err, Error::Auth(_)), "got {err:?}");
    stream_mock.assert_hits(0);
    assert_eq!(std::fs::read_dir(&cache_dir).unwrap().count(), 0);
}

fn tempdir(label: &str) -> String {
    let dir = std::env::temp_dir().join(format!("subsonic-rs-test-{label}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir.to_str().unwrap().to_string()
}
