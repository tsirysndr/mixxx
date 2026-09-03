#pragma once

#include <QSharedPointer>

#include "library/baseexternalplaylistmodel.h"

class SubsonicFeature;
class TrackCollectionManager;
class BaseTrackCache;

class SubsonicPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    SubsonicPlaylistModel(SubsonicFeature* pFeature,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);

    TrackPointer getTrack(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    bool prepareTrackLoad(const QModelIndex& index) override;

  protected:
    QString resolveLocation(const QString& nativeLocation) const override;

  private:
    SubsonicFeature* m_pFeature;
};
