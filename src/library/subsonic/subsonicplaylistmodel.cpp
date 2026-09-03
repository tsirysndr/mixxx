#include "library/subsonic/subsonicplaylistmodel.h"

#include <QFileInfo>

#include "library/subsonic/subsonicfeature.h"
#include "moc_subsonicplaylistmodel.cpp"
#include "track/track.h"

SubsonicPlaylistModel::SubsonicPlaylistModel(SubsonicFeature* pFeature,
        TrackCollectionManager* pTrackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(pFeature,
                  pTrackCollectionManager,
                  "mixxx.db.model.subsonic_playlist",
                  "subsonic_playlists",
                  "subsonic_playlist_tracks",
                  trackSource),
          m_pFeature(pFeature) {
}

TrackPointer SubsonicPlaylistModel::getTrack(const QModelIndex& index) const {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    const QString cachePath = m_pFeature->cachePathForLocation(nativeLocation);
    if (cachePath.isEmpty() || !QFileInfo::exists(cachePath)) {
        // Side-effect-free display path; downloads only start in
        // prepareTrackLoad(). See SubsonicTrackModel::getTrack.
        TrackPointer pTrack = Track::newTemporary(cachePath);
        pTrack->setArtist(getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_ARTIST));
        pTrack->setTitle(getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_TITLE));
        pTrack->setAlbum(getFieldString(index, ColumnCache::COLUMN_LIBRARYTABLE_ALBUM));
        pTrack->setAlbumArtist(getFieldString(
                index, ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST));
        pTrack->setDuration(
                getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_DURATION)
                        .toDouble());
        return pTrack;
    }
    return BaseExternalPlaylistModel::getTrack(index);
}

bool SubsonicPlaylistModel::prepareTrackLoad(
        const QModelIndex& index, const QString& group) {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    return !m_pFeature->requestTrackDownload(nativeLocation, group).isEmpty();
}

TrackId SubsonicPlaylistModel::getTrackId(const QModelIndex& index) const {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    const QString cachePath = m_pFeature->cachePathForLocation(nativeLocation);
    if (cachePath.isEmpty() || !QFileInfo::exists(cachePath)) {
        // Selection must never trigger network I/O.
        return TrackId();
    }
    return BaseExternalPlaylistModel::getTrackId(index);
}

QString SubsonicPlaylistModel::resolveLocation(const QString& nativeLocation) const {
    return m_pFeature->cachePathForLocation(nativeLocation);
}

bool SubsonicPlaylistModel::addSelectionToAutoDJ(
        const QModelIndexList& indices, AutoDJLocation loc) {
    // See SubsonicTrackModel::addSelectionToAutoDJ.
    QStringList locations;
    locations.reserve(indices.size());
    for (const QModelIndex& index : indices) {
        locations.append(index.sibling(index.row(), fieldIndex("location"))
                        .data()
                        .toString());
    }
    m_pFeature->enqueueLocationsToAutoDJ(locations,
            loc == AutoDJLocation::Top ? PlaylistDAO::AutoDJSendLoc::TOP
                    : loc == AutoDJLocation::Replace
                    ? PlaylistDAO::AutoDJSendLoc::REPLACE
                    : PlaylistDAO::AutoDJSendLoc::BOTTOM);
    return true;
}
