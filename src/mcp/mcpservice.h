#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <optional>

#include "mcp-bridge/mcp_bridge.h"
#include "preferences/usersettings.h"
#include "rust/cxx.h"
#include "track/track_decl.h"

class ControlProxy;
class PlayerManager;
class TrackCollectionManager;

/// Serves the Model Context Protocol bridge: a loopback JSON-RPC endpoint
/// that lets an AI agent (through the `mixxx-mcp` CLI) drive the decks,
/// the mixer, the library and Auto DJ.
///
/// The Rust side owns the socket and its runtime; requests arrive on a
/// worker thread and are marshalled here by the request handler, so every
/// method below runs on this object's thread and may touch Qt and the
/// database freely. Answers travel back through respond().
///
/// Disabled with `[Mcp] Enabled = 0`; `[Mcp] Port` pins the port (0, the
/// default, lets the OS choose and relies on the endpoint descriptor for
/// discovery).
class McpService : public QObject {
    Q_OBJECT
  public:
    McpService(UserSettingsPointer pConfig,
            PlayerManager* pPlayerManager,
            TrackCollectionManager* pTrackCollectionManager,
            QObject* pParent = nullptr);
    ~McpService() override;

    bool isActive() const {
        return m_pServer.has_value();
    }

    /// Port the endpoint listens on, or 0 when inactive.
    quint16 port() const;

    /// Entry point for the Rust runtime, already marshalled to this
    /// object's thread by the request handler.
    void handleRequest(quint64 id, const QString& method, const QString& params);

  private slots:
    void slotPlayingTrackChanged(TrackPointer pTrack);
    void slotAutoDJQueueChanged(const QSet<int>& playlistIds);
    void slotDeckPlayChanged(int deck, double value);

  private:
    using Handler = QJsonValue (McpService::*)(const QJsonObject&);

    void registerHandlers();
    void connectEventSources();
    void respond(quint64 id, const QJsonValue& result);
    void respondError(quint64 id, int code, const QString& message);
    void publishEvent(const QString& type, const QJsonObject& detail = {});

    // --- request handlers (one per RPC method) ----------------------
    QJsonValue getState(const QJsonObject& params);
    QJsonValue getDeck(const QJsonObject& params);
    QJsonValue play(const QJsonObject& params);
    QJsonValue cue(const QJsonObject& params);
    QJsonValue seek(const QJsonObject& params);
    QJsonValue beatjump(const QJsonObject& params);
    QJsonValue loadTrack(const QJsonObject& params);
    QJsonValue eject(const QJsonObject& params);
    QJsonValue cloneDeck(const QJsonObject& params);
    QJsonValue setVolume(const QJsonObject& params);
    QJsonValue setGain(const QJsonObject& params);
    QJsonValue setCrossfader(const QJsonObject& params);
    QJsonValue setEq(const QJsonObject& params);
    QJsonValue setRate(const QJsonObject& params);
    QJsonValue sync(const QJsonObject& params);
    QJsonValue setLoop(const QJsonObject& params);
    QJsonValue hotcue(const QJsonObject& params);
    QJsonValue headphone(const QJsonObject& params);
    QJsonValue searchLibrary(const QJsonObject& params);
    QJsonValue getTrack(const QJsonObject& params);
    QJsonValue suggestNext(const QJsonObject& params);
    QJsonValue listPlaylists(const QJsonObject& params);
    QJsonValue getPlaylist(const QJsonObject& params);
    QJsonValue listCrates(const QJsonObject& params);
    QJsonValue getCrate(const QJsonObject& params);
    QJsonValue autoDj(const QJsonObject& params);
    QJsonValue autoDjQueue(const QJsonObject& params);
    QJsonValue autoDjAdd(const QJsonObject& params);
    QJsonValue autoDjEdit(const QJsonObject& params);
    QJsonValue getControl(const QJsonObject& params);
    QJsonValue setControl(const QJsonObject& params);

    // --- helpers ----------------------------------------------------
    /// Validates a 1-based deck number and returns its "[ChannelN]" group.
    QString deckGroup(const QJsonObject& params, const char* key = "deck") const;
    QJsonObject deckState(int deck) const;
    /// Resolves the track referenced by "track_id" or "location".
    TrackPointer resolveTrack(const QJsonObject& params) const;
    int autoDjPlaylistId() const;

    UserSettingsPointer m_pConfig;
    PlayerManager* const m_pPlayerManager;
    TrackCollectionManager* const m_pTrackCollectionManager;

    QHash<QString, Handler> m_handlers;
    std::optional<rust::Box<mixxxmcp::Server>> m_pServer;

    /// One per deck, so play/pause becomes an event agents can wait on.
    QList<ControlProxy*> m_playProxies;

    /// Tracks played this session, so suggest_next can avoid repeats.
    QSet<int> m_playedTrackIds;
};
