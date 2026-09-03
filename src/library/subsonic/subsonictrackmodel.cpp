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
    if (m_pFeature->requestTrackDownload(nativeLocation).isEmpty()) {
        // Not cached yet: the download runs in the background and the
        // track is loaded into a deck when it finishes. Never block the
        // UI thread on the network here.
        return TrackPointer();
    }
    return BaseExternalTrackModel::getTrack(index);
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
