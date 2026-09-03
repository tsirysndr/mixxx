use serde::Deserialize;

/// Top-level envelope every Subsonic JSON response is wrapped in.
#[derive(Debug, Deserialize)]
pub struct Envelope {
    #[serde(rename = "subsonic-response")]
    pub inner: ResponseBody,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ResponseBody {
    pub status: String,
    pub version: String,
    pub error: Option<ApiError>,
    pub album_list2: Option<AlbumList2>,
    pub album: Option<AlbumWithSongs>,
    pub playlists: Option<Playlists>,
    pub playlist: Option<PlaylistWithSongs>,
    pub search_result3: Option<SearchResult3>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct SearchResult3 {
    pub song: Vec<Child>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct ApiError {
    pub code: i32,
    pub message: String,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AlbumList2 {
    pub album: Vec<AlbumID3>,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AlbumID3 {
    pub id: String,
    pub name: String,
    pub artist: String,
    pub year: i32,
    pub song_count: i32,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct AlbumWithSongs {
    #[serde(flatten)]
    pub info: AlbumID3,
    pub song: Vec<Child>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Playlists {
    pub playlist: Vec<Playlist>,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Playlist {
    pub id: String,
    pub name: String,
    pub song_count: i32,
}

#[derive(Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct PlaylistWithSongs {
    #[serde(flatten)]
    pub info: Playlist,
    pub entry: Vec<Child>,
}

/// A media item ("song") as returned by getAlbum/getPlaylist.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Child {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub genre: String,
    pub year: i32,
    pub track: i32,
    pub duration: i32,
    pub bit_rate: i32,
    pub suffix: String,
    pub cover_art: String,
    pub size: i64,
}
