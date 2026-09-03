#include "library/subsonic/subsonicdao.h"

#include <QSqlError>
#include <QVariant>
#include <QtDebug>

#include "library/queryutil.h"

namespace {

const QStringList kCreateTableQueries = {
        QStringLiteral(
                "CREATE TABLE IF NOT EXISTS subsonic_library ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    subsonic_id TEXT UNIQUE,"
                "    artist TEXT,"
                "    title TEXT,"
                "    album TEXT,"
                "    album_artist TEXT,"
                "    genre TEXT,"
                "    grouping TEXT,"
                "    year INTEGER,"
                "    tracknumber TEXT,"
                "    location TEXT UNIQUE,"
                "    comment TEXT,"
                "    duration INTEGER,"
                "    bitrate INTEGER,"
                "    bpm FLOAT,"
                "    rating INTEGER,"
                "    cover_art_id TEXT,"
                "    coverart_source INTEGER,"
                "    coverart_type INTEGER,"
                "    coverart_location TEXT,"
                "    coverart_color INTEGER,"
                "    coverart_digest BLOB,"
                "    coverart_hash INTEGER"
                ")"),
        QStringLiteral(
                "CREATE TABLE IF NOT EXISTS subsonic_playlists ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    subsonic_id TEXT,"
                "    name TEXT"
                ")"),
        QStringLiteral(
                "CREATE TABLE IF NOT EXISTS subsonic_playlist_tracks ("
                "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "    playlist_id INTEGER REFERENCES subsonic_playlists(id),"
                "    track_id INTEGER REFERENCES subsonic_library(id),"
                "    position INTEGER"
                ")"),
};

const QStringList kTableNames = {
        QStringLiteral("subsonic_playlist_tracks"),
        QStringLiteral("subsonic_playlists"),
        QStringLiteral("subsonic_library"),
};

} // anonymous namespace

// static
bool SubsonicDAO::createTables(const QSqlDatabase& database) {
    for (const QString& queryString : kCreateTableQueries) {
        QSqlQuery query(database);
        query.prepare(queryString);
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            return false;
        }
    }
    return true;
}

// static
void SubsonicDAO::dropTables(const QSqlDatabase& database) {
    for (const QString& tableName : kTableNames) {
        QSqlQuery query(database);
        query.prepare("DROP TABLE IF EXISTS " + tableName);
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
        }
    }
}

// static
void SubsonicDAO::clearTables(const QSqlDatabase& database) {
    for (const QString& tableName : kTableNames) {
        QSqlQuery query(database);
        query.prepare("DELETE FROM " + tableName);
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
        }
    }
}

// static
int SubsonicDAO::trackCount(const QSqlDatabase& database) {
    QSqlQuery query(database);
    query.prepare("SELECT COUNT(*) FROM subsonic_library");
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

// static
QList<QPair<int, QString>> SubsonicDAO::allPlaylists(const QSqlDatabase& database) {
    QList<QPair<int, QString>> playlists;
    QSqlQuery query(database);
    query.prepare("SELECT id, name FROM subsonic_playlists ORDER BY name");
    if (!query.exec()) {
        return playlists;
    }
    while (query.next()) {
        playlists.append({query.value(0).toInt(), query.value(1).toString()});
    }
    return playlists;
}

void SubsonicDAO::initialize(const QSqlDatabase& database) {
    m_insertTrackQuery = QSqlQuery(database);
    m_insertTrackQuery.prepare(
            "INSERT INTO subsonic_library ("
            "    subsonic_id, artist, title, album, album_artist, genre,"
            "    grouping, year, tracknumber, location, comment, duration,"
            "    bitrate, bpm, rating, cover_art_id"
            ") VALUES ("
            "    :subsonic_id, :artist, :title, :album, :album_artist,"
            "    :genre, '', :year, :tracknumber, :location, '', :duration,"
            "    :bitrate, 0, 0, :cover_art_id"
            ")");

    m_insertPlaylistQuery = QSqlQuery(database);
    m_insertPlaylistQuery.prepare(
            "INSERT INTO subsonic_playlists (subsonic_id, name) "
            "VALUES (:subsonic_id, :name)");

    m_insertPlaylistTrackQuery = QSqlQuery(database);
    m_insertPlaylistTrackQuery.prepare(
            "INSERT INTO subsonic_playlist_tracks ("
            "    playlist_id, track_id, position"
            ") VALUES (:playlist_id, :track_id, :position)");

    m_updateCoverArtQuery = QSqlQuery(database);
    m_updateCoverArtQuery.prepare(
            "UPDATE subsonic_library SET"
            "    coverart_source = :coverart_source,"
            "    coverart_type = :coverart_type,"
            "    coverart_location = :coverart_location,"
            "    coverart_digest = :coverart_digest,"
            "    coverart_hash = :coverart_hash "
            "WHERE cover_art_id = :cover_art_id");
}

bool SubsonicDAO::importTrack(const SubsonicTrackRow& track) {
    QSqlQuery& query = m_insertTrackQuery;
    query.bindValue(":subsonic_id", track.subsonicId);
    query.bindValue(":artist", track.artist);
    query.bindValue(":title", track.title);
    query.bindValue(":album", track.album);
    query.bindValue(":album_artist", track.albumArtist);
    query.bindValue(":genre", track.genre);
    query.bindValue(":year", track.year);
    query.bindValue(":tracknumber", QString::number(track.trackNumber));
    query.bindValue(":location", track.location);
    query.bindValue(":duration", track.duration);
    query.bindValue(":bitrate", track.bitrate);
    query.bindValue(":cover_art_id", track.coverArtId);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    m_trackIdBySubsonicId[track.subsonicId] = query.lastInsertId().toInt();
    return true;
}

bool SubsonicDAO::updateCoverArt(const QString& coverArtId,
        const QString& coverLocation,
        const QByteArray& imageDigest,
        quint16 legacyHash) {
    QSqlQuery& query = m_updateCoverArtQuery;
    // Numeric values match CoverInfo::Source/Type (stored in the DB).
    query.bindValue(":coverart_source", 1); // CoverInfo::GUESSED
    query.bindValue(":coverart_type", 2);   // CoverInfo::FILE
    query.bindValue(":coverart_location", coverLocation);
    query.bindValue(":coverart_digest", imageDigest);
    query.bindValue(":coverart_hash", legacyHash);
    query.bindValue(":cover_art_id", coverArtId);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

int SubsonicDAO::importPlaylist(const QString& subsonicId, const QString& name) {
    QSqlQuery& query = m_insertPlaylistQuery;
    query.bindValue(":subsonic_id", subsonicId);
    query.bindValue(":name", name);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return -1;
    }
    return query.lastInsertId().toInt();
}

bool SubsonicDAO::importPlaylistTrack(
        int playlistId, const QString& subsonicTrackId, int position) {
    const auto it = m_trackIdBySubsonicId.constFind(subsonicTrackId);
    if (it == m_trackIdBySubsonicId.constEnd()) {
        // The playlist references a track that the album sweep did not
        // return (e.g. a podcast episode or a permission-filtered song).
        qInfo() << "Skipping unknown Subsonic track in playlist:"
                << subsonicTrackId;
        return false;
    }

    QSqlQuery& query = m_insertPlaylistTrackQuery;
    query.bindValue(":playlist_id", playlistId);
    query.bindValue(":track_id", it.value());
    query.bindValue(":position", position);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}
