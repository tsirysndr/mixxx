#include "library/subsonic/subsonicplaylistmodel.h"

#include <QFileInfo>

#include "library/subsonic/subsonicfeature.h"
#include "moc_subsonicplaylistmodel.cpp"

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
        // prepareTrackLoad().
        return TrackPointer();
    }
    return BaseExternalPlaylistModel::getTrack(index);
}

bool SubsonicPlaylistModel::prepareTrackLoad(const QModelIndex& index) {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    return !m_pFeature->requestTrackDownload(nativeLocation).isEmpty();
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
