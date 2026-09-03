#include "library/subsonic/subsonictrackmodel.h"

#include <QFileInfo>

#include "library/subsonic/subsonicfeature.h"
#include "moc_subsonictrackmodel.cpp"
#include "track/track.h"

SubsonicTrackModel::SubsonicTrackModel(SubsonicFeature* pFeature,
        TrackCollectionManager* pTrackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalTrackModel(pFeature,
                  pTrackCollectionManager,
                  "mixxx.db.model.subsonic",
                  "subsonic_library",
                  trackSource),
          m_pFeature(pFeature) {
}

TrackPointer SubsonicTrackModel::getTrack(const QModelIndex& index) const {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    const QString cachePath = m_pFeature->cachePathForLocation(nativeLocation);
    if (cachePath.isEmpty() || !QFileInfo::exists(cachePath)) {
        // Deliberately side-effect-free: this is also called from
        // selection/display and context-menu paths. Downloads are
        // triggered exclusively through prepareTrackLoad(). A temporary
        // (not-in-library) track carries the metadata so menus and
        // widgets can render; players never receive it because the load
        // paths are gated on prepareTrackLoad().
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
    return BaseExternalTrackModel::getTrack(index);
}

bool SubsonicTrackModel::prepareTrackLoad(
        const QModelIndex& index, const QString& group) {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    // Cached: proceed with the regular load. Otherwise a background
    // download starts and the feature loads the track (into `group` if
    // given) when it finishes.
    return !m_pFeature->requestTrackDownload(nativeLocation, group).isEmpty();
}

TrackId SubsonicTrackModel::getTrackId(const QModelIndex& index) const {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    const QString cachePath = m_pFeature->cachePathForLocation(nativeLocation);
    if (cachePath.isEmpty() || !QFileInfo::exists(cachePath)) {
        // Selection and hover paths call this; they must never trigger a
        // download or any other slow work.
        return TrackId();
    }
    return BaseExternalTrackModel::getTrackId(index);
}

QString SubsonicTrackModel::resolveLocation(const QString& nativeLocation) const {
    return m_pFeature->cachePathForLocation(nativeLocation);
}

bool SubsonicTrackModel::addSelectionToAutoDJ(
        const QModelIndexList& indices, AutoDJLocation loc) {
    // The generic path needs valid library track ids, which undownloaded
    // tracks do not have. Stream the selection through the feature's
    // download-and-enqueue pipeline instead (cached tracks pass through
    // it instantly).
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
