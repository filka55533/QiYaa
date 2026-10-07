# `src/core` — playback controller, sources and cover cache

This folder decides what plays. `Player` holds the queue of the current source and moves through
it: play, pause, stop, next, previous, shuffle and repeat. It asks an endless source (a wave)
for more tracks before the queue runs out. For each track it resolves the download link through
`Yandex::ApiClient` and streams the mp3 bytes into `Audio::AudioEngine`. It also preloads the
following track, so the engine can go on without a gap, and it reports each track's start and end:
a play report to Yandex and a per-queue callback that the wave uses for feedback. `Sources` turns
what the user picks (likes, playlists, artists, albums, waves, stations, search) into the
`Player`'s queue and builds the wave's callbacks; it is the desktop counterpart of the Android
app's queue manager, and `spec/player` describes both. `JamMode` is the queue while the app hosts
a jam: the current track, the jam part that mirrors the server's queue, and the jam wave
(`spec/jam/host.md`). `CoverCache`
downloads album covers and keeps them in memory and on disk. The folder does not decode or output
audio or chain streams at the sample level ([src/audio](../audio/README.md)). It does not speak
the Yandex API, sign links or build cover URLs itself ([src/yandex](../yandex/README.md)). It
builds no menus ([src/ui](../ui/README.md), `library_menu.cpp`) and shows nothing on screen. Namespace `Core`, library `qiyaa_core`.

| File | Contains |
|---|---|
| `player.h/.cpp` | `Player`: the queue and cursor, transport, shuffle and repeat, source-request tickets, the endless-source hook `TLoadMoreCallback`, track events `TEventCallback`, link resolution, download streaming, the gapless preload, and the engine's poll timer |
| `track_navigator.h/.cpp` | `TrackNavigator`, `ShuffleAlgorithm`, `MakeTrackNavigator`: sequential, random and stored shuffled traversal |
| `failure_policy.h/.cpp` | `FailureKind`, `FailureAction`, `DecideOnFailure`: what the `Player` does about a track that cannot play |
| `jam_mode.h/.cpp` | `JamMode`, `JamEntry`, `JamSlot`, `JamPlayback`: the jam mode of the queue; it knows nothing of the network or the protocol |
| `sources.h/.cpp` | `Sources`: playing a source (likes, playlist, recommendations, artist, album, wave or station, search) with a ticket each, the wave's load-more and feedback callbacks, like and dislike by track ID |
| `cover_cache.h/.cpp` | `CoverCache`: cover images by URL, with an LRU of 30 in memory, files in a cache directory and one download per URL at a time |

## Dependencies

- `qiyaa_core` links PUBLIC `qiyaa_audio`, `qiyaa_yandex`, `Qt6::Gui` (`QImage` in
  `cover_cache.h`) and `Qt6::Network` (`QNetworkAccessManager`, `QNetworkReply`). It links nothing
  PRIVATE.
- It deliberately does not link `Qt6::Widgets`, `qiyaa_skins`, `qiyaa_vis`, or anything above it
  in the order `audio`, `yandex`, `skins` → `vis`, `core` → `ui`, `integrations` → `app`. It
  reaches miniaudio only through `qiyaa_audio`, which links it PRIVATE. Which modules link
  `qiyaa_core` is in [docs/architecture.md](../../docs/architecture.md#modules).
- `player` and `cover_cache` do not include each other.
- `Player` is the only caller of `AudioEngine::poll()` in `src/`. Everything the engine reports
  asynchronously therefore arrives through `Player`'s timer (see [Threads and
  lifetime](#threads-and-lifetime)).

```bash
grep -rnE --include='*.h' --include='*.cpp' '#include "(ui|vis|skins|integrations|app)/' src/core/     # must print nothing
grep -rnE --include='*.h' --include='*.cpp' '#include <(QWidget|QApplication|QDialog|QMenu|QtWidgets)' src/core/   # must print nothing
grep -rnwE --include='*.h' --include='*.cpp' 'throw|catch' src/core/                                    # must print nothing
grep -rn --include='*.h' --include='*.cpp' -e '->poll()' src/ | grep -v '^src/core/player.cpp:'         # must print nothing
```

## `player.h`: `Player`

```cpp
class Player : public QObject {
public:
    using TLoadMoreCallback = std::function<void(std::function<void(const QList<Yandex::Track>&)> done)>;
    enum class TrackEvent { Started, Finished, Skipped };
    using TEventCallback =
        std::function<void(TrackEvent event, const Yandex::Track& track, double playedSeconds)>;

    Player(Yandex::Library* library, Audio::AudioEngine* engine, QObject* parent = nullptr);

    quint64 newSourceRequest();                       // new ticket; every older one stops being the latest
    bool isLatestSourceRequest(quint64 ticket) const;

    void setQueue(
        const QList<Yandex::Track>& tracks,
        const QString& title,
        bool autoplay,                                // playIndex(0) right away
        TLoadMoreCallback more = {},                            // set = endless source
        TEventCallback events = {}
    );
    void appendTracks(const QList<Yandex::Track>& tracks);
    void insertTracks(int index, const QList<Yandex::Track>& tracks);   // at index, clamped
    void removeTracks(QList<int> indices);            // indices before removal, any order, duplicates ok
    void clearQueue();
    void changeSource(const QString& title, TLoadMoreCallback more, TEventCallback events, QueueRules rules);
    void requestMore();                               // the load-more check now

    void play();                                      // resume / restart while playing / start
    void pause();                                     // toggles
    void stop();
    void next();
    void previous();
    void playIndex(int index);
    void shutDown();                                  // for quitting; irreversible
    bool seekTo(double seconds);                      // false if the engine can't seek (yet)
    bool seekFraction(double fraction);               // 0..1 of durationSeconds()
    void setShuffle(bool on);
    void setShuffleAlgorithm(ShuffleAlgorithm algorithm);
    ShuffleAlgorithm shuffleAlgorithm() const;
    void setRepeat(bool on);
    bool shuffle() const;                             // the user's choice
    bool shuffleActive() const;                       // shuffle && the queue is not endless
    bool repeat() const;
    const QueueRules& rules() const;                  // QueueRules{playReports, previousRestartsOnly}
    bool isWaitingForMore() const;                    // stopped at the end of an endless queue
    bool trackInProgress() const;                     // a track is resolving, open or parked

    const QList<Yandex::Track>& playlist() const;
    const QString& queueTitle() const;
    int currentIndex() const;                         // -1 exactly when the queue is empty
    const Yandex::Track* currentTrack() const;        // nullptr exactly when the queue is empty
    int currentBitrate() const;                       // kbps of the current link; 0 until it resolves
    double durationSeconds() const;                   // Track::durationMs / 1000; 0 without a track
    Audio::AudioEngine* engine() const;
    Yandex::Library* library() const;
    int preloadedIndex() const;                       // -1 until the preload has an engine stream

    void setNetworkOnline(std::optional<bool> online); // the system's view; nullopt: cannot tell
    void setNetworkRetryDelays(int firstMs, int maxMs); // 2 s doubling to 60 s by default
    bool isWaitingForNetwork() const;

Q_SIGNALS:
    void statusMessage(const QString& text);
    void playlistChanged();
    void queueReplaced();
    void currentTrackChanged();
    void positionTick();                              // every 100 ms while the engine isn't Stopped
    void modesChanged();                              // shuffle or repeat
    void seeked(double seconds);                      // the clamped target
};
```

### Threads and lifetime

- Everything runs on the thread that owns the `Player`, which is the GUI thread in the app. There
  are no locks. Link, download and load-more results arrive as Qt signals or callbacks on that
  thread. `TLoadMoreCallback` and `TEventCallback` are called on it, and a `TLoadMoreCallback` must
  call `done` on it too.
- The engine emits `trackFinished` and `trackAdvanced` only from `AudioEngine::poll()`. The only
  caller of `poll()` is `Player`'s own `QTimer` (100 ms interval). The timer runs whenever the
  engine state is not `Stopped` (it follows `AudioEngine::stateChanged`), so the end of a track
  is noticed with every window hidden or minimised. Each tick runs `poll()` first, together with
  any track change it triggers, then adds to the played seconds, then emits `positionTick`.
- `Player` does not own the `Library` or the `AudioEngine`, and neither pointer is checked for
  null. Both must outlive the `Player`.

### The queue and the cursor

- `setQueue` and `appendTracks` keep only tracks with `Track::available == true`. Indices always
  refer to this filtered list.
- `currentIndex()` is a cursor. It is -1 exactly when the queue is empty. Otherwise it is a valid
  index, also while nothing plays. `setQueue` sets it to 0 and plays only with `autoplay`.
  `appendTracks` moves it from -1 to 0 and does not play.
- `removeTracks` takes indices of the queue before the removal. Duplicates and out-of-range indices
  are ignored. Removing tracks before the cursor shifts the cursor down. If the current track is
  removed, playback stops (it does not skip), and the cursor moves to the track that followed it,
  or to the new last track.
- `clearQueue()` takes a new ticket and calls `setQueue({}, {}, false)`. This also drops the
  `TLoadMoreCallback` and the `TEventCallback`.
- `insertTracks(index, tracks)` puts the available tracks at `index` (clamped to the queue). An
  index at or before the cursor shifts the cursor up by the number inserted; an empty queue gets
  the cursor 0. Then `playlistChanged` and `refreshPreload()`, which keeps the preload when its
  track still follows.
- `changeSource(title, more, events, rules)` keeps the queue, the cursor and the track that plays,
  and replaces what is behind them: the title, the `TLoadMoreCallback`, the `TEventCallback` (the
  open track still closes through the one it started with) and the `QueueRules`. It ends the wait
  for more, bumps `queueGeneration` (a late `done` of the old source is dropped), and emits
  `playlistChanged` and `modesChanged`, then `refreshPreload()`. `setQueue` resets the rules to
  the defaults.

### State machine

The state has three parts. The current track's phase:

| Phase | Engine state | Track open¹ | Entered by | Left by |
|---|---|---|---|---|
| **Empty** | Stopped | no | `setQueue` with no available track, `clearQueue`, removing every track | `setQueue` with tracks, `appendTracks` (to Stopped) |
| **Stopped** | Stopped | no | `stop`, `setQueue` without `autoplay`, the end of a finite queue, removing the current track, a failure the policy stops on (see [Failures](#failures-during-playback)) | `play`, `playIndex`, `next`, `previous` |
| **Resolving** | Buffering² | no | `playIndex` without a usable preload: the engine stream is begun, and the link request is in flight | link OK: **Open**. Link error: the [failure policy](#failures-during-playback). `stop`, `playIndex` or `setQueue`: the reply is dropped by `generation` |
| **Open**, downloading | Buffering, Playing, Paused | yes | the link resolved and the download started, or the preload was taken over while still downloading | download OK: **Open, downloaded**. Download failed: the failure policy at once if no audio arrived, else **Open, failed**. End or skip: see [Inputs](#inputs-in-the-order-they-act) |
| **Open**, downloaded | Buffering, Playing, Paused | yes | the current download finished without error, or a finished preload was taken over | end or skip. Only in this phase can a new preload start |
| **Open**, failed | Buffering, Playing, Paused | yes | the current download failed after some audio arrived; the engine plays what arrived | end: a network failure **Waiting for the network**, any other failure the end reported as `Skipped`. Skip |
| **Waiting for the network** | Paused (parked) | as before | a network failure (ERR-01 to ERR-03) | the link and download work: **Open** again, playing if it played before. Another failure: the policy. `stop`, `playIndex`, `setQueue`: the wait ends |
| **Waiting for more** | Stopped | no | `next()` at the end of an endless queue | `done` with new tracks: `playIndex(cursor + 1)`. `done({})`: stays Stopped. `play`, `playIndex`, `stop` or `setQueue` clear the wait |
| **Shut down** | Stopped | no | `shutDown()` | never |

¹ Its `Started` was reported, and `Finished` or `Skipped` has not been reported yet (`openTrack`).
² Without an output device, `beginStream()` leaves the engine Stopped and returns stream 0. The
link is still resolved, and the download runs into the ignored stream 0.

The preload's phase:

| Phase | `preloadedIndex()` | Meaning |
|---|---|---|
| none | -1 | |
| resolving | -1 | `preload` holds the index, the track id and a fresh `preloadGeneration`, and its link request is in flight |
| queued | its index | `AudioEngine::queueStream()` gave it a stream (`preload->stream != 0`). Its download runs, or has ended (`downloadDone`, `failed`) |

The load-more request: idle, or in flight (`loadingMore`). It is independent of both phases
above.

### Inputs, in the order they act

- **`setQueue(tracks, title, autoplay, more, events)`**
  1. `stop()`: the open track gets `Skipped` from the old `TEventCallback`, and the old link
     request, download and preload are dropped.
  2. The queue becomes the available tracks, and the title, `TLoadMoreCallback` and `TEventCallback`
     are replaced. The load-more and waiting flags are reset, and `queueGeneration` is bumped.
  3. The cursor goes to 0, or to -1 when the queue is empty.
  4. `queueReplaced`, then `playlistChanged`, then `currentTrackChanged`.
  5. If `autoplay` is set and the queue is not empty: `playIndex(0)`.
- **`playIndex(i)`** does nothing after `shutDown()` or when `i` is out of range. Otherwise:
  1. The wait for more ends and the open track gets `Skipped`. `generation` is bumped. The
     current download is disconnected and aborted, so it never reaches the finish handler. The
     cursor moves to `i`, the bitrate becomes 0 and the download flags are cleared.
  2. **The preload is used** when it has an engine stream, its track id equals `track.id`, and
     that stream is still `AudioEngine::queuedStream()`. `playQueuedNow()` starts it from its
     beginning at once, reusing the bytes already downloaded. The preload's reply and download
     flags become the current track's. Then `currentTrackChanged`, the load-more check, and
     `trackStarted`.
  3. **Otherwise:** `cancelPreload()`, then `AudioEngine::beginStream()`, then
     `currentTrackChanged` (with `currentBitrate()` still 0), then the load-more check, then
     `ApiClient::resolveTrackUrl`. The reply is ignored if `generation` has moved on. On an error:
     the [failure policy](#failures-during-playback); without an output device (stream 0) only
     `statusMessage("Cannot get link: <error>")` and a stop. On success: the download starts into
     the stream begun above, then `trackStarted`.
- **`trackStarted(track, bitrate)`** (private): stores the bitrate, sends
  `ApiClient::reportPlayStarted(account, track, <new UUID>)`, opens the track and remembers the
  current `TEventCallback` for its closing event. It resets the played seconds, sends
  `Started(track, 0)`, emits `currentTrackChanged` and calls `maybePreload()`.
- **`next()`**: with an empty queue, nothing happens. If a preload exists in any phase:
  `playIndex(preload index)`. Else if `pickNext() >= 0`: `playIndex` of it. Else, if the source is
  endless: `stop()`, mark waiting, run the load-more check, then emit
  `statusMessage("Loading more tracks…")`. Else (the end of a finite queue with repeat off):
  `stop()`, and the cursor stays on the last track.
- **`previous()`** (spec TR-01, TR-02): more than
  `kPreviousRestartsAfterSeconds` (3 s) into a playing or paused track, it restarts that track
  with `seekTo(0)`. That is a seek, not a new start: no events and no play report; if the engine
  refuses the seek, `playIndex(cursor)`. Earlier, or while stopped: `playIndex(cursor - 1)`; at
  position 0 the last track if repeat is on, the first otherwise. With shuffle, the cursor
  and target follow the navigator: stored order for WithoutRepeats, source order for Random.
- **`play()`**, by engine state: Paused resumes. Playing calls `playIndex(cursor)`, which restarts
  the track as Winamp does, with `Skipped` and then a new `Started`. Buffering does nothing.
  Stopped calls `playIndex(cursor)`, or `playIndex(0)` when the cursor is -1.
- **`pause()`**: Paused resumes. Playing or Buffering pauses. Stopped does nothing.
- **`stop()`**: sends `Skipped` for the open track, ends the wait for more, bumps `generation`,
  cancels the preload, aborts the download, stops the engine and sets the stream id to 0. It keeps
  the queue, the cursor, the `TLoadMoreCallback` and the bitrate. It emits no `Player` signal; the
  engine emits `stateChanged(Stopped)`, and that stops the timer.
- **Engine `trackFinished`**: the open track gets `Finished`, or `Skipped` if its download failed,
  with the played seconds. Then, if repeat is on and the queue has exactly one track,
  `playIndex(cursor)`; otherwise `next()`.
- **Engine `trackAdvanced`** (the engine crossed into the queued stream with no gap): the open track
  gets `Finished` or `Skipped` as above. If there is no preload, or its stream is not the engine's
  `currentStream()`, or its index is out of range, the player resyncs:
  `playIndex(pickNext())`, or `stop()` when that is -1. A cancelled preload is always cleared
  from the engine, so this branch is not expected to run. Otherwise the preload becomes the current
  track: `generation` is bumped, the preload's reply, stream, index and download flags are taken
  over, the wait ends, and then `currentTrackChanged`, the load-more check and `trackStarted`.
- **Download finished**:
  - If it is the current stream and it failed: with no audio bytes received, the failure policy
    at once; otherwise the failure kind is kept for the track's end (a network failure then
    waits, ERR-03; any other shows `statusMessage("Download failed: <Qt error>")` and ends the
    track as `Skipped`).
  - If it is the current stream and it succeeded: `currentDownloaded`, then `maybePreload()`.
  - If it is the preload's stream: a failure cancels the preload (that track is fetched again when
    its turn comes); success records `downloadDone`.
  - Anything else is ignored.
- **`appendTracks`**: appends the available tracks. If none were added, it returns without a
  signal. Otherwise the cursor moves from -1 to 0 if needed, then `playlistChanged`, then
  `refreshPreload()`.
- **`removeTracks`**: see [the queue](#the-queue-and-the-cursor). If the current track was
  removed: `stop()`, then `currentTrackChanged`. Then, always, `playlistChanged` and
  `refreshPreload()`.
- **`setShuffle`, `setRepeat`**: nothing happens without a change. With a change:
  `modesChanged`, then `refreshPreload()`. Switching shuffle on while the queue is endless also
  emits `statusMessage("Shuffle does not apply to a vibe")`; `setQueue` emits the same when an
  endless queue starts with shuffle on.
- **`seekTo(seconds)`**: clamps the target to `[0, durationSeconds()]`, or to `>= 0` when the
  duration is 0. It returns `false` and emits nothing when `AudioEngine::seek` refuses, which
  happens while stopped and until the engine's decoder has opened the stream. On success it moves
  the played-seconds baseline to the target and emits `seeked(target)`. `seekFraction(f)` is
  `seekTo(clamp(f, 0, 1) * durationSeconds())`, and returns `false` when the duration is 0.
- **`shutDown()`**: takes a new ticket, so source loads still in flight become stale. Then
  `stop()`, which reports the open track as `Skipped`; for a wave that becomes the "skip"
  feedback. From then on `playIndex` and `maybePreload` do nothing.

### Tickets and generation counters

Four counters, each bumped by different calls and each guarding one kind of late reply:

| Counter | Bumped by | Drops |
|---|---|---|
| `sourceRequest` (the ticket) | `newSourceRequest()`: callers before a source load, `clearQueue`, `shutDown` | a caller's source-load reply, when the caller checks `isLatestSourceRequest(ticket)`. `Player` never checks it itself |
| `queueGeneration` | `setQueue` (and so `clearQueue`) | a `TLoadMoreCallback`'s `done` for a queue that has been replaced (a `QPointer` also covers a destroyed `Player`) |
| `generation` | `stop`, `playIndex`, gapless advance | the link reply of a track that is no longer current |
| `preloadGeneration` | every new preload | the link reply of a preload that was cancelled or replaced |

Tickets start at 1, so 0 never matches. `Sources` takes one for every pick.

### Endless sources (`TLoadMoreCallback`)

- A queue is endless when `setQueue` got a `TLoadMoreCallback`. At its end `pickNext()`
  returns -1 even with repeat on, so it never wraps. `next()` waits for more instead.
- The load-more check (`maybeLoadMore`) calls the `TLoadMoreCallback` when all of these hold: the
  queue has one, no request is in flight for it, and `playlist().size() - currentIndex() <=
  kLoadMoreWhenLeft` (2). That means the current track is the last or the second to last.
- The check runs when a track becomes current (`playIndex` in both branches, before the link is
  resolved, and the gapless advance) and when `next()` reaches the end. It does not run from
  `setQueue` without `autoplay`, `appendTracks`, `removeTracks` or a mode change.
- `done(tracks)` is ignored if the `Player` is gone or the queue has been replaced. Otherwise the
  request is no longer in flight and `appendTracks(tracks)` runs. If `next()` was waiting and a
  track now follows the cursor, `playIndex(cursor + 1)` runs, the first new track; if nothing came,
  the player keeps waiting (`isWaitingForMore()`), and a later `next()`, or tracks inserted by the
  jam, go on from there.
- `requestMore()` runs the check from outside, for a source that learns of new material between
  track changes (the jam's new seeds).
- `done` may be called synchronously from inside the `TLoadMoreCallback`.

### Next, shuffle, repeat

- `pickNext()` delegates to `TrackNavigator::next`, passing Repeat only for finite queues.
  Previous uses the navigator too, unless it must restart the current track after 3 s.
- Shuffle does not apply to an endless queue (spec WAVE-10, WAVE-11): a wave always uses sequential
  traversal. The shuffle button and preferred algorithm retain the user's choices for finite queues.
- `shuffleAlgorithm()` defaults to WithoutRepeats. The application restores and saves it. Switching
  algorithms while shuffle is active rebuilds navigation and refreshes preload; while inactive it
  changes only the preference. Audio, position and pause state are preserved in either case.
- A new queue, a source change, edits (append, insert, remove), or switching shuffle resets the
  navigator from the current track. Queue edits start a new shuffled pass, which can include
  tracks heard before the edit. A direct `playIndex` selects the entry in the existing order.
- `next()` and preload read the same next entry. Looking ahead never consumes it. Explicit Next
  and gapless advance call `select` when a track becomes current.
- Repeat with a single track replays it, including through gapless preload. An endless queue with
  one track does not replay it: it waits for more (spec WAVE-12).

### Downloads

- The GET goes through `Library::api()->network()`, the `QNetworkAccessManager` that `ApiClient`
  uses. The signed link needs no auth header. Redirects use `NoLessSafeRedirectPolicy`, and
  `setTransferTimeout(kDownloadTimeoutMs)` aborts the transfer after 30 s without data.
- Every `readyRead` passes the bytes to `AudioEngine::appendData(stream, …)`, with the stream id
  captured at the start. At `finished`, success appends the rest and calls `finishData(stream)`;
  an error calls `failData(stream)`. The engine ignores data for streams it has dropped.
- `Player` sets no byte limit. Every byte goes to the engine, which keeps the stream in memory.
- At most one track download is in flight. A preload's link is requested only after the current
  track's download has finished without error. Aborted downloads are disconnected before
  `abort()`, so they never reach the finish handler.
- An HTTP error page (status 400 or more) is not passed to the engine. A download fails by
  `Yandex::ClassifyReply`: a broken connection is a network failure even after its status line.

### Failures during playback

The rules are [spec/player/errors.md](../../spec/player/errors.md); `tests/failures_test.cpp`
checks them by ID. `failure_policy.h` holds the decision as pure functions, the counterpart of
the Android app's `ErrorPolicy`:

```cpp
enum class FailureKind { Network, Auth, Track };
enum class FailureAction { WaitForNetwork, Next, Stop, StopAfterLimit };
inline constexpr int kMaxTrackFailuresInRow = 3;
FailureKind KindOf(const Yandex::RequestError& error);     // Network; HTTP 401/403 Auth; else Track
FailureAction DecideOnFailure(FailureKind kind, int failuresInRow, bool hasNext, bool endless);
```

- **Where a failure comes from:** the link (`resolveTrackUrl`'s `RequestError`), the download
  (`ClassifyReply` of the stream's reply) and `AudioEngine::streamUndecodable` (a Track failure,
  or a Network one if the download had already broken off).
- **Track** (ERR-04, ERR-05, ERR-07): `"The track does not play: <text>"` and `next()`, which in a wave at
  its end waits for more. The third in a row stops instead, with `"Stopped: 3 tracks in a row did
  not play"`; with no next track in a finite queue it stops at once. The count goes back to 0
  when the engine reaches Playing, and in `setQueue` (ERR-06).
- **Auth** (ERR-08): `"Access error: <text>"` and a stop. It is not counted.
- **Network** (ERR-01 to ERR-03): the track is parked. `"No network — waiting for it…"` once; the
  download and the preload are dropped, and a new stream is begun at the position the track had
  reached and paused, so every view shows it paused there. The track stays open: no `Skipped`
  now, and no new `Started` or play report when it continues. A retry follows after 2 s, then
  4 s, 8 s … up to 60 s (`setNetworkRetryDelays`); `setNetworkOnline(true)` retries at once,
  `setNetworkOnline(false)` holds the retries until then. A retry resolves the link again and
  streams into the parked stream, which seeks to the kept position before any audio; it plays on
  if the track was playing and the user has not paused since (Play and Pause during the wait only
  set that). `stop`, `playIndex` and `setQueue` end the wait.
- `src/app` feeds `setNetworkOnline` from `QNetworkInformation` when a backend can tell
  reachability; without one the timer alone ends a wait.

### Gapless preload

- **Started** by `maybePreload()` when all of these hold: there is no preload; the player is not
  shut down; the current download has finished without error; a track is open; the engine is not
  `Stopped` (Paused counts); and `pickNext() >= 0`. It is called at the end of `trackStarted`,
  when the current download finishes, and from `refreshPreload()`.
- **Link reply** (ignored if the `Player` is gone or `preloadGeneration` has moved on): the preload
  asks the engine for a queued stream. Its download then feeds that stream and `preloadedIndex()`
  becomes its index. On a link error, or when `queueStream()` returns 0 because the engine has
  stopped on its own meanwhile, the preload is dropped without a message. That track then starts the
  ordinary way when its turn comes.
- **Taken over** in one of two ways:
  1. The engine crosses the boundary (`trackAdvanced`), with no gap and no second link request.
  2. `playIndex` of a track with the preload's id while its stream is still the engine's queued
     stream. `next()` always takes this path when the preload is queued.

  In both cases a download that is still running continues as the current download, and a failed
  one has already cancelled the preload.
- **Cancelled** by `cancelPreload()`, in `stop()`, in every `playIndex` that does not take it over,
  and in `refreshPreload()`. Cancelling disconnects and aborts its reply, and calls
  `AudioEngine::clearQueued()` if its stream is still the queued one. The engine may already have
  committed to chaining it (the last seconds of the current track; see
  [src/audio](../audio/README.md)). In that case playback halts at the boundary and `trackFinished`
  follows, so `next()` picks again: the result is a gap, never the wrong track.
- **Refreshed** by `refreshPreload()` after `appendTracks`, `removeTracks`, and any actual change of
  shuffle or repeat. The preload is kept, and its index updated, when its track id is still what
  follows at `pickNext()`, in either mode. Otherwise the preload is cancelled.
  Whenever no preload is kept, `maybePreload()` tries a new one.
- A queued stream that the engine cannot decode is forgotten by the engine, and
  `queuedStream()` returns 0 for it. The next `playIndex` of that track then fetches it again the
  ordinary way.

### Track events and play reports

- Every track that opens gets `Started(track, 0)`, followed by exactly one `Finished` or
  `Skipped` with its played seconds. The closing event goes to the `TEventCallback` that received
  `Started`. It is always sent before the `Started` of the next track, because `playIndex` and
  both engine handlers close the open track first.
- `Started` is sent:
  - after the link resolves and the download starts, before any audio of it is heard, and even
    without an output device;
  - at once, when the preload is taken over by `playIndex` or `next()`;
  - at the gapless boundary.
- `Finished` is sent when the engine reports the track's end (`trackFinished` or `trackAdvanced`)
  and its download did not fail.
- `Skipped` is sent in every other case: `stop`; `playIndex`, `next` or `previous`; `play` while
  playing; `setQueue` or `clearQueue`; removing the track; `shutDown`; or an end after a failed
  download, where the track only drained to where the data stopped.
- No event is sent for a track whose link failed, because it never opened. None is sent when the
  `Player` is destroyed with a track open either; call `shutDown()` first, as
  `App::Application::quit` does. A link reply that arrives after the `Player` is gone is dropped
  (every resolve callback holds a `QPointer` to it).
- The played seconds are the seconds actually heard. On every tick and at close, the position
  step since the previous reading is added if the engine is `Playing` and `0 < step < 1.0`.
  Normal steps are about 0.1 s. Seeks and other jumps of 1 s or more, backward moves, and paused
  or buffering time do not count. `seekTo` also moves the baseline to the target.
- Each `Started` also sends a play report through `ApiClient::reportPlayStarted`, with a new UUID
  as the play id, whether or not the queue has a `TEventCallback` — unless the source's
  `QueueRules::playReports` is false (the jam, HOST-15).
- With `QueueRules::previousRestartsOnly` (the jam, HOST-07), `previous()` always restarts the
  current track by `seekTo(0)`, or `playIndex(cursor)` when the engine refuses the seek.

### Signals

| Signal | Emitted |
|---|---|
| `statusMessage(text)` | `"Audio error: <message>"` (from `AudioEngine::errorOccurred`), the failure statuses (`"The track does not play: …"`, `"Stopped: 3 tracks in a row did not play"`, `"Access error: …"`, `"No network — waiting for it…"`, `"Cannot get link: …"` without an output device, `"Download failed: …"`), and `"Loading more tracks…"` (loading more tracks). `Sources` also emits it on the `Player` for source loads, likes and dislikes, which makes it the app's one status line |
| `playlistChanged` | by `setQueue`; by `appendTracks` when something was added; by `removeTracks` always, even when no index was valid |
| `queueReplaced` | by `setQueue` and `clearQueue` only, before `playlistChanged` |
| `currentTrackChanged` | by `setQueue`, even for an empty queue; by `playIndex` at once; again from `trackStarted` once the bitrate is known; twice on a gapless advance; by `removeTracks` when the current track was removed |
| `positionTick` | after each timer tick's `poll()`, every 100 ms while the engine is Buffering, Playing or Paused |
| `modesChanged` | by `setShuffle`, `setShuffleAlgorithm` and `setRepeat` on a change |
| `seeked(target)` | after a successful `seekTo` or `seekFraction`, with the clamped target |

**Traps:**

- `pause()` toggles, and `play()` restarts the track while it is playing. Code that serves media
  keys has to check the state first, as `src/integrations` does.
- `setQueue` ignores tickets. An asynchronous source load must take `newSourceRequest()` before
  the request and drop the reply unless `isLatestSourceRequest(ticket)`. A direct `setQueue` does
  not invalidate loads in flight; only a new ticket does. That is why `clearQueue` and `shutDown`
  call `newSourceRequest()` and throw the ticket away. Do not remove those calls.
- A `TLoadMoreCallback` must call `done` exactly once, on the `Player`'s thread, with an empty list
  on failure. Until it does, that queue never asks again. After `done({})`, a waiting `next()` stays
  stopped with "Loading more tracks…" on the status line; the next `next()` asks again. If `done`
  runs synchronously inside the `TLoadMoreCallback`, playback has already moved on when `next()`
  emits that message.
- `TLoadMoreCallback` and `TEventCallback` run synchronously inside `Player` methods and the timer's
  handler. They must not call `playIndex`, `stop` or `setQueue`, and they must not throw (see
  [Errors](#errors)).
- The link reply in `playIndex` captures a raw `this`, unlike the preload's, which uses a
  `QPointer`. Do not destroy a `Player` while its `ApiClient` lives on and a link request may still
  be pending. In the app the `Player` is destroyed before the `ApiClient`, and no event loop runs
  in between.
- The poll timer calls `accumulatePlayedSeconds()` for its side effect; dropping the call loses
  the played time. A stall of the GUI thread longer than 1 s between two ticks also loses that
  stretch.
- `durationSeconds()` is `static_cast<double>(durationMs) / 1000.0`, using the metadata duration,
  not the decoded length. `seekTo` clamps to it.
- Removing a track before the current one changes `currentIndex()` without
  `currentTrackChanged`. The same is true when `appendTracks` moves the cursor from -1 to 0. UI
  that depends on the index must also listen to `playlistChanged`.
- A track that cannot play is skipped, waited on or stopped on, by the
  [failure policy](#failures-during-playback); without an output device (stream 0) a link failure
  only stops, as nothing can be waited on or skipped to.
- `shutDown()` blocks playback only. `setQueue`, `appendTracks` and `removeTracks` still change
  the list, and `next()` at the end of an endless queue still calls the `TLoadMoreCallback`.
- `Started` means "the download began", not "audio is audible". Without an output device it is
  still sent, together with the play report.
- `stop()` and `setQueue` do not reset `currentBitrate()`.
- The timer starts on the engine's next `stateChanged`. An engine that is already playing when
  the `Player` is constructed is not polled until its state changes.

## `track_navigator.h`: `TrackNavigator`

```cpp
enum class ShuffleAlgorithm { Random, WithoutRepeats };
class TrackNavigator {
    virtual void reset(int size, int current) = 0;
    virtual void select(int index) = 0;
    virtual int next(bool repeat) const = 0;
    virtual int previous(bool repeat) const = 0;
};
std::unique_ptr<TrackNavigator> MakeTrackNavigator(std::optional<ShuffleAlgorithm> algorithm);
```

No algorithm means sequential traversal. The three implementations are private to the `.cpp`;
Player knows only the interface. Navigation is independent of audio, networking, settings and UI.
`reset` takes a queue size and its valid current index (or -1 for an empty queue). `select` takes
an existing queue index. `next` and `previous` return queue indices, or -1 when no entry follows.
Repeated lookahead returns the same candidate and does not advance. Empty queues return -1.

- `SequentialNavigator`: follows source order. Repeat wraps at boundaries; Previous without
  Repeat on the first entry returns that entry. O(1) space and operations.
- `RandomNavigator`: chooses uniformly from all indices except the current one on `select` or
  `reset`. The candidate is cached for lookahead/preload. Repeats across transitions are allowed;
  with at least two entries playback continues even with Repeat off, matching the old random
  behaviour. Previous follows source order. O(1) space and operations.
- `ShuffledNavigator`: keeps a permutation, with the current entry first and the rest shuffled
  via `std::shuffle` and `QRandomGenerator::global()`. Each entry appears once per forward pass;
  duplicate track ids remain separate entries. Repeat wraps through the same permutation and
  Previous follows it. Storage is one `int` per entry; reset and select are O(n), lookahead O(1).
  The order is session-only. The displayed playlist stays in source order.

Tests cover the interface without an audio engine, plus Player's real preload and gapless transitions.

## `sources.h`: `Sources`

```cpp
class Sources : public QObject {
public:
    static inline const QString kMyWaveSeed = "user:onyourwave";
    Sources(Player* player, Yandex::Library* library, QObject* parent = nullptr);

    void playLikes(bool autoplay);                               // "Liked"
    void playPlaylist(const Yandex::PlaylistReference& playlist);
    void playRecommendations(const Yandex::PlaylistReference& playlist);   // "<title>: similar"
    void playArtist(const QString& artistId, const QString& name);        // top tracks
    void playAlbum(const QString& albumId, const QString& title);
    void playWave(const QStringList& seeds, const QString& title);        // waves and stations
    void playMyWave();                                           // {kMyWaveSeed}, "My Vibe"
    void search(const QString& text);                            // "Search: <text>"

    void setLiked(const QString& trackId, bool liked);
    void dislikeAndSkip(const QString& trackId);                 // skip only if the ID is current

    const QStringList& lastWaveSeeds() const;                    // what the wheel of waves matches
};
```

The scenarios are [spec/player/sources.md](../../spec/player/sources.md),
[wave.md](../../spec/player/wave.md) and [tracking.md](../../spec/player/tracking.md);
`tests/sources_test.cpp` checks them by ID.

- **One ticket per pick.** Every `play…` and `search` takes `Player::newSourceRequest()` before
  its request, and its reply is applied only if `isLatestSourceRequest(ticket)` (SRC-01, SRC-02).
  A search whose best result is an artist or album asks for that one's tracks with the same
  ticket (SRC-03). Replies are dropped silently; the HTTP request is not aborted. The `Player` is
  held through `QPointer`.
- **Listening along** (`spec/jam/listen.md`). `JamPlayback::link` is `Player::currentLink()`: the
  file of the current track once its link resolved, including a preloaded track that took over;
  `JamPlayback::nextLink` is `Player::nextLink()`, the preloaded next file. The host session sends
  them to the jam's guests when the host allows it.
- **Statuses**, all through `Player::statusMessage` (English sources, shown translated; see
  [translations](../../translations/README.md)): `<title>: loading…` (vibes, likes),
  `Search: <text>…`, `Error: …`, `Vibe error: …`, `Search error: …`, `<title>: empty`,
  `Nothing found`, `Liked: N tracks`, `Added to Liked`,
  `Removed from Liked`, `Disliked`.
- **An empty source keeps the queue** (SRC-07, SRC-08, WAVE-03): when a reply has no available
  track (none at all, or all `available: false`), the status is `<title>: empty` (search:
  `Nothing found`) and nothing else happens: no `setQueue`, and for a wave no `RadioStarted`.
- **Search** (SRC-09 to SRC-12): a best artist queues its top 100 under its name, a best album its
  tracks under its title, anything else the found tracks as `Search: <text>`.
- **A wave** (`playWave`): `Library::startWave(seeds)`. On success the wave state (session,
  station = first seed, track id → batch id) is shared by two callbacks for the life of that
  queue: `more` asks `moreWave` with the ids of the last 5 queued tracks (WAVE-05), and `events`
  maps `TrackEvent` Started, Finished, Skipped to `WaveEvent` TrackStarted, TrackFinished, Skip
  with the batch the track came in (TRK-04 to TRK-06). `RadioStarted` is sent before
  `setQueue`, so before the first track's `trackStarted` (TRK-03). The seeds become
  `lastWaveSeeds()`.
- **Likes**: `setLiked` and `dislikeAndSkip` call the `Library` and report in the status line;
  `dislikeAndSkip` calls `Player::next()` at once only when `trackId` matches the current track,
  which closes it with `Skip` (TRK-07). Disliking another track leaves playback alone, including
  when playback advances while a menu for the previous track is open.

**Traps:**
- The wave's callbacks hold the `Library` and a `QPointer<Player>`, not the `Sources`: the
  `Player` closes the open track, and sends its feedback, while the application shuts down,
  after the `Sources` may be gone.
- `playLikes(false)` (after login) fills the queue without starting playback.

## `jam_mode.h`: `JamMode`

```cpp
struct JamEntry { QString itemId; Yandex::Track track; QString addedBy; };
struct JamSlot { enum class Kind { Item, Wave, Other }; Kind kind; QString itemId; QString addedBy; };
struct JamPlayback { enum class Kind { Item, Wave, Idle }; Kind kind; QString itemId;
                     std::optional<Yandex::Track> track; qint64 positionMs; bool paused; };
inline constexpr int kJamPlayingReportMs = 10'000;
inline constexpr int kJamWaveHistory = 5;

class JamMode : public QObject {
public:
    JamMode(Player* player, Yandex::Library* library, QObject* parent = nullptr);
    bool isActive() const;
    const QList<JamSlot>& queueSlots() const;         // one per track of Player::playlist()
    JamSlot currentSlot() const;
    void start(const QString& title, bool waveFeedback);
    void setQueue(const QList<JamEntry>& entries, const QStringList& seeds, int seedsVersion);  // each state
    void skip(const QString& itemId);                 // command{skip}
    void end();
    void reportPlayback();                            // now, as after `resumed` (HOST-26)
    void setPlayingReportInterval(int ms);            // for tests
Q_SIGNALS:
    void itemStarted(const QString& itemId);          // -> started{itemId}
    void playback(const Core::JamPlayback& playback); // -> playing
};
```

The scenarios are [spec/jam/host.md](../../spec/jam/host.md) with Desktop = Kickoman/QiYaa#13;
`tests/jam_mode_test.cpp` checks them by ID. The network, the protocol and the server's messages are
the host session's (Kickoman/QiYaa#14); `JamMode` gets the queue as `JamEntry`s and reports through
its two signals.

- **The queue** is the current track, then the **jam part** (the mirror of the server's queue), then
  the **jam wave**. `queueSlots()` says which each track of `Player::playlist()` is: `Item` (with
  its item id and who added it), `Wave`, or `Other` (the track that played when the jam started).
  Every change of the queue during the jam goes through `JamMode`, which changes its slots first
  and the `Player` second; a size that differs afterwards (someone else edited the queue) is
  logged and the slots are cut or padded.
- **Start** (HOST-14): every track but the current one is removed, and `Player::changeSource`
  makes the queue endless with `QueueRules{playReports = false, previousRestartsOnly = true}`
  (HOST-15, HOST-07). The track that plays goes on and is reported as a wave track. An endless
  queue already turns shuffle off (WAVE-10) and repeat does not wrap or replay it (WAVE-12), so the
  user's modes need no change.
- **The jam part** (HOST-01 to HOST-04): `setQueue` keeps the longest common prefix of the jam part
  and the server's queue, removes the rest of the jam part and inserts what differs, so a next
  track that did not change keeps its preload. The current item is left out even while the state
  still lists it. Into an empty queue, the first item plays at once; a new item after the cursor
  plays at once when the player waits at the end or nothing is in progress (HOST-08).
- **The jam wave** (HOST-10 to HOST-13) is the `Player`'s load-more: when at most 2 tracks are left,
  `JamMode` asks `Library::startWave` with the last state's seeds (no seeds: `track:<current id>`;
  nothing played: no wave, `playing{idle}`), or `moreWave` of its session with the last 5 wave
  tracks. The session remembers the `seedsVersion` it started with. When the jam part becomes empty
  again under new seeds (the current track is not a wave track, no item follows), the old
  session's deferred tracks are removed and the next load starts a new session.
- **Not learning** (HOST-15, HOST-16): no play reports; rotor feedback (radioStarted, trackStarted,
  trackFinished, skip) only for the jam wave's tracks, to the session each came from, and only when
  `start` got `waveFeedback`.
- **Reports**: `itemStarted(itemId)` when a jam item's `Started` fires; `playback` after every
  track event, on the engine's state changes, seeks, and every 10 s while playing (HOST-05,
  HOST-06). `Idle` when nothing plays (empty queue, or stopped waiting at the end).
- **Skip** (HOST-18, HOST-19): `skip(itemId)` is `Player::next()` only when that item is current.
- **End** (HOST-32, HOST-33): the wave tracks after the cursor are removed, the jam items stay as
  ordinary tracks, the current track plays on, and `changeSource` with the default rules brings the
  play reports and a finite queue back.
- `Sources::setJamMode(jam)`: while the jam is on, every pick (likes, playlists, artists, albums,
  waves, stations, search) leaves the queue alone and says `A jam is on: add tracks to the jam`
  (HOST-21). Likes and dislikes still work (HOST-17).

**Traps:**
- The `Player`'s load-more and event callbacks hold a `QPointer<JamMode>` and a shared wave state:
  a wave track that closes after `end()`, or while the app shuts down, still gets its feedback.
- HOST-09 (a broken last jam track waits for the jam wave) is the `Player`'s own ERR-07 for an
  endless queue: `DecideOnFailure` says Next (the policy table in `tests/failures_test.cpp`), and
  `next()` at the end waits for more. `jam_mode_test` does not repeat it.

## `cover_cache.h`: `CoverCache`

```cpp
class CoverCache : public QObject {
public:
    // cacheDirectory empty = QStandardPaths::writableLocation(CacheLocation) + "/covers"
    explicit CoverCache(
        QNetworkAccessManager* networkAccessManager,
        const QString& cacheDirectory = {},
        QObject* parent = nullptr
    );

    QImage get(const QUrl& url);               // null while it downloads; ready(url) follows on success
    QString localFile(const QUrl& url) const;  // empty until the file is on disk

Q_SIGNALS:
    void ready(const QUrl& url);
};
```

- The app has one instance, shared by the "Now playing" window and the system media integration.
  The integration needs a local file for the MPRIS `artUrl`, which is why the covers are also
  kept on disk.
- The constructor creates the directory with `QDir::mkpath` and does not check the result. The
  directory comes from `src/app`, which passes a folder inside its temporary directory when it
  runs with read-only settings, and an empty string otherwise.
- `get(url)` returns a null image for an empty URL. Otherwise it looks in memory, then on disk,
  then goes to the network:
  - A memory hit makes the URL the most recently used and returns the image.
  - If `localFile(url)` exists and `QImage` decodes it, the image goes into memory and is
    returned.
  - Otherwise `get` returns a null image and starts a download, unless one for this URL is
    already pending or `networkAccessManager` is null.
- The download is a GET through `networkAccessManager` with `NoLessSafeRedirectPolicy` and
  `setTransferTimeout(kTimeoutMs)`, which aborts it after 20 s without data. When it finishes, the
  URL stops being pending. On an error, or bytes that `QImage::loadFromData` cannot decode, nothing
  else happens. Otherwise the bytes are written to the file exactly as received, the image goes into
  memory, and `ready(url)` is emitted.
- `ready(url)` fires once for each successful download, after the file has been written and the
  image is in memory. A handler's `get(url)` is therefore a memory hit, and `localFile(url)` is set
  if the write worked. It never fires for memory or disk hits, and never on failure.
- The memory holds at most `kMemoryItems` = 30 decoded images, counted by number, not bytes. The
  31st insert evicts the least recently used, where both `get` hits and inserts count as use. This
  class never evicts or expires files on disk.
- Everything runs on the owner's thread. `get` reads and decodes a disk file synchronously, on the
  caller's thread. The reply handler is guarded by a `QPointer`: if the cache is destroyed during a
  download, the result is dropped and the reply is still deleted. The cache does not own
  `networkAccessManager`.

**Traps:**

- The key is the exact encoded URL, so the same cover at another size is another entry and another
  file. The callers (`ui/now_playing_window.cpp`, `integrations/media_controls.cpp`) request
  `Track::coverUrl(400)` and compare `ready(url)` with the same URL.
- Failures are not remembered. Once an attempt has finished, every `get` of a failing URL starts a
  new request.
- `localFile()` only checks that the file exists, so it can return a corrupt or half-written file.
  `get` treats such a file as a miss and overwrites it after a successful download.
- A failed file write is silent (no directory, `open` fails, short write). The cover still reaches
  memory and `ready` fires, but `localFile()` stays empty, and so the MPRIS `artUrl` is missing.
- There is no byte limit on the downloaded body, which `readAll()` reads whole, and no size limit
  on the directory.
- With `networkAccessManager == nullptr` the cache serves only files already on disk.
  `tests/mpris_test.cpp` uses this.

## Cover files on disk

`CoverCache` reads and writes one file per cover URL:

```
<directory>/<sha1>.jpg
    <sha1>    40 lowercase hex digits: SHA-1 of QUrl::toEncoded() of the cover URL
    content   the HTTP response body, byte for byte, in whatever format the server sent
```

- A file is written only after `QImage::loadFromData` has accepted the bytes.
- The extension is always `.jpg`, whatever the content is. `QImage` detects the real format;
  anyone else who reads `localFile()` gets the `.jpg` name either way.
- On reading, a file that `QImage` cannot decode is treated as missing. There is no size check.

`Player` reads no files. The audio bytes go straight from the network to the engine.

## Errors

The module has no `Error` class and nothing in it throws or catches, and it calls none of the
loaders that throw (the project's error policy is in
[docs/architecture.md](../../docs/architecture.md#errors)). Every failure is data:

| Where | Failure arrives as |
|---|---|
| link, current track | the failure policy, by `Yandex::RequestError` kind; without an output device `statusMessage("Cannot get link: <error>")` and a stop |
| link, preload | nothing; the preload is dropped |
| download, current track | the failure policy; after some audio, `statusMessage("Download failed: …")` and `Skipped` at the end, or for the network a wait |
| download, preload | the preload is cancelled, with no message |
| audio engine | `statusMessage("Audio error: <message>")`; an undecodable current stream also goes to the failure policy (`AudioEngine::streamUndecodable`) |
| endless source | the `TLoadMoreCallback` reports a failed load as `done({})`; a waiting `next()` stays stopped |
| seek | `seekTo` and `seekFraction` return `false` |
| index out of range | `playIndex` does nothing; `removeTracks` skips it |
| cover | a null `QImage` from `get`, an empty `QString` from `localFile`, and no `ready` |

`TLoadMoreCallback`, `TEventCallback` and the `done` callback run inside Qt slots. Per `CLAUDE.md`,
no exception may cross the event loop, so they must not throw.

## Not here

- Decoding, the ring buffer, stream buffers, when a queued stream is chained, the commit window
  before the boundary, when `trackAdvanced` and `trackFinished` fire, seeking within the
  downloaded part, and the `errorOccurred` texts: [src/audio](../audio/README.md).
- `download-info` and the signed link, `Track` and `Track::coverUrl`, the `/play-audio` report,
  and wave sessions and feedback: [src/yandex](../yandex/README.md).
- The Yandex menu that calls `Sources`, the status line, the playlist and "Now playing" windows:
  [src/ui](../ui/README.md).
- Media keys, MPRIS and SMTC state, and the cover `artUrl` built from `localFile()`:
  [src/integrations](../integrations/README.md).
- Creating the `Player` and the cache and choosing the cache directory, loading the likes without
  `autoplay` after login when the queue is empty, and calling `shutDown()` on quit before waiting
  for the last POSTs: [src/app](../app/README.md).
- Tests:
  - `tests/library_test.cpp`: track events, the preload and the gapless advance, re-preloading
    after a queue change, the endless source, and a slow load against a newer choice.
  - `tests/audio_test.cpp`: `playerPollsTheEngine`.
  - `tests/windows_test.cpp`: `nothingPlaysAfterShutDown`.
  - `tests/mpris_test.cpp`: the cache without a network manager.
