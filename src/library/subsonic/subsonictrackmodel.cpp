#include "library/subsonic/subsonictrackmodel.h"

#include <QFileInfo>

#include "library/subsonic/subsonicfeature.h"
#include "moc_subsonictrackmodel.cpp"

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
        // selection/display paths. Downloads are triggered exclusively
        // through prepareTrackLoad().
        return TrackPointer();
    }
    return BaseExternalTrackModel::getTrack(index);
}

bool SubsonicTrackModel::prepareTrackLoad(const QModelIndex& index) {
    const QString nativeLocation =
            index.sibling(index.row(), fieldIndex("location"))
                    .data()
                    .toString();
    // Cached: proceed with the regular load. Otherwise a background
    // download starts and the feature loads the track when it finishes.
    return !m_pFeature->requestTrackDownload(nativeLocation).isEmpty();
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
