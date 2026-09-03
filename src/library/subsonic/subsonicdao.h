#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

/// A track row for the runtime-created subsonic_library table.
/// The location holds a "subsonic://track?id=...&suffix=..." URI that
/// SubsonicTrackModel/SubsonicPlaylistModel resolve to a cached local file.
struct SubsonicTrackRow {
    QString subsonicId;
    QString artist;
    QString title;
    QString album;
    QString albumArtist;
    QString genre;
    int year;
    int trackNumber;
    QString location;
    int duration;
    int bitrate;
    QString coverArtId;
};

/// Wrapper around the runtime-created Subsonic tables in the Mixxx
/// database. Unlike the iTunes tables these are not part of schema.xml;
/// they are created on demand (Rekordbox pattern).
class SubsonicDAO {
  public:
    static bool createTables(const QSqlDatabase& database);
    /// The tables hold transient data (rebuilt on every activation), so
    /// schema changes are handled by dropping and recreating them.
    static void dropTables(const QSqlDatabase& database);
    static void clearTables(const QSqlDatabase& database);
    /// Number of cached tracks, or 0 if the table is missing/empty.
    static int trackCount(const QSqlDatabase& database);
    /// Cached playlists as (database id, name), sorted by name.
    static QList<QPair<int, QString>> allPlaylists(const QSqlDatabase& database);

    void initialize(const QSqlDatabase& database);

    bool importTrack(const SubsonicTrackRow& track);
    /// Returns the database id of the inserted playlist or -1 on failure.
    int importPlaylist(const QString& subsonicId, const QString& name);
    bool importPlaylistTrack(
            int playlistId, const QString& subsonicTrackId, int position);
    /// Attaches a downloaded cover art file to every track that shares
    /// the given cover art id.
    bool updateCoverArt(const QString& coverArtId,
            const QString& coverLocation,
            const QByteArray& imageDigest,
            quint16 legacyHash);

  private:
    QHash<QString, int> m_trackIdBySubsonicId;

    // These queries reference the database, which must outlive the DAO.
    QSqlQuery m_insertTrackQuery;
    QSqlQuery m_insertPlaylistQuery;
    QSqlQuery m_insertPlaylistTrackQuery;
    QSqlQuery m_updateCoverArtQuery;
};
