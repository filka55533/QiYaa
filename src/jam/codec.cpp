#include "jam/codec.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <climits>
#include <cmath>
#include <utility>

namespace Jam {

namespace {

// The numbers and patterns of spec/jam/protocol/schemas/defs.schema.json and spec/jam/limits.md.
constexpr int kMaxNameLength = 24;
constexpr int kMaxTitleLength = 150;
constexpr int kMaxArtists = 10;
constexpr int kMaxArtistLength = 64;
constexpr qint64 kMaxDurationMs = 86'400'000;
constexpr int kMaxCoverLength = 300;
constexpr int kMaxSearchLength = 100;
constexpr int kMaxTracksPerReply = 20;
constexpr int kMaxParticipants = 31;
constexpr int kMaxQueue = 300;
constexpr int kMaxRecent = 10;
constexpr int kMaxSeeds = 5;
constexpr int kMaxOutbox = 500;
constexpr int kMaxDetailLength = 200;
constexpr int kMaxJoinUrlLength = 200;
constexpr int kMaxListenUrlLength = 400;
constexpr int kMinPendingPerGuest = 1;
constexpr int kMaxPendingPerGuest = 50;

const QStringList kReasons = {
    QStringLiteral("bad-secret"),      QStringLiteral("room-not-found"),
    QStringLiteral("room-full"),       QStringLiteral("join-closed"),
    QStringLiteral("kicked"),          QStringLiteral("rate-limited"),
    QStringLiteral("queue-limit"),     QStringLiteral("duplicate"),
    QStringLiteral("not-allowed"),     QStringLiteral("stale"),
    QStringLiteral("unknown-track"),   QStringLiteral("track-unavailable"),
    QStringLiteral("host-offline"),    QStringLiteral("host-timeout"),
    QStringLiteral("host-error"),      QStringLiteral("invalid-message"),
    QStringLiteral("update-required"), QStringLiteral("server-full"),
};

const QStringList kSecretFields = {
    QStringLiteral("hostKey"),
    QStringLiteral("hostSecret"),
    QStringLiteral("joinSecret"),
    QStringLiteral("participantId"),
};

const QRegularExpression& Pattern(const char* pattern) {
    static QHash<const char*, QRegularExpression> compiled;
    auto found = compiled.find(pattern);
    if (found == compiled.end()) {
        found = compiled.insert(pattern, QRegularExpression(QString::fromLatin1(pattern)));
    }
    return *found;
}

const char* const kRequestId = "^[A-Za-z0-9_-]{1,36}$";
const char* const kRoomId = "^[0-9a-hjkmnp-tv-z]{8}$";
const char* const kPublicId = "^[0-9a-hjkmnp-tv-z]{6}$";
const char* const kItemId = "^i[1-9][0-9]{0,8}$";
const char* const kCatalogId = "^[0-9A-Za-z_-]{1,64}$";
const char* const kJoinSecret = "^[A-Za-z0-9_-]{22}$";
const char* const kHostSecret = "^[A-Za-z0-9_-]{43}$";
const char* const kParticipantId =
    "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$";
const char* const kAppVersion = "^[0-9A-Za-z.+_-]{1,32}$";
const char* const kSeed = "^track:[0-9A-Za-z_-]{1,64}$";
const char* const kCoverUri = "^\\S*%%\\S*$";
const char* const kJoinUrl =
    "^https?://[^\\s#/]+(?:/[^\\s#]*)?/j/[0-9a-hjkmnp-tv-z]{8}#[A-Za-z0-9_-]{22}$";
const char* const kListenUrl = "^https://[A-Za-z0-9-]+\\.storage\\.yandex\\.net/get-mp3/"
                               "[0-9a-f]{32}/[0-9a-f]{1,32}/[^\\s#?]+$";
const char* const kName = "^[^\\s\\x00-\\x1F\\x7F](?:[^\\x00-\\x1F\\x7F]*[^\\s\\x00-\\x1F\\x7F])?$";

qsizetype CodePoints(const QString& text) {
    return text.toUcs4().size();
}

bool HasControl(const QString& text) {
    for (const QChar character : text) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7F) {
            return true;
        }
    }
    return false;
}

// Reads one JSON object field by field and keeps the first problem: a missing field, a wrong type,
// a value out of its pattern or range. Unknown fields are ignored, as the protocol says.
class Reader {
public:
    Reader(QJsonObject object, QString where, QString* problem)
        : json(std::move(object))
        , place(std::move(where))
        , firstProblem(problem) { }

    bool has(const QString& key) const { return json.contains(key); }

    QString string(const QString& key) {
        const QJsonValue value = json.value(key);
        if (!value.isString()) {
            fail(
                key,
                value.isUndefined() ? QStringLiteral("is missing")
                                    : QStringLiteral("is not a string")
            );
            return {};
        }
        return value.toString();
    }

    QString optionalString(const QString& key) { return has(key) ? string(key) : QString(); }

    QString matching(const QString& key, const char* pattern) {
        const QString value = string(key);
        if (ok() && !Pattern(pattern).match(value).hasMatch()) {
            fail(key, QStringLiteral("does not match ") + QString::fromLatin1(pattern));
        }
        return value;
    }

    QString optionalMatching(const QString& key, const char* pattern) {
        return has(key) ? matching(key, pattern) : QString();
    }

    qint64 integer(const QString& key, qint64 minimum, qint64 maximum) {
        const QJsonValue value = json.value(key);
        if (!value.isDouble()) {
            fail(
                key,
                value.isUndefined() ? QStringLiteral("is missing")
                                    : QStringLiteral("is not a number")
            );
            return 0;
        }
        const double number = value.toDouble();
        if (std::floor(number) != number || number < static_cast<double>(minimum)
            || number > static_cast<double>(maximum)) {
            fail(
                key,
                QStringLiteral("is %1, not a whole number from %2 to %3")
                    .arg(number)
                    .arg(minimum)
                    .arg(maximum)
            );
            return 0;
        }
        return static_cast<qint64>(number);
    }

    qint64 time(const QString& key) { return integer(key, 0, kMaxSafeInteger); }

    bool boolean(const QString& key) {
        const QJsonValue value = json.value(key);
        if (!value.isBool()) {
            fail(
                key,
                value.isUndefined() ? QStringLiteral("is missing")
                                    : QStringLiteral("is not a boolean")
            );
            return false;
        }
        return value.toBool();
    }

    QJsonObject object(const QString& key) {
        const QJsonValue value = json.value(key);
        if (!value.isObject()) {
            fail(
                key,
                value.isUndefined() ? QStringLiteral("is missing")
                                    : QStringLiteral("is not an object")
            );
            return {};
        }
        return value.toObject();
    }

    QJsonArray array(const QString& key, qsizetype minimum, qsizetype maximum) {
        const QJsonValue value = json.value(key);
        if (!value.isArray()) {
            fail(
                key,
                value.isUndefined() ? QStringLiteral("is missing")
                                    : QStringLiteral("is not an array")
            );
            return {};
        }
        const QJsonArray items = value.toArray();
        if (items.size() < minimum || items.size() > maximum) {
            fail(
                key,
                QStringLiteral("has %1 entries, not %2 to %3")
                    .arg(items.size())
                    .arg(minimum)
                    .arg(maximum)
            );
        }
        return items;
    }

    template <typename T>
    T choice(const QString& key, std::initializer_list<std::pair<const char*, T>> options) {
        const QString value = string(key);
        for (const auto& [name, option] : options) {
            if (value == QLatin1String(name)) {
                return option;
            }
        }
        if (ok()) {
            fail(key, QStringLiteral("is \"%1\", not a known value").arg(value));
        }
        return options.begin()->second;
    }

    Reader child(const QJsonObject& object, const QString& key) const {
        return Reader(object, place + QLatin1Char('.') + key, firstProblem);
    }

    void require(bool condition, const QString& problem) {
        if (!condition) {
            fail({}, problem);
        }
    }

    bool ok() const { return firstProblem->isEmpty(); }
    const QString& where() const { return place; }

    static constexpr qint64 kMaxSafeInteger = 9'007'199'254'740'991;

private:
    void fail(const QString& key, const QString& what) {
        if (firstProblem->isEmpty()) {
            *firstProblem = key.isEmpty()
                ? place + QStringLiteral(": ") + what
                : place + QLatin1Char('.') + key + QLatin1Char(' ') + what;
        }
    }

    QJsonObject json;
    QString place;
    QString* firstProblem;
};

QJsonObject ObjectAt(const QJsonArray& items, qsizetype index, Reader& parent, const QString& key) {
    const QJsonValue value = items.at(index);
    parent.require(value.isObject(), QStringLiteral("%1[%2] is not an object").arg(key).arg(index));
    return value.toObject();
}

QString NameIn(Reader& reader, const QString& key) {
    const QString value = reader.string(key);
    if (reader.ok()) {
        const qsizetype length = CodePoints(value);
        reader.require(
            length >= 1 && length <= kMaxNameLength && Pattern(kName).match(value).hasMatch(),
            key + QStringLiteral(" is not a trimmed name of 1 to 24 characters")
        );
    }
    return value;
}

QString SearchTextIn(Reader& reader, const QString& key) {
    const QString value = reader.string(key);
    if (reader.ok()) {
        const qsizetype length = CodePoints(value);
        reader.require(
            length >= 1 && length <= kMaxSearchLength && !HasControl(value)
                && !value.trimmed().isEmpty(),
            key + QStringLiteral(" is not a search text of 1 to 100 characters")
        );
    }
    return value;
}

}  // namespace

bool IsListenUrl(const QString& url) {
    return url.size() <= kMaxListenUrlLength && Pattern(kListenUrl).match(url).hasMatch();
}

namespace {

// Listening along (spec/jam/listen.md): absent is an empty string.
QString ListenUrlIn(Reader& reader, const QString& key) {
    if (!reader.has(key)) {
        return {};
    }
    const QString value = reader.string(key);
    if (reader.ok()) {
        reader.require(
            IsListenUrl(value), key + QStringLiteral(" is not a file of Yandex Music's storage")
        );
    }
    return value;
}

QString JoinUrlIn(Reader& reader, const QString& key) {
    const QString value = reader.string(key);
    if (reader.ok()) {
        reader.require(
            value.size() <= kMaxJoinUrlLength && Pattern(kJoinUrl).match(value).hasMatch(),
            key + QStringLiteral(" is not a join link with its secret")
        );
    }
    return value;
}

Track ReadTrack(Reader reader) {
    Track track;
    track.id = reader.matching(QStringLiteral("id"), kCatalogId);
    track.albumId = reader.optionalMatching(QStringLiteral("albumId"), kCatalogId);
    track.title = reader.string(QStringLiteral("title"));
    if (reader.ok()) {
        const qsizetype length = CodePoints(track.title);
        reader.require(
            length >= 1 && length <= kMaxTitleLength && !HasControl(track.title),
            QStringLiteral("title is not 1 to 150 characters without control characters")
        );
    }
    const QJsonArray artists = reader.array(QStringLiteral("artists"), 0, kMaxArtists);
    for (const QJsonValue& artist : artists) {
        const qsizetype length = artist.isString() ? CodePoints(artist.toString()) : 0;
        reader.require(
            artist.isString() && length >= 1 && length <= kMaxArtistLength
                && !HasControl(artist.toString()),
            QStringLiteral("an artist is not 1 to 64 characters")
        );
        track.artists.append(artist.toString());
    }
    track.durationMs = reader.integer(QStringLiteral("durationMs"), 0, kMaxDurationMs);
    track.coverUri = reader.optionalString(QStringLiteral("coverUri"));
    if (reader.ok() && reader.has(QStringLiteral("coverUri"))) {
        reader.require(
            track.coverUri.size() <= kMaxCoverLength
                && Pattern(kCoverUri).match(track.coverUri).hasMatch(),
            QStringLiteral("coverUri is not a %% template of up to 300 characters")
        );
    }
    return track;
}

std::vector<Track> ReadTracks(Reader& reader, const QString& key, qsizetype maximum) {
    std::vector<Track> tracks;
    const QJsonArray items = reader.array(key, 0, maximum);
    for (qsizetype index = 0; index < items.size() && reader.ok(); ++index) {
        tracks.push_back(ReadTrack(reader.child(ObjectAt(items, index, reader, key), key)));
    }
    return tracks;
}

int ReadMaxPending(Reader& reader) {
    return static_cast<int>(reader.integer(
        QStringLiteral("maxPendingPerGuest"), kMinPendingPerGuest, kMaxPendingPerGuest
    ));
}

Order ReadOrder(Reader& reader) {
    return reader.choice<Order>(
        QStringLiteral("order"), {{"round-robin", Order::RoundRobin}, {"fifo", Order::Fifo}}
    );
}

Settings ReadSettings(Reader reader) {
    Settings settings;
    settings.order = ReadOrder(reader);
    settings.guestsCanSkip = reader.boolean(QStringLiteral("guestsCanSkip"));
    settings.joinOpen = reader.boolean(QStringLiteral("joinOpen"));
    settings.maxPendingPerGuest = ReadMaxPending(reader);
    return settings;
}

SettingsPatch ReadSettingsPatch(Reader reader) {
    SettingsPatch patch;
    if (reader.has(QStringLiteral("order"))) {
        patch.order = ReadOrder(reader);
    }
    if (reader.has(QStringLiteral("guestsCanSkip"))) {
        patch.guestsCanSkip = reader.boolean(QStringLiteral("guestsCanSkip"));
    }
    if (reader.has(QStringLiteral("joinOpen"))) {
        patch.joinOpen = reader.boolean(QStringLiteral("joinOpen"));
    }
    if (reader.has(QStringLiteral("maxPendingPerGuest"))) {
        patch.maxPendingPerGuest = ReadMaxPending(reader);
    }
    return patch;
}

Source ReadSource(Reader& reader) {
    return reader.choice<Source>(
        QStringLiteral("source"),
        {{"item", Source::Item}, {"wave", Source::Wave}, {"idle", Source::Idle}}
    );
}

NowPlaying ReadNowPlaying(Reader reader) {
    NowPlaying nowPlaying;
    nowPlaying.source = ReadSource(reader);
    nowPlaying.itemId = reader.optionalMatching(QStringLiteral("itemId"), kItemId);
    if (reader.has(QStringLiteral("track"))) {
        nowPlaying.track =
            ReadTrack(reader.child(reader.object(QStringLiteral("track")), QStringLiteral("track"))
            );
    }
    nowPlaying.addedBy = reader.optionalMatching(QStringLiteral("addedBy"), kPublicId);
    nowPlaying.positionMs = reader.time(QStringLiteral("positionMs"));
    nowPlaying.paused = reader.boolean(QStringLiteral("paused"));
    nowPlaying.reportedAt = reader.time(QStringLiteral("reportedAt"));
    nowPlaying.listenUrl = ListenUrlIn(reader, QStringLiteral("listenUrl"));
    nowPlaying.listenNextUrl = ListenUrlIn(reader, QStringLiteral("listenNextUrl"));
    if (reader.ok() && nowPlaying.source == Source::Item) {
        reader.require(
            !nowPlaying.itemId.isEmpty() && nowPlaying.track && !nowPlaying.addedBy.isEmpty(),
            QStringLiteral("an item needs itemId, track and addedBy")
        );
    }
    if (reader.ok() && nowPlaying.source == Source::Wave) {
        reader.require(nowPlaying.track.has_value(), QStringLiteral("a wave track needs track"));
    }
    return nowPlaying;
}

bool NoSecrets(const QJsonValue& value) {
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto field = object.begin(); field != object.end(); ++field) {
            if (kSecretFields.contains(field.key()) || !NoSecrets(field.value())) {
                return false;
            }
        }
    } else if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            if (!NoSecrets(item)) {
                return false;
            }
        }
    }
    return true;
}

void CheckSnapshot(Reader& reader, const QJsonObject& data) {
    const QJsonValue format = data.value(QStringLiteral("format"));
    reader.require(
        format.isDouble() && format.toDouble() == 1, QStringLiteral("snapshot format is not 1")
    );
    reader.require(
        data.value(QStringLiteral("room")).isObject(), QStringLiteral("snapshot has no room")
    );
    reader.require(NoSecrets(data), QStringLiteral("snapshot carries a secret"));
}

Room ReadRoom(Reader reader) {
    Room room;
    room.id = reader.matching(QStringLiteral("id"), kRoomId);
    room.hostName = NameIn(reader, QStringLiteral("hostName"));
    room.hostOnline = reader.boolean(QStringLiteral("hostOnline"));
    room.settings = ReadSettings(
        reader.child(reader.object(QStringLiteral("settings")), QStringLiteral("settings"))
    );
    Reader you = reader.child(reader.object(QStringLiteral("you")), QStringLiteral("you"));
    room.you.publicId = you.matching(QStringLiteral("publicId"), kPublicId);
    room.you.isHost = you.boolean(QStringLiteral("isHost"));

    const QString participants = QStringLiteral("participants");
    const QJsonArray people = reader.array(participants, 0, kMaxParticipants);
    for (qsizetype index = 0; index < people.size() && reader.ok(); ++index) {
        Reader item = reader.child(ObjectAt(people, index, reader, participants), participants);
        Participant participant;
        participant.publicId = item.matching(QStringLiteral("publicId"), kPublicId);
        participant.name = NameIn(item, QStringLiteral("name"));
        participant.kind = item.choice<ParticipantKind>(
            QStringLiteral("kind"),
            {{"host", ParticipantKind::Host},
             {"web", ParticipantKind::Web},
             {"qiyaa", ParticipantKind::Qiyaa}}
        );
        participant.online = item.boolean(QStringLiteral("online"));
        participant.pending =
            static_cast<int>(item.integer(QStringLiteral("pending"), 0, kMaxQueue));
        room.participants.push_back(participant);
    }

    room.nowPlaying = ReadNowPlaying(
        reader.child(reader.object(QStringLiteral("nowPlaying")), QStringLiteral("nowPlaying"))
    );

    const QString queue = QStringLiteral("queue");
    const QJsonArray items = reader.array(queue, 0, kMaxQueue);
    for (qsizetype index = 0; index < items.size() && reader.ok(); ++index) {
        Reader item = reader.child(ObjectAt(items, index, reader, queue), queue);
        QueueItem entry;
        entry.itemId = item.matching(QStringLiteral("itemId"), kItemId);
        entry.track =
            ReadTrack(item.child(item.object(QStringLiteral("track")), QStringLiteral("track")));
        entry.addedBy = item.matching(QStringLiteral("addedBy"), kPublicId);
        entry.addedAt = item.time(QStringLiteral("addedAt"));
        entry.pinned = item.boolean(QStringLiteral("pinned"));
        room.queue.push_back(entry);
    }

    const QString recent = QStringLiteral("recent");
    const QJsonArray played = reader.array(recent, 0, kMaxRecent);
    for (qsizetype index = 0; index < played.size() && reader.ok(); ++index) {
        Reader item = reader.child(ObjectAt(played, index, reader, recent), recent);
        RecentItem entry;
        entry.itemId = item.matching(QStringLiteral("itemId"), kItemId);
        entry.track =
            ReadTrack(item.child(item.object(QStringLiteral("track")), QStringLiteral("track")));
        entry.addedBy = item.matching(QStringLiteral("addedBy"), kPublicId);
        entry.playedAt = item.time(QStringLiteral("playedAt"));
        room.recent.push_back(entry);
    }

    Reader fallback =
        reader.child(reader.object(QStringLiteral("fallback")), QStringLiteral("fallback"));
    const QJsonArray seeds = fallback.array(QStringLiteral("seeds"), 0, kMaxSeeds);
    for (const QJsonValue& seed : seeds) {
        fallback.require(
            seed.isString() && Pattern(kSeed).match(seed.toString()).hasMatch(),
            QStringLiteral("a seed is not track:<id>")
        );
        room.fallback.seeds.append(seed.toString());
    }
    fallback.require(
        QSet<QString>(room.fallback.seeds.begin(), room.fallback.seeds.end()).size()
            == room.fallback.seeds.size(),
        QStringLiteral("seeds repeat")
    );
    room.fallback.seedsVersion =
        static_cast<int>(fallback.integer(QStringLiteral("seedsVersion"), 0, INT_MAX));
    return room;
}

std::optional<ServerMessage>
ReadServer(const QString& type, const QJsonObject& json, QString* problem) {
    Reader reader(json, type, problem);
    if (type == QLatin1String("welcome")) {
        Welcome message;
        message.protocol = static_cast<int>(reader.integer(QStringLiteral("protocol"), 1, INT_MAX));
        message.serverTime = reader.time(QStringLiteral("serverTime"));
        return message;
    }
    if (type == QLatin1String("rejected")) {
        Rejected message;
        message.id = reader.optionalMatching(QStringLiteral("id"), kRequestId);
        message.reason = reader.string(QStringLiteral("reason"));
        message.detail = reader.optionalString(QStringLiteral("detail"));
        reader.require(
            message.detail.size() <= kMaxDetailLength,
            QStringLiteral("detail is over 200 characters")
        );
        if (reader.has(QStringLiteral("serverProtocol"))) {
            message.serverProtocol =
                static_cast<int>(reader.integer(QStringLiteral("serverProtocol"), 1, INT_MAX));
        }
        if (reader.ok() && !IsKnownReason(message.reason)) {
            *problem = QStringLiteral("rejected: unknown reason ") + message.reason;
        }
        return message;
    }
    if (type == QLatin1String("ack")) {
        return Ack{reader.matching(QStringLiteral("id"), kRequestId)};
    }
    if (type == QLatin1String("created")) {
        Created message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.roomId = reader.matching(QStringLiteral("roomId"), kRoomId);
        message.hostSecret = reader.matching(QStringLiteral("hostSecret"), kHostSecret);
        message.joinSecret = reader.matching(QStringLiteral("joinSecret"), kJoinSecret);
        message.joinUrl = JoinUrlIn(reader, QStringLiteral("joinUrl"));
        message.publicId = reader.matching(QStringLiteral("publicId"), kPublicId);
        return message;
    }
    if (type == QLatin1String("resumed")) {
        Resumed message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.restored = reader.boolean(QStringLiteral("restored"));
        return message;
    }
    if (type == QLatin1String("joined")) {
        Joined message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.publicId = reader.matching(QStringLiteral("publicId"), kPublicId);
        return message;
    }
    if (type == QLatin1String("searchResults")) {
        SearchResults message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.tracks = ReadTracks(reader, QStringLiteral("tracks"), kMaxTracksPerReply);
        return message;
    }
    if (type == QLatin1String("linkRotated")) {
        LinkRotated message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.joinSecret = reader.matching(QStringLiteral("joinSecret"), kJoinSecret);
        message.joinUrl = JoinUrlIn(reader, QStringLiteral("joinUrl"));
        return message;
    }
    if (type == QLatin1String("searchRequest")) {
        SearchRequest message;
        message.requestId = reader.matching(QStringLiteral("requestId"), kRequestId);
        message.text = SearchTextIn(reader, QStringLiteral("text"));
        return message;
    }
    if (type == QLatin1String("validateRequest")) {
        ValidateRequest message;
        message.requestId = reader.matching(QStringLiteral("requestId"), kRequestId);
        const QJsonArray ids = reader.array(QStringLiteral("trackIds"), 1, kMaxTracksPerReply);
        for (const QJsonValue& id : ids) {
            reader.require(
                id.isString() && Pattern(kCatalogId).match(id.toString()).hasMatch(),
                QStringLiteral("a track id is not a catalogue id")
            );
            message.trackIds.append(id.toString());
        }
        return message;
    }
    if (type == QLatin1String("command")) {
        Command message;
        message.kind =
            reader.choice<CommandKind>(QStringLiteral("kind"), {{"skip", CommandKind::Skip}});
        message.itemId = reader.matching(QStringLiteral("itemId"), kItemId);
        return message;
    }
    if (type == QLatin1String("snapshot")) {
        Snapshot message;
        message.data = reader.object(QStringLiteral("data"));
        if (reader.ok()) {
            CheckSnapshot(reader, message.data);
        }
        return message;
    }
    if (type == QLatin1String("state")) {
        State message;
        message.version = reader.integer(QStringLiteral("version"), 1, Reader::kMaxSafeInteger);
        message.serverTime = reader.time(QStringLiteral("serverTime"));
        const QJsonObject room = reader.object(QStringLiteral("room"));
        reader.require(NoSecrets(room), QStringLiteral("room carries a secret"));
        if (reader.ok()) {
            message.room = ReadRoom(reader.child(room, QStringLiteral("room")));
        }
        return message;
    }
    if (type == QLatin1String("ended")) {
        return Ended{reader.choice<EndReason>(
            QStringLiteral("reason"),
            {{"host-ended", EndReason::HostEnded}, {"expired", EndReason::Expired}}
        )};
    }
    if (type == QLatin1String("kicked")) {
        return Kicked{};
    }
    return std::nullopt;
}

std::optional<ClientMessage>
ReadClient(const QString& type, const QJsonObject& json, QString* problem) {
    Reader reader(json, type, problem);
    const auto requestAndItem = [&reader]() {
        return std::pair{
            reader.matching(QStringLiteral("id"), kRequestId),
            reader.matching(QStringLiteral("itemId"), kItemId)
        };
    };
    if (type == QLatin1String("hello")) {
        Hello message;
        message.protocol = static_cast<int>(reader.integer(QStringLiteral("protocol"), 1, INT_MAX));
        message.app = reader.choice<App>(
            QStringLiteral("app"),
            {{"desktop", App::Desktop}, {"android", App::Android}, {"web", App::Web}}
        );
        message.appVersion = reader.matching(QStringLiteral("appVersion"), kAppVersion);
        return message;
    }
    if (type == QLatin1String("create")) {
        Create message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.hostName = NameIn(reader, QStringLiteral("hostName"));
        if (reader.has(QStringLiteral("settings"))) {
            message.settings = ReadSettingsPatch(
                reader.child(reader.object(QStringLiteral("settings")), QStringLiteral("settings"))
            );
        }
        return message;
    }
    if (type == QLatin1String("resume")) {
        Resume message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.roomId = reader.matching(QStringLiteral("roomId"), kRoomId);
        message.hostSecret = reader.matching(QStringLiteral("hostSecret"), kHostSecret);
        const QJsonValue snapshot = json.value(QStringLiteral("snapshot"));
        reader.require(
            snapshot.isObject() || snapshot.isNull(),
            QStringLiteral("snapshot is not an object or null")
        );
        if (reader.ok() && snapshot.isObject()) {
            message.snapshot = snapshot.toObject();
            CheckSnapshot(reader, *message.snapshot);
        }
        const QJsonArray outbox = reader.array(QStringLiteral("outbox"), 0, kMaxOutbox);
        for (qsizetype index = 0; index < outbox.size() && reader.ok(); ++index) {
            Reader entry = reader.child(
                ObjectAt(outbox, index, reader, QStringLiteral("outbox")), QStringLiteral("outbox")
            );
            entry.require(
                entry.string(QStringLiteral("type")) == QLatin1String("started"),
                QStringLiteral("the outbox holds only started")
            );
            message.outbox.append(entry.matching(QStringLiteral("itemId"), kItemId));
        }
        return message;
    }
    if (type == QLatin1String("playing")) {
        Playing message;
        message.source = ReadSource(reader);
        message.itemId = reader.optionalMatching(QStringLiteral("itemId"), kItemId);
        if (reader.has(QStringLiteral("track"))) {
            message.track = ReadTrack(
                reader.child(reader.object(QStringLiteral("track")), QStringLiteral("track"))
            );
        }
        message.positionMs = reader.time(QStringLiteral("positionMs"));
        message.paused = reader.boolean(QStringLiteral("paused"));
        message.listenUrl = ListenUrlIn(reader, QStringLiteral("listenUrl"));
        message.listenNextUrl = ListenUrlIn(reader, QStringLiteral("listenNextUrl"));
        if (reader.ok()) {
            reader.require(
                message.source != Source::Item || !message.itemId.isEmpty(),
                QStringLiteral("an item needs itemId")
            );
            reader.require(
                message.source != Source::Wave || message.track.has_value(),
                QStringLiteral("a wave track needs track")
            );
        }
        return message;
    }
    if (type == QLatin1String("started")) {
        return Started{reader.matching(QStringLiteral("itemId"), kItemId)};
    }
    if (type == QLatin1String("add")) {
        Add message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.trackId = reader.optionalMatching(QStringLiteral("trackId"), kCatalogId);
        if (reader.has(QStringLiteral("track"))) {
            message.track = ReadTrack(
                reader.child(reader.object(QStringLiteral("track")), QStringLiteral("track"))
            );
        }
        reader.require(
            reader.has(QStringLiteral("trackId")) != reader.has(QStringLiteral("track")),
            QStringLiteral("needs exactly one of trackId and track")
        );
        return message;
    }
    if (type == QLatin1String("pin")) {
        const auto [id, itemId] = requestAndItem();
        return Pin{id, itemId};
    }
    if (type == QLatin1String("remove")) {
        const auto [id, itemId] = requestAndItem();
        return Remove{id, itemId};
    }
    if (type == QLatin1String("skip")) {
        const auto [id, itemId] = requestAndItem();
        return Skip{id, itemId};
    }
    if (type == QLatin1String("kick")) {
        Kick message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.publicId = reader.matching(QStringLiteral("publicId"), kPublicId);
        return message;
    }
    if (type == QLatin1String("settings")) {
        ChangeSettings message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.settings = ReadSettingsPatch(
            reader.child(reader.object(QStringLiteral("settings")), QStringLiteral("settings"))
        );
        reader.require(!message.settings.isEmpty(), QStringLiteral("settings change nothing"));
        return message;
    }
    if (type == QLatin1String("rotateLink")) {
        return RotateLink{reader.matching(QStringLiteral("id"), kRequestId)};
    }
    if (type == QLatin1String("end")) {
        return End{reader.matching(QStringLiteral("id"), kRequestId)};
    }
    if (type == QLatin1String("searchResult")) {
        SearchResult message;
        message.requestId = reader.matching(QStringLiteral("requestId"), kRequestId);
        if (reader.has(QStringLiteral("tracks"))) {
            message.tracks = ReadTracks(reader, QStringLiteral("tracks"), kMaxTracksPerReply);
        }
        if (reader.has(QStringLiteral("error"))) {
            message.error = reader.choice<SearchError>(
                QStringLiteral("error"),
                {{"failed", SearchError::Failed}, {"unauthorized", SearchError::Unauthorized}}
            );
        }
        reader.require(
            message.tracks.has_value() != message.error.has_value(),
            QStringLiteral("needs exactly one of tracks and error")
        );
        return message;
    }
    if (type == QLatin1String("validateResult")) {
        ValidateResult message;
        message.requestId = reader.matching(QStringLiteral("requestId"), kRequestId);
        const QString results = QStringLiteral("results");
        const QJsonArray items = reader.array(results, 1, kMaxTracksPerReply);
        for (qsizetype index = 0; index < items.size() && reader.ok(); ++index) {
            Reader item = reader.child(ObjectAt(items, index, reader, results), results);
            ValidateEntry entry;
            entry.trackId = item.matching(QStringLiteral("trackId"), kCatalogId);
            if (item.has(QStringLiteral("track"))) {
                entry.track = ReadTrack(
                    item.child(item.object(QStringLiteral("track")), QStringLiteral("track"))
                );
            }
            if (item.has(QStringLiteral("reason"))) {
                entry.reason = item.choice<ValidateReason>(
                    QStringLiteral("reason"),
                    {{"track-unavailable", ValidateReason::TrackUnavailable},
                     {"failed", ValidateReason::Failed}}
                );
            }
            item.require(
                entry.track.has_value() != entry.reason.has_value(),
                QStringLiteral("needs exactly one of track and reason")
            );
            message.results.push_back(entry);
        }
        return message;
    }
    if (type == QLatin1String("join")) {
        Join message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.roomId = reader.matching(QStringLiteral("roomId"), kRoomId);
        message.joinSecret = reader.matching(QStringLiteral("joinSecret"), kJoinSecret);
        message.participantId = reader.matching(QStringLiteral("participantId"), kParticipantId);
        message.name = NameIn(reader, QStringLiteral("name"));
        return message;
    }
    if (type == QLatin1String("search")) {
        Search message;
        message.id = reader.matching(QStringLiteral("id"), kRequestId);
        message.text = SearchTextIn(reader, QStringLiteral("text"));
        return message;
    }
    return std::nullopt;
}

template <typename T>
Decoded<T> DecodeWith(
    QByteArrayView text,
    std::optional<T> (*read)(const QString&, const QJsonObject&, QString*)
) {
    Decoded<T> decoded;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toByteArray(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        decoded.problem = QStringLiteral("not JSON: ") + parseError.errorString();
        return decoded;
    }
    if (!document.isObject()) {
        decoded.problem = QStringLiteral("not a JSON object");
        return decoded;
    }
    const QJsonObject json = document.object();
    const QString type = json.value(QStringLiteral("type")).toString();
    std::optional<T> message = read(type, json, &decoded.problem);
    if (!message) {
        decoded.unknownType = true;
        decoded.problem = QStringLiteral("unknown type \"%1\"").arg(type);
        return decoded;
    }
    const bool unknownReason =
        decoded.problem.startsWith(QStringLiteral("rejected: unknown reason"));
    if (decoded.problem.isEmpty() || unknownReason) {
        decoded.message = std::move(message);
    }
    return decoded;
}

QString ToString(Order order) {
    return order == Order::Fifo ? QStringLiteral("fifo") : QStringLiteral("round-robin");
}

QString ToString(Source source) {
    switch (source) {
        case Source::Item: return QStringLiteral("item");
        case Source::Wave: return QStringLiteral("wave");
        case Source::Idle: break;
    }
    return QStringLiteral("idle");
}

QString ToString(App app) {
    switch (app) {
        case App::Desktop: return QStringLiteral("desktop");
        case App::Android: return QStringLiteral("android");
        case App::Web: break;
    }
    return QStringLiteral("web");
}

QJsonObject TrackJson(const Track& track) {
    QJsonObject json{
        {QStringLiteral("id"), track.id},
        {QStringLiteral("title"), track.title},
        {QStringLiteral("artists"), QJsonArray::fromStringList(track.artists)},
        {QStringLiteral("durationMs"), static_cast<double>(track.durationMs)},
    };
    if (!track.albumId.isEmpty()) {
        json.insert(QStringLiteral("albumId"), track.albumId);
    }
    if (!track.coverUri.isEmpty()) {
        json.insert(QStringLiteral("coverUri"), track.coverUri);
    }
    return json;
}

QJsonArray TracksJson(const std::vector<Track>& tracks) {
    QJsonArray json;
    for (const Track& track : tracks) {
        json.append(TrackJson(track));
    }
    return json;
}

QJsonObject PatchJson(const SettingsPatch& patch) {
    QJsonObject json;
    if (patch.order) {
        json.insert(QStringLiteral("order"), ToString(*patch.order));
    }
    if (patch.guestsCanSkip) {
        json.insert(QStringLiteral("guestsCanSkip"), *patch.guestsCanSkip);
    }
    if (patch.joinOpen) {
        json.insert(QStringLiteral("joinOpen"), *patch.joinOpen);
    }
    if (patch.maxPendingPerGuest) {
        json.insert(QStringLiteral("maxPendingPerGuest"), *patch.maxPendingPerGuest);
    }
    return json;
}

QJsonObject IdOnly(const QString& id) {
    return {{QStringLiteral("id"), id}};
}

QJsonObject IdAndItem(const QString& id, const QString& itemId) {
    return {{QStringLiteral("id"), id}, {QStringLiteral("itemId"), itemId}};
}

QJsonObject Body(const Hello& message) {
    return {
        {QStringLiteral("protocol"), message.protocol},
        {QStringLiteral("app"), ToString(message.app)},
        {QStringLiteral("appVersion"), message.appVersion},
    };
}

QJsonObject Body(const Create& message) {
    QJsonObject json{
        {QStringLiteral("id"), message.id}, {QStringLiteral("hostName"), message.hostName}
    };
    if (message.settings) {
        json.insert(QStringLiteral("settings"), PatchJson(*message.settings));
    }
    return json;
}

QJsonObject Body(const Resume& message) {
    QJsonArray outbox;
    for (const QString& itemId : message.outbox) {
        outbox.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("started")}, {QStringLiteral("itemId"), itemId}
        });
    }
    return {
        {QStringLiteral("id"), message.id},
        {QStringLiteral("roomId"), message.roomId},
        {QStringLiteral("hostSecret"), message.hostSecret},
        {QStringLiteral("snapshot"),
         message.snapshot ? QJsonValue(*message.snapshot) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("outbox"), outbox},
    };
}

QJsonObject Body(const Playing& message) {
    QJsonObject json{
        {QStringLiteral("source"), ToString(message.source)},
        {QStringLiteral("positionMs"), static_cast<double>(message.positionMs)},
        {QStringLiteral("paused"), message.paused},
    };
    if (!message.itemId.isEmpty()) {
        json.insert(QStringLiteral("itemId"), message.itemId);
    }
    if (message.track) {
        json.insert(QStringLiteral("track"), TrackJson(*message.track));
    }
    if (!message.listenUrl.isEmpty()) {
        json.insert(QStringLiteral("listenUrl"), message.listenUrl);
    }
    if (!message.listenNextUrl.isEmpty()) {
        json.insert(QStringLiteral("listenNextUrl"), message.listenNextUrl);
    }
    return json;
}

QJsonObject Body(const Started& message) {
    return {{QStringLiteral("itemId"), message.itemId}};
}

QJsonObject Body(const Add& message) {
    QJsonObject json = IdOnly(message.id);
    if (!message.trackId.isEmpty()) {
        json.insert(QStringLiteral("trackId"), message.trackId);
    }
    if (message.track) {
        json.insert(QStringLiteral("track"), TrackJson(*message.track));
    }
    return json;
}

QJsonObject Body(const Pin& message) {
    return IdAndItem(message.id, message.itemId);
}

QJsonObject Body(const Remove& message) {
    return IdAndItem(message.id, message.itemId);
}

QJsonObject Body(const Skip& message) {
    return IdAndItem(message.id, message.itemId);
}

QJsonObject Body(const Kick& message) {
    return {{QStringLiteral("id"), message.id}, {QStringLiteral("publicId"), message.publicId}};
}

QJsonObject Body(const ChangeSettings& message) {
    return {
        {QStringLiteral("id"), message.id},
        {QStringLiteral("settings"), PatchJson(message.settings)}
    };
}

QJsonObject Body(const RotateLink& message) {
    return IdOnly(message.id);
}

QJsonObject Body(const End& message) {
    return IdOnly(message.id);
}

QJsonObject Body(const SearchResult& message) {
    QJsonObject json{{QStringLiteral("requestId"), message.requestId}};
    if (message.tracks) {
        json.insert(QStringLiteral("tracks"), TracksJson(*message.tracks));
    }
    if (message.error) {
        json.insert(
            QStringLiteral("error"),
            *message.error == SearchError::Unauthorized ? QStringLiteral("unauthorized")
                                                        : QStringLiteral("failed")
        );
    }
    return json;
}

QJsonObject Body(const ValidateResult& message) {
    QJsonArray results;
    for (const ValidateEntry& entry : message.results) {
        QJsonObject json{{QStringLiteral("trackId"), entry.trackId}};
        if (entry.track) {
            json.insert(QStringLiteral("track"), TrackJson(*entry.track));
        }
        if (entry.reason) {
            json.insert(
                QStringLiteral("reason"),
                *entry.reason == ValidateReason::TrackUnavailable
                    ? QStringLiteral("track-unavailable")
                    : QStringLiteral("failed")
            );
        }
        results.append(json);
    }
    return {{QStringLiteral("requestId"), message.requestId}, {QStringLiteral("results"), results}};
}

QJsonObject Body(const Join& message) {
    return {
        {QStringLiteral("id"), message.id},
        {QStringLiteral("roomId"), message.roomId},
        {QStringLiteral("joinSecret"), message.joinSecret},
        {QStringLiteral("participantId"), message.participantId},
        {QStringLiteral("name"), message.name},
    };
}

QJsonObject Body(const Search& message) {
    return {{QStringLiteral("id"), message.id}, {QStringLiteral("text"), message.text}};
}

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

bool SettingsPatch::isEmpty() const {
    return !order && !guestsCanSkip && !joinOpen && !maxPendingPerGuest;
}

QByteArray Encode(const ClientMessage& message) {
    QJsonObject json = std::visit([](const auto& body) { return Body(body); }, message);
    json.insert(QStringLiteral("type"), TypeOf(message));
    return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

Decoded<ServerMessage> DecodeServer(QByteArrayView text) {
    return DecodeWith<ServerMessage>(text, &ReadServer);
}

Decoded<ClientMessage> DecodeClient(QByteArrayView text) {
    return DecodeWith<ClientMessage>(text, &ReadClient);
}

bool IsKnownReason(const QString& reason) {
    return kReasons.contains(reason);
}

QString TypeOf(const ClientMessage& message) {
    return std::visit(
        Overloaded{
            [](const Hello&) { return QStringLiteral("hello"); },
            [](const Create&) { return QStringLiteral("create"); },
            [](const Resume&) { return QStringLiteral("resume"); },
            [](const Playing&) { return QStringLiteral("playing"); },
            [](const Started&) { return QStringLiteral("started"); },
            [](const Add&) { return QStringLiteral("add"); },
            [](const Pin&) { return QStringLiteral("pin"); },
            [](const Remove&) { return QStringLiteral("remove"); },
            [](const Kick&) { return QStringLiteral("kick"); },
            [](const ChangeSettings&) { return QStringLiteral("settings"); },
            [](const RotateLink&) { return QStringLiteral("rotateLink"); },
            [](const End&) { return QStringLiteral("end"); },
            [](const SearchResult&) { return QStringLiteral("searchResult"); },
            [](const ValidateResult&) { return QStringLiteral("validateResult"); },
            [](const Join&) { return QStringLiteral("join"); },
            [](const Search&) { return QStringLiteral("search"); },
            [](const Skip&) { return QStringLiteral("skip"); },
        },
        message
    );
}

QString TypeOf(const ServerMessage& message) {
    return std::visit(
        Overloaded{
            [](const Welcome&) { return QStringLiteral("welcome"); },
            [](const Rejected&) { return QStringLiteral("rejected"); },
            [](const Ack&) { return QStringLiteral("ack"); },
            [](const Created&) { return QStringLiteral("created"); },
            [](const Resumed&) { return QStringLiteral("resumed"); },
            [](const Joined&) { return QStringLiteral("joined"); },
            [](const SearchResults&) { return QStringLiteral("searchResults"); },
            [](const LinkRotated&) { return QStringLiteral("linkRotated"); },
            [](const SearchRequest&) { return QStringLiteral("searchRequest"); },
            [](const ValidateRequest&) { return QStringLiteral("validateRequest"); },
            [](const Command&) { return QStringLiteral("command"); },
            [](const Snapshot&) { return QStringLiteral("snapshot"); },
            [](const State&) { return QStringLiteral("state"); },
            [](const Ended&) { return QStringLiteral("ended"); },
            [](const Kicked&) { return QStringLiteral("kicked"); },
        },
        message
    );
}

}  // namespace Jam
