#pragma once

#include <QFuture>
#include <QFutureWatcher>
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
#include "subsonic-bridge/bridge.h"
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
    /// returns an empty string, and loads the track into a deck when the
    /// most recently requested download finishes.
    QString requestTrackDownload(const QString& nativeLocation);
    /// Pure string computation mapping a subsonic:// location to its cache
    /// file path (no I/O, no network). Thread-safe.
    QString cachePathForLocation(const QString& nativeLocation) const;

    static QString makeLocation(const QString& trackId, const QString& suffix);
    static bool parseLocation(
            const QString& nativeLocation, QString* pTrackId, QString* pSuffix);

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
    /// Worker thread: fetches cover art for the track into the cache.
    QString ensureCoverDownloaded(const QString& trackId);
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
};
