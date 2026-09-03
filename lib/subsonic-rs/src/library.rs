use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::thread;

use crate::client::SubsonicClient;
use crate::error::{Error, Result};
use crate::model::Child;

pub const ALBUM_PAGE_SIZE: usize = 500;
pub const SONG_PAGE_SIZE: usize = 500;
/// Concurrent getAlbum requests in the fallback album sweep.
const ALBUM_FETCH_CONCURRENCY: usize = 8;

/// Callback signature: `(tracks_loaded, albums_done, albums_total) -> keep_going`.
/// `albums_done`/`albums_total` are zero while the bulk search3 path is used.
pub type ProgressFn<'a> = &'a mut dyn FnMut(usize, usize, usize) -> bool;

/// Fetches every track on the server. Fast path: page all songs via
/// `search3` with an empty query (a couple of requests per thousand
/// tracks on Navidrome/OpenSubsonic servers). Fallback for servers that
/// return nothing there: page `getAlbumList2` and fetch each album's
/// songs with [`ALBUM_FETCH_CONCURRENCY`] parallel requests. Returns
/// `Error::Cancelled` as soon as the progress callback returns `false`.
pub fn fetch_all_tracks(client: &SubsonicClient, progress: ProgressFn) -> Result<Vec<Child>> {
    fetch_all_tracks_paged(client, ALBUM_PAGE_SIZE, progress)
}

/// Same as [`fetch_all_tracks`] with a configurable page size (for tests).
pub fn fetch_all_tracks_paged(
    client: &SubsonicClient,
    page_size: usize,
    progress: ProgressFn,
) -> Result<Vec<Child>> {
    match fetch_all_tracks_search3(client, page_size, progress) {
        Ok(tracks) if !tracks.is_empty() => return Ok(tracks),
        Err(Error::Cancelled) => return Err(Error::Cancelled),
        // Empty result or an error (e.g. the server rejects empty search
        // queries): fall back to the album sweep.
        Ok(_) | Err(_) => {}
    }
    fetch_all_tracks_by_album(client, page_size, progress)
}

fn fetch_all_tracks_search3(
    client: &SubsonicClient,
    page_size: usize,
    progress: ProgressFn,
) -> Result<Vec<Child>> {
    const PAGE_CONCURRENCY: usize = 4;
    let next_page = AtomicUsize::new(0);
    // Set when a short (= last) page was seen, on error, or on cancel;
    // workers stop taking new pages but always complete a taken page so
    // the page sequence below the last page has no gaps.
    let stop = AtomicBool::new(false);
    let (tx, rx) = mpsc::channel::<(usize, Result<Vec<Child>>)>();

    let mut cancelled = false;
    // Lowest failed page index and lowest short (= last) page index.
    // Failures beyond the last page are expected over-fetch from the
    // concurrent workers and must be ignored.
    let mut first_failure: Option<(usize, Error)> = None;
    let mut short_page: Option<usize> = None;
    let mut pages: Vec<Option<Vec<Child>>> = Vec::new();
    thread::scope(|scope| {
        for _ in 0..PAGE_CONCURRENCY {
            let tx = tx.clone();
            let next_page = &next_page;
            let stop = &stop;
            scope.spawn(move || loop {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                let page_index = next_page.fetch_add(1, Ordering::Relaxed);
                let result = client.search3_songs(page_index * page_size, page_size);
                match &result {
                    Ok(page) if page.len() < page_size => stop.store(true, Ordering::Relaxed),
                    Ok(_) => {}
                    Err(_) => stop.store(true, Ordering::Relaxed),
                }
                if tx.send((page_index, result)).is_err() {
                    break;
                }
            });
        }
        drop(tx);

        let mut tracks_loaded = 0;
        for (page_index, result) in rx.iter() {
            match result {
                Ok(page) => {
                    tracks_loaded += page.len();
                    if page.len() < page_size && short_page.map_or(true, |s| page_index < s) {
                        short_page = Some(page_index);
                    }
                    if pages.len() <= page_index {
                        pages.resize_with(page_index + 1, || None);
                    }
                    pages[page_index] = Some(page);
                    if !progress(tracks_loaded, 0, 0) {
                        cancelled = true;
                        stop.store(true, Ordering::Relaxed);
                    }
                }
                Err(e) => {
                    if first_failure
                        .as_ref()
                        .map_or(true, |(f, _)| page_index < *f)
                    {
                        first_failure = Some((page_index, e));
                    }
                }
            }
        }
    });

    if cancelled {
        return Err(Error::Cancelled);
    }
    if let Some((failed_page, e)) = first_failure {
        if short_page.map_or(true, |s| failed_page < s) {
            return Err(e);
        }
    }
    // Concatenate in offset order up to and including the first short
    // page; anything beyond it is server noise (usually empty pages).
    let mut tracks: Vec<Child> = Vec::new();
    for page in pages.into_iter().flatten() {
        let page_len = page.len();
        tracks.extend(page);
        if page_len < page_size {
            break;
        }
    }
    Ok(tracks)
}

fn fetch_all_tracks_by_album(
    client: &SubsonicClient,
    page_size: usize,
    progress: ProgressFn,
) -> Result<Vec<Child>> {
    let mut albums = Vec::new();
    loop {
        let page = client.get_album_list2(albums.len(), page_size)?;
        let page_len = page.len();
        albums.extend(page);
        if !progress(0, 0, albums.len()) {
            return Err(Error::Cancelled);
        }
        if page_len < page_size {
            break;
        }
    }

    let albums_total = albums.len();
    // Indexed so the final track order matches the album list order even
    // though responses arrive out of order.
    let mut songs_by_album: Vec<Option<Vec<Child>>> = vec![None; albums_total];
    let next_album = AtomicUsize::new(0);
    let cancel = AtomicBool::new(false);
    let (tx, rx) = mpsc::channel::<(usize, Result<Vec<Child>>)>();

    let mut failure: Option<Error> = None;
    thread::scope(|scope| {
        for _ in 0..ALBUM_FETCH_CONCURRENCY.min(albums_total.max(1)) {
            let tx = tx.clone();
            let albums = &albums;
            let next_album = &next_album;
            let cancel = &cancel;
            scope.spawn(move || loop {
                if cancel.load(Ordering::Relaxed) {
                    break;
                }
                let index = next_album.fetch_add(1, Ordering::Relaxed);
                if index >= albums.len() {
                    break;
                }
                let result = client.get_album(&albums[index].id).map(|album| album.song);
                if tx.send((index, result)).is_err() {
                    break;
                }
            });
        }
        drop(tx);

        let mut tracks_loaded = 0;
        let mut albums_done = 0;
        for (index, result) in rx.iter() {
            albums_done += 1;
            match result {
                Ok(songs) => {
                    tracks_loaded += songs.len();
                    songs_by_album[index] = Some(songs);
                }
                Err(e) => {
                    if failure.is_none() {
                        failure = Some(e);
                    }
                    cancel.store(true, Ordering::Relaxed);
                }
            }
            if !progress(tracks_loaded, albums_done, albums_total) {
                if failure.is_none() {
                    failure = Some(Error::Cancelled);
                }
                cancel.store(true, Ordering::Relaxed);
            }
        }
    });

    if let Some(e) = failure {
        return Err(e);
    }
    Ok(songs_by_album.into_iter().flatten().flatten().collect())
}

pub fn fetch_playlist_track_ids(client: &SubsonicClient, playlist_id: &str) -> Result<Vec<String>> {
    Ok(client
        .get_playlist(playlist_id)?
        .entry
        .into_iter()
        .map(|child| child.id)
        .collect())
}
