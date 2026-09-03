#pragma once

#include <QString>
#include <QTimer>
#include <memory>
#include <optional>

#include "preferences/usersettings.h"
#include "rocksky-bridge/rocksky_bridge.h"
#include "rust/cxx.h"
#include "track/track_decl.h"

class SubsonicFeature;
class TrackCollectionManager;

/// Rocksky (rocksky.app) integration: registers Mixxx as a remote player
/// (shown as "Mixxx — <current track title>" in the device pickers,
/// play/pause/next/seek remote control, Auto DJ queue advertised with
/// jump/remove/move mapped onto it) and autoscrobbles played tracks
/// using the Last.fm rule (half the track or 4 minutes, whichever is
/// less). Requires a token from `rocksky login` (~/.rocksky/token.json or
/// ROCKSKY_TOKEN); without one the service stays inactive.
class RockskyService : public QObject {
    Q_OBJECT
  public:
    RockskyService(UserSettingsPointer pConfig,
            TrackCollectionManager* pTrackCollectionManager,
            SubsonicFeature* pSubsonicFeature,
            QObject* pParent = nullptr);
    ~RockskyService() override;

    bool isActive() const {
        return m_pClient.has_value();
    }

    // Invoked (already marshalled to this object's thread) by the remote
    // command handler.
    void handlePlay(bool play);
    void handleNext();
    void handleSeek(uint64_t positionMs);
    void handleQueueJump(uint32_t index);
    void handleQueueRemove(uint32_t index);
    void handleQueueMove(uint32_t from, uint32_t to);
    /// items: (subsonic track id, title); mode: 0 = now, 1 = next, 2 = last.
    void handleEnqueue(
            const QList<QPair<QString, QString>>& items, int mode);

  private slots:
    void slotPlayingTrackChanged(TrackPointer pTrack);
    void slotTick();
    void slotAutoDJQueueChanged(int playlistId);
    void slotPushQueue();

  private:
    void resetWatcher(const TrackPointer& pTrack);
    static QString trackKey(const TrackPointer& pTrack);

    UserSettingsPointer m_pConfig;
    TrackCollectionManager* const m_pTrackCollectionManager;
    // The Subsonic feature resolves enqueued server track ids into
    // playable (downloadable) tracks; nullptr when Subsonic is disabled.
    SubsonicFeature* const m_pSubsonicFeature;
    std::optional<rust::Box<rocksky::Client>> m_pClient;
    QTimer m_timer;

    // Auto DJ queue mirroring (advertised over the remote protocol,
    // rebuilt debounced on playlist changes).
    int m_autoDjPlaylistId = -1;
    QTimer m_queueDebounce;
    // The advertised queue is [session history..., currently playing,
    // upcoming playlist entries...] with index = history size, because
    // controllers derive their History tab from the entries before the
    // index and Auto DJ removes tracks from the playlist once loaded.
    // Incoming queue-command indices are translated by this offset
    // (= number of advertised entries preceding the playlist entries).
    int m_queueHeadOffset = 0;

    struct PlayedEntry {
        QString title;
        QString artist;
        QString album;
        QString albumArtist;
        quint64 durationMs = 0;
        int trackNumber = 0;
    };
    // Tracks of this session that crossed the scrobble threshold.
    QList<PlayedEntry> m_playedHistory;
    // Metadata of the currently watched track (for the history append
    // when the next track takes over).
    PlayedEntry m_watchedInfo;

    // Scrobble watcher (playerd rules): one submission per play, keyed on
    // the track, threshold = min(duration / 2, 4 minutes) of elapsed
    // position, timestamp = wall clock at play start.
    QString m_watchedKey;
    qint64 m_playStartUnixSeconds = 0;
    bool m_scrobbleSubmitted = false;
    bool m_wasPlaying = false;
};
