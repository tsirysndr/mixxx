#include "rocksky/rockskyservice.h"

#include <QDateTime>
#include <QPointer>
#include <QSysInfo>
#include <QtDebug>
#include <algorithm>

#include "control/controlobject.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#ifdef __SUBSONIC__
#include "library/subsonic/subsonicfeature.h"
#endif
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_rockskyservice.cpp"
#include "rocksky/rockskycommandhandler.h"
#include "track/track.h"

namespace {

const QString kConfigGroup = QStringLiteral("[Rocksky]");

constexpr double kScrobbleCapSeconds = 4.0 * 60.0;

QString fromRust(const rust::String& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

/// Bridges Rust-thread command callbacks onto the service's thread.
class ServiceCommandHandler : public rocksky::CommandHandler {
  public:
    explicit ServiceCommandHandler(RockskyService* pService)
            : m_pService(pService) {
    }

    void onPlay() override {
        invoke([](RockskyService* pService) { pService->handlePlay(true); });
    }
    void onPause() override {
        invoke([](RockskyService* pService) { pService->handlePlay(false); });
    }
    void onNext() override {
        invoke([](RockskyService* pService) { pService->handleNext(); });
    }
    void onPrevious() override {
        // A DJ deck has no meaningful "previous track"; ignore.
    }
    void onSeek(uint64_t positionMs) override {
        invoke([positionMs](RockskyService* pService) {
            pService->handleSeek(positionMs);
        });
    }
    void onQueueJump(uint32_t index) override {
        invoke([index](RockskyService* pService) {
            pService->handleQueueJump(index);
        });
    }
    void onQueueRemove(uint32_t index) override {
        invoke([index](RockskyService* pService) {
            pService->handleQueueRemove(index);
        });
    }
    void onQueueMove(uint32_t from, uint32_t to) override {
        invoke([from, to](RockskyService* pService) {
            pService->handleQueueMove(from, to);
        });
    }
    void onEnqueue(rust::Vec<rocksky::QueueItemData> items, uint8_t mode) override {
        // Convert to plain Qt types on this (Rust) thread; the queued
        // std::function must be copyable.
        QList<QPair<QString, QString>> tracks;
        tracks.reserve(static_cast<qsizetype>(items.size()));
        for (const rocksky::QueueItemData& item : items) {
            tracks.append({QString::fromUtf8(
                                   item.track_id.data(),
                                   static_cast<int>(item.track_id.size())),
                    QString::fromUtf8(item.title.data(),
                            static_cast<int>(item.title.size()))});
        }
        invoke([tracks, mode](RockskyService* pService) {
            pService->handleEnqueue(tracks, mode);
        });
    }

  private:
    void invoke(std::function<void(RockskyService*)> fn) {
        // Queued to the service's thread. m_pService cannot dangle here:
        // ~RockskyService joins the command loop before destruction.
        QPointer<RockskyService> pGuard(m_pService);
        QMetaObject::invokeMethod(
                m_pService,
                [pGuard, fn = std::move(fn)] {
                    if (pGuard) {
                        fn(pGuard.data());
                    }
                },
                Qt::QueuedConnection);
    }

    RockskyService* const m_pService;
};

} // anonymous namespace

RockskyService::RockskyService(UserSettingsPointer pConfig,
        TrackCollectionManager* pTrackCollectionManager,
        SubsonicFeature* pSubsonicFeature,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pSubsonicFeature(pSubsonicFeature) {
    if (!m_pConfig->getValue(
                ConfigKey(kConfigGroup, QStringLiteral("Enabled")), true)) {
        qInfo() << "Rocksky integration disabled by [Rocksky] Enabled";
        return;
    }
    QString token;
    try {
        token = fromRust(rocksky::resolve_token());
    } catch (const rust::Error& e) {
        qInfo() << "Rocksky integration inactive:" << e.what();
        return;
    }
    // The device pickers render "<clientName> — <current track title>"
    // from the live now-playing state, so the registered name stays plain.
    const QString deviceName = QStringLiteral("Mixxx");
    try {
        m_pClient.emplace(rocksky::new_client(token.toStdString()));
        rocksky::start_remote_player(**m_pClient,
                deviceName.toStdString(),
                std::unique_ptr<rocksky::CommandHandler>(
                        new ServiceCommandHandler(this)));
    } catch (const rust::Error& e) {
        qWarning() << "Failed to start Rocksky integration:" << e.what();
        m_pClient.reset();
        return;
    }

    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &RockskyService::slotPlayingTrackChanged);
    connect(&m_timer, &QTimer::timeout, this, &RockskyService::slotTick);
    m_timer.start(1000);

    // Mirror the Auto DJ queue over the remote protocol.
    PlaylistDAO& playlistDao = m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    m_autoDjPlaylistId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
    if (m_autoDjPlaylistId >= 0) {
        m_queueDebounce.setSingleShot(true);
        m_queueDebounce.setInterval(500);
        connect(&m_queueDebounce,
                &QTimer::timeout,
                this,
                &RockskyService::slotPushQueue);
        connect(&playlistDao,
                &PlaylistDAO::trackAdded,
                this,
                [this](int playlistId, TrackId, int) {
                    slotAutoDJQueueChanged(playlistId);
                });
        connect(&playlistDao,
                &PlaylistDAO::trackRemoved,
                this,
                [this](int playlistId, TrackId, int) {
                    slotAutoDJQueueChanged(playlistId);
                });
        connect(&playlistDao,
                &PlaylistDAO::playlistContentChanged,
                this,
                [this](const QSet<int>& playlistIds) {
                    if (playlistIds.contains(m_autoDjPlaylistId)) {
                        m_queueDebounce.start();
                    }
                });
        slotPushQueue();
    }
    qInfo() << "Rocksky integration active as" << deviceName;
}

void RockskyService::slotAutoDJQueueChanged(int playlistId) {
    if (playlistId == m_autoDjPlaylistId) {
        m_queueDebounce.start();
    }
}

void RockskyService::slotPushQueue() {
    if (!m_pClient || m_autoDjPlaylistId < 0) {
        return;
    }
    // Bounded: controllers only need the upcoming stretch of the queue.
    constexpr int kMaxQueueItems = 100;
    const QList<TrackId> trackIds =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO().getTrackIdsInPlaylistOrder(
                    m_autoDjPlaylistId);
    rust::Vec<rocksky::QueueItemData> items;
    items.reserve(std::min<qsizetype>(trackIds.size(), kMaxQueueItems));
    for (const TrackId& trackId : trackIds) {
        if (static_cast<int>(items.size()) >= kMaxQueueItems) {
            break;
        }
        const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(trackId);
        if (!pTrack) {
            continue;
        }
        items.push_back(rocksky::QueueItemData{
                std::string(), // track_id: unknown for local library rows
                pTrack->getTitle().toStdString(),
                pTrack->getArtist().toStdString(),
                pTrack->getAlbum().toStdString(),
                pTrack->getAlbumArtist().toStdString(),
                static_cast<uint64_t>(pTrack->getDuration() * 1000.0),
                pTrack->getTrackNumber().toInt(),
        });
    }
    rocksky::update_queue(**m_pClient, std::move(items), 0);
}

void RockskyService::handleQueueJump(uint32_t index) {
    if (m_autoDjPlaylistId < 0) {
        return;
    }
    // Make the chosen track the next one, then fade into it.
    if (index > 0) {
        m_pTrackCollectionManager->internalCollection()->getPlaylistDAO().moveTrack(
                m_autoDjPlaylistId, static_cast<int>(index) + 1, 1);
    }
    handleNext();
}

void RockskyService::handleQueueRemove(uint32_t index) {
    if (m_autoDjPlaylistId < 0) {
        return;
    }
    m_pTrackCollectionManager->internalCollection()->getPlaylistDAO().removeTrackFromPlaylist(
            m_autoDjPlaylistId, static_cast<int>(index) + 1);
}

void RockskyService::handleEnqueue(
        const QList<QPair<QString, QString>>& items, int mode) {
#ifdef __SUBSONIC__
    if (!m_pSubsonicFeature) {
        qWarning() << "Rocksky: enqueue ignored, Subsonic feature disabled";
        return;
    }
    QStringList locations;
    int skipped = 0;
    for (const auto& [trackId, title] : items) {
        const QString location =
                m_pSubsonicFeature->locationForSubsonicId(trackId);
        if (location.isEmpty()) {
            qInfo() << "Rocksky: cannot enqueue" << title
                    << "- not in the Subsonic library";
            skipped++;
            continue;
        }
        locations.append(location);
    }
    if (skipped > 0) {
        qWarning() << "Rocksky: skipped" << skipped
                   << "enqueued tracks with no Subsonic match";
    }
    if (locations.isEmpty()) {
        return;
    }
    if (mode == 0) {
        // "now": load the first track into the next available deck; the
        // rest becomes next-up in the Auto DJ queue.
        m_pSubsonicFeature->loadTrackByLocation(locations.takeFirst());
        if (!locations.isEmpty()) {
            m_pSubsonicFeature->enqueueLocationsToAutoDJ(
                    locations, PlaylistDAO::AutoDJSendLoc::TOP);
        }
    } else {
        m_pSubsonicFeature->enqueueLocationsToAutoDJ(locations,
                mode == 1 ? PlaylistDAO::AutoDJSendLoc::TOP
                          : PlaylistDAO::AutoDJSendLoc::BOTTOM);
    }
#else
    Q_UNUSED(items);
    Q_UNUSED(mode);
    qWarning() << "Rocksky: enqueue is unsupported without the Subsonic "
                  "integration";
#endif
}

void RockskyService::handleQueueMove(uint32_t from, uint32_t to) {
    if (m_autoDjPlaylistId < 0 || from == to) {
        return;
    }
    m_pTrackCollectionManager->internalCollection()->getPlaylistDAO().moveTrack(m_autoDjPlaylistId,
            static_cast<int>(from) + 1,
            static_cast<int>(to) + 1);
}

RockskyService::~RockskyService() {
    if (m_pClient) {
        // Joins the command loop; no handler callbacks after this.
        rocksky::stop_remote_player(**m_pClient);
    }
}

// static
QString RockskyService::trackKey(const TrackPointer& pTrack) {
    return pTrack->getLocation();
}

void RockskyService::resetWatcher(const TrackPointer& pTrack) {
    m_watchedKey = trackKey(pTrack);
    m_playStartUnixSeconds = QDateTime::currentSecsSinceEpoch();
    m_scrobbleSubmitted = false;
}

void RockskyService::slotPlayingTrackChanged(TrackPointer pTrack) {
    // Fires repeatedly for the same track (deck flip-flops, crossfader
    // moves) and with null pointers; only an actual track change starts a
    // new play.
    if (!pTrack || trackKey(pTrack) == m_watchedKey) {
        return;
    }
    resetWatcher(pTrack);
}

void RockskyService::slotTick() {
    if (!m_pClient) {
        return;
    }
    PlayerInfo& playerInfo = PlayerInfo::instance();
    const TrackPointer pTrack = playerInfo.getCurrentPlayingTrack();
    const int deck = playerInfo.getCurrentPlayingDeck();
    if (!pTrack || deck < 0) {
        if (m_wasPlaying) {
            rocksky::set_stopped(**m_pClient);
            m_wasPlaying = false;
        }
        return;
    }
    m_wasPlaying = true;
    if (trackKey(pTrack) != m_watchedKey) {
        // The timer can observe a new track before the signal does.
        resetWatcher(pTrack);
    }

    const QString group = PlayerManager::groupForDeck(deck);
    const double durationSeconds = pTrack->getDuration();
    const double playPosition = std::clamp(
            ControlObject::get(ConfigKey(group, QStringLiteral("playposition"))),
            0.0,
            1.0);
    const double elapsedSeconds = playPosition * durationSeconds;

    const QString title = pTrack->getTitle();
    const QString artist = pTrack->getArtist();

    rocksky::NowPlayingData nowPlaying{
            title.toStdString(),
            artist.toStdString(),
            pTrack->getAlbum().toStdString(),
            pTrack->getAlbumArtist().toStdString(),
            static_cast<uint64_t>(durationSeconds * 1000.0),
            static_cast<uint64_t>(elapsedSeconds * 1000.0),
            true,
            pTrack->getType().toLower().toStdString(),
            pTrack->getSampleRate() > 0
                    ? static_cast<uint32_t>(pTrack->getSampleRate())
                    : 0,
    };
    rocksky::update_now_playing(**m_pClient, nowPlaying);

    // Last.fm rule: scrobble once after half the track or 4 minutes.
    if (m_scrobbleSubmitted || durationSeconds <= 0 || title.isEmpty() ||
            artist.isEmpty()) {
        return;
    }
    const double threshold =
            std::min(durationSeconds / 2.0, kScrobbleCapSeconds);
    if (elapsedSeconds < threshold) {
        return;
    }
    rocksky::ScrobbleData scrobble{
            title.toStdString(),
            artist.toStdString(),
            pTrack->getAlbum().toStdString(),
            pTrack->getAlbumArtist().toStdString(),
            static_cast<uint64_t>(durationSeconds * 1000.0),
            m_playStartUnixSeconds,
            pTrack->getTrackNumber().toInt(),
            pTrack->getYear().toInt(),
    };
    rocksky::submit_scrobble(**m_pClient, scrobble);
    m_scrobbleSubmitted = true;
    qInfo() << "Rocksky: scrobbling" << artist << "-" << title;
}

void RockskyService::handlePlay(bool play) {
    int deck = PlayerInfo::instance().getCurrentPlayingDeck();
    if (deck < 0) {
        deck = 0;
    }
    ControlObject::set(
            ConfigKey(PlayerManager::groupForDeck(deck), QStringLiteral("play")),
            play ? 1.0 : 0.0);
}

void RockskyService::handleNext() {
    // "Next" in a DJ context: let Auto DJ fade to the next queued track.
    ControlObject::set(
            ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("fade_now")),
            1.0);
}

void RockskyService::handleSeek(uint64_t positionMs) {
    const int deck = PlayerInfo::instance().getCurrentPlayingDeck();
    if (deck < 0) {
        return;
    }
    const TrackPointer pTrack = PlayerInfo::instance().getCurrentPlayingTrack();
    if (!pTrack || pTrack->getDuration() <= 0) {
        return;
    }
    const double position = std::clamp(
            static_cast<double>(positionMs) / 1000.0 / pTrack->getDuration(),
            0.0,
            1.0);
    ControlObject::set(
            ConfigKey(PlayerManager::groupForDeck(deck),
                    QStringLiteral("playposition")),
            position);
}
