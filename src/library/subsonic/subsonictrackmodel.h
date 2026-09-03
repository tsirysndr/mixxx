#pragma once

#include <QSharedPointer>

#include "library/baseexternaltrackmodel.h"

class SubsonicFeature;
class TrackCollectionManager;
class BaseTrackCache;

class SubsonicTrackModel : public BaseExternalTrackModel {
    Q_OBJECT
  public:
    SubsonicTrackModel(SubsonicFeature* pFeature,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);

    TrackPointer getTrack(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;

  protected:
    /// Maps "subsonic://track?id=...&suffix=..." to the local cache path.
    /// Pure string computation; the actual download happens in getTrack().
    QString resolveLocation(const QString& nativeLocation) const override;

  private:
    SubsonicFeature* m_pFeature;
};
