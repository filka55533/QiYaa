#include "jam/host_session.h"

#include "core/jam_mode.h"
#include "jam/codec.h"
#include "yandex/api_client.h"
#include "yandex/library.h"

#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace Jam {

namespace {

constexpr int kMaxTitleLength = 150;
constexpr int kMaxArtists = 10;
constexpr int kMaxArtistLength = 64;
constexpr int kMaxCoverLength = 300;
constexpr qint64 kMaxDurationMs = 86'400'000;

const QStringList kGoneReasons = {QStringLiteral("room-not-found"), QStringLiteral("bad-secret")};

const QRegularExpression& CatalogId() {
    static const QRegularExpression pattern(QStringLiteral("^[0-9A-Za-z_-]{1,64}$"));
    return pattern;
}

QString Clean(QString text) {
    for (QChar& character : text) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7F) {
            character = QLatin1Char(' ');
        }
    }
    return text.trimmed();
}

// The Yandex template of a cover: no scheme, "%%" for the size.
bool IsCoverTemplate(const QString& uri) {
    if (uri.size() > kMaxCoverLength || !uri.contains(QStringLiteral("%%"))) {
        return false;
    }
    return std::none_of(uri.cbegin(), uri.cend(), [](QChar character) {
        return character.unicode() <= 0x20 || character.unicode() == 0x7F;
    });
}

QString Cut(const QString& text, int maximum) {
    if (text.size() <= maximum) {
        return text;
    }
    const int end = text.at(maximum - 1).isHighSurrogate() ? maximum - 1 : maximum;
    return text.left(end).trimmed();
}

}  // namespace

std::optional<Track> JamTrackOf(const Yandex::Track& track) {
    if (!CatalogId().match(track.id).hasMatch()) {
        return std::nullopt;
    }
    Track jamTrack;
    jamTrack.id = track.id;
    jamTrack.albumId = CatalogId().match(track.albumId).hasMatch() ? track.albumId : QString();
    jamTrack.title = Cut(Clean(track.title), kMaxTitleLength);
    if (jamTrack.title.isEmpty()) {
        return std::nullopt;
    }
    for (const QString& artist : track.artists) {
        const QString name = Cut(Clean(artist), kMaxArtistLength);
        if (!name.isEmpty() && jamTrack.artists.size() < kMaxArtists) {
            jamTrack.artists << name;
        }
    }
    jamTrack.durationMs = std::clamp<qint64>(track.durationMs, 0, kMaxDurationMs);
    if (IsCoverTemplate(track.coverUri)) {
        jamTrack.coverUri = track.coverUri;
    }
    return jamTrack;
}

Yandex::Track YandexTrackOf(const Track& track) {
    Yandex::Track yandexTrack;
    yandexTrack.id = track.id;
    yandexTrack.albumId = track.albumId;
    yandexTrack.title = track.title;
    yandexTrack.artists = track.artists;
    yandexTrack.durationMs = track.durationMs;
    yandexTrack.coverUri = track.coverUri;
    yandexTrack.available = true;
    return yandexTrack;
}

HostSession::HostSession(
    Core::JamMode* jamMode,
    Yandex::Library* library,
    SessionStore sessionStore,
    HostOptions hostOptions,
    QObject* parent
)
    : QObject(parent)
    , jam(jamMode)
    , yandexLibrary(library)
    , store(std::move(sessionStore))
    , options(std::move(hostOptions))
    , client(options.client) {
    if (!options.newId) {
        options.newId = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    }
    if (!options.config) {
        options.config = [] { return HostConfig{}; };
    }
    stored = store.load();
    retryTimer.setSingleShot(true);
    connect(&retryTimer, &QTimer::timeout, this, [this] {
        if (session) {
            connectTo(serverUrl);
        }
    });
    connect(&client, &Client::welcomed, this, &HostSession::welcomed);
    connect(&client, &Client::messageReceived, this, &HostSession::received);
    connect(&client, &Client::statusChanged, this, [this](Status status) {
        if (status != Status::Online) {
            connected = false;
        }
        Q_EMIT changed();
    });
    connect(&client, &Client::outboxChanged, this, [this](const QStringList& outbox) {
        if (session) {
            Session updated = *session;
            updated.outbox = outbox;
            save(updated);  // HOST-22
        }
    });
    connect(jam, &Core::JamMode::itemStarted, this, [this](const QString& itemId) {
        if (session) {
            client.started(itemId, connected);  // HOST-25: kept in the outbox until accepted
        }
    });
    connect(jam, &Core::JamMode::playback, this, &HostSession::sendPlaying);
}

HostSession::~HostSession() = default;

HostPhase HostSession::phase() const {
    if (session) {
        return HostPhase::Active;
    }
    return creating ? HostPhase::Creating : HostPhase::None;
}

QString HostSession::joinUrl() const {
    return session ? session->joinUrl : QString();
}

bool HostSession::create(const QString& hostName, const std::optional<SettingsPatch>& settings) {
    if (session || creating) {
        return false;
    }
    const HostConfig config = options.config();
    const QString name = hostName.trimmed().left(kMaxHostNameLength).trimmed();
    if (config.serverUrl.trimmed().isEmpty()) {
        return false;
    }
    // A message the server would refuse closes the connection, and a reconnect would send it again.
    if (!DecodeClient(Encode(Create{QStringLiteral("probe"), name, settings})).isValid()) {
        return false;
    }
    if (discarding) {
        finishDiscard();
    }
    creating = CreateRequest{name, settings};
    connectTo(config.serverUrl);
    Q_EMIT changed();
    return true;
}

void HostSession::cancelCreate() {
    if (!creating) {
        return;
    }
    creating.reset();
    dropConnection();
    Q_EMIT changed();
}

void HostSession::continueStored() {
    if (!stored) {
        return;
    }
    session = std::exchange(stored, std::nullopt);
    const HostConfig config = options.config();
    client.restoreOutbox(session->outbox);
    jam->start(options.queueTitle, config.waveFeedback);
    connectTo(config.serverUrl);
    Q_EMIT changed();
}

void HostSession::discardStored() {
    if (!stored) {
        return;
    }
    const Session old = *std::exchange(stored, std::nullopt);
    store.clear();
    const HostConfig config = options.config();
    if (!config.serverUrl.trimmed().isEmpty()) {
        discarding = old;  // HOST-23 "no": resume only to end it, so the guests see the end
        connectTo(config.serverUrl);
    }
    Q_EMIT changed();
}

void HostSession::end() {
    if (!session) {
        return;
    }
    if (connected) {
        client.send(End{nextId()});
    }
    endLocally(HostEnd::ByHost);
}

bool HostSession::add(const Yandex::Track& track) {
    const std::optional<Track> jamTrack = JamTrackOf(track);
    return jamTrack && request([&](const QString& id) { return Add{id, {}, *jamTrack}; });
}

bool HostSession::playNext(const Yandex::Track& track) {
    if (currentRoom) {
        for (const QueueItem& item : currentRoom->queue) {
            if (item.track.id == track.id) {
                return pin(item.itemId);
            }
        }
    }
    const std::optional<Track> jamTrack = JamTrackOf(track);
    const QString id = nextId();
    if (!jamTrack || !connected || !client.send(Add{id, {}, *jamTrack})) {
        return false;
    }
    pendingPinTrack = track.id;
    pendingPinAdd = id;
    return true;
}

bool HostSession::pin(const QString& itemId) {
    return request([&](const QString& id) { return Pin{id, itemId}; });
}

bool HostSession::remove(const QString& itemId) {
    return request([&](const QString& id) { return Remove{id, itemId}; });
}

bool HostSession::kick(const QString& publicId) {
    return request([&](const QString& id) { return Kick{id, publicId}; });
}

bool HostSession::changeSettings(const SettingsPatch& patch) {
    return request([&](const QString& id) { return ChangeSettings{id, patch}; });
}

bool HostSession::rotateLink() {
    return request([](const QString& id) { return RotateLink{id}; });
}

void HostSession::networkBack() {
    client.networkBack();
}

void HostSession::welcomed() {
    connected = false;
    lastVersion = -1;  // the first state of a new connection is taken as it is
    if (session) {
        sendResume(*session, client.outbox());
    } else if (discarding) {
        sendResume(*discarding, discarding->outbox);
    } else if (creating) {
        requestId = nextId();
        client.send(Create{requestId, creating->hostName, creating->settings});
    }
    Q_EMIT changed();
}

void HostSession::received(const ServerMessage& message) {
    if (const auto* created = std::get_if<Created>(&message)) {
        if (created->id == requestId) {
            this->created(*created);
        }
    } else if (const auto* resumedMessage = std::get_if<Resumed>(&message)) {
        if (resumedMessage->id == requestId) {
            resumed();
        }
    } else if (const auto* rejectedMessage = std::get_if<Rejected>(&message)) {
        rejected(*rejectedMessage);
    } else if (const auto* ack = std::get_if<Ack>(&message)) {
        if (!endId.isEmpty() && ack->id == endId) {
            finishDiscard();
        }
    } else if (const auto* state = std::get_if<State>(&message)) {
        applyState(*state);
    } else if (const auto* snapshot = std::get_if<Snapshot>(&message)) {
        if (session) {
            Session updated = *session;
            updated.snapshot = snapshot->data;
            save(updated);
        }
    } else if (const auto* rotated = std::get_if<LinkRotated>(&message)) {
        if (session) {
            Session updated = *session;
            updated.joinUrl = rotated->joinUrl;
            save(updated);
        }
    } else if (const auto* searchRequest = std::get_if<SearchRequest>(&message)) {
        search(*searchRequest);
    } else if (const auto* validateRequest = std::get_if<ValidateRequest>(&message)) {
        validate(*validateRequest);
    } else if (const auto* command = std::get_if<Command>(&message)) {
        if (session && command->kind == CommandKind::Skip) {
            jam->skip(command->itemId);  // HOST-18, HOST-19
        }
    } else if (const auto* endedMessage = std::get_if<Ended>(&message)) {
        serverEnded(endedMessage->reason);
    }
    Q_EMIT changed();
}

void HostSession::created(const Created& message) {
    creating.reset();
    requestId.clear();
    connected = true;
    save(Session{message.roomId, message.hostSecret, message.joinUrl, std::nullopt, {}});
    jam->start(options.queueTitle, options.config().waveFeedback);
}

void HostSession::resumed() {
    requestId.clear();
    if (discarding) {
        endId = nextId();
        client.send(End{endId});
        return;
    }
    connected = true;
    client.clearOutbox();
    jam->reportPlayback();  // HOST-26
}

void HostSession::rejected(const Rejected& message) {
    const QString& id = message.id;
    if (!id.isEmpty() && id == pendingPinAdd) {
        pendingPinTrack.clear();
        pendingPinAdd.clear();
        Q_EMIT refused(message.reason);
    } else if (!id.isEmpty() && (id == endId || (id == requestId && discarding))) {
        finishDiscard();
    } else if (!id.isEmpty() && id == requestId && creating) {
        creating.reset();
        requestId.clear();
        dropConnection();
        Q_EMIT refused(message.reason);
    } else if (!id.isEmpty() && id == requestId && session) {
        requestId.clear();
        if (kGoneReasons.contains(message.reason)) {
            endLocally(HostEnd::Gone);  // HOST-24
        } else {
            Q_EMIT refused(message.reason);
            retryLater();
        }
    } else {
        Q_EMIT refused(message.reason);
    }
}

void HostSession::applyState(const State& message) {
    if (!session || !connected) {
        return;
    }
    if (lastVersion >= 0 && message.version <= lastVersion) {
        return;
    }
    lastVersion = message.version;
    currentRoom = message.room;
    QList<Core::JamEntry> entries;
    for (const QueueItem& item : message.room.queue) {
        entries << Core::JamEntry{item.itemId, YandexTrackOf(item.track), item.addedBy};
    }
    jam->setQueue(entries, message.room.fallback.seeds, message.room.fallback.seedsVersion);
    if (pendingPinTrack.isEmpty()) {
        return;
    }
    for (const QueueItem& item : message.room.queue) {
        if (item.track.id == pendingPinTrack && item.addedBy == message.room.you.publicId) {
            pendingPinTrack.clear();
            pendingPinAdd.clear();
            if (!item.pinned) {
                pin(item.itemId);  // HOST-20: "play next" is add, then pin
            }
            return;
        }
    }
}

void HostSession::search(const SearchRequest& message) {
    QPointer<HostSession> self(this);
    const QString id = message.requestId;
    yandexLibrary->searchTracks(
        message.text,
        [self, id](const QList<Yandex::Track>& tracks, const Yandex::RequestError& error) {
            if (!self) {
                return;
            }
            SearchResult reply;
            reply.requestId = id;
            if (error.isError()) {
                qWarning("jam: a guest's search failed: %s", qPrintable(error.text));
                const bool unauthorized = error.httpStatus == 401 || error.httpStatus == 403;
                reply.error = unauthorized ? SearchError::Unauthorized : SearchError::Failed;
            } else {
                std::vector<Track> found;
                for (const Yandex::Track& track : tracks) {
                    const std::optional<Track> jamTrack =
                        track.available ? JamTrackOf(track) : std::nullopt;
                    if (jamTrack && found.size() < static_cast<size_t>(kMaxSearchResults)) {
                        found.push_back(*jamTrack);
                    }
                }
                reply.tracks = found;
            }
            if (self->connected) {
                self->client.send(reply);  // HOST-28, HOST-29
            }
        }
    );
}

void HostSession::validate(const ValidateRequest& message) {
    QPointer<HostSession> self(this);
    const QString id = message.requestId;
    const QStringList ids = message.trackIds;
    yandexLibrary->tracksByIds(
        ids,
        [self, id, ids](const QList<Yandex::Track>& tracks, const QString& error) {
            if (!self) {
                return;
            }
            QHash<QString, Yandex::Track> byId;
            for (const Yandex::Track& track : tracks) {
                byId.insert(track.id, track);
            }
            ValidateResult reply;
            reply.requestId = id;
            for (const QString& trackId : ids) {
                ValidateEntry entry;
                entry.trackId = trackId;
                const auto found = byId.constFind(trackId);
                const std::optional<Track> jamTrack =
                    error.isEmpty() && found != byId.constEnd() && found->available
                    ? JamTrackOf(*found)
                    : std::nullopt;
                if (!error.isEmpty()) {
                    entry.reason = ValidateReason::Failed;
                } else if (jamTrack) {
                    entry.track = jamTrack;
                } else {
                    entry.reason = ValidateReason::TrackUnavailable;
                }
                reply.results.push_back(entry);
            }
            if (!error.isEmpty()) {
                qWarning("jam: a guest's track check failed: %s", qPrintable(error));
            }
            if (self->connected) {
                self->client.send(reply);  // HOST-30
            }
        }
    );
}

void HostSession::serverEnded(EndReason reason) {
    if (discarding) {
        return finishDiscard();
    }
    if (session) {
        endLocally(reason == EndReason::Expired ? HostEnd::Expired : HostEnd::ByServer);
    }
}

void HostSession::sendPlaying(const Core::JamPlayback& playback) {
    if (!connected || !session) {
        return;  // HOST-25: playing is not queued
    }
    Playing message;
    message.positionMs = std::max<qint64>(0, playback.positionMs);
    message.paused = playback.paused;
    switch (playback.kind) {
        case Core::JamPlayback::Kind::Item:
            message.source = Source::Item;
            message.itemId = playback.itemId;
            break;
        case Core::JamPlayback::Kind::Wave:
            message.source = Source::Wave;
            message.track = playback.track ? JamTrackOf(*playback.track) : std::nullopt;
            if (!message.track) {
                return;  // a wave track the protocol cannot carry is not reported
            }
            break;
        case Core::JamPlayback::Kind::Idle:
            message.source = Source::Idle;
            message.positionMs = 0;
            message.paused = true;
            break;
    }
    // Listening along (LISTEN-01 to LISTEN-04): only Yandex files, which the server takes.
    if (message.source != Source::Idle && options.config().shareAudio) {
        const QString link = playback.link.toString();
        const QString next = playback.nextLink.toString();
        message.listenUrl = IsListenUrl(link) ? link : QString();
        message.listenNextUrl = IsListenUrl(next) ? next : QString();
    }
    client.send(message);
}

void HostSession::sendResume(const Session& resumed, const QStringList& outbox) {
    requestId = nextId();
    client.send(Resume{
        requestId, resumed.roomId, resumed.hostSecret, resumed.snapshot,
        outbox.mid(std::max<qsizetype>(0, outbox.size() - kMaxOutboxEvents))
    });
}

bool HostSession::request(const std::function<ClientMessage(const QString& id)>& build) {
    return connected && client.send(build(nextId()));
}

QString HostSession::nextId() {
    return options.newId();
}

void HostSession::endLocally(HostEnd why) {
    session.reset();
    pendingPinTrack.clear();
    pendingPinAdd.clear();
    requestId.clear();
    dropConnection();
    client.clearOutbox();
    store.clear();
    jam->end();
    currentRoom.reset();
    Q_EMIT ended(why);
    Q_EMIT changed();
}

void HostSession::finishDiscard() {
    discarding.reset();
    endId.clear();
    requestId.clear();
    if (!session && !creating) {
        dropConnection();
    }
}

void HostSession::connectTo(const QString& url) {
    retryTimer.stop();
    if (url.trimmed().isEmpty()) {
        return;  // no server set: the jam plays on without one
    }
    serverUrl = url;
    client.start(Client::SocketUrl(url));
}

void HostSession::dropConnection() {
    retryTimer.stop();
    connected = false;
    client.stop();
}

void HostSession::retryLater() {
    dropConnection();
    retryTimer.start(options.resumeRetryMs);
}

void HostSession::save(Session updated) {
    session = std::move(updated);
    store.save(*session);
}

}  // namespace Jam
