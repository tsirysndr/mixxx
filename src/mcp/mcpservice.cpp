#include "mcp/mcpservice.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QtDebug>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crateschema.h"
#include "mcp/mcprequesthandler.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_mcpservice.cpp"
#include "track/keyutils.h"
#include "track/track.h"
#include "track/trackref.h"

namespace {

const QString kConfigGroup = QStringLiteral("[Mcp]");

// Mirrors mixxx_mcp::protocol::error_code on the Rust side.
constexpr int kErrorInvalidRequest = -32602;
constexpr int kErrorInternal = -32603;

/// Failure of a single request. Thrown by handlers, caught by the
/// dispatcher and turned into a JSON-RPC error.
struct RpcError {
    int code;
    QString message;
};

[[noreturn]] void fail(const QString& message, int code = kErrorInvalidRequest) {
    throw RpcError{code, message};
}

/// Wraps a search term for SQL LIKE. Returns a QString explicitly: with
/// QT_USE_QSTRINGBUILDER the concatenation is otherwise a lazy builder.
QString likePattern(const QString& text) {
    return QStringLiteral("%") + text + QStringLiteral("%");
}

/// TrackId deliberately hides its integer; agents address tracks by it.
int trackIdValue(const TrackId& trackId) {
    return trackId.toVariant().toInt();
}

QString fromRust(const rust::String& s) {
    return QString::fromUtf8(s.data(), static_cast<int>(s.size()));
}

// --- parameter accessors ------------------------------------------------

bool hasValue(const QJsonObject& params, const char* key) {
    const auto it = params.constFind(QLatin1String(key));
    return it != params.constEnd() && !it->isNull() && !it->isUndefined();
}

std::optional<double> optNumber(const QJsonObject& params, const char* key) {
    if (!hasValue(params, key)) {
        return std::nullopt;
    }
    const QJsonValue value = params.value(QLatin1String(key));
    if (value.isDouble()) {
        return value.toDouble();
    }
    if (value.isBool()) {
        return value.toBool() ? 1.0 : 0.0;
    }
    // Some agents stringify numbers; accept that rather than failing.
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) {
            return parsed;
        }
    }
    return std::nullopt;
}

double requireNumber(const QJsonObject& params, const char* key) {
    const auto value = optNumber(params, key);
    if (!value) {
        fail(QStringLiteral("'%1' is required and must be a number")
                        .arg(QLatin1String(key)));
    }
    return *value;
}

int numberOr(const QJsonObject& params, const char* key, int fallback) {
    const auto value = optNumber(params, key);
    return value ? static_cast<int>(std::llround(*value)) : fallback;
}

bool boolOr(const QJsonObject& params, const char* key, bool fallback) {
    if (!hasValue(params, key)) {
        return fallback;
    }
    const QJsonValue value = params.value(QLatin1String(key));
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toDouble() != 0.0;
    }
    return fallback;
}

QString stringOr(const QJsonObject& params, const char* key, const QString& fallback = QString()) {
    if (!hasValue(params, key)) {
        return fallback;
    }
    return params.value(QLatin1String(key)).toString(fallback);
}

QString requireString(const QJsonObject& params, const char* key) {
    const QString value = stringOr(params, key);
    if (value.isEmpty()) {
        fail(QStringLiteral("'%1' is required").arg(QLatin1String(key)));
    }
    return value;
}

// --- control object helpers ---------------------------------------------

double controlGet(const QString& group, const QString& key) {
    return ControlObject::get(ConfigKey(group, key));
}

void controlSet(const QString& group, const QString& key, double value) {
    ControlObject::set(ConfigKey(group, key), value);
}

/// Push buttons act on the press; releasing keeps them usable next time.
void controlTrigger(const QString& group, const QString& key) {
    ControlObject::set(ConfigKey(group, key), 1.0);
    ControlObject::set(ConfigKey(group, key), 0.0);
}

/// Adds `key` only when the control exists, so the JSON never claims a
/// feature this build does not have.
void insertControl(QJsonObject& target,
        const QString& name,
        const QString& group,
        const QString& key) {
    const ConfigKey configKey(group, key);
    if (ControlObject::exists(configKey)) {
        target.insert(name, ControlObject::get(configKey));
    }
}

QString equalizerGroup(const QString& deckGroup) {
    return QStringLiteral("[EqualizerRack1_%1_Effect1]").arg(deckGroup);
}

QString quickEffectGroup(const QString& deckGroup) {
    return QStringLiteral("[QuickEffectRack1_%1]").arg(deckGroup);
}

// --- key helpers ---------------------------------------------------------

using mixxx::track::io::key::ChromaticKey;

ChromaticKey keyFromText(const QString& text) {
    if (text.isEmpty()) {
        return mixxx::track::io::key::INVALID;
    }
    return KeyUtils::guessKeyFromText(text);
}

/// Both notations, because agents reason about Camelot ("8A") while the
/// tags in most libraries are traditional ("Am").
void insertKey(QJsonObject& target, ChromaticKey key) {
    if (key == mixxx::track::io::key::INVALID) {
        target.insert(QStringLiteral("key"), QJsonValue());
        target.insert(QStringLiteral("key_camelot"), QJsonValue());
        return;
    }
    target.insert(QStringLiteral("key"),
            KeyUtils::keyToString(key, KeyUtils::KeyNotation::Traditional));
    target.insert(QStringLiteral("key_camelot"),
            KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot));
}

ChromaticKey keyFromNumeric(double value) {
    return KeyUtils::keyFromNumericValue(value);
}

// --- library queries -----------------------------------------------------

/// Columns shared by every track-returning method, so an agent always
/// sees the same shape whether it searched, browsed a crate or peeked at
/// the Auto DJ queue.
const char* const kTrackColumns =
        "library.id AS id, "
        "library.artist AS artist, "
        "library.title AS title, "
        "library.album AS album, "
        "library.album_artist AS album_artist, "
        "library.genre AS genre, "
        "library.year AS year, "
        "library.tracknumber AS tracknumber, "
        "library.duration AS duration, "
        "library.bpm AS bpm, "
        "library.key_id AS key_id, "
        "library.key AS key_text, "
        "library.rating AS rating, "
        "library.timesplayed AS timesplayed, "
        "library.comment AS comment, "
        "library.datetime_added AS datetime_added, "
        "track_locations.location AS location";

const char* const kTrackFrom =
        " FROM library INNER JOIN track_locations"
        " ON library.location = track_locations.id"
        " WHERE library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0";

QJsonObject trackRowToJson(const QSqlQuery& query) {
    QJsonObject track;
    track.insert(QStringLiteral("track_id"), query.value(QStringLiteral("id")).toInt());
    track.insert(QStringLiteral("artist"), query.value(QStringLiteral("artist")).toString());
    track.insert(QStringLiteral("title"), query.value(QStringLiteral("title")).toString());
    track.insert(QStringLiteral("album"), query.value(QStringLiteral("album")).toString());
    track.insert(QStringLiteral("album_artist"),
            query.value(QStringLiteral("album_artist")).toString());
    track.insert(QStringLiteral("genre"), query.value(QStringLiteral("genre")).toString());
    track.insert(QStringLiteral("year"), query.value(QStringLiteral("year")).toString());
    track.insert(QStringLiteral("track_number"),
            query.value(QStringLiteral("tracknumber")).toString());
    track.insert(QStringLiteral("duration_seconds"),
            query.value(QStringLiteral("duration")).toDouble());
    track.insert(QStringLiteral("bpm"), query.value(QStringLiteral("bpm")).toDouble());
    track.insert(QStringLiteral("rating"), query.value(QStringLiteral("rating")).toInt());
    track.insert(QStringLiteral("play_count"),
            query.value(QStringLiteral("timesplayed")).toInt());
    track.insert(QStringLiteral("comment"), query.value(QStringLiteral("comment")).toString());
    track.insert(QStringLiteral("date_added"),
            query.value(QStringLiteral("datetime_added")).toString());
    track.insert(QStringLiteral("location"), query.value(QStringLiteral("location")).toString());

    const ChromaticKey key = keyFromNumeric(query.value(QStringLiteral("key_id")).toDouble());
    if (key == mixxx::track::io::key::INVALID) {
        // Fall back to the imported tag when Mixxx has not analysed the key.
        const ChromaticKey tagged = keyFromText(query.value(QStringLiteral("key_text")).toString());
        insertKey(track, tagged);
    } else {
        insertKey(track, key);
    }
    return track;
}

/// Metadata for a set of track ids, keyed by id, in one round trip.
QHash<int, QJsonObject> tracksByIds(const QSqlDatabase& database, const QList<int>& ids) {
    QHash<int, QJsonObject> result;
    if (ids.isEmpty()) {
        return result;
    }
    QStringList placeholders;
    placeholders.reserve(ids.size());
    for (int id : ids) {
        placeholders.append(QString::number(id));
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1%2 AND library.id IN (%3)")
                    .arg(QLatin1String(kTrackColumns),
                            QLatin1String(kTrackFrom),
                            placeholders.join(QChar(','))));
    if (!query.exec()) {
        qWarning() << "MCP: track lookup failed:" << query.lastError();
        return result;
    }
    while (query.next()) {
        result.insert(query.value(QStringLiteral("id")).toInt(), trackRowToJson(query));
    }
    return result;
}

QString orderByClause(const QString& sort) {
    if (sort == QLatin1String("artist")) {
        return QStringLiteral(" ORDER BY library.artist COLLATE NOCASE, library.title COLLATE NOCASE");
    }
    if (sort == QLatin1String("title")) {
        return QStringLiteral(" ORDER BY library.title COLLATE NOCASE");
    }
    if (sort == QLatin1String("bpm")) {
        return QStringLiteral(" ORDER BY library.bpm");
    }
    if (sort == QLatin1String("year")) {
        return QStringLiteral(" ORDER BY library.year DESC");
    }
    if (sort == QLatin1String("rating")) {
        return QStringLiteral(" ORDER BY library.rating DESC");
    }
    if (sort == QLatin1String("played")) {
        return QStringLiteral(" ORDER BY library.timesplayed DESC");
    }
    if (sort == QLatin1String("recent")) {
        return QStringLiteral(" ORDER BY library.datetime_added DESC");
    }
    if (sort == QLatin1String("random")) {
        return QStringLiteral(" ORDER BY RANDOM()");
    }
    return QStringLiteral(" ORDER BY library.artist COLLATE NOCASE, library.title COLLATE NOCASE");
}

PlaylistDAO::AutoDJSendLoc autoDjLocation(const QString& position) {
    if (position == QLatin1String("top")) {
        return PlaylistDAO::AutoDJSendLoc::TOP;
    }
    if (position == QLatin1String("replace")) {
        return PlaylistDAO::AutoDJSendLoc::REPLACE;
    }
    return PlaylistDAO::AutoDJSendLoc::BOTTOM;
}

/// Bridges Rust-thread requests onto the service's thread.
class ServiceRequestHandler : public mixxxmcp::RequestHandler {
  public:
    explicit ServiceRequestHandler(McpService* pService)
            : m_pService(pService) {
    }

    void onRequest(uint64_t id, rust::String method, rust::String params) override {
        // Convert to Qt types here, on the Rust thread: the queued
        // std::function must be copyable and own its data.
        const QString methodName = fromRust(method);
        const QString paramsJson = fromRust(params);
        QPointer<McpService> pGuard(m_pService);
        QMetaObject::invokeMethod(
                m_pService,
                [pGuard, id, methodName, paramsJson] {
                    if (pGuard) {
                        pGuard->handleRequest(id, methodName, paramsJson);
                    }
                },
                Qt::QueuedConnection);
    }

  private:
    McpService* const m_pService;
};

} // anonymous namespace

McpService::McpService(UserSettingsPointer pConfig,
        PlayerManager* pPlayerManager,
        TrackCollectionManager* pTrackCollectionManager,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pPlayerManager(pPlayerManager),
          m_pTrackCollectionManager(pTrackCollectionManager) {
    if (!m_pConfig->getValue(
                ConfigKey(kConfigGroup, QStringLiteral("Enabled")), true)) {
        qInfo() << "MCP server disabled by [Mcp] Enabled";
        return;
    }
    registerHandlers();

    const quint16 port = static_cast<quint16>(m_pConfig->getValue(
            ConfigKey(kConfigGroup, QStringLiteral("Port")), 0));
    // The descriptor is how `mixxx-mcp` finds this instance; it lives
    // next to mixxx.cfg and is removed again on shutdown.
    const QString endpointFile =
            QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("mcp.json"));
    const QString configuredToken =
            m_pConfig->getValueString(ConfigKey(kConfigGroup, QStringLiteral("Token")));

    // rust::Str borrows: these must outlive the call.
    const std::string tokenUtf8 = configuredToken.toStdString();
    const std::string endpointUtf8 = endpointFile.toStdString();
    try {
        m_pServer.emplace(mixxxmcp::start_server(port,
                tokenUtf8,
                endpointUtf8,
                std::unique_ptr<mixxxmcp::RequestHandler>(
                        new ServiceRequestHandler(this))));
    } catch (const rust::Error& e) {
        qWarning() << "MCP server failed to start:" << e.what();
        m_pServer.reset();
        return;
    }

    connectEventSources();
    qInfo() << "MCP server listening on 127.0.0.1:" << this->port()
            << "- endpoint descriptor:" << endpointFile;
}

McpService::~McpService() {
    if (m_pServer) {
        // Returns once the listener is down; no handler calls after this.
        mixxxmcp::stop_server(**m_pServer);
    }
}

quint16 McpService::port() const {
    return m_pServer ? mixxxmcp::server_port(**m_pServer) : 0;
}

void McpService::registerHandlers() {
    m_handlers = {
            {QStringLiteral("mixxx.get_state"), &McpService::getState},
            {QStringLiteral("mixxx.get_deck"), &McpService::getDeck},
            {QStringLiteral("mixxx.play"), &McpService::play},
            {QStringLiteral("mixxx.cue"), &McpService::cue},
            {QStringLiteral("mixxx.seek"), &McpService::seek},
            {QStringLiteral("mixxx.beatjump"), &McpService::beatjump},
            {QStringLiteral("mixxx.load_track"), &McpService::loadTrack},
            {QStringLiteral("mixxx.eject"), &McpService::eject},
            {QStringLiteral("mixxx.clone_deck"), &McpService::cloneDeck},
            {QStringLiteral("mixxx.set_volume"), &McpService::setVolume},
            {QStringLiteral("mixxx.set_gain"), &McpService::setGain},
            {QStringLiteral("mixxx.set_crossfader"), &McpService::setCrossfader},
            {QStringLiteral("mixxx.set_eq"), &McpService::setEq},
            {QStringLiteral("mixxx.set_rate"), &McpService::setRate},
            {QStringLiteral("mixxx.sync"), &McpService::sync},
            {QStringLiteral("mixxx.set_loop"), &McpService::setLoop},
            {QStringLiteral("mixxx.hotcue"), &McpService::hotcue},
            {QStringLiteral("mixxx.headphone"), &McpService::headphone},
            {QStringLiteral("mixxx.search_library"), &McpService::searchLibrary},
            {QStringLiteral("mixxx.get_track"), &McpService::getTrack},
            {QStringLiteral("mixxx.suggest_next"), &McpService::suggestNext},
            {QStringLiteral("mixxx.list_playlists"), &McpService::listPlaylists},
            {QStringLiteral("mixxx.get_playlist"), &McpService::getPlaylist},
            {QStringLiteral("mixxx.list_crates"), &McpService::listCrates},
            {QStringLiteral("mixxx.get_crate"), &McpService::getCrate},
            {QStringLiteral("mixxx.autodj"), &McpService::autoDj},
            {QStringLiteral("mixxx.autodj_queue"), &McpService::autoDjQueue},
            {QStringLiteral("mixxx.autodj_add"), &McpService::autoDjAdd},
            {QStringLiteral("mixxx.autodj_edit"), &McpService::autoDjEdit},
            {QStringLiteral("mixxx.get_control"), &McpService::getControl},
            {QStringLiteral("mixxx.set_control"), &McpService::setControl},
    };
}

void McpService::connectEventSources() {
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &McpService::slotPlayingTrackChanged);

    // One proxy per deck turns play/pause into something an agent can
    // wait on instead of polling.
    const int deckCount = m_pPlayerManager->numberOfDecks();
    for (int deck = 1; deck <= deckCount; ++deck) {
        auto* pProxy = new ControlProxy(
                PlayerManager::groupForDeck(deck - 1), QStringLiteral("play"), this);
        pProxy->connectValueChanged(this, [this, deck](double value) {
            slotDeckPlayChanged(deck, value);
        });
        m_playProxies.append(pProxy);
    }

    if (m_pTrackCollectionManager) {
        connect(&m_pTrackCollectionManager->internalCollection()->getPlaylistDAO(),
                &PlaylistDAO::playlistContentChanged,
                this,
                &McpService::slotAutoDJQueueChanged);
    }
}

// --- request plumbing ----------------------------------------------------

void McpService::handleRequest(quint64 id, const QString& method, const QString& params) {
    const auto handler = m_handlers.constFind(method);
    if (handler == m_handlers.constEnd()) {
        respondError(id,
                kErrorInvalidRequest,
                QStringLiteral("unknown method: %1").arg(method));
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(params.toUtf8(), &parseError);
    const QJsonObject arguments = document.isObject() ? document.object() : QJsonObject();

    try {
        respond(id, (this->*handler.value())(arguments));
    } catch (const RpcError& e) {
        respondError(id, e.code, e.message);
    } catch (const std::exception& e) {
        qWarning() << "MCP:" << method << "failed:" << e.what();
        respondError(id, kErrorInternal, QString::fromUtf8(e.what()));
    }
}

void McpService::respond(quint64 id, const QJsonValue& result) {
    if (!m_pServer) {
        return;
    }
    QJsonDocument document;
    if (result.isArray()) {
        document = QJsonDocument(result.toArray());
    } else if (result.isObject()) {
        document = QJsonDocument(result.toObject());
    } else {
        // JSON documents cannot hold bare scalars; wrap them.
        document = QJsonDocument(QJsonObject{{QStringLiteral("value"), result}});
    }
    const QByteArray json = document.toJson(QJsonDocument::Compact);
    mixxxmcp::respond(**m_pServer,
            id,
            rust::Str(json.constData(), static_cast<size_t>(json.size())));
}

void McpService::respondError(quint64 id, int code, const QString& message) {
    if (!m_pServer) {
        return;
    }
    const QByteArray utf8 = message.toUtf8();
    mixxxmcp::respond_error(**m_pServer,
            id,
            code,
            rust::Str(utf8.constData(), static_cast<size_t>(utf8.size())));
}

void McpService::publishEvent(const QString& type, const QJsonObject& detail) {
    if (!m_pServer) {
        return;
    }
    QJsonObject event = detail;
    event.insert(QStringLiteral("type"), type);
    const QByteArray json = QJsonDocument(event).toJson(QJsonDocument::Compact);
    mixxxmcp::publish_event(
            **m_pServer, rust::Str(json.constData(), static_cast<size_t>(json.size())));
}

void McpService::slotPlayingTrackChanged(TrackPointer pTrack) {
    QJsonObject detail;
    if (pTrack) {
        const TrackId trackId = pTrack->getId();
        if (trackId.isValid()) {
            // Remembered so suggest_next stops proposing what already played.
            m_playedTrackIds.insert(trackIdValue(trackId));
            detail.insert(QStringLiteral("track_id"), trackIdValue(trackId));
        }
        detail.insert(QStringLiteral("artist"), pTrack->getArtist());
        detail.insert(QStringLiteral("title"), pTrack->getTitle());
        detail.insert(QStringLiteral("deck"),
                PlayerInfo::instance().getCurrentPlayingDeck() + 1);
    }
    publishEvent(QStringLiteral("playing_track_changed"), detail);
}

void McpService::slotAutoDJQueueChanged(const QSet<int>& playlistIds) {
    const int autoDjId = autoDjPlaylistId();
    if (autoDjId < 0 || !playlistIds.contains(autoDjId)) {
        return;
    }
    publishEvent(QStringLiteral("autodj_queue_changed"),
            QJsonObject{{QStringLiteral("length"),
                    m_pTrackCollectionManager->internalCollection()
                            ->getPlaylistDAO()
                            .tracksInPlaylist(autoDjId)}});
}

void McpService::slotDeckPlayChanged(int deck, double value) {
    publishEvent(QStringLiteral("play_changed"),
            QJsonObject{{QStringLiteral("deck"), deck},
                    {QStringLiteral("playing"), value > 0.0}});
}

// --- helpers -------------------------------------------------------------

QString McpService::deckGroup(const QJsonObject& params, const char* key) const {
    const int deck = static_cast<int>(requireNumber(params, key));
    const int deckCount = m_pPlayerManager->numberOfDecks();
    if (deck < 1 || deck > deckCount) {
        fail(QStringLiteral("deck %1 does not exist (this session has %2 decks)")
                        .arg(deck)
                        .arg(deckCount));
    }
    return PlayerManager::groupForDeck(deck - 1);
}

QJsonObject McpService::deckState(int deck) const {
    const QString group = PlayerManager::groupForDeck(deck - 1);
    QJsonObject state;
    state.insert(QStringLiteral("deck"), deck);
    state.insert(QStringLiteral("group"), group);

    const double duration = controlGet(group, QStringLiteral("duration"));
    // playposition is a fraction and can go slightly negative before the start.
    const double fraction = controlGet(group, QStringLiteral("playposition"));
    const double position = std::max(0.0, fraction * duration);

    state.insert(QStringLiteral("playing"), controlGet(group, QStringLiteral("play")) > 0.0);
    state.insert(QStringLiteral("duration_seconds"), duration);
    state.insert(QStringLiteral("position_seconds"), position);
    state.insert(QStringLiteral("remaining_seconds"), std::max(0.0, duration - position));
    state.insert(QStringLiteral("position_fraction"), fraction);
    state.insert(QStringLiteral("bpm"), controlGet(group, QStringLiteral("bpm")));
    state.insert(QStringLiteral("file_bpm"), controlGet(group, QStringLiteral("file_bpm")));
    state.insert(QStringLiteral("rate_ratio"), controlGet(group, QStringLiteral("rate_ratio")));
    state.insert(QStringLiteral("volume"), controlGet(group, QStringLiteral("volume")));
    state.insert(QStringLiteral("gain"), controlGet(group, QStringLiteral("pregain")));
    state.insert(QStringLiteral("sync_enabled"),
            controlGet(group, QStringLiteral("sync_enabled")) > 0.0);
    state.insert(QStringLiteral("sync_leader"),
            controlGet(group, QStringLiteral("sync_leader")) > 0.0);
    state.insert(QStringLiteral("headphone"), controlGet(group, QStringLiteral("pfl")) > 0.0);
    state.insert(QStringLiteral("loop_enabled"),
            controlGet(group, QStringLiteral("loop_enabled")) > 0.0);
    state.insert(QStringLiteral("loop_beats"),
            controlGet(group, QStringLiteral("beatloop_size")));
    insertKey(state, keyFromNumeric(controlGet(group, QStringLiteral("key"))));

    QJsonObject eq;
    const QString eqGroup = equalizerGroup(group);
    insertControl(eq, QStringLiteral("low"), eqGroup, QStringLiteral("parameter1"));
    insertControl(eq, QStringLiteral("mid"), eqGroup, QStringLiteral("parameter2"));
    insertControl(eq, QStringLiteral("high"), eqGroup, QStringLiteral("parameter3"));
    insertControl(eq, QStringLiteral("filter"), quickEffectGroup(group), QStringLiteral("super1"));
    state.insert(QStringLiteral("eq"), eq);

    // Intro/outro markers are engine sample positions; express them in
    // seconds so an agent can plan a transition against the clock.
    const double trackSamples = controlGet(group, QStringLiteral("track_samples"));
    const auto markerSeconds = [&](const QString& key) -> QJsonValue {
        const double value = controlGet(group, key);
        if (trackSamples <= 0.0 || value < 0.0 || duration <= 0.0) {
            return QJsonValue();
        }
        return value / trackSamples * duration;
    };
    state.insert(QStringLiteral("intro_start_seconds"),
            markerSeconds(QStringLiteral("intro_start_position")));
    state.insert(QStringLiteral("intro_end_seconds"),
            markerSeconds(QStringLiteral("intro_end_position")));
    state.insert(QStringLiteral("outro_start_seconds"),
            markerSeconds(QStringLiteral("outro_start_position")));
    state.insert(QStringLiteral("outro_end_seconds"),
            markerSeconds(QStringLiteral("outro_end_position")));

    BaseTrackPlayer* pPlayer = m_pPlayerManager->getPlayer(group);
    const TrackPointer pTrack = pPlayer ? pPlayer->getLoadedTrack() : TrackPointer();
    if (pTrack) {
        QJsonObject track;
        if (pTrack->getId().isValid()) {
            track.insert(QStringLiteral("track_id"), trackIdValue(pTrack->getId()));
        }
        track.insert(QStringLiteral("artist"), pTrack->getArtist());
        track.insert(QStringLiteral("title"), pTrack->getTitle());
        track.insert(QStringLiteral("album"), pTrack->getAlbum());
        track.insert(QStringLiteral("genre"), pTrack->getGenre());
        track.insert(QStringLiteral("location"), pTrack->getLocation());
        state.insert(QStringLiteral("track"), track);
    } else {
        state.insert(QStringLiteral("track"), QJsonValue());
    }
    return state;
}

TrackPointer McpService::resolveTrack(const QJsonObject& params) const {
    if (hasValue(params, "track_id")) {
        const int rawId = numberOr(params, "track_id", -1);
        // DbId(int) is deleted on purpose; go through QVariant.
        const TrackId trackId{QVariant(rawId)};
        if (!trackId.isValid()) {
            fail(QStringLiteral("track_id %1 is not a valid id").arg(rawId));
        }
        const TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(trackId);
        if (!pTrack) {
            fail(QStringLiteral("no track with id %1 in the library").arg(rawId));
        }
        return pTrack;
    }
    const QString location = stringOr(params, "location");
    if (location.isEmpty()) {
        fail(QStringLiteral("either 'track_id' or 'location' is required"));
    }
    const TrackPointer pTrack = m_pTrackCollectionManager->getOrAddTrack(
            TrackRef::fromFilePath(location));
    if (!pTrack) {
        fail(QStringLiteral("cannot load '%1'").arg(location));
    }
    return pTrack;
}

int McpService::autoDjPlaylistId() const {
    return m_pTrackCollectionManager->internalCollection()
            ->getPlaylistDAO()
            .getPlaylistIdFromName(QStringLiteral(AUTODJ_TABLE));
}

// --- state ---------------------------------------------------------------

QJsonValue McpService::getState(const QJsonObject& params) {
    Q_UNUSED(params);
    QJsonObject state;

    QJsonArray decks;
    const int deckCount = m_pPlayerManager->numberOfDecks();
    for (int deck = 1; deck <= deckCount; ++deck) {
        decks.append(deckState(deck));
    }
    state.insert(QStringLiteral("decks"), decks);

    QJsonObject master;
    insertControl(master, QStringLiteral("crossfader"), QStringLiteral("[Master]"), QStringLiteral("crossfader"));
    insertControl(master, QStringLiteral("gain"), QStringLiteral("[Master]"), QStringLiteral("gain"));
    insertControl(master, QStringLiteral("balance"), QStringLiteral("[Master]"), QStringLiteral("balance"));
    insertControl(master, QStringLiteral("headphone_mix"), QStringLiteral("[Master]"), QStringLiteral("headMix"));
    insertControl(master, QStringLiteral("headphone_gain"), QStringLiteral("[Master]"), QStringLiteral("headGain"));
    state.insert(QStringLiteral("master"), master);

    const int playingDeck = PlayerInfo::instance().getCurrentPlayingDeck();
    state.insert(QStringLiteral("playing_deck"),
            playingDeck >= 0 ? QJsonValue(playingDeck + 1) : QJsonValue());

    state.insert(QStringLiteral("autodj"), autoDj(QJsonObject{
            {QStringLiteral("action"), QStringLiteral("status")}}).toObject());
    return state;
}

QJsonValue McpService::getDeck(const QJsonObject& params) {
    // deckGroup() does the range checking; the number is what we need.
    deckGroup(params);
    return deckState(static_cast<int>(requireNumber(params, "deck")));
}

// --- transport -----------------------------------------------------------

QJsonValue McpService::play(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const bool playing = controlGet(group, QStringLiteral("play")) > 0.0;
    const bool target = boolOr(params, "play", !playing);
    controlSet(group, QStringLiteral("play"), target ? 1.0 : 0.0);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("playing"), target}};
}

QJsonValue McpService::cue(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const QString action = requireString(params, "action");
    if (action == QLatin1String("set")) {
        controlTrigger(group, QStringLiteral("cue_set"));
    } else if (action == QLatin1String("goto")) {
        controlTrigger(group, QStringLiteral("cue_goto"));
    } else if (action == QLatin1String("play")) {
        controlTrigger(group, QStringLiteral("cue_gotoandplay"));
    } else {
        fail(QStringLiteral("unknown cue action '%1' (use set, goto or play)").arg(action));
    }
    return QJsonObject{{QStringLiteral("ok"), true}};
}

QJsonValue McpService::seek(const QJsonObject& params) {
    const QString group = deckGroup(params);
    double fraction;
    if (const auto explicitFraction = optNumber(params, "fraction")) {
        fraction = *explicitFraction;
    } else if (const auto seconds = optNumber(params, "position_seconds")) {
        const double duration = controlGet(group, QStringLiteral("duration"));
        if (duration <= 0.0) {
            fail(QStringLiteral("deck has no track loaded"));
        }
        fraction = *seconds / duration;
    } else {
        fail(QStringLiteral("either 'position_seconds' or 'fraction' is required"));
    }
    fraction = std::clamp(fraction, 0.0, 1.0);
    controlSet(group, QStringLiteral("playposition"), fraction);
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("position_fraction"), fraction}};
}

QJsonValue McpService::beatjump(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const double beats = requireNumber(params, "beats");
    // Setting the control performs the jump; it is not a latched button.
    controlSet(group, QStringLiteral("beatjump"), beats);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("beats"), beats}};
}

QJsonValue McpService::loadTrack(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const bool playing = controlGet(group, QStringLiteral("play")) > 0.0;
    if (playing && !boolOr(params, "force", false)) {
        fail(QStringLiteral("deck is playing; pass force=true to replace the track anyway"));
    }
    const TrackPointer pTrack = resolveTrack(params);
    const bool startPlaying = boolOr(params, "play", false);
#ifdef __STEM__
    m_pPlayerManager->slotLoadTrackToPlayer(
            pTrack, group, mixxx::StemChannelSelection(), startPlaying);
#else
    m_pPlayerManager->slotLoadTrackToPlayer(pTrack, group, startPlaying);
#endif
    QJsonObject result{{QStringLiteral("ok"), true},
            {QStringLiteral("group"), group},
            {QStringLiteral("artist"), pTrack->getArtist()},
            {QStringLiteral("title"), pTrack->getTitle()}};
    if (pTrack->getId().isValid()) {
        result.insert(QStringLiteral("track_id"), trackIdValue(pTrack->getId()));
    }
    return result;
}

QJsonValue McpService::eject(const QJsonObject& params) {
    controlTrigger(deckGroup(params), QStringLiteral("eject"));
    return QJsonObject{{QStringLiteral("ok"), true}};
}

QJsonValue McpService::cloneDeck(const QJsonObject& params) {
    const QString from = deckGroup(params, "from_deck");
    const QString to = deckGroup(params, "to_deck");
    m_pPlayerManager->slotCloneDeck(from, to);
    return QJsonObject{{QStringLiteral("ok"), true}};
}

// --- mixer ---------------------------------------------------------------

QJsonValue McpService::setVolume(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const double value = std::clamp(requireNumber(params, "value"), 0.0, 1.0);
    controlSet(group, QStringLiteral("volume"), value);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("volume"), value}};
}

QJsonValue McpService::setGain(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const double value = std::clamp(requireNumber(params, "value"), 0.0, 4.0);
    controlSet(group, QStringLiteral("pregain"), value);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("gain"), value}};
}

QJsonValue McpService::setCrossfader(const QJsonObject& params) {
    const double value = std::clamp(requireNumber(params, "value"), -1.0, 1.0);
    controlSet(QStringLiteral("[Master]"), QStringLiteral("crossfader"), value);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("crossfader"), value}};
}

QJsonValue McpService::setEq(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const QString eqGroup = equalizerGroup(group);
    QJsonObject applied;
    const auto band = [&](const char* name, const QString& parameter) {
        if (const auto value = optNumber(params, name)) {
            const double clamped = std::clamp(*value, 0.0, 4.0);
            controlSet(eqGroup, parameter, clamped);
            applied.insert(QLatin1String(name), clamped);
        }
    };
    band("low", QStringLiteral("parameter1"));
    band("mid", QStringLiteral("parameter2"));
    band("high", QStringLiteral("parameter3"));
    if (const auto filter = optNumber(params, "filter")) {
        const double clamped = std::clamp(*filter, 0.0, 1.0);
        controlSet(quickEffectGroup(group), QStringLiteral("super1"), clamped);
        applied.insert(QStringLiteral("filter"), clamped);
    }
    if (applied.isEmpty()) {
        fail(QStringLiteral("pass at least one of low, mid, high or filter"));
    }
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("applied"), applied}};
}

QJsonValue McpService::setRate(const QJsonObject& params) {
    const QString group = deckGroup(params);
    if (const auto bpm = optNumber(params, "bpm")) {
        if (*bpm <= 0.0) {
            fail(QStringLiteral("bpm must be positive"));
        }
        // Writing the deck's bpm control moves the rate slider to match.
        controlSet(group, QStringLiteral("bpm"), *bpm);
    } else if (const auto ratio = optNumber(params, "ratio")) {
        if (*ratio <= 0.0) {
            fail(QStringLiteral("ratio must be positive"));
        }
        controlSet(group, QStringLiteral("rate_ratio"), *ratio);
    } else {
        fail(QStringLiteral("either 'bpm' or 'ratio' is required"));
    }
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("bpm"), controlGet(group, QStringLiteral("bpm"))},
            {QStringLiteral("rate_ratio"), controlGet(group, QStringLiteral("rate_ratio"))}};
}

QJsonValue McpService::sync(const QJsonObject& params) {
    const QString group = deckGroup(params);
    if (boolOr(params, "leader", false)) {
        controlSet(group, QStringLiteral("sync_leader"), 1.0);
    } else {
        controlSet(group,
                QStringLiteral("sync_enabled"),
                boolOr(params, "enabled", true) ? 1.0 : 0.0);
    }
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("sync_enabled"),
                    controlGet(group, QStringLiteral("sync_enabled")) > 0.0},
            {QStringLiteral("sync_leader"),
                    controlGet(group, QStringLiteral("sync_leader")) > 0.0}};
}

QJsonValue McpService::setLoop(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const bool enable = boolOr(params, "enabled", true);
    if (!enable) {
        if (controlGet(group, QStringLiteral("loop_enabled")) > 0.0) {
            controlTrigger(group, QStringLiteral("reloop_toggle"));
        }
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("loop_enabled"), false}};
    }
    const double beats = requireNumber(params, "beats");
    if (beats < 0.03125 || beats > 64.0) {
        fail(QStringLiteral("beats must be between 1/32 and 64"));
    }
    controlSet(group, QStringLiteral("beatloop_size"), beats);
    controlTrigger(group, QStringLiteral("beatloop_activate"));
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("loop_enabled"),
                    controlGet(group, QStringLiteral("loop_enabled")) > 0.0},
            {QStringLiteral("beats"), beats}};
}

QJsonValue McpService::hotcue(const QJsonObject& params) {
    const QString group = deckGroup(params);
    const int number = numberOr(params, "number", 0);
    if (number < 1) {
        fail(QStringLiteral("'number' must be a hotcue number of 1 or more"));
    }
    const QString action = requireString(params, "action");
    QString control;
    if (action == QLatin1String("set")) {
        control = QStringLiteral("set");
    } else if (action == QLatin1String("goto")) {
        control = QStringLiteral("goto");
    } else if (action == QLatin1String("play")) {
        control = QStringLiteral("gotoandplay");
    } else if (action == QLatin1String("clear")) {
        control = QStringLiteral("clear");
    } else {
        fail(QStringLiteral("unknown hotcue action '%1' (use set, goto, play or clear)")
                        .arg(action));
    }
    const QString key = QStringLiteral("hotcue_%1_%2").arg(number).arg(control);
    if (!ControlObject::exists(ConfigKey(group, key))) {
        fail(QStringLiteral("hotcue %1 does not exist on this deck").arg(number));
    }
    controlTrigger(group, key);
    return QJsonObject{{QStringLiteral("ok"), true}};
}

QJsonValue McpService::headphone(const QJsonObject& params) {
    QJsonObject applied;
    if (hasValue(params, "deck")) {
        const QString group = deckGroup(params);
        const bool enabled = boolOr(params, "enabled", true);
        controlSet(group, QStringLiteral("pfl"), enabled ? 1.0 : 0.0);
        applied.insert(QStringLiteral("enabled"), enabled);
    }
    if (const auto mix = optNumber(params, "mix")) {
        const double clamped = std::clamp(*mix, -1.0, 1.0);
        controlSet(QStringLiteral("[Master]"), QStringLiteral("headMix"), clamped);
        applied.insert(QStringLiteral("mix"), clamped);
    }
    if (const auto gain = optNumber(params, "gain")) {
        const double clamped = std::clamp(*gain, 0.0, 4.0);
        controlSet(QStringLiteral("[Master]"), QStringLiteral("headGain"), clamped);
        applied.insert(QStringLiteral("gain"), clamped);
    }
    if (applied.isEmpty()) {
        fail(QStringLiteral("pass a deck to cue, or a mix/gain value"));
    }
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("applied"), applied}};
}

// --- library -------------------------------------------------------------

QJsonValue McpService::searchLibrary(const QJsonObject& params) {
    const QSqlDatabase database = m_pTrackCollectionManager->internalCollection()->database();
    const int limit = std::clamp(numberOr(params, "limit", 25), 1, 200);
    const int offset = std::max(0, numberOr(params, "offset", 0));

    QString sql = QStringLiteral("SELECT %1%2").arg(
            QLatin1String(kTrackColumns), QLatin1String(kTrackFrom));
    QHash<QString, QVariant> bindings;

    const QStringList terms = stringOr(params, "query")
                                      .split(QChar(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < terms.size(); ++i) {
        const QString placeholder = QStringLiteral(":term%1").arg(i);
        sql += QStringLiteral(
                " AND (library.artist LIKE %1 OR library.title LIKE %1"
                " OR library.album LIKE %1 OR library.album_artist LIKE %1"
                " OR library.genre LIKE %1 OR library.comment LIKE %1"
                " OR track_locations.location LIKE %1)")
                       .arg(placeholder);
        bindings.insert(placeholder, likePattern(terms.at(i)));
    }
    if (const auto bpmMin = optNumber(params, "bpm_min")) {
        sql += QStringLiteral(" AND library.bpm >= :bpmMin");
        bindings.insert(QStringLiteral(":bpmMin"), *bpmMin);
    }
    if (const auto bpmMax = optNumber(params, "bpm_max")) {
        sql += QStringLiteral(" AND library.bpm <= :bpmMax");
        bindings.insert(QStringLiteral(":bpmMax"), *bpmMax);
    }
    if (hasValue(params, "genre")) {
        sql += QStringLiteral(" AND library.genre LIKE :genre");
        bindings.insert(QStringLiteral(":genre"), likePattern(stringOr(params, "genre")));
    }
    if (hasValue(params, "min_rating")) {
        sql += QStringLiteral(" AND library.rating >= :minRating");
        bindings.insert(QStringLiteral(":minRating"), numberOr(params, "min_rating", 0));
    }
    if (hasValue(params, "year_min")) {
        sql += QStringLiteral(" AND CAST(library.year AS INTEGER) >= :yearMin");
        bindings.insert(QStringLiteral(":yearMin"), numberOr(params, "year_min", 0));
    }
    if (hasValue(params, "year_max")) {
        sql += QStringLiteral(" AND CAST(library.year AS INTEGER) <= :yearMax");
        bindings.insert(QStringLiteral(":yearMax"), numberOr(params, "year_max", 0));
    }
    if (hasValue(params, "key")) {
        const QString keyText = stringOr(params, "key");
        const ChromaticKey key = keyFromText(keyText);
        if (key == mixxx::track::io::key::INVALID) {
            fail(QStringLiteral("cannot parse key '%1'").arg(keyText));
        }
        sql += QStringLiteral(" AND library.key_id = :keyId");
        bindings.insert(QStringLiteral(":keyId"), static_cast<int>(key));
    }

    sql += orderByClause(stringOr(params, "sort", QStringLiteral("relevance")));
    sql += QStringLiteral(" LIMIT :limit OFFSET :offset");
    bindings.insert(QStringLiteral(":limit"), limit);
    bindings.insert(QStringLiteral(":offset"), offset);

    QSqlQuery query(database);
    query.prepare(sql);
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        query.bindValue(it.key(), it.value());
    }
    if (!query.exec()) {
        fail(QStringLiteral("library query failed: %1").arg(query.lastError().text()),
                kErrorInternal);
    }

    QJsonArray tracks;
    while (query.next()) {
        tracks.append(trackRowToJson(query));
    }
    return QJsonObject{{QStringLiteral("tracks"), tracks},
            {QStringLiteral("count"), static_cast<int>(tracks.size())},
            {QStringLiteral("offset"), offset}};
}

QJsonValue McpService::getTrack(const QJsonObject& params) {
    const int trackId = numberOr(params, "track_id", -1);
    if (trackId < 0) {
        fail(QStringLiteral("'track_id' is required"));
    }
    const QHash<int, QJsonObject> tracks = tracksByIds(
            m_pTrackCollectionManager->internalCollection()->database(), {trackId});
    const auto it = tracks.constFind(trackId);
    if (it == tracks.constEnd()) {
        fail(QStringLiteral("no track with id %1 in the library").arg(trackId));
    }
    return *it;
}

QJsonValue McpService::suggestNext(const QJsonObject& params) {
    double seedBpm = 0.0;
    ChromaticKey seedKey = mixxx::track::io::key::INVALID;
    QJsonObject seed;

    if (hasValue(params, "deck")) {
        const QString group = deckGroup(params);
        seedBpm = controlGet(group, QStringLiteral("bpm"));
        seedKey = keyFromNumeric(controlGet(group, QStringLiteral("key")));
        seed.insert(QStringLiteral("from_deck"), numberOr(params, "deck", 0));
    } else if (hasValue(params, "track_id")) {
        const int trackId = numberOr(params, "track_id", -1);
        const QHash<int, QJsonObject> tracks = tracksByIds(
                m_pTrackCollectionManager->internalCollection()->database(), {trackId});
        const auto it = tracks.constFind(trackId);
        if (it == tracks.constEnd()) {
            fail(QStringLiteral("no track with id %1 in the library").arg(trackId));
        }
        seedBpm = it->value(QStringLiteral("bpm")).toDouble();
        seedKey = keyFromText(it->value(QStringLiteral("key")).toString());
        seed.insert(QStringLiteral("from_track_id"), trackId);
    }
    if (const auto bpm = optNumber(params, "bpm")) {
        seedBpm = *bpm;
    }
    if (hasValue(params, "key")) {
        seedKey = keyFromText(stringOr(params, "key"));
    }
    if (seedBpm <= 0.0) {
        fail(QStringLiteral("no seed tempo: pass a deck with a loaded track, a track_id, or bpm"));
    }
    seed.insert(QStringLiteral("bpm"), seedBpm);
    insertKey(seed, seedKey);

    const double tolerance = std::clamp(
            optNumber(params, "bpm_tolerance").value_or(6.0), 0.0, 50.0);
    const bool harmonicOnly = boolOr(params, "harmonic_only", true) &&
            seedKey != mixxx::track::io::key::INVALID;
    const int limit = std::clamp(numberOr(params, "limit", 20), 1, 100);

    QString sql = QStringLiteral("SELECT %1%2 AND library.bpm BETWEEN :bpmMin AND :bpmMax")
                          .arg(QLatin1String(kTrackColumns), QLatin1String(kTrackFrom));
    QHash<QString, QVariant> bindings;
    bindings.insert(QStringLiteral(":bpmMin"), seedBpm * (1.0 - tolerance / 100.0));
    bindings.insert(QStringLiteral(":bpmMax"), seedBpm * (1.0 + tolerance / 100.0));

    if (harmonicOnly) {
        // Same key, relative major/minor and the neighbouring fifths:
        // the moves that do not clash when two tracks overlap.
        QStringList keyIds;
        for (const ChromaticKey& compatible : KeyUtils::getCompatibleKeys(seedKey)) {
            keyIds.append(QString::number(static_cast<int>(compatible)));
        }
        if (!keyIds.isEmpty()) {
            sql += QStringLiteral(" AND library.key_id IN (%1)").arg(keyIds.join(QChar(',')));
        }
    }
    if (hasValue(params, "genre")) {
        sql += QStringLiteral(" AND library.genre LIKE :genre");
        bindings.insert(QStringLiteral(":genre"), likePattern(stringOr(params, "genre")));
    }
    if (boolOr(params, "exclude_played", true) && !m_playedTrackIds.isEmpty()) {
        QStringList played;
        for (int id : m_playedTrackIds) {
            played.append(QString::number(id));
        }
        sql += QStringLiteral(" AND library.id NOT IN (%1)").arg(played.join(QChar(',')));
    }
    // Closest tempo first: the least work to beatmatch.
    sql += QStringLiteral(" ORDER BY ABS(library.bpm - :seedBpm) LIMIT :limit");
    bindings.insert(QStringLiteral(":seedBpm"), seedBpm);
    bindings.insert(QStringLiteral(":limit"), limit);

    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.prepare(sql);
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        query.bindValue(it.key(), it.value());
    }
    if (!query.exec()) {
        fail(QStringLiteral("suggestion query failed: %1").arg(query.lastError().text()),
                kErrorInternal);
    }

    QJsonArray candidates;
    while (query.next()) {
        QJsonObject candidate = trackRowToJson(query);
        const double bpm = candidate.value(QStringLiteral("bpm")).toDouble();
        // How far the incoming track has to be pitched to match the seed.
        candidate.insert(QStringLiteral("tempo_change_percent"),
                bpm > 0.0 ? QJsonValue((seedBpm - bpm) / bpm * 100.0)
                          : QJsonValue());
        candidates.append(candidate);
    }
    return QJsonObject{{QStringLiteral("seed"), seed},
            {QStringLiteral("harmonic_only"), harmonicOnly},
            {QStringLiteral("bpm_tolerance"), tolerance},
            {QStringLiteral("candidates"), candidates}};
}

QJsonValue McpService::listPlaylists(const QJsonObject& params) {
    Q_UNUSED(params);
    PlaylistDAO& playlistDao =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    QJsonArray playlists;
    for (const auto& [id, name] : playlistDao.getPlaylists(PlaylistDAO::PLHT_NOT_HIDDEN)) {
        playlists.append(QJsonObject{{QStringLiteral("playlist_id"), id},
                {QStringLiteral("name"), name},
                {QStringLiteral("track_count"), playlistDao.tracksInPlaylist(id)}});
    }
    return QJsonObject{{QStringLiteral("playlists"), playlists}};
}

QJsonValue McpService::getPlaylist(const QJsonObject& params) {
    PlaylistDAO& playlistDao =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    int playlistId = numberOr(params, "playlist_id", -1);
    if (playlistId < 0) {
        const QString name = requireString(params, "name");
        playlistId = playlistDao.getPlaylistIdFromName(name);
        if (playlistId < 0) {
            fail(QStringLiteral("no playlist named '%1'").arg(name));
        }
    }
    if (!playlistDao.playlistExists(playlistId)) {
        fail(QStringLiteral("no playlist with id %1").arg(playlistId));
    }

    const int limit = std::clamp(numberOr(params, "limit", 100), 1, 500);
    QList<int> ids;
    for (const TrackId& trackId : playlistDao.getTrackIdsInPlaylistOrder(playlistId)) {
        if (ids.size() >= limit) {
            break;
        }
        ids.append(trackIdValue(trackId));
    }
    const QHash<int, QJsonObject> metadata = tracksByIds(
            m_pTrackCollectionManager->internalCollection()->database(), ids);

    QJsonArray tracks;
    int position = 1;
    for (int id : ids) {
        QJsonObject track = metadata.value(id);
        track.insert(QStringLiteral("position"), position++);
        tracks.append(track);
    }
    return QJsonObject{{QStringLiteral("playlist_id"), playlistId},
            {QStringLiteral("name"), playlistDao.getPlaylistName(playlistId)},
            {QStringLiteral("tracks"), tracks}};
}

QJsonValue McpService::listCrates(const QJsonObject& params) {
    Q_UNUSED(params);
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.prepare(QStringLiteral(
            "SELECT %1.%2 AS id, %1.%3 AS name,"
            " (SELECT COUNT(*) FROM %4 WHERE %4.%5 = %1.%2) AS track_count"
            " FROM %1 ORDER BY %1.%3 COLLATE NOCASE")
                          .arg(QStringLiteral(CRATE_TABLE),
                                  CRATETABLE_ID,
                                  CRATETABLE_NAME,
                                  QStringLiteral(CRATE_TRACKS_TABLE),
                                  CRATETRACKSTABLE_CRATEID));
    if (!query.exec()) {
        fail(QStringLiteral("crate query failed: %1").arg(query.lastError().text()),
                kErrorInternal);
    }
    QJsonArray crates;
    while (query.next()) {
        crates.append(QJsonObject{
                {QStringLiteral("crate_id"), query.value(QStringLiteral("id")).toInt()},
                {QStringLiteral("name"), query.value(QStringLiteral("name")).toString()},
                {QStringLiteral("track_count"),
                        query.value(QStringLiteral("track_count")).toInt()}});
    }
    return QJsonObject{{QStringLiteral("crates"), crates}};
}

QJsonValue McpService::getCrate(const QJsonObject& params) {
    const QSqlDatabase database = m_pTrackCollectionManager->internalCollection()->database();
    int crateId = numberOr(params, "crate_id", -1);
    if (crateId < 0) {
        const QString name = requireString(params, "name");
        QSqlQuery lookup(database);
        lookup.prepare(QStringLiteral("SELECT %1 FROM %2 WHERE %3 = :name")
                               .arg(CRATETABLE_ID, QStringLiteral(CRATE_TABLE), CRATETABLE_NAME));
        lookup.bindValue(QStringLiteral(":name"), name);
        if (!lookup.exec() || !lookup.next()) {
            fail(QStringLiteral("no crate named '%1'").arg(name));
        }
        crateId = lookup.value(0).toInt();
    }

    const int limit = std::clamp(numberOr(params, "limit", 100), 1, 500);
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1%2 AND library.id IN"
                                 " (SELECT %3 FROM %4 WHERE %5 = :crateId)"
                                 " ORDER BY library.artist COLLATE NOCASE LIMIT :limit")
                          .arg(QLatin1String(kTrackColumns),
                                  QLatin1String(kTrackFrom),
                                  CRATETRACKSTABLE_TRACKID,
                                  QStringLiteral(CRATE_TRACKS_TABLE),
                                  CRATETRACKSTABLE_CRATEID));
    query.bindValue(QStringLiteral(":crateId"), crateId);
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        fail(QStringLiteral("crate query failed: %1").arg(query.lastError().text()),
                kErrorInternal);
    }
    QJsonArray tracks;
    while (query.next()) {
        tracks.append(trackRowToJson(query));
    }
    return QJsonObject{{QStringLiteral("crate_id"), crateId},
            {QStringLiteral("tracks"), tracks}};
}

// --- auto dj -------------------------------------------------------------

QJsonValue McpService::autoDj(const QJsonObject& params) {
    const QString group = QStringLiteral("[AutoDJ]");
    const QString action = stringOr(params, "action", QStringLiteral("status"));
    PlaylistDAO& playlistDao =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();

    if (action == QLatin1String("enable") || action == QLatin1String("disable")) {
        controlSet(group,
                QStringLiteral("enabled"),
                action == QLatin1String("enable") ? 1.0 : 0.0);
    } else if (action == QLatin1String("fade_now")) {
        controlTrigger(group, QStringLiteral("fade_now"));
    } else if (action == QLatin1String("skip_next")) {
        controlTrigger(group, QStringLiteral("skip_next"));
    } else if (action == QLatin1String("shuffle")) {
        controlTrigger(group, QStringLiteral("shuffle_playlist"));
    } else if (action == QLatin1String("add_random")) {
        controlTrigger(group, QStringLiteral("add_random_track"));
    } else if (action == QLatin1String("clear")) {
        playlistDao.clearAutoDJQueue();
    } else if (action != QLatin1String("status")) {
        fail(QStringLiteral("unknown Auto DJ action '%1'").arg(action));
    }

    const int autoDjId = autoDjPlaylistId();
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("enabled"), controlGet(group, QStringLiteral("enabled")) > 0.0},
            {QStringLiteral("queue_length"),
                    autoDjId >= 0 ? playlistDao.tracksInPlaylist(autoDjId) : 0}};
}

QJsonValue McpService::autoDjQueue(const QJsonObject& params) {
    const int autoDjId = autoDjPlaylistId();
    if (autoDjId < 0) {
        fail(QStringLiteral("the Auto DJ playlist is missing from this library"),
                kErrorInternal);
    }
    const int limit = std::clamp(numberOr(params, "limit", 50), 1, 500);
    QList<int> ids;
    for (const TrackId& trackId :
            m_pTrackCollectionManager->internalCollection()
                    ->getPlaylistDAO()
                    .getTrackIdsInPlaylistOrder(autoDjId)) {
        if (ids.size() >= limit) {
            break;
        }
        ids.append(trackIdValue(trackId));
    }
    const QHash<int, QJsonObject> metadata = tracksByIds(
            m_pTrackCollectionManager->internalCollection()->database(), ids);

    QJsonArray queue;
    int position = 1;
    for (int id : ids) {
        QJsonObject track = metadata.value(id);
        // 1-based, matching the positions mixxx.autodj_edit expects.
        track.insert(QStringLiteral("position"), position++);
        queue.append(track);
    }
    return QJsonObject{{QStringLiteral("queue"), queue},
            {QStringLiteral("count"), static_cast<int>(queue.size())}};
}

QJsonValue McpService::autoDjAdd(const QJsonObject& params) {
    const QJsonArray rawIds = params.value(QStringLiteral("track_ids")).toArray();
    if (rawIds.isEmpty()) {
        fail(QStringLiteral("'track_ids' must be a non-empty array of library track ids"));
    }
    QList<TrackId> trackIds;
    trackIds.reserve(rawIds.size());
    for (const QJsonValue& value : rawIds) {
        const TrackId trackId{QVariant(static_cast<int>(value.toDouble(-1)))};
        if (!trackId.isValid()) {
            fail(QStringLiteral("'%1' is not a valid track id")
                            .arg(value.toVariant().toString()));
        }
        trackIds.append(trackId);
    }
    const QString position = stringOr(params, "position", QStringLiteral("bottom"));
    m_pTrackCollectionManager->internalCollection()->getPlaylistDAO().addTracksToAutoDJQueue(
            trackIds, autoDjLocation(position));
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("added"), static_cast<int>(trackIds.size())},
            {QStringLiteral("position"), position}};
}

QJsonValue McpService::autoDjEdit(const QJsonObject& params) {
    const int autoDjId = autoDjPlaylistId();
    if (autoDjId < 0) {
        fail(QStringLiteral("the Auto DJ playlist is missing from this library"),
                kErrorInternal);
    }
    PlaylistDAO& playlistDao =
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    const int queueLength = playlistDao.tracksInPlaylist(autoDjId);
    const QString action = requireString(params, "action");
    const int position = numberOr(params, "position", 0);
    if (position < 1 || position > queueLength) {
        fail(QStringLiteral("position %1 is outside the queue (1..%2)")
                        .arg(position)
                        .arg(queueLength));
    }

    if (action == QLatin1String("remove")) {
        playlistDao.removeTrackFromPlaylist(autoDjId, position);
    } else if (action == QLatin1String("move")) {
        const int destination = numberOr(params, "to", 0);
        if (destination < 1 || destination > queueLength) {
            fail(QStringLiteral("'to' must be between 1 and %1").arg(queueLength));
        }
        playlistDao.moveTrack(autoDjId, position, destination);
    } else {
        fail(QStringLiteral("unknown action '%1' (use remove or move)").arg(action));
    }
    return QJsonObject{{QStringLiteral("ok"), true}};
}

// --- raw control surface -------------------------------------------------

QJsonValue McpService::getControl(const QJsonObject& params) {
    const QString group = requireString(params, "group");
    const QString key = requireString(params, "key");
    const ConfigKey configKey(group, key);
    if (!ControlObject::exists(configKey)) {
        fail(QStringLiteral("no control %1 %2").arg(group, key));
    }
    return QJsonObject{{QStringLiteral("group"), group},
            {QStringLiteral("key"), key},
            {QStringLiteral("value"), ControlObject::get(configKey)}};
}

QJsonValue McpService::setControl(const QJsonObject& params) {
    const QString group = requireString(params, "group");
    const QString key = requireString(params, "key");
    const double value = requireNumber(params, "value");
    const ConfigKey configKey(group, key);
    if (!ControlObject::exists(configKey)) {
        fail(QStringLiteral("no control %1 %2").arg(group, key));
    }
    ControlObject::set(configKey, value);
    return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("group"), group},
            {QStringLiteral("key"), key},
            {QStringLiteral("value"), ControlObject::get(configKey)}};
}
