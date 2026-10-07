# `src/jam` — the jam: protocol, connection, the host's session

The connection to a jam server (Kickoman/QiYaa-jam) by the protocol of `spec/jam/protocol`: the
message model, its JSON with the schema's checks, the WebSocket client with its handshake,
reconnects, clock offset and outbox, the file where the host keeps its jam between runs, and the
host's session that ties them to the queue and to Yandex. It does **not** decide what plays (the
jam mode of the queue is `Core::JamMode`) and has no windows (Kickoman/QiYaa#15). It reads no
`QSettings` and does not know where files live: `src/app` passes the server address and the
session file's path.

The module is optional: it is built only with Qt WebSockets (`QIYAA_HAVE_JAM`, see
[docs/building.md](../../docs/building.md#параметры-cmake)). It links Qt Core, Qt WebSockets,
`qiyaa_core` and `qiyaa_yandex`; only `host_session.*` uses the last two.

```bash
grep -rln 'include "\(ui\|app\|audio\|skins\|vis\|integrations\)/' src/jam/   # must print nothing
grep -ln 'include "\(core\|yandex\)/' src/jam/*                                # host_session.cpp only
```

| File | Contains |
|---|---|
| `protocol.h` | `ClientMessage`, `ServerMessage` and every message and model of the protocol: `Track`, `Room`, `Settings`, `SettingsPatch`… |
| `codec.h/.cpp` | `Encode`, `DecodeServer`, `DecodeClient`, `Decoded<T>`, `IsKnownReason`, `TypeOf` |
| `client.h/.cpp` | `Client`, `ClientOptions`, `Status` — one connection to the server at a time |
| `session_store.h/.cpp` | `Session`, `SessionStore`, `EncodeSession`, `DecodeSession` |
| `host_session.h/.cpp` | `HostSession`, `HostOptions`, `HostConfig`, `HostPhase`, `HostEnd`, `JamTrackOf`, `YandexTrackOf` |

## `protocol.h`

The structs follow `spec/jam/protocol/schemas` field by field, one struct per message `type`, and
compare with `==`. Names that the protocol uses for something else in C++: `settings` is
`ChangeSettings`, `searchResult` is `SearchResult`, `snapshot` is `Snapshot`, `state` is `State`.

- An optional string field (`Track::albumId`, `coverUri`, `NowPlaying::itemId`, `Rejected::id`…) is
  an empty `QString` when absent; optional objects and enums are `std::optional`.
- `Track::coverUri` is Yandex's template with `%%` for the size and no scheme, the same as
  `Yandex::Track::coverUri`.
- `Resume::outbox` and `Session::outbox` hold the item ids of `started` events in order; the codec
  writes them as `{"type": "started", "itemId": …}`.
- `Resume::snapshot` and `Snapshot::data` are `QJsonObject`: the host stores the snapshot and sends
  it back without reading it.

## `codec.h`

```cpp
template <typename T> struct Decoded { std::optional<T> message; QString problem; bool unknownType; bool isValid() const; };
QByteArray Encode(const ClientMessage& message);            // compact JSON
Decoded<ServerMessage> DecodeServer(QByteArrayView text);
Decoded<ClientMessage> DecodeClient(QByteArrayView text);   // what the server would accept
bool IsKnownReason(const QString& reason);
QString TypeOf(const ClientMessage&);  QString TypeOf(const ServerMessage&);
```

Decoding checks what the schemas check: required fields and their JSON types, enum values, the
patterns of ids and secrets, lengths of names, titles and texts (in code points), limits of
numbers and lists, "exactly one of" (`add`, `searchResult`, `validateResult` entries), the
conditional fields of `playing` and `nowPlaying`, an outbox of `started` only, the snapshot's
`format` 1, and **no field named like a secret** at any depth of a `state`'s room or a snapshot.
Unknown fields are ignored. `problem` names the first failure with its place (`state.room.queue
itemId does not match …`).

- An unknown `type` gives `unknownType` and no message: the client skips it.
- A `rejected` with a reason the app does not know is not valid (`problem` says so), but its
  `message` is kept: the protocol says to show a general failure for it.
- `DecodeClient` exists for the tests and for `Client::send`, which never sends what it would
  refuse.

The numbers are a copy of `spec/jam/protocol/schemas/defs.schema.json` and `spec/jam/limits.md`.
`jam_test` decodes every file of `spec/jam/protocol/examples`: the valid ones must pass and the
`invalid-*` ones must fail, so a change in the spec that the codec does not follow fails a test.

**Traps:**
- `\s` in these `QRegularExpression`s is ASCII whitespace only (no `UseUnicodePropertiesOption`),
  as in the Android codec, while Ajv on the server also counts Unicode spaces. A name that ends
  in a non-breaking space passes here and is refused by the server.
- A number must be whole: JSON has no integer type, so `2.5` for `positionMs` reads as a number and
  is refused here. `Reader::integer` checks the range before `static_cast<qint64>`.

## `client.h`

```cpp
enum class Status { Idle, Connecting, Online, Offline, Stopped };
struct ClientOptions { QString appVersion; std::vector<int> reconnectDelaysMs = {1000, 2000, 4000, 8000, 16000, 30000};
                       int pingIntervalMs = 20000; std::function<qint64()> clock; };
class Client : public QObject {
    Status status() const;  qint64 clockOffsetMs() const;  qint64 serverNow() const;  const QStringList& outbox() const;
    void start(const QUrl& url);  void stop();
    bool send(const ClientMessage& message);          // false: not online, or the message would be refused
    void started(const QString& itemId, bool sendNow = true);
    void restoreOutbox(const QStringList& itemIds);  void clearOutbox();
    void networkBack();                               // reconnect at once when Offline
    static QUrl SocketUrl(const QString& serverUrl);  // https://jam.example.org -> wss://jam.example.org/ws
Q_SIGNALS:
    void statusChanged(Jam::Status);  void welcomed();  void messageReceived(const Jam::ServerMessage&);
    void outboxChanged(const QStringList&);
};
```

- On open the client sends `hello{protocol: 1, app: desktop, appVersion}`. The version keeps only
  what the schema allows (`[0-9A-Za-z.+_-]`, 32 at most; `unknown` when nothing is left): the
  server refuses any other `hello`, and bans an address that keeps sending one. `welcome` makes it
  `Online`, sets the clock offset (server time − `clock()`) and emits `welcomed()`: the owner then
  sends `create`, `resume` or `join`. Every `state` updates the offset too.
- A close or an error of the socket makes it `Offline` and schedules a new connection after
  1, 2, 4, 8, 16, 30, 30 … s (`reconnectDelaysMs`); `welcome` starts the count over.
  `networkBack()` (in the app, from `QNetworkInformation`) reconnects at once.
- The client pings every `pingIntervalMs` and drops a connection that did not answer the previous
  ping: a dead network then shows as `Offline` in seconds, not when TCP gives up.
- After `ended`, `kicked` and `rejected{update-required}` the message is emitted first, then the
  client stops: `Stopped`, no more reconnects.
- A message that does not decode is skipped with a warning; an unknown `type` silently.
- `send` refuses (returns false) without a connection that got `welcome`, and refuses a message
  that `DecodeClient` would not accept: the server would close the connection over it, and a
  reconnect would send it again.
- The outbox only collects: `started(itemId, sendNow)` sends at once, or keeps the item id when it
  cannot or when `sendNow` is false (before the server accepted `create` or `resume`, HOST-25). An
  item id already waiting is not added again.
  The owner sends the outbox in `resume` and calls `clearOutbox()` after `resumed`.
- No `Origin` header is sent; the server accepts apps without one.

**Traps:**
- Each connection is a new `QWebSocket`; a signal of an older one is ignored. Do not keep a pointer
  to the socket outside the client.
- `messageReceived` is emitted synchronously from the socket's signal: a slot that calls `stop()`
  or `start()` is fine, but must not delete the client.

## `session_store.h`

What the host keeps while its jam lasts (HOST-22): `roomId`, `hostSecret`, `joinUrl`, the last
snapshot and the outbox. The file is compact JSON, the same format as the Android app's:

```json
{"version":1,"roomId":"7k3m9q2x","hostSecret":"…","joinUrl":"https://…/j/7k3m9q2x#…","snapshot":{"format":1,"room":{…}},"outbox":["i4"]}
```

`save` writes through `QSaveFile` (a temporary file and a rename) and creates the folder; it
returns false and logs when the file cannot be written. `load` gives `nullopt` for a missing,
broken or foreign file (another `version`, a required field missing) and for one over
`kMaxSessionFileBytes` (4 MiB) before reading it. `clear` deletes it.

## `host_session.h`

```cpp
struct HostConfig { QString serverUrl; bool waveFeedback = true; bool shareAudio = false; };
struct HostOptions { ClientOptions client; std::function<HostConfig()> config; QString queueTitle;
                     std::function<QString()> newId; int resumeRetryMs = 30'000; };
enum class HostPhase { None, Creating, Active };
enum class HostEnd { ByHost, ByServer, Expired, Gone };
std::optional<Track> JamTrackOf(const Yandex::Track& track);   // nullopt: the protocol cannot carry it
Yandex::Track YandexTrackOf(const Track& track);
class HostSession : public QObject {
    HostSession(Core::JamMode*, Yandex::Library*, SessionStore, HostOptions, QObject* parent = nullptr);
    HostPhase phase() const;  Status connection() const;  bool isConnected() const;
    const std::optional<Room>& room() const;  QString joinUrl() const;  bool hasStoredSession() const;
    bool create(const QString& hostName, const std::optional<SettingsPatch>& settings = {});
    void cancelCreate();  void continueStored();  void discardStored();  void end();
    bool add(const Yandex::Track&);  bool playNext(const Yandex::Track&);  bool pin(const QString& itemId);
    bool remove(const QString& itemId);  bool kick(const QString& publicId);
    bool changeSettings(const SettingsPatch&);  bool rotateLink();
    void networkBack();
Q_SIGNALS:
    void changed();  void refused(const QString& reason);  void ended(Jam::HostEnd why);
};
```

The host's side of `spec/jam/host.md`, a port of the Android app's `JamHost`: the `Client` to the
server, `Core::JamMode` for the queue, `Yandex::Library` for the guests' search and checks, the
`SessionStore` for the jam between runs.

- **Create.** `create` trims the name and cuts it to 24 characters; it returns false while a jam
  is on or being created, without a server address (`config().serverUrl`), and for a name or
  settings the server would refuse. After `welcome` it sends `create`; `created` stores the session
  and starts `JamMode` with `queueTitle`. A refused `create` (`rate-limited`, `server-full`…) is
  `refused(reason)` and no jam.
- **Resume.** Every `welcome` of an active jam sends `resume` with the stored snapshot and the
  client's outbox (the last 500). After `resumed` the outbox is cleared and `JamMode` reports what
  plays (HOST-26). `room-not-found` or `bad-secret` ends the jam here (`ended(Gone)`, HOST-24);
  another refusal is `refused` and a new try after `resumeRetryMs`.
- **"Continue the jam?"** (HOST-23). The constructor loads the stored session; `hasStoredSession()`
  asks the app to put the question. `continueStored()` starts `JamMode` at once (the queue is not
  kept between runs: its jam part comes with the first `state`) and connects. `discardStored()`
  clears the file at once, then connects only to `resume` and `end` the room; the guests see the
  end.
- **State.** A `state` older than the last one of the same connection is ignored; the first after
  a `welcome` is taken whatever its version. It goes to `JamMode::setQueue`, and to `room()`.
- **The queue's events.** `JamMode::itemStarted` → `Client::started` (to the outbox while not
  connected, HOST-25); `JamMode::playback` → `playing`, only while connected. A wave track the
  protocol cannot carry is not reported.
- **Guests.** `searchRequest` → `Library::searchTracks` (`type=track`), the available tracks that
  `JamTrackOf` takes, 20 at most; a 401 or 403 is `error: unauthorized`, any other failure `failed`
  (HOST-28, HOST-29). `validateRequest` → `Library::tracksByIds`: an entry per id in order, a track
  or `track-unavailable`; a failed request fails them all (HOST-30). Answers go only while
  connected.
- **The host's actions** (HOST-20) return false without a connection. `playNext` pins a track
  already waiting; otherwise it adds it and pins the item when a `state` brings it (added by the
  host), unless it came pinned. A refused add of `playNext` is `refused` and nothing is pinned.
- **End.** `end()` sends `end` when connected, then ends here: the file is cleared, `JamMode::end`
  keeps the jam items as ordinary tracks (HOST-32), the client stops, `ended(ByHost)`. `ended` from
  the server does the same without `end` (`ByServer`, `Expired`).
- **Listening along** (`spec/jam/listen.md`, LISTEN-01 to LISTEN-05). With `config().shareAudio`,
  a `playing` for an item or a wave track carries `listenUrl`, the file the host plays
  (`JamPlayback::link`), and `listenNextUrl`, the preloaded next one (`JamPlayback::nextLink`),
  each when it is a file of Yandex Music's storage (`IsListenUrl`); any other link (a test's mock
  server) is left out, so the server takes the report. Guests on the jam page may then play it.
- `changed()` follows any change of `phase()`, the connection, `room()` or `joinUrl()`.

`JamTrackOf` builds the protocol's track like the server checks it: a catalog id
(`[0-9A-Za-z_-]{1,64}`, else nullopt), the album id only when it is one, control characters as
spaces, the title cut to 150 characters (nullopt when empty), up to 10 non-empty artists of 64
characters, the duration within 0 … 24 h, and the cover only as a template with `%%`, no scheme
and no spaces, 300 characters at most. Cutting never splits a surrogate pair.

**Traps:**
- The search's and the check's callbacks may come after the session is gone or the connection
  changed: they hold a `QPointer` and send only while connected.
- `request` takes a builder, so a refused action does not use up a request id.

## Not here

- What the server does and the protocol's rules: Kickoman/QiYaa-jam and `spec/jam/`.
- The jam mode of the queue (mirror of the state, the jam wave, reports): `Core::JamMode`,
  [src/core/README.md](../core/README.md).
- Where the settings and the session file live, the question at start and the texts of refusals:
  [src/app/README.md](../app/README.md).
