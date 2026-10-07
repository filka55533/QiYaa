# `src/yandex` — Yandex Music HTTP client

Talks to the unofficial Yandex Music API (`https://api.music.yandex.net`) and to Yandex OAuth
(`https://oauth.yandex.ru`): sends authorised JSON and form requests, parses the account, tracks,
playlists, albums, artists, stations, waves and search results, reports plays and wave feedback,
turns a track id into a signed mp3 link, runs the OAuth device-code login, and finds, normalises
and stores the token. The endpoints are the ones Yaamp used. Every network call is asynchronous
and answers through a callback or a signal. The module does not download or play audio, fetch
cover images or decide what plays next: that is [src/core](../core/README.md) (`Player`,
`CoverCache`). It shows nothing on screen: the login dialog and the library menu are in
[src/ui](../ui/README.md), and where the token file lives is decided in
[src/app](../app/README.md). Namespace `Yandex`, library `qiyaa_yandex`.

| File | Contains |
|---|---|
| `api_client.h/.cpp` | `ApiClient` — authorised GET/POST, the `{"result": …}` envelope, the pending-POST count; `Account`, `Track`, `ResolvedUrl`, `RequestError`, `ClassifyReply`, `TForm`; `accountStatus`, `tracks`, `resolveTrackUrl`, `reportPlayStarted` |
| `library.h/.cpp` | `Library` — the logged-in account, likes and dislikes, playlists, "For you" playlists, artists, albums, stations, waves (rotor sessions, feedback, the wheel), search; `NamedReference`, `PlaylistReference`, `Station`, `WaveBatch`, `Wave`, `WaveEvent`, `SearchResult` |
| `oauth.h/.cpp` | `DeviceLogin` — the OAuth device-code flow; `BrowserLoginUrl` for the implicit-grant fallback |
| `token.h/.cpp` | `NormalizeToken`, `FindToken`, `SaveToken`, `ForgetToken`; `TokenSource` |
| `track_url.h/.cpp` | `ParseDownloadVariants`, `PickBestVariant`, `ParseDownloadInfo`, `BuildTrackUrl` — from download-info to a signed mp3 link; `DownloadVariant`, `DownloadInfo` |

## Dependencies

- `qiyaa_yandex` links PUBLIC `Qt6::Core` and `Qt6::Network`, nothing PRIVATE.
- It deliberately links no other project module and neither `Qt6::Gui` nor `Qt6::Widgets`. It sits
  at the bottom of the module order; which modules link it is in
  [docs/architecture.md](../../docs/architecture.md#modules).
- Inside the module: `library.h` includes `api_client.h`, and `api_client.cpp` includes
  `track_url.h`. `oauth` and `token` include only their own headers.

```bash
grep -rn --include='*.cpp' --include='*.h' '#include "' src/yandex/ | grep -v '"yandex/'   # must print nothing
grep -rnw --include='*.cpp' --include='*.h' 'throw' src/yandex/                           # must print nothing
```

## `api_client.h` — `ApiClient`

```cpp
struct Account {
    QString uid;
    QString login;
    QString displayName;   // login when the account has no display name
};

struct Track {
    QString id;            // decimal id as text
    QString albumId;       // first album; empty when the track has none
    QString title;         // " (version)" appended when the track has a version
    QStringList artists;
    qint64 durationMs = 0;
    bool available = true;
    QString albumTitle;
    int year = 0;
    QString genre;
    QString coverUri;      // "avatars.yandex.net/get-music-content/.../%%", %% = size

    QString displayTitle() const;          // "Artist1, Artist2 - Title"
    QUrl webUrl() const;                   // the track's music.yandex.ru page
    QUrl coverUrl(int size = 400) const;   // empty when coverUri is empty
};

struct ResolvedUrl {
    QUrl url;
    int bitrateKbps = 0;
};

struct RequestError {
    enum class Kind { None, Network, Http, Content };
    Kind kind = Kind::None;
    int httpStatus = 0;   // the server's status, when it answered
    QString text;         // the same text the QString callbacks get
    bool isError() const;
};
RequestError ClassifyReply(const QNetworkReply& reply);   // text left empty

using TForm = QList<std::pair<QString, QString>>;

class ApiClient : public QObject {
public:
    template <typename T>
    using TCallback = std::function<void(const T& value, const QString& error)>;
    using TJsonCallback = std::function<void(const QJsonValue& result, const QString& error)>;
    using TClassifiedJsonCallback = std::function<void(const QJsonValue& result, const RequestError& error)>;

    explicit ApiClient(QNetworkAccessManager* networkAccessManager, QObject* parent = nullptr);

    void setToken(const QString& token);
    const QString& token() const;
    bool hasToken() const;
    QNetworkAccessManager* network() const;
    void setBaseUrl(const QString& base);   // tests: the mock server

    void getJson(const QString& path, const QUrlQuery& query, TJsonCallback callback);
    void getClassifiedJson(const QString& path, const QUrlQuery& query, TClassifiedJsonCallback callback);
    void postForm(const QString& path, const TForm& form, TJsonCallback callback);
    void postJson(const QString& path, const QJsonObject& body, TJsonCallback callback);

    void accountStatus(TCallback<Account> callback);
    void tracks(const QStringList& ids, TCallback<QList<Track>> callback);
    using TUrlCallback = std::function<void(const ResolvedUrl& link, const RequestError& error)>;
    void resolveTrackUrl(const QString& trackId, TUrlCallback callback);
    void reportPlayStarted(const Account& account, const Track& track, const QString& playId);

    int pendingPosts() const;   // POSTs whose reply has not finished

    static Track ParseTrack(const QJsonValue& value);
    static QString IdString(const QJsonValue& value);   // JSON number or string -> text

Q_SIGNALS:
    void postsSettled();   // pendingPosts() dropped to 0
};
```

- The client does not own `networkAccessManager`; the manager must outlive it. Use both from the
  thread that owns the manager (the GUI thread in the application).
- Every callback runs from the event loop on the client's thread, after the call that started the
  request has returned. It runs once per request: `callback(result, "")` on success, where
  `result` is the unwrapped envelope, and `callback({}, error)` on failure; typed calls pass a
  default-constructed value with the error. If the client is destroyed before the reply, the
  callback is dropped (the connection uses the client as its context).
- `path` is appended to the base URL (default `https://api.music.yandex.net`) as is. Headers,
  timeouts, body encodings and the envelope are in [API requests](#api-requests-and-the-envelope).
- `ParseTrack` reads the fields listed under [Track objects](#track-objects). `IdString` turns a
  JSON number into integer text (through `static_cast<qint64>`), returns a string as is, and
  anything else as an empty string.
- `Track::coverUrl(size)` replaces `%%` with `<size>x<size>` and prepends `https://` unless the URI
  already starts with `http`. `webUrl()` is `https://music.yandex.ru/album/<albumId>/track/<id>`, or
  `https://music.yandex.ru/track/<id>` without an album. `displayTitle()` is the bare title when
  there are no artists.
- `tracks(ids)` sends every id in one request; `Library::tracksByIds` does the chunking.
- `resolveTrackUrl(trackId)` uses the part of `trackId` before the first `:` (so `id:albumId` is
  accepted) and makes two GETs, described in [Download info](#download-info-and-the-signed-link).
  The result is the signed link and the chosen variant's bitrate.
- `reportPlayStarted` posts `/play-audio` and reports a start only: the played and end positions
  are always 0. It has no callback; a failure is logged with `qWarning` (`play-audio failed: …`).
- `pendingPosts()` counts `postForm` and `postJson` requests; GETs, including both requests of
  `resolveTrackUrl`, are not counted. A POST is uncounted only after its callback has run, so a
  POST sent from inside a callback is counted before the total can reach 0. `postsSettled()` fires
  every time the count drops to 0. The application waits for it when quitting
  ([src/app](../app/README.md)).

**Traps:**
- The order of connections in `postForm` and `postJson` is load-bearing: `handleJson` connects
  first, `trackPost` second, so the count drops after the callback. Swapping the two calls makes
  `postsSettled` fire before a wave-feedback fallback is sent (test
  `postsSettleOnlyAfterTheFallback`).
- The error text is parsed downstream: `Library::waveFeedback` falls back only on errors that
  start with `HTTP 4`. Keep the format `HTTP <status> from <path>: <message>`.
- A 2xx reply whose top level is a JSON array or scalar is reported as an error. A 2xx object that
  happens to have a `result` member is unwrapped even if it is not an envelope.
- `IdString` goes through `double`: an id above 2^53 loses digits.
- `setToken` does not touch requests already in flight; they finish with the old token.
- `accountStatus` treats a reply without `account.uid` as an error even when the status is 200.
- `resolveTrackUrl` sends the `Authorization` header to whatever host `downloadInfoUrl` names.
- `resolveTrackUrl` can end up with a preview or a non-mp3 variant (see `PickBestVariant`).

## `library.h` — `Library`

```cpp
struct NamedReference { QString id; QString name; };

struct PlaylistReference {
    QString ownerUid;
    QString kind;
    QString title;
    int trackCount = 0;
};

struct Station {
    QString id;     // "type:tag", e.g. "genre:rock", "user:onyourwave"
    QString type;   // "genre", "mood", ...
    QString name;
};

struct WaveBatch {
    QString sessionId;
    QString batchId;
    QList<Track> tracks;
};

struct Wave {       // an entry of the wheel of waves: a preset with seeds
    QString name;
    QString description;
    QStringList seeds;
};

enum class WaveEvent { RadioStarted, TrackStarted, TrackFinished, Skip };

struct SearchResult {
    enum class Kind { None, Artist, Album, Track, Playlist, Other };
    Kind bestKind = Kind::None;  // the "type" of "best": None when absent, Other when unknown
    QString bestId;
    QString bestName;
    QList<Track> tracks;
};

class Library : public QObject {
public:
    template <typename T>
    using TCallback = ApiClient::TCallback<T>;

    explicit Library(ApiClient* api, QObject* parent = nullptr);
    ApiClient* api() const;

    void connectAccount(TCallback<Account> callback);   // for the client's current token
    void logout();
    bool isLoggedIn() const;
    const Account& account() const;

    void likedTracks(TCallback<QList<Track>> callback);
    void tracksByIds(const QStringList& ids, TCallback<QList<Track>> callback);
    void userPlaylists(TCallback<QList<PlaylistReference>> callback);
    void playlistTracks(const PlaylistReference& playlist, TCallback<QList<Track>> callback);
    void likedArtists(TCallback<QList<NamedReference>> callback);
    void artistTopTracks(const QString& artistId, TCallback<QList<Track>> callback);
    void likedAlbums(TCallback<QList<NamedReference>> callback);
    void albumTracks(const QString& albumId, TCallback<QList<Track>> callback);
    void stations(TCallback<QList<Station>> callback);
    void startWave(const QStringList& seeds, TCallback<WaveBatch> callback);
    void moreWave(const QString& sessionId, const QStringList& queue, TCallback<WaveBatch> callback);
    void search(const QString& text, TCallback<SearchResult> callback);
    void searchTracks(const QString& text,
                      std::function<void(const QList<Track>& tracks, const RequestError& error)> callback);

    void personalPlaylists(TCallback<QList<PlaylistReference>> callback);   // "For you"
    void playlistRecommendations(const PlaylistReference& playlist, TCallback<QList<Track>> callback);
    void wheelWaves(const QStringList& seeds, TCallback<QList<Wave>> callback);

    void waveFeedback(
        const QString& sessionId,
        const QString& stationId,
        const QString& batchId,
        WaveEvent event,
        const Track* track = nullptr,
        double playedSeconds = 0
    );
    static QString WaveEventName(WaveEvent event);

    bool isLiked(const QString& trackId) const;
    void setLiked(const QString& trackId, bool liked, TCallback<bool> callback);
    void dislike(const QString& trackId, TCallback<bool> callback);

    static QList<Track> ParseTrackArray(const QJsonArray& items);
    static WaveBatch ParseWaveBatch(const QJsonValue& result);

Q_SIGNALS:
    void accountChanged();
    void likesChanged();
};
```

- The library does not own `api`; the client must outlive it. Threads and callback timing are the
  client's (see above), with the one exception in the traps.
- `likedTracks`, `userPlaylists`, `likedArtists`, `likedAlbums`, `setLiked` and `dislike` use
  `/users/<account().uid>/…`. Every request and reply shape is in the
  [endpoint table](#endpoints-of-apimusicyandexnet).
- `connectAccount` asks `accountStatus`. On success it stores the account and emits
  `accountChanged` before the callback runs.
- `logout` clears the account and the liked set, sets the client's token to empty, and emits
  `accountChanged`, then `likesChanged`. It does not touch the token file; the application calls
  `ForgetToken` for that.
- The liked set behind `isLiked` is replaced by `likedTracks` (with `likesChanged` emitted as soon
  as the ids arrive, before the track metadata), and updated by `setLiked` and `dislike` when the
  server accepts them; `dislike` emits `likesChanged` only if the track was liked. `isLiked` is
  false for every id until `likedTracks` has answered.
- `tracksByIds` asks for 250 ids per request (`kTracksPerRequest`), one request after another,
  and concatenates the replies in request order; within a request the order is the server's, and
  ids it does not return are missing. After an error the callback gets the tracks of the requests
  that succeeded, together with the error.
- `playlistTracks` and `playlistRecommendations` use the track objects embedded in the items when
  every one of them has a title, and otherwise fetch all ids through `tracksByIds`.
- `artistTopTracks` fetches the first 100 ids by rating (`kArtistTopLimit`).
- `likedAlbums` resolves the liked ids with one `POST /albums`, drops podcasts and names each
  album `First artist - Title` (the title alone without artists).
- `albumTracks` joins all volumes (discs) in order; a track without an album id gets the
  requested one.
- `startWave` opens a rotor session and fails when the reply has no `radioSessionId`. `moreWave`
  asks the session for the next batch, given the ids of the queued tracks; when the reply has no
  session id, the batch keeps the one passed in.
- `personalPlaylists` returns the "For you" playlists (Плейлист дня, Дежавю, Премьера,
  Тайник, …). `wheelWaves` returns waves suggested around `seeds`, for example the wave that is
  playing.
- `waveFeedback` tells the wave what the user did, so that "My Vibe" adapts. It is
  fire-and-forget: no callback, failures are logged. It builds one
  [event object](#wave-feedback-event) and sends it:
  1. to the station endpoint when `sessionId` is empty or the session is already marked;
  2. otherwise to the session endpoint. If that fails with an error starting with `HTTP 4`, it
     logs with `qInfo`, marks the session and re-sends the event to the station endpoint. Any
     other error (a 5xx, or status 0 for a timeout) is logged with `qWarning` and not re-sent,
     because the server may have counted the event; a second copy would count twice.

  The station endpoint is skipped when `stationId` is empty.
- `setLiked` and `dislike` call back with `true, ""` on success and `false, error` otherwise.
- If the library is destroyed before a reply, callbacks that need the library afterwards are
  dropped: `connectAccount`, `tracksByIds` and everything that ends in it (`likedTracks`,
  `playlistTracks`, `playlistRecommendations`, `artistTopTracks`), `likedAlbums`, `setLiked`,
  `dislike` and the feedback fallback. The others still call back while the client lives.

**Traps:**
- `tracksByIds({})` calls back synchronously, before it returns.
- A user-scoped call before `connectAccount` has succeeded requests `/users//…` and fails.
- A failed `connectAccount` keeps the previous account.
- Embedded tracks are all or nothing: one item without a title makes the library fetch every id
  again, and an item shaped `{"track": {…}}` without a top-level `id` adds an empty id to that
  request.
- `likedAlbums` puts every album id into one form; unlike tracks, albums are not chunked.
- The feedback fallback depends on the error text of `ApiClient` (`HTTP 4…`). A session marked for
  the station endpoint stays marked for the life of the `Library`, logout included; the set only
  grows.
- `sessionId` and `stationId` go into the path unencoded (`user:onyourwave` keeps its colon); only
  `batch-id` is percent-encoded.
- The session endpoint wants `{"event": …, "batchId": …}`, the station endpoint the bare event.
  Do not unify the bodies.

## `oauth.h` — `DeviceLogin`

```cpp
class DeviceLogin : public QObject {
public:
    explicit DeviceLogin(QNetworkAccessManager* networkAccessManager, QObject* parent = nullptr);
    ~DeviceLogin() override;   // cancels

    void setBaseUrl(const QString& base);   // tests: instead of https://oauth.yandex.ru

    void start();
    void cancel();

    static QUrl BrowserLoginUrl();   // implicit-grant login page for the fallback

Q_SIGNALS:
    void codeReady(const QString& userCode, const QUrl& verificationUrl);
    void succeeded(const QString& token);
    void failed(const QString& error);
};
```

Two ways to a token without an embedded browser:
1. **Device flow** (`start`): the server hands out a short code, the user enters it at
   `ya.ru/device` in any browser, even on a phone, and `DeviceLogin` polls until the token is
   issued.
2. **Fallback** (`BrowserLoginUrl`): the login page opens in the system browser. After logging in,
   Yandex redirects to `music.yandex.ru/#access_token=…`; the user pastes that address, or the
   bare token, into the dialog ([src/ui](../ui/README.md)), which passes it through
   `NormalizeToken`.

- `start()` cancels a running flow, then asks for a device code. Each `start()` ends in exactly one
  of `succeeded` or `failed`, unless `cancel()` or the destructor comes first; `codeReady` fires
  at most once before that.
- Polling uses a single-shot timer with the server's `interval` (default 5 s, at least 1 s).
  `authorization_pending` polls again; `slow_down` adds 2 s to the interval, then polls again. The
  code lives `expires_in` seconds (default 300); the first poll after that emits
  `failed("the code has expired, start again")`.
- `cancel()` stops the timer, aborts the request in flight and forgets the device code. It emits
  nothing, and the aborted reply is ignored.
- Requests carry no `Authorization` header and have a 20 s transfer timeout. The client id and
  secret are those of Yandex Music's public OAuth client (the note next to them in `oauth.cpp`
  says whose).
- Only the access token is passed on; `expires_in` and any refresh token of the `/token` reply
  are ignored.
- Same threading and ownership rules as `ApiClient`: `networkAccessManager` is not owned and must
  outlive the object.

**Traps:**
- A transport error while polling (timeout, network down) ends the flow with `failed(<Qt error>)`;
  there is no retry.
- `BrowserLoginUrl` is static and always points at `https://oauth.yandex.ru`; `setBaseUrl` does
  not change it.
- The expiry text is Russian because the dialog shows it as is; the other failure texts come from
  the server or from Qt.
- The form encoding is a copy of `ApiClient::postForm`'s (`FormBody`). `DeviceLogin` does not use
  `ApiClient` because OAuth lives on another host and must not get the `OAuth` header.
- The HTTP status of `/device/code` is not checked: the reply counts as good when it has both
  `device_code` and `user_code`.

## `token.h` — token discovery and storage

```cpp
struct TokenSource {
    QString token;    // empty when none was found
    QString origin;   // "environment variable QIYAA_TOKEN" or the file path
};

QString NormalizeToken(const QByteArray& raw);   // empty when nothing token-like is found
TokenSource FindToken(const QString& tokenFile, const QStringList& importFiles);
bool SaveToken(const QString& tokenFile, const QString& token);   // false when not written
void ForgetToken(const QString& tokenFile);
```

- `FindToken` looks in this order and stops at the first hit:
  1. `$QIYAA_TOKEN`, if it normalises to a token; origin `environment variable QIYAA_TOKEN`.
  2. `tokenFile`, if it can be opened for reading: its token, or an empty `TokenSource` when the
     file is empty or holds no token. The import files are then not tried, because an empty own
     file means the user logged out.
  3. Each of `importFiles` in order; files that do not open or hold no token are skipped.
  4. Otherwise an empty `TokenSource`.

  The application passes its own `token` file and Yaamp's `token.json` files
  ([src/app](../app/README.md)).
- `SaveToken` creates or truncates the file, sets owner read/write (mode 0600) and writes the
  token as UTF-8 with no newline. It returns false when the file does not open or nothing was
  written, so an empty token gives false. The parent directory must exist.
- `ForgetToken` leaves an empty file with mode 0600, which also stops `FindToken` from importing
  the old Yaamp token again. Failures are silent.
- There is no keychain: the token sits in a plain file.

**Traps:**
- The application tells an imported token by its origin (not under its config directory and not
  starting with `environment`) and then saves it to its own file. Changing the origin texts
  breaks that.
- The mode is set after the file is opened, so a newly created file briefly has the default
  (umask) mode. On Windows Qt maps the mode to the read-only attribute only; the file keeps the
  permissions of its folder.
- Truncate-then-write is not atomic: a crash in between leaves an empty file, which `FindToken`
  reads as "logged out".

## `track_url.h` — download-info and link signing

```cpp
struct DownloadVariant {   // one entry of GET /tracks/{id}/download-info
    QString codec;
    int bitrateKbps = 0;
    bool preview = false;
    QUrl downloadInfoUrl;
};

struct DownloadInfo {      // reply of GET {downloadInfoUrl}&format=json
    QString host;
    QString path;
    QString ts;
    QString s;
};

QList<DownloadVariant> ParseDownloadVariants(const QJsonArray& result);
// nullopt when empty
std::optional<DownloadVariant> PickBestVariant(const QList<DownloadVariant>& variants);
std::optional<DownloadInfo> ParseDownloadInfo(const QByteArray& json);   // nullopt when unusable
QUrl BuildTrackUrl(const DownloadInfo& info);
```

- Pure functions: no network, no state, callable from any thread.
- `ParseDownloadVariants` drops entries whose `downloadInfoUrl` is missing or not a valid URL.
- `PickBestVariant` takes the highest `bitrateKbps` among the `mp3` variants that are not
  previews; on a tie the first wins. If none qualifies it returns the first variant whatever its
  codec or preview flag, and `nullopt` only for an empty list.
- `ParseDownloadInfo` and `BuildTrackUrl` follow
  [Download info and the signed link](#download-info-and-the-signed-link).

**Traps:**
- The fallback to the first variant can give the player a preview or an AAC stream, still under
  `/get-mp3/`.
- `ts` is not required; an empty one yields `…/get-mp3/<sign>//<path>`.
- Tests `signsTrackUrlLikeYaamp`, `downloadVariantsAreParsedAndTheBestIsPicked` and
  `storageReplyGivesTheSignedLink` in `tests/yandex_test.cpp` pin the signing and the choice.
  The last two read `spec/fixtures/yandex/tracks-download-info` and `storage-download-info`,
  whose expected links the Android tests check too.

## Formats read

### Token file

A text file, read up to 64 KiB (`64 * 1024` bytes); the rest is ignored. `NormalizeToken` also
handles `$QIYAA_TOKEN` and text pasted into the login dialog. After UTF-8 decoding and trimming
whitespace, it applies these steps in order:

1. If the text starts with `{` or `"`, it is parsed as JSON: a string is taken as the token, an
   object gives its `access_token`.
2. If the text contains `access_token=`, the token is what follows, up to `&`, `#` or whitespace
   (a redirect URL such as `https://music.yandex.ru/#access_token=y0_…&token_type=bearer`).
3. A leading `OAuth ` (any case) is removed.
4. What remains must match `^[A-Za-z0-9._\-]{10,}$`: tokens are URL-safe ASCII, and anything else
   means the parse found garbage.

Refused (empty result): an empty file, and anything that fails step 4. Files written by
`SaveToken` hold the bare token without a newline; an empty file means "logged out".

### API requests and the envelope

- Base URL `https://api.music.yandex.net`. Every request has `Accept-Language: ru` and, when the
  token is not empty, `Authorization: OAuth <token>`. Transfer timeout 20 000 ms (`kTimeoutMs`).
  Redirects are followed unless they are less safe (`NoLessSafeRedirectPolicy`: no https → http).
- Form bodies are `application/x-www-form-urlencoded`, keys and values percent-encoded and joined
  with `&`. JSON bodies are `application/json`, compact.
- A reply with a network error or status ≥ 400 is a failure (see [Errors](#errors)). Otherwise the
  body must be a JSON object. `{"result": X}` passes `X` on; an object without `result` is passed
  on whole. Most endpoints wrap the payload in `result`; newer ones, such as `/wheel/new`, do not.

### Endpoints of api.music.yandex.net

Replies are listed after unwrapping. `<uid>` is the logged-in account's uid.

| Call | Request | Reply fields read |
|---|---|---|
| `accountStatus` | `GET /account/status` | `account.uid` (required), `account.login`, `account.displayName` |
| `tracks`, `tracksByIds` | `POST /tracks/` form `track-ids=<id,id,…>`, `with-positions=false` | array of [track objects](#track-objects) |
| `resolveTrackUrl` | `GET /tracks/<id>/download-info`, then see [below](#download-info-and-the-signed-link) | array of `{codec, bitrateInKbps, preview, downloadInfoUrl}` |
| `reportPlayStarted` | `POST /play-audio` form `track-id`, `album-id`, `from=web-own_tracks-track-track-main`, `play-id`, `uid`, `timestamp` and `client-now` (both the current UTC time, ISO 8601 with ms), `track-length-seconds` (`durationMs / 1000.0`, may have a fraction), `total-played-seconds=0`, `end-position-seconds=0` | none |
| `likedTracks` | `GET /users/<uid>/likes/tracks` | `library.tracks[].id`, then `tracksByIds` |
| `userPlaylists` | `GET /users/<uid>/playlists/list` | array of playlists: `uid` (or `owner.uid`), `kind`, `title`, `trackCount`; entries without `kind` dropped |
| `playlistTracks` | `GET /users/<ownerUid>/playlists/<kind>` | `tracks[]` items |
| `playlistRecommendations` | `GET /users/<ownerUid>/playlists/<kind>/recommendations` | `tracks[]` items |
| `personalPlaylists` | `GET /landing3?blocks=personalplaylists` | `blocks[].entities[].data`: a "generated playlist" whose `data` member, when it is an object, is the playlist itself: `uid` (or `owner.uid`), `kind`, `title`, `trackCount`; entries without owner or `kind` dropped |
| `wheelWaves` | `POST /wheel/new` JSON `{"context": {"type": "WAVE", "data": {"seeds": [...]}}, "feedbacks": []}` | not wrapped: `items[]` with `type == "WAVE"` give `data.wave.{name, description, seeds[]}`; entries without name or seeds dropped |
| `waveFeedback` (session) | `POST /rotor/session/<sessionId>/feedback` JSON `{"event": <event>, "batchId": "<batchId>"}`, `batchId` left out when empty | none |
| `waveFeedback` (station) | `POST /rotor/station/<stationId>/feedback?batch-id=<batchId>` JSON `<event>`, query left out when `batchId` is empty | none |
| `likedArtists` | `GET /users/<uid>/likes/artists` | array of artist objects or `{"artist": {…}}`: `id`, `name`; entries without `id` dropped |
| `artistTopTracks` | `GET /artists/<id>/track-ids-by-rating` | `tracks[]` ids, the first 100 then `tracksByIds` |
| `likedAlbums` | `GET /users/<uid>/likes/albums`, then `POST /albums` form `album-ids=<id,id,…>` (skipped when there are no ids) | first: array of album objects or `{"album": {…}}`: `id`; second: array of `{id, title, type, artists[0].name}`, `type == "podcast"` dropped |
| `albumTracks` | `GET /albums/<id>/with-tracks` | `volumes[][]`, arrays of track objects |
| `stations` | `GET /rotor/stations/list?language=ru` | `[].station.{id.type, id.tag, name}`; entries without `id.type` dropped |
| `startWave` | `POST /rotor/session/new` JSON `{"seeds": [...], "includeTracksInResponse": true, "includeWaveModel": true, "interactive": true}` | `radioSessionId` (required), `batchId`, `sequence[]` |
| `moreWave` | `POST /rotor/session/<sessionId>/tracks` JSON `{"queue": ["<track id>", ...]}` | `radioSessionId` (optional), `batchId`, `sequence[]` |
| `search` | `GET /search?text=<text>&type=all&page=0` | `best.type`, `best.result.id`, `best.result.name` or else `best.result.title`, `tracks.results[]` |
| `searchTracks` | `GET /search?text=<text>&type=track&page=0` (the jam's search for guests, HOST-28) | `tracks.results[]`; the error by kind and HTTP status, so a 401 or 403 can be told apart |
| `setLiked` | `POST /users/<uid>/likes/tracks/add-multiple` (like) or `…/likes/tracks/remove` (unlike), form `track-ids=<id>` | none |
| `dislike` | `POST /users/<uid>/dislikes/tracks/add-multiple` form `track-ids=<id>` | none |

#### Track objects

`ParseTrack` reads `id` (number or string), `title`, `version` (appended as ` (version)`),
`artists[].name`, the first album `albums[0].{id, title, year, genre, coverUri}`, the track's own
`coverUri` and then `ogImage` when the album has no cover, `durationMs`, and `available` (true
when absent). `ParseTrackArray` parses an element from its `track` member when that member is an
object, as in `{"track": {…}}` or `{"id": …, "track": {…}}` (wave sequences, playlist items);
otherwise from the element itself.

#### Wave feedback event

| Field | Value | When |
|---|---|---|
| `type` | `radioStarted`, `trackStarted`, `trackFinished`, `skip` (`WaveEventName`) | always |
| `timestamp` | current UTC time, ISO 8601 with ms | always |
| `from` | `web-main-rup-radio-main` | `RadioStarted` |
| `trackId` | `<id>:<albumId>`, or `<id>` when the track has no album | a track is given |
| `totalPlayedSeconds` | `playedSeconds` rounded to 0.1 | `TrackFinished`, `Skip` |

### OAuth endpoints of oauth.yandex.ru

| Call | Request | Reply fields read |
|---|---|---|
| `start` | `POST /device/code` form `client_id`, `device_name=QiYaa (<host name>)` | `device_code` and `user_code` (both required), `verification_url` (`https://ya.ru/device` when missing or invalid), `interval` (s, default 5, at least 1), `expires_in` (s, default 300); on failure `error_description`, else `error` |
| poll | `POST /token` form `grant_type=device_code`, `code=<device_code>`, `client_id`, `client_secret` | `access_token`; otherwise `error` (`authorization_pending` and `slow_down` keep polling, anything else fails) and `error_description` |
| `BrowserLoginUrl` | `https://oauth.yandex.ru/authorize?response_type=token&client_id=<id>`, opened in the browser | — |

### Download info and the signed link

1. `GET /tracks/<id>/download-info` (enveloped) gives the variants; `PickBestVariant` chooses one.
2. `GET <downloadInfoUrl>` with `format=json` added to its query, with the same headers, not
   enveloped. The body must be a JSON object; `host`, `path`, `ts` and `s` are read as strings (a
   number is taken as integer text), other members such as `regional-host` are ignored. Refused
   (`nullopt`): not a JSON object, empty `host`, `path` not starting with `/`, empty `s`.
3. The link is

   ```
   https://{host}/get-mp3/{md5(salt + path[1:] + s)}/{ts}{path}
   ```

   The MD5 runs over the bytes of the salt `kSignSalt` (`track_url.cpp`), the UTF-8 path without its
   leading `/`, and the UTF-8 `s`, and is written as 32 lowercase hex digits. The path keeps its
   leading `/`, so it follows `ts` directly. Example from `signsTrackUrlLikeYaamp`: host
   `s123vla.storage.yandex.net`, path `/rmusic/U2FsdGVk/abc`, ts `000612a3b4c5d`, s `deadbeef` give
   `https://s123vla.storage.yandex.net/get-mp3/5c38e49c01f9a959428986790af6f9ca/000612a3b4c5d/rmusic/U2FsdGVk/abc`.

## Errors

The module throws nothing and catches nothing, and it has no `Error` class (the project's error
policy is in [docs/architecture.md](../../docs/architecture.md#errors)). Every failure is data:

| Where | Failure arrives as |
|---|---|
| `ApiClient`, `Library` calls | `callback(<empty value>, error)`; `tracksByIds` passes the tracks fetched so far; `setLiked` and `dislike` pass `false` |
| `reportPlayStarted`, `waveFeedback` | log lines only (`qWarning`, `qInfo` for the fallback) |
| `DeviceLogin` | the `failed(error)` signal |
| `NormalizeToken`, `FindToken` | an empty `QString`, an empty `TokenSource` |
| `SaveToken`, `ForgetToken` | `false`; nothing |
| `track_url` functions | `std::nullopt` |

`resolveTrackUrl` reports a `RequestError`, so the `Player` can tell what to do without reading the
text (spec/player/errors.md). `ClassifyReply` decides the kind:

- **Network**: refused, closed by the remote side (also a body cut short after its status line),
  host not found, timeout, the transfer timeout (`OperationCanceledError` before Qt 6.11), a TLS
  handshake failure (captive portals), a temporary network failure and the proxy errors. These
  win over any status.
- **Http**: otherwise a status of 400 or more, in `httpStatus`.
- **Content**: any other failed reply, a reply that is not a JSON object, no usable download
  variant, a download-info reply without host, path and s.

The error strings, all in English except the device-code expiry:

| Text | Cause |
|---|---|
| `HTTP <status> from <path>: <message>` | network error or status ≥ 400. `<status>` is 0 when there was no HTTP reply (timeout, DNS, TLS, refused). `<message>` is the reply's `error.message`, else its `error` string, else Qt's error text. `<path>` is the URL path without the query. |
| `<path>: the <n>-byte reply is not a JSON object` | a 2xx reply that is not a JSON object |
| `not authorized (no uid) — token expired?` | `accountStatus`: no `account.uid` |
| `track <id>: none of <n> download variants has a usable link` | `resolveTrackUrl`: no variant with a valid `downloadInfoUrl` |
| `download-info: <Qt error>` | `resolveTrackUrl`: the second request failed |
| `download-info: no host, path and s in a <n>-byte reply` | `resolveTrackUrl`: `ParseDownloadInfo` refused the reply |
| `/rotor/session/new: the reply has no radioSessionId` | `startWave` |
| `the code has expired, start again` | `DeviceLogin`: the device code expired |
| the server's `error_description` or `error`, else Qt's error text | other `DeviceLogin` failures |

## Not here

- Downloading the mp3 bytes, preloading the next track, the play queue, the endless-wave "more"
  hook, cover images: [src/core](../core/README.md) (`Player`, `CoverCache`).
- The token file's location, the Yaamp `token.json` paths, when a token is saved, the wait for
  `postsSettled` on quit: [src/app](../app/README.md).
- The login dialog (showing the code, the paste field, opening the browser) and the wave glue in
  the library menu (which batch a track came from, the last 5 queued ids sent to `moreWave`, the
  station being the first seed): [src/ui](../ui/README.md).
- MPRIS and SMTC metadata built from `Track`: [src/integrations](../integrations/README.md).
- Tests: `tests/yandex_test.cpp` (pure functions: signing, variant choice, tokens, track parsing)
  and `tests/library_test.cpp` (every endpoint against `Tests::MockHttpServer` in `tests/support/`).
  The responses they serve and what parsing must give are shared with the Android app in
  `spec/fixtures/yandex` and `spec/expected/yandex` ([tests](../../tests/README.md#spec-fixtures)).
