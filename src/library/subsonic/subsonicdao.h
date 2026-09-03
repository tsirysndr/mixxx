#pragma once

#include <QHash>
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
};

/// Wrapper around the runtime-created Subsonic tables in the Mixxx
/// database. Unlike the iTunes tables these are not part of schema.xml;
/// they are created on demand (Rekordbox pattern).
class SubsonicDAO {
  public:
    static bool createTables(const QSqlDatabase& database);
    static void clearTables(const QSqlDatabase& database);

    void initialize(const QSqlDatabase& database);

    bool importTrack(const SubsonicTrackRow& track);
    /// Returns the database id of the inserted playlist or -1 on failure.
    int importPlaylist(const QString& subsonicId, const QString& name);
    bool importPlaylistTrack(
            int playlistId, const QString& subsonicTrackId, int position);

  private:
    QHash<QString, int> m_trackIdBySubsonicId;

    // These queries reference the database, which must outlive the DAO.
    QSqlQuery m_insertTrackQuery;
    QSqlQuery m_insertPlaylistQuery;
    QSqlQuery m_insertPlaylistTrackQuery;
};
