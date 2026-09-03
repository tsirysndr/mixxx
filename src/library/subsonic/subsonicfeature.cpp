#include "library/subsonic/subsonicfeature.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QMenu>
#include <QProgressDialog>
#include <QMessageBox>
#include <QSqlError>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrentRun>
#include <QtDebug>

#include "library/basesqltablemodel.h"
#include "library/basetrackcache.h"
#include "library/coverart.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/subsonic/dlgsubsonicconnection.h"
#include "library/subsonic/subsoniccredentials.h"
#include "library/subsonic/subsonicdao.h"
#include "library/subsonic/subsonicimportprogress.h"
#include "library/dao/trackschema.h"
#include "library/subsonic/subsonicplaylistmodel.h"
#include "library/subsonic/subsonictrackmodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "moc_subsonicfeature.cpp"
#include "track/track.h"
#include "track/trackref.h"
#include "widget/wlibrarysidebar.h"

namespace {

const QString kConfigGroup = QStringLiteral("[Subsonic]");
const QString kHostKey = QStringLiteral("Host");
const QString kUsernameKey = QStringLiteral("Username");
const QString kPasswordKey = QStringLiteral("Password");
const QString kVerifyTlsKey = QStringLiteral("VerifyTls");

// Server-side scaled cover size for the track browser column.
constexpr int kCoverThumbnailSize = 256;

QString fromRust(const rust::String& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

} // anonymous namespace

SubsonicFeature::SubsonicFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("subsonic")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_cancelImport(false),
          m_isActivated(false) {
    m_database = QSqlDatabase::cloneDatabase(
            pLibrary->trackCollectionManager()->internalCollection()->database(),
            "SUBSONIC_SCANNER");
    if (!m_database.open()) {
        qWarning() << "Failed to open database for Subsonic scanner:"
                   << m_database.lastError();
    }
    // The tables must exist before the models create their views over
    // them. They persist across sessions so the previous import can be
    // shown instantly; drop them only when their schema changed.
    constexpr int kSchemaVersion = 2;
    const ConfigKey schemaVersionKey(kConfigGroup, QStringLiteral("SchemaVersion"));
    if (m_pConfig->getValue(schemaVersionKey, 0) != kSchemaVersion) {
        SubsonicDAO::dropTables(m_database);
        m_pConfig->setValue(schemaVersionKey, kSchemaVersion);
    }
    SubsonicDAO::createTables(m_database);

    QString tableName = "subsonic_library";
    QString idColumn = "id";
    QStringList columns = {
            "id",
            "artist",
            "title",
            "album",
            "album_artist",
            "year",
            "genre",
            "grouping",
            "tracknumber",
            "location",
            "comment",
            "duration",
            "bitrate",
            "bpm",
            "rating",
            "coverart_source",
            "coverart_type",
            "coverart_location",
            "coverart_color",
            "coverart_digest",
            "coverart_hash"};
    QStringList searchColumns = {
            "artist",
            "album",
            "album_artist",
            "title",
            "genre"};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            pLibrary->trackCollectionManager()->internalCollection(),
            std::move(tableName),
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pTrackModel = new SubsonicTrackModel(
            this, pLibrary->trackCollectionManager(), m_trackSource);
    m_pPlaylistModel = new SubsonicPlaylistModel(
            this, pLibrary->trackCollectionManager(), m_trackSource);
    m_title = tr("Subsonic");

    m_cacheDir =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/subsonic");
    QDir().mkpath(m_cacheDir);

    // Keep deck-load downloads off the shared global pool so a running
    // library import cannot starve them (and vice versa).
    m_downloadPool.setMaxThreadCount(2);

    connect(&m_futureWatcher,
            &QFutureWatcher<TreeItem*>::finished,
            this,
            &SubsonicFeature::onTrackCollectionLoaded);

    m_pTrackModel->setSearch(""); // enable search.
}

SubsonicFeature::~SubsonicFeature() {
    cancelPendingImport();
    m_coverGeneration++;
    m_coverFuture.waitForFinished();
    m_downloadPool.clear();
    m_downloadPool.waitForDone();
    m_database.close();
    delete m_pTrackModel;
    delete m_pPlaylistModel;
}

QVariant SubsonicFeature::title() {
    return m_title;
}

void SubsonicFeature::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    m_pSidebarWidget = pSidebarWidget;
    BaseExternalLibraryFeature::bindSidebarWidget(pSidebarWidget);
}

TreeItemModel* SubsonicFeature::sidebarModel() const {
    return m_pSidebarModel;
}

bool SubsonicFeature::isConfigured() const {
    return !m_pConfig->getValue(ConfigKey(kConfigGroup, kHostKey), QString())
                    .isEmpty();
}

bool SubsonicFeature::showConnectionDialog() {
    DlgSubsonicConnection dialog(m_pConfig);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    // Connection settings may have changed; drop the cached client.
    QMutexLocker locker(&m_clientMutex);
    m_pClient.reset();
    m_clientConfigId.clear();
    return true;
}

void SubsonicFeature::activate() {
    activate(false);
    emit enableCoverArtDisplay(true);
}

void SubsonicFeature::activate(bool forceReload) {
    if (!isConfigured()) {
        if (!showConnectionDialog()) {
            return;
        }
        forceReload = true;
    }

    if (!m_isActivated) {
        m_isActivated = true;
        // Show the previous import instantly (if any); fresh data
        // replaces it silently when the background refresh finishes.
        showCachedLibrary();
        startImport();
    } else if (forceReload) {
        startImport();
    }
    emit showTrackModel(m_pTrackModel);
    emit enableCoverArtDisplay(true);
}

void SubsonicFeature::showCachedLibrary() {
    if (SubsonicDAO::trackCount(m_database) <= 0) {
        return;
    }
    m_trackSource->buildIndex();
    auto pRoot = TreeItem::newRoot(this);
    const auto playlists = SubsonicDAO::allPlaylists(m_database);
    for (const auto& [playlistId, name] : playlists) {
        pRoot->appendChild(name, playlistId);
    }
    m_pSidebarModel->setRootItem(std::move(pRoot));
}

void SubsonicFeature::startImport() {
    if (m_future.isRunning()) {
        // A refresh is already underway; never block the UI waiting.
        return;
    }
    // Abandon any running cover pass: it would race the import worker on
    // the database connection, and the new import restarts it anyway.
    m_coverGeneration++;
    m_lastImportError.clear();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_future = QtConcurrent::run(&SubsonicFeature::importLibrary, this);
#else
    m_future = QtConcurrent::run(this, &SubsonicFeature::importLibrary);
#endif
    m_futureWatcher.setFuture(m_future);
    m_title = tr("(loading) Subsonic");
    emit featureIsLoading(this, true);
}

void SubsonicFeature::activateChild(const QModelIndex& index) {
    TreeItem* pTreeItem = static_cast<TreeItem*>(index.internalPointer());
    const int playlistId = pTreeItem->getData().toInt();
    m_pPlaylistModel->setPlaylistById(playlistId);
    emit showTrackModel(m_pPlaylistModel);
    emit enableCoverArtDisplay(true);
}

void SubsonicFeature::onRightClick(const QPoint& globalPos) {
    BaseExternalLibraryFeature::onRightClick(globalPos);
    QMenu menu(m_pSidebarWidget);
    QAction refresh(tr("Refresh Library"), &menu);
    QAction settings(tr("Connection Settings..."), &menu);
    menu.addAction(&refresh);
    menu.addAction(&settings);
    QAction* chosen = menu.exec(globalPos);
    if (chosen == &refresh) {
        activate(true);
    } else if (chosen == &settings) {
        if (showConnectionDialog()) {
            activate(true);
        }
    }
}

std::unique_ptr<BaseSqlTableModel>
SubsonicFeature::createPlaylistModelForPlaylist(const QVariant& data) {
    bool ok = false;
    const int playlistId = data.toInt(&ok);
    VERIFY_OR_DEBUG_ASSERT(ok) {
        return {};
    }
    auto pModel = std::make_unique<SubsonicPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    pModel->setPlaylistById(playlistId);
    return pModel;
}

QStringList SubsonicFeature::playlistLocationsFromRightClickIndex(
        QString* pPlaylistName) {
    const QModelIndex index = lastRightClickedIndex();
    if (!index.isValid()) {
        return {};
    }
    const auto* pTreeItem = static_cast<TreeItem*>(index.internalPointer());
    VERIFY_OR_DEBUG_ASSERT(pTreeItem) {
        return {};
    }
    if (pPlaylistName) {
        *pPlaylistName = pTreeItem->getLabel();
    }
    const std::unique_ptr<BaseSqlTableModel> pPlaylistModel =
            createPlaylistModelForPlaylist(pTreeItem->getData());
    if (!pPlaylistModel || !pPlaylistModel->initialized()) {
        return {};
    }
    pPlaylistModel->setSort(
            pPlaylistModel->fieldIndex(
                    ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION),
            Qt::AscendingOrder);
    pPlaylistModel->select();

    QStringList locations;
    const int rows = pPlaylistModel->rowCount();
    for (int i = 0; i < rows; ++i) {
        const QModelIndex rowIndex = pPlaylistModel->index(
                i, pPlaylistModel->fieldIndex("location"));
        locations.append(rowIndex.data().toString());
    }
    return locations;
}

void SubsonicFeature::appendTrackIdsFromRightClickIndex(
        QList<TrackId>* trackIds, QString* pPlaylist) {
    const QStringList locations = playlistLocationsFromRightClickIndex(pPlaylist);
    if (locations.isEmpty()) {
        return;
    }

    // Download everything up front (the tracks must exist locally to be
    // registered in the Mixxx library). Downloads run concurrently on the
    // pool; the modal progress dialog keeps the UI alive and cancellable.
    QList<QFuture<QString>> futures;
    futures.reserve(locations.size());
    for (const QString& location : locations) {
        futures.append(QtConcurrent::run(&m_downloadPool, [this, location] {
            return ensureTrackDownloaded(location);
        }));
    }

    QProgressDialog progressDialog(
            tr("Downloading %1 tracks from Subsonic...").arg(locations.size()),
            tr("Cancel"),
            0,
            locations.size(),
            nullptr);
    progressDialog.setWindowModality(Qt::ApplicationModal);
    progressDialog.setMinimumDuration(0);

    for (int i = 0; i < futures.size(); ++i) {
        while (!futures[i].isFinished() && !progressDialog.wasCanceled()) {
            QCoreApplication::processEvents(
                    QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
        }
        if (progressDialog.wasCanceled()) {
            // Already running downloads finish in the background and just
            // warm the cache; nothing gets added.
            trackIds->clear();
            return;
        }
        progressDialog.setValue(i + 1);
        const QString cachePath = futures[i].result();
        if (cachePath.isEmpty()) {
            qWarning() << "Skipping Subsonic track that failed to download:"
                       << locations[i];
            continue;
        }
        const TrackPointer pTrack =
                m_pLibrary->trackCollectionManager()->getOrAddTrack(
                        TrackRef::fromFilePath(cachePath));
        if (pTrack) {
            trackIds->append(pTrack->getId());
        }
    }
}

void SubsonicFeature::addToAutoDJ(PlaylistDAO::AutoDJSendLoc loc) {
    QString playlistName;
    const QStringList locations =
            playlistLocationsFromRightClickIndex(&playlistName);
    if (locations.isEmpty()) {
        return;
    }
    // Supersede any previous pipeline and stream this playlist: download
    // with a small lookahead, append each track as soon as it is ready.
    m_autoDJ.generation++;
    m_autoDJ.locations = locations;
    m_autoDJ.nextToDownload = 0;
    m_autoDJ.nextToEnqueue = 0;
    m_autoDJ.finishedDownloads.clear();
    m_autoDJ.sendLoc = loc;
    m_autoDJ.firstEnqueued = false;
    // Approximation of PlaylistDAO's TOP insert position (1 or 2
    // depending on Auto DJ state, which is not visible from here);
    // inserting at 2 keeps the streamed block contiguous and in order.
    m_autoDJ.topInsertPosition = 2;
    qInfo() << "Streaming" << locations.size()
            << "Subsonic tracks from playlist" << playlistName
            << "into the Auto DJ queue";
    pumpAutoDJPipeline();
}

void SubsonicFeature::pumpAutoDJPipeline() {
    constexpr int kLookahead = 2;
    const int inFlight = m_autoDJ.nextToDownload - m_autoDJ.nextToEnqueue -
            static_cast<int>(m_autoDJ.finishedDownloads.size());
    int freeSlots = kLookahead - inFlight;
    while (freeSlots-- > 0 &&
            m_autoDJ.nextToDownload < m_autoDJ.locations.size()) {
        const int sequence = m_autoDJ.nextToDownload++;
        const QString location = m_autoDJ.locations[sequence];
        const int generation = m_autoDJ.generation;
        QtConcurrent::run(&m_downloadPool, [this, generation, sequence, location] {
            const QString path = ensureTrackDownloaded(location);
            QMetaObject::invokeMethod(
                    this,
                    [this, generation, sequence, path] {
                        onAutoDJTrackReady(generation, sequence, path);
                    },
                    Qt::QueuedConnection);
        });
    }
}

void SubsonicFeature::onAutoDJTrackReady(
        int generation, int sequence, const QString& cachePath) {
    if (generation != m_autoDJ.generation) {
        // A newer pipeline superseded this one; the download still warmed
        // the cache.
        return;
    }
    m_autoDJ.finishedDownloads.insert(sequence, cachePath);
    PlaylistDAO& playlistDao = m_pTrackCollection->getPlaylistDAO();
    // Enqueue every completed track whose predecessors are all handled,
    // so the Auto DJ queue always grows in playlist order.
    while (m_autoDJ.finishedDownloads.contains(m_autoDJ.nextToEnqueue)) {
        const QString path =
                m_autoDJ.finishedDownloads.take(m_autoDJ.nextToEnqueue);
        m_autoDJ.nextToEnqueue++;
        if (path.isEmpty()) {
            qWarning() << "Skipping Subsonic Auto DJ track that failed to download";
            continue;
        }
        const TrackPointer pTrack =
                m_pLibrary->trackCollectionManager()->getOrAddTrack(
                        TrackRef::fromFilePath(path));
        if (!pTrack) {
            continue;
        }
        const QList<TrackId> trackIds{pTrack->getId()};
        switch (m_autoDJ.sendLoc) {
        case PlaylistDAO::AutoDJSendLoc::TOP: {
            const int autoDjId =
                    playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
            if (autoDjId >= 0) {
                playlistDao.insertTracksIntoPlaylist(
                        trackIds, autoDjId, m_autoDJ.topInsertPosition++);
            }
            break;
        }
        case PlaylistDAO::AutoDJSendLoc::REPLACE:
            playlistDao.addTracksToAutoDJQueue(trackIds,
                    m_autoDJ.firstEnqueued
                            ? PlaylistDAO::AutoDJSendLoc::BOTTOM
                            : PlaylistDAO::AutoDJSendLoc::REPLACE);
            break;
        case PlaylistDAO::AutoDJSendLoc::BOTTOM:
            playlistDao.addTracksToAutoDJQueue(
                    trackIds, PlaylistDAO::AutoDJSendLoc::BOTTOM);
            break;
        }
        m_autoDJ.firstEnqueued = true;
    }
    pumpAutoDJPipeline();
}

// static
QString SubsonicFeature::makeLocation(
        const QString& trackId, const QString& suffix) {
    QUrl url;
    url.setScheme(QStringLiteral("subsonic"));
    url.setHost(QStringLiteral("track"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), trackId);
    query.addQueryItem(QStringLiteral("suffix"), suffix);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

// static
bool SubsonicFeature::parseLocation(
        const QString& nativeLocation, QString* pTrackId, QString* pSuffix) {
    const QUrl url(nativeLocation);
    if (url.scheme() != QStringLiteral("subsonic")) {
        return false;
    }
    const QUrlQuery query(url);
    *pTrackId = query.queryItemValue(QStringLiteral("id"), QUrl::FullyDecoded);
    *pSuffix = query.queryItemValue(QStringLiteral("suffix"), QUrl::FullyDecoded);
    return !pTrackId->isEmpty();
}

QString SubsonicFeature::cachePathForLocation(
        const QString& nativeLocation) const {
    QString trackId;
    QString suffix;
    if (!parseLocation(nativeLocation, &trackId, &suffix)) {
        return {};
    }
    const rust::String fileName = subsonic::cache_file_name(
            trackId.toStdString(), suffix.toStdString());
    return m_cacheDir + QChar('/') + fromRust(fileName);
}

QString SubsonicFeature::ensureTrackDownloaded(const QString& nativeLocation) {
    QString trackId;
    QString suffix;
    if (!parseLocation(nativeLocation, &trackId, &suffix)) {
        return {};
    }
    const QString cachePath = cachePathForLocation(nativeLocation);
    if (QFileInfo::exists(cachePath)) {
        return cachePath;
    }
    ClientPtr pClient = ensureClient();
    if (!pClient) {
        return {};
    }
    try {
        const rust::String path = subsonic::download_track(
                **pClient,
                trackId.toStdString(),
                suffix.toStdString(),
                m_cacheDir.toStdString());
        return fromRust(path);
    } catch (const rust::Error& e) {
        qWarning() << "Failed to download Subsonic track" << trackId << ":"
                   << e.what();
        return {};
    }
}

QString SubsonicFeature::requestTrackDownload(const QString& nativeLocation) {
    const QString cachePath = cachePathForLocation(nativeLocation);
    if (cachePath.isEmpty()) {
        return {};
    }
    if (QFileInfo::exists(cachePath)) {
        return cachePath;
    }
    // Only the most recently requested track is loaded into a deck when
    // its download finishes; bulk operations just warm the cache.
    m_autoLoadLocation = nativeLocation;
    if (m_pendingDownloads.contains(nativeLocation)) {
        return {};
    }
    m_pendingDownloads.insert(nativeLocation);
    QtConcurrent::run(&m_downloadPool, [this, nativeLocation] {
        const QString path = ensureTrackDownloaded(nativeLocation);
        QString coverPath;
        QString trackId;
        QString suffix;
        if (!path.isEmpty() && parseLocation(nativeLocation, &trackId, &suffix)) {
            coverPath = ensureCoverDownloaded(trackId);
        }
        QMetaObject::invokeMethod(
                this,
                [this, nativeLocation, path, coverPath] {
                    onTrackDownloadFinished(nativeLocation, path, coverPath);
                },
                Qt::QueuedConnection);
    });
    return {};
}

QString SubsonicFeature::ensureCoverDownloaded(
        const QString& coverArtId, int size) {
    ClientPtr pClient = ensureClient();
    if (!pClient) {
        return {};
    }
    try {
        // Subsonic servers resolve cover art for a song id directly.
        const rust::String path = subsonic::download_cover_art(
                **pClient,
                coverArtId.toStdString(),
                m_cacheDir.toStdString(),
                static_cast<uint32_t>(size));
        return fromRust(path);
    } catch (const rust::Error& e) {
        qInfo() << "No Subsonic cover art for" << coverArtId << ":" << e.what();
        return {};
    }
}

void SubsonicFeature::onTrackDownloadFinished(const QString& nativeLocation,
        const QString& cachePath,
        const QString& coverPath) {
    m_pendingDownloads.remove(nativeLocation);
    if (cachePath.isEmpty()) {
        qWarning() << "Subsonic track download failed for" << nativeLocation;
        if (m_autoLoadLocation == nativeLocation) {
            m_autoLoadLocation.clear();
        }
        return;
    }
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getOrAddTrack(
            TrackRef::fromFilePath(cachePath));
    if (!pTrack) {
        return;
    }
    if (!coverPath.isEmpty() &&
            pTrack->getCoverInfo().type == CoverInfo::NONE) {
        const QImage image(coverPath);
        if (!image.isNull()) {
            CoverInfoRelative coverInfo;
            coverInfo.type = CoverInfo::FILE;
            coverInfo.source = CoverInfo::USER_SELECTED;
            // Relative to the track's folder; track and cover share the
            // cache directory.
            coverInfo.coverLocation = QFileInfo(coverPath).fileName();
            coverInfo.setImageDigest(image);
            pTrack->setCoverInfo(coverInfo);
        }
    }
    if (m_autoLoadLocation == nativeLocation) {
        m_autoLoadLocation.clear();
        emit loadTrack(pTrack);
    }
}

SubsonicFeature::ClientPtr SubsonicFeature::ensureClient() {
    const QString host =
            m_pConfig->getValue(ConfigKey(kConfigGroup, kHostKey), QString());
    if (host.isEmpty()) {
        return nullptr;
    }
    const QString username = m_pConfig->getValue(
            ConfigKey(kConfigGroup, kUsernameKey), QString());
    const bool verifyTls =
            m_pConfig->getValue(ConfigKey(kConfigGroup, kVerifyTlsKey), true);
    // The password is not part of the id: the connection dialog resets the
    // cached client whenever the settings (incl. password) change.
    const QString configId = host + QChar('\n') + username + QChar('\n') +
            (verifyTls ? QChar('1') : QChar('0'));

    QMutexLocker locker(&m_clientMutex);
    if (m_pClient && m_clientConfigId == configId) {
        return m_pClient;
    }
    // Password lives in the OS keychain; a non-empty config value is the
    // legacy plaintext fallback (also used when no keychain backend exists).
    QString password = subsoniccredentials::read(host, username);
    if (password.isEmpty()) {
        password = m_pConfig->getValue(
                ConfigKey(kConfigGroup, kPasswordKey), QString());
    }
    try {
        subsonic::ConnectionConfig config{
                rust::String(host.toStdString()),
                rust::String(username.toStdString()),
                rust::String(password.toStdString()),
                verifyTls,
        };
        m_pClient = std::make_shared<rust::Box<subsonic::Client>>(
                subsonic::new_client(config));
        m_clientConfigId = configId;
        return m_pClient;
    } catch (const rust::Error& e) {
        qWarning() << "Failed to create Subsonic client:" << e.what();
        m_pClient.reset();
        m_clientConfigId.clear();
        return nullptr;
    }
}

void SubsonicFeature::cancelPendingImport() {
    if (m_future.isRunning()) {
        m_cancelImport = true;
        m_future.waitForFinished();
        m_cancelImport = false;
    }
}

// This method runs on a QtConcurrent worker thread.
TreeItem* SubsonicFeature::importLibrary() {
    QThread::currentThread()->setPriority(QThread::LowPriority);

    ClientPtr pClient = ensureClient();
    if (!pClient) {
        m_lastImportError = tr("No valid server configuration.");
        return nullptr;
    }

    ScopedTransaction transaction(m_database);
    // Clearing happens inside the transaction: other connections (the
    // visible models) keep seeing the previous import until the commit
    // atomically swaps in the fresh data.
    SubsonicDAO::clearTables(m_database);
    SubsonicDAO dao;
    dao.initialize(m_database);

    auto pRoot = TreeItem::newRoot(this);
    try {
        subsonic::ImportProgress progress(&m_cancelImport);
        const rust::Vec<subsonic::FfiTrack> tracks =
                subsonic::fetch_all_tracks(**pClient, progress);
        QSet<QString> coverArtIds;
        for (const subsonic::FfiTrack& track : tracks) {
            const QString trackId = fromRust(track.id);
            const QString suffix = fromRust(track.suffix);
            QString coverArtId = fromRust(track.cover_art_id);
            if (coverArtId.isEmpty()) {
                // Servers resolve cover art for a song id directly.
                coverArtId = trackId;
            }
            coverArtIds.insert(coverArtId);
            dao.importTrack(SubsonicTrackRow{
                    trackId,
                    fromRust(track.artist),
                    fromRust(track.title),
                    fromRust(track.album),
                    fromRust(track.album_artist),
                    fromRust(track.genre),
                    track.year,
                    track.track_number,
                    makeLocation(trackId, suffix),
                    track.duration_seconds,
                    track.bitrate_kbps,
                    coverArtId,
            });
            if (m_cancelImport.load()) {
                transaction.commit();
                return nullptr;
            }
        }

        // Cover thumbnails are NOT fetched here: the library must become
        // browsable as soon as the track pages are in. The ids are handed
        // to a second background pass started after this import finishes.
        m_pendingCoverArtIds = QStringList(coverArtIds.begin(), coverArtIds.end());

        const rust::Vec<subsonic::FfiPlaylist> playlists =
                subsonic::fetch_playlists(**pClient);
        for (const subsonic::FfiPlaylist& playlist : playlists) {
            if (m_cancelImport.load()) {
                transaction.commit();
                return nullptr;
            }
            const QString name = fromRust(playlist.name);
            const int playlistId =
                    dao.importPlaylist(fromRust(playlist.id), name);
            if (playlistId < 0) {
                continue;
            }
            const rust::Vec<rust::String> trackIds =
                    subsonic::fetch_playlist_track_ids(
                            **pClient, rust::Str(playlist.id));
            int position = 0;
            for (const rust::String& trackId : trackIds) {
                dao.importPlaylistTrack(playlistId, fromRust(trackId), position++);
            }
            pRoot->appendChild(name, playlistId);
        }
    } catch (const rust::Error& e) {
        // Commit whatever was imported before the failure (iTunes pattern).
        transaction.commit();
        if (m_cancelImport.load()) {
            return nullptr;
        }
        qWarning() << "Subsonic library import failed:" << e.what();
        m_lastImportError = QString::fromUtf8(e.what());
        return pRoot.release();
    }

    transaction.commit();
    return pRoot.release();
}

void SubsonicFeature::onTrackCollectionLoaded() {
    std::unique_ptr<TreeItem> pRoot(m_future.result());
    if (pRoot) {
        m_pSidebarModel->setRootItem(std::move(pRoot));
        m_trackSource->buildIndex();
        emit showTrackModel(m_pTrackModel);
        qDebug() << "Subsonic library loaded";
    }
    if (!m_lastImportError.isEmpty()) {
        QMessageBox::warning(
                nullptr,
                tr("Error Loading Subsonic Library"),
                tr("There was an error loading your Subsonic library:\n%1")
                        .arg(m_lastImportError));
    }
    m_title = tr("Subsonic");
    emit featureLoadingFinished(this);
    if (!m_pendingCoverArtIds.isEmpty()) {
        startCoverFetch(m_pendingCoverArtIds);
        m_pendingCoverArtIds.clear();
    }
    activate();
}

void SubsonicFeature::startCoverFetch(const QStringList& coverArtIds) {
    const int generation = ++m_coverGeneration;
    m_coverFuture = QtConcurrent::run([this, generation, coverArtIds] {
        QThreadPool coverPool;
        coverPool.setMaxThreadCount(16);
        QList<QPair<QString, QFuture<QString>>> downloads;
        downloads.reserve(coverArtIds.size());
        for (const QString& coverArtId : coverArtIds) {
            downloads.append({coverArtId,
                    QtConcurrent::run(&coverPool, [this, coverArtId] {
                        return ensureCoverDownloaded(
                                coverArtId, kCoverThumbnailSize);
                    })});
        }
        QList<CoverFetchResult> results;
        results.reserve(downloads.size());
        for (auto& [coverArtId, future] : downloads) {
            if (m_cancelImport.load() || generation != m_coverGeneration) {
                coverPool.clear();
                coverPool.waitForDone();
                return;
            }
            future.waitForFinished();
            const QString thumbPath = future.result();
            if (thumbPath.isEmpty()) {
                continue;
            }
            const QImage image(thumbPath);
            if (image.isNull()) {
                continue;
            }
            CoverInfoRelative coverInfo;
            // Type/source must be set before attaching a non-null image
            // (asserted by setImageDigest) and match what updateCoverArt
            // stores in the DB columns.
            coverInfo.type = CoverInfo::FILE;
            coverInfo.source = CoverInfo::GUESSED;
            coverInfo.coverLocation = thumbPath;
            coverInfo.setImageDigest(image);
            results.append(CoverFetchResult{coverArtId,
                    thumbPath,
                    coverInfo.imageDigest(),
                    coverInfo.legacyHash()});
        }
        QMetaObject::invokeMethod(
                this,
                [this, generation, results] {
                    applyCoverResults(generation, results);
                },
                Qt::QueuedConnection);
    });
}

void SubsonicFeature::applyCoverResults(
        int generation, const QList<CoverFetchResult>& results) {
    // A newer import/cover pass supersedes this one. The generation also
    // guarantees no import worker is holding a transaction on m_database
    // right now (starting one bumps the generation).
    if (generation != m_coverGeneration || results.isEmpty()) {
        return;
    }
    ScopedTransaction transaction(m_database);
    SubsonicDAO dao;
    dao.initialize(m_database);
    for (const CoverFetchResult& result : results) {
        dao.updateCoverArt(result.coverArtId,
                result.thumbPath,
                result.imageDigest,
                result.legacyHash);
    }
    transaction.commit();
    // Repaint the visible views with the new cover columns.
    m_trackSource->buildIndex();
    m_pTrackModel->select();
    m_pPlaylistModel->select();
}
