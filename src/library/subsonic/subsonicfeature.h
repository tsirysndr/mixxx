#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <memory>

#include "library/baseexternallibraryfeature.h"
#include "rust/cxx.h"
#include "subsonic-bridge/subsonic_bridge.h"
#include "util/parented_ptr.h"

class BaseTrackCache;
class SubsonicPlaylistModel;
class SubsonicTrackModel;
class TreeItem;
class TreeItemModel;
class WLibrarySidebar;

/// Browses a remote Subsonic/Navidrome server as an external library.
/// The protocol layer is implemented in Rust (lib/subsonic-rs) and
/// accessed through the cxx bridge in the subsonic namespace. Tracks are
/// downloaded into a local cache on first load into a deck.
class SubsonicFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    SubsonicFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SubsonicFeature() override;
    static bool isSupported() {
        return true;
    }

    QVariant title() override;
    void bindSidebarWidget(WLibrarySidebar* pSidebarWidget) override;
    TreeItemModel* sidebarModel() const override;

    /// Blocking: downloads the track behind the given
    /// "subsonic://track?id=...&suffix=..." location into the cache unless
    /// it is already cached. Returns the local file path or an empty
    /// string on failure. Thread-safe.
    QString ensureTrackDownloaded(const QString& nativeLocation);
    /// Non-blocking (UI thread): returns the cache path if the track is
    /// already downloaded. Otherwise schedules a background download,
    /// returns an empty string, and loads the track when the most
    /// recently requested download finishes — into `group` if given,
    /// else into the next available deck.
    QString requestTrackDownload(
            const QString& nativeLocation, const QString& group = QString());
    /// Pure string computation mapping a subsonic:// location to its cache
    /// file path (no I/O, no network). Thread-safe.
    QString cachePathForLocation(const QString& nativeLocation) const;

    static QString makeLocation(const QString& trackId, const QString& suffix);
    static bool parseLocation(
            const QString& nativeLocation, QString* pTrackId, QString* pSuffix);

    /// Looks up the subsonic:// location of an imported track by its
    /// server-side id. Empty if unknown.
    QString locationForSubsonicId(const QString& subsonicId) const;
    /// Streams the given locations into the Auto DJ queue (downloading
    /// with a small lookahead, appended in order).
    void enqueueLocationsToAutoDJ(
            const QStringList& locations, PlaylistDAO::AutoDJSendLoc loc);
    /// Loads the track into `group` (or the next available deck if
    /// empty), downloading first when needed.
    void loadTrackByLocation(
            const QString& nativeLocation, const QString& group = QString());

  public slots:
    void activate() override;
    void activate(bool forceReload);
    void activateChild(const QModelIndex& index) override;
    void onRightClick(const QPoint& globalPos) override;
    void onTrackCollectionLoaded();

  private:
    using ClientPtr = std::shared_ptr<rust::Box<subsonic::Client>>;

    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;
    /// Bulk path behind "Import as Playlist/Crate": downloads all playlist
    /// tracks (concurrently, with a cancellable progress dialog) so they
    /// can be registered in the Mixxx library.
    void appendTrackIdsFromRightClickIndex(
            QList<TrackId>* trackIds, QString* pPlaylist) override;
    /// Streaming path behind "Add to Auto DJ": tracks are downloaded with
    /// a small lookahead and appended to the Auto DJ queue in playlist
    /// order as soon as each one is available.
    void addToAutoDJ(PlaylistDAO::AutoDJSendLoc loc) override;
    /// Runs on a QtConcurrent worker thread.
    TreeItem* importLibrary();
    /// Returns a client for the configured server, creating it on first
    /// use or when the connection settings changed. Returns nullptr if no
    /// server is configured or the configuration is invalid. Thread-safe.
    ClientPtr ensureClient();
    bool isConfigured() const;
    /// Shows the modal connection dialog; returns true if accepted.
    bool showConnectionDialog();
    void cancelPendingImport();
    /// Starts the background import unless one is already running.
    void startImport();
    /// Populates the sidebar/track views from the cached tables of the
    /// previous import so the feature is browsable instantly.
    void showCachedLibrary();
    /// Worker thread: fetches cover art for the track into the cache.
    /// size > 0 requests a server-side scaled thumbnail; 0 = original.
    QString ensureCoverDownloaded(const QString& coverArtId, int size = 0);
    /// Ordered playlist track locations of the right-clicked sidebar item.
    QStringList playlistLocationsFromRightClickIndex(QString* pPlaylistName);
    void pumpAutoDJPipeline();
    void onAutoDJTrackReady(
            int generation, int sequence, const QString& cachePath);
    /// UI thread: finalizes an async download (cover art, deck load).
    void onTrackDownloadFinished(const QString& nativeLocation,
            const QString& cachePath,
            const QString& coverPath);

    SubsonicTrackModel* m_pTrackModel;
    SubsonicPlaylistModel* m_pPlaylistModel;
    parented_ptr<TreeItemModel> m_pSidebarModel;
    QSharedPointer<BaseTrackCache> m_trackSource;

    // A separate DB connection for the import worker thread.
    QSqlDatabase m_database;
    std::atomic<bool> m_cancelImport;
    bool m_isActivated;
    QString m_title;
    // Written by the worker thread, read after the future has finished.
    QString m_lastImportError;

    QString m_cacheDir;

    // Deferred cover-thumbnail fetching: the import commits and shows the
    // library first, covers arrive in a second background pass.
    struct CoverFetchResult {
        QString coverArtId;
        QString thumbPath;
        QByteArray imageDigest;
        quint16 legacyHash;
    };
    void startCoverFetch(const QStringList& coverArtIds);
    void applyCoverResults(int generation, const QList<CoverFetchResult>& results);
    // Written by the import worker, consumed on the UI thread after the
    // import future has finished (sequenced by future completion).
    QStringList m_pendingCoverArtIds;
    QFuture<void> m_coverFuture;
    // Bumped on the UI thread, checked by the cover worker thread.
    std::atomic<int> m_coverGeneration{0};

    mutable QMutex m_clientMutex;
    ClientPtr m_pClient;
    QString m_clientConfigId;

    QFutureWatcher<TreeItem*> m_futureWatcher;
    QFuture<TreeItem*> m_future;
    QPointer<WLibrarySidebar> m_pSidebarWidget;

    // Async track downloads (deck loading). UI-thread state except the
    // pool's worker lambdas.
    QThreadPool m_downloadPool;
    QSet<QString> m_pendingDownloads;
    QString m_autoLoadLocation;
    QString m_autoLoadGroup;

    // Streaming "Add to Auto DJ" pipeline (UI-thread state). Downloads
    // run with a small lookahead; completed tracks are appended in
    // playlist order. A generation bump abandons a superseded pipeline.
    struct AutoDJPipeline {
        int generation = 0;
        QStringList locations;
        int nextToDownload = 0;
        int nextToEnqueue = 0;
        QHash<int, QString> finishedDownloads;
        PlaylistDAO::AutoDJSendLoc sendLoc = PlaylistDAO::AutoDJSendLoc::BOTTOM;
        int topInsertPosition = 0;
        bool firstEnqueued = false;
    };
    AutoDJPipeline m_autoDJ;
};
