# `src/integrations` — the desktop's media controls: MPRIS on Linux, SMTC on Windows, MediaPlayer on macOS

This folder lets the operating system drive the player and show what is playing: media keys,
the GNOME/KDE media panels, `playerctl` and the lock screen on Linux/BSD through MPRIS 2 on the
D-Bus session bus; media keys, the media panel of the volume flyout and the lock screen on
Windows 10 and later through the System Media Transport Controls (SMTC). `MediaControls` is the
platform-neutral part: it turns `Core::Player` into the commands and the state an OS expects, and
takes volume, raise and quit as `Hooks`, so nothing here knows the windows. Exactly one backend
(`Mpris`, `Smtc` or `MacMediaControls`) is compiled, or none. The folder does not play, queue or download anything
([src/core](../core/README.md), [src/audio](../audio/README.md)), does not decide whether media
integration is on, build the hooks or pick the backend ([src/app](../app/README.md)), and uses Apple MediaPlayer on macOS.

| File | Contains |
|---|---|
| `media_controls.h/.cpp` | `MediaControls` and its `Hooks` — commands, status, cover URLs and change signals over `Core::Player` |
| `mpris.h/.cpp` | `Mpris` (bus name, registration, `PropertiesChanged`) and its two D-Bus adaptors, `MprisRootAdaptor` (`org.mpris.MediaPlayer2`) and `MprisPlayerAdaptor` (`org.mpris.MediaPlayer2.Player`); built only with `QIYAA_HAVE_MPRIS` |
| `mac_media_controls.h/.mm` | `MacMediaControls` — remote commands and Now Playing through Apple MediaPlayer; built only on macOS |
| `smtc.h/.cpp` | `Smtc` — SMTC through C++/WinRT; built only with `QIYAA_HAVE_SMTC` |

## Dependencies

`qiyaa_integrations` links PUBLIC `Qt6::Gui` and PRIVATE `qiyaa_audio`, `qiyaa_core` and
`qiyaa_yandex` (the sources include `audio/audio_engine.h` and `yandex/api_client.h` besides the
`core` headers). In the module order it sits beside `ui`, above `core`
and below `app`. The top-level `CMakeLists.txt` adds at most one backend:

| Backend | Built when | Adds |
|---|---|---|
| MPRIS | `UNIX AND NOT APPLE`, and `find_package(Qt6 6.4 COMPONENTS DBus)` succeeds | `mpris.*`, PUBLIC `Qt6::DBus`, PUBLIC define `QIYAA_HAVE_MPRIS` |
| SMTC | `WIN32 AND MSVC`, option `QIYAA_WITH_SMTC` (ON by default), and `winrt/Windows.Media.h` compiles with `/std:c++20 /EHsc` | `smtc.*`, PRIVATE `windowsapp` and `Qt6::Widgets`, PUBLIC define `QIYAA_HAVE_SMTC` |
| MediaPlayer | `APPLE` | `mac_media_controls.h/.mm`, Objective-C++20 with ARC, PRIVATE Foundation and MediaPlayer frameworks, PUBLIC define `QIYAA_HAVE_MAC_MEDIA_CONTROLS` |

Everywhere else (MinGW, Linux without Qt D-Bus) only `media_controls.*` is built; the app
still constructs a `MediaControls` there, with no backend attached. On Linux/BSD CMake prints
`MPRIS: enabled` or `MPRIS: disabled (...)`, and on MSVC `SMTC: enabled` or `SMTC: disabled (...)`.

Deliberately not linked: `qiyaa_ui`, `qiyaa_vis`, `qiyaa_skins` and `qiyaa_app`. The windows are
reached only through `Hooks`, plus one bare `QWidget*` for SMTC; `Qt6::Widgets` is linked for
SMTC alone, for `QWidget::winId()`. The headers include no project header (they forward-declare
`Core::Player` and `Core::CoverCache`), which is what keeps these three PRIVATE.

```bash
grep -n '#include "\(app\|ui\|vis\|skins\)/' src/integrations/*.h src/integrations/*.cpp   # must print nothing
grep -n '#include "' src/integrations/*.h                                                  # must print nothing
grep -ln 'QDBus\|winrt\|QWidget' src/integrations/media_controls.*                         # must print nothing
```

## `MediaControls` (`media_controls.h`)

```cpp
class MediaControls : public QObject {
public:
    struct Hooks {
        std::function<int()> volume;         // 0..100
        std::function<void(int)> setVolume;  // 0..100
        std::function<void()> raise;         // bring the windows to front
        std::function<void()> quit;
    };

    MediaControls(Core::Player* player, Core::CoverCache* covers, Hooks hooks, QObject* parent = nullptr);

    Core::Player* player() const;
    const Hooks& hooks() const;

    void play();                  // no-op while playing
    void pause();                 // no-op unless playing; never resumes
    void playPause();
    void stop();
    void next();
    void previous();
    bool seekTo(double seconds);  // false if the track can't seek (yet)
    bool canSeek() const;

    enum class Status { Playing, Paused, Stopped };
    Status status() const;
    QUrl artUrl() const;          // cached cover as file://, else the https URL, else empty
    QUrl remoteArtUrl() const;    // always the https URL (or empty)

Q_SIGNALS:
    void trackChanged();
    void statusChanged();
    void artChanged();
    void modesChanged();          // shuffle or repeat
    void seeked(double seconds);
    void volumeChanged();         // never emitted by this class
};
```

- **Commands have the semantics media keys expect**, which are not those of `Core::Player`'s
  Winamp buttons: `Player::play` restarts the track while playing, and `Player::pause` toggles.
  `play()` returns while `status()` is `Playing`, otherwise calls `Player::play`, which resumes
  from pause or, when stopped, starts the current track (the first one if there is none).
  `pause()` calls `Player::pause` only while `Playing`. `playPause()` picks one of the two from
  `status()`. `stop()`, `next()` and `previous()` forward unchanged.
- **Status** folds the engine's four states into three: `Buffering` counts as `Playing`.
- **Seeking.** `canSeek()` is `Player::durationSeconds() > 0 && status() != Stopped`.
  `seekTo(seconds)` returns false when the duration is 0, otherwise the result of
  `Player::seekTo`, which clamps the target to the track and fails when the engine can't seek
  there yet.
- **Covers** are the 400×400 px variant (`kCoverPixels`, via `Yandex::Track::coverUrl(400)`). On
  every `Player::currentTrackChanged` it calls `CoverCache::get` for that URL *before* emitting
  `trackChanged`: a cover already on disk is therefore a `file://` URL in `artUrl()` when the
  listeners run, and a missing one starts downloading. When `CoverCache::ready` reports the current
  track's URL, `artChanged` fires and `artUrl()` has become `file://` (the cache writes the file
  before it signals, unless that write fails). With `covers == nullptr`, `artUrl()` is always the
  https URL and `artChanged` never fires. A failed download leaves the https URL and emits nothing.
- **Signals.** `trackChanged` follows `Player::currentTrackChanged`; `statusChanged` follows
  `AudioEngine::stateChanged` of `player->engine()`; `modesChanged` follows
  `Player::modesChanged`; `seeked` follows `Player::seeked` (the clamped target in seconds, after
  every successful seek, whoever asked for it). `volumeChanged` is emitted by whoever owns the
  volume: the app connects `Ui::MainWindow::volumeChanged` to it. Everything runs on the thread of
  `Core::Player`, the GUI thread.
- **Ownership.** `player` and `covers` are raw pointers that must outlive the object; `player`
  must not be null, and its engine is connected in the constructor. `hooks` is copied in; any of
  the four may be empty, and every caller in this folder checks before calling.

**Traps:**
- Do not "simplify" `play()` and `pause()` into straight calls to `Player::play()` and
  `Player::pause()`: a play key while playing would restart the track, and a pause key while
  paused would resume it.
- `canSeek()` being true does not mean `seekTo` succeeds: the engine seeks only within the part
  of the track already downloaded, so an early seek returns false and emits no `seeked`.
- A `setVolume` hook must lead to `volumeChanged`, or MPRIS clients keep the old value.
  `Ui::MainWindow::setVolume` emits only when the clamped value actually changed.
- `hooks()` is the copy taken at construction; changing the caller's `Hooks` later has no effect.

## `Mpris` (`mpris.h`)

Implements the [MPRIS 2 specification](https://specifications.freedesktop.org/mpris-spec/latest/).

```cpp
class Mpris : public QObject {
public:
    explicit Mpris(
        MediaControls* controls,
        const QString& serviceSuffix = QStringLiteral("qiyaa"),      // after "org.mpris.MediaPlayer2."
        const QDBusConnection& bus = QDBusConnection::sessionBus(),
        QObject* parent = nullptr
    );
    ~Mpris() override;

    bool isRegistered() const;
    QString serviceName() const;  // empty without a bus
    MediaControls* controls() const;
    QVariantMap metadata() const;
    QString playbackStatus() const;
    void emitPropertiesChanged(const QString& interface, const QVariantMap& changed);
};

class MprisRootAdaptor : public QDBusAbstractAdaptor { /* org.mpris.MediaPlayer2 */ };
class MprisPlayerAdaptor : public QDBusAbstractAdaptor { /* org.mpris.MediaPlayer2.Player */ };
```

**Registration**, in the constructor, in this order:

1. Both adaptors are created as children of `this`, with or without a bus.
2. If `bus` is not connected: `qInfo` "MPRIS: no D-Bus session bus", and it stays unregistered.
3. The name is `org.mpris.MediaPlayer2.<serviceSuffix>`. If that name already has an owner on
   the bus (another running copy), `.instance<pid>` is appended
   (`QCoreApplication::applicationPid()`), as the spec suggests for several instances.
4. `this` is registered at `/org/mpris/MediaPlayer2` with Qt's default `ExportAdaptors`; if the
   path is already taken on this connection it warns and gives up.
5. The name is registered; on failure it warns with the bus error, unregisters the path and gives
   up.
6. Only now is `isRegistered()` true and are the change signals connected. The destructor
   unregisters name and path, and does nothing when registration failed.

**D-Bus names.** The adaptors' `Q_PROPERTY` names and their public slots and signals are exported
by Qt D-Bus under exactly their C++ names, so they *are* the member names of the MPRIS
interfaces. That is why they are PascalCase (`Raise`, `PlayPause`, `Seeked`, `CanGoNext`) against
the project's camelCase rule for methods. Their READ/WRITE accessors (`playbackStatus()`,
`setLoopStatus()`) are ordinary methods and follow the rule.

`org.mpris.MediaPlayer2`:

| Member | Behaviour |
|---|---|
| `CanQuit`, `CanRaise` | always `true` |
| `HasTrackList` | `false`; there is no `TrackList` interface |
| `Identity` | `"QiYaa"` |
| `DesktopEntry` | `"qiyaa"`, the basename of `packaging/linux/qiyaa.desktop` |
| `SupportedUriSchemes`, `SupportedMimeTypes` | empty |
| `Raise()`, `Quit()` | `Hooks::raise` / `Hooks::quit`; nothing when the hook is empty |

`org.mpris.MediaPlayer2.Player`:

| Member | Behaviour |
|---|---|
| `PlaybackStatus` | `"Playing"`, `"Paused"` or `"Stopped"` from `MediaControls::status()` |
| `LoopStatus` (rw) | reads `"Playlist"` when `Player::repeat()`, else `"None"`. Writing `"None"` turns repeat off; any other string, `"Track"` included, turns it on |
| `Shuffle` (rw) | `Player::shuffle()` / `Player::setShuffle()` |
| `Rate`, `MinimumRate`, `MaximumRate` | `1.0`; writing `Rate` is ignored |
| `Volume` (rw) | `Hooks::volume() / 100.0` (`1.0` without the hook). A write is clamped to 0..1, rounded to a percent and passed to `Hooks::setVolume` |
| `Position` | `static_cast<qlonglong>(positionSeconds() * 1e6)`, engine position in µs |
| `Metadata` | see below |
| `CanGoNext`, `CanGoPrevious`, `CanPlay`, `CanPause`, `CanControl` | always `true` |
| `CanSeek` | `MediaControls::canSeek()` |
| `Next`, `Previous`, `Pause`, `PlayPause`, `Stop`, `Play` | the `MediaControls` command of the same name |
| `Seek(offsetUs)` | ignored when `CanSeek` is false. Target = max(0, `Position` + offset); at or past the track's duration it calls `Next()` (spec), otherwise `MediaControls::seekTo` |
| `SetPosition(trackId, positionUs)` | ignored when `CanSeek` is false, when there is no current track, when `trackId` is not the current track's id (a stale request), or when the position is outside 0..`mpris:length`; otherwise `MediaControls::seekTo` |
| `OpenUri` | ignored |
| signal `Seeked(positionUs)` | on every `MediaControls::seeked`, i.e. after every successful seek, from D-Bus or from the app |

`Metadata` keys:

| Key | D-Bus type | Value |
|---|---|---|
| `mpris:trackid` | `o` | `/io/github/kickoman/qiyaa/track/<id>`, every character of `Track::id` outside ASCII letters and digits replaced by `_` (`none` for an empty id). Without a current track: `/org/mpris/MediaPlayer2/TrackList/NoTrack`, and it is the only key |
| `mpris:length` | `x` | `durationMs * 1000` (µs) |
| `xesam:title` | `s` | `Track::title` |
| `xesam:artist` | `as` | `Track::artists` |
| `xesam:url` | `s` | `Track::webUrl()` (the track's page on music.yandex.ru) |
| `xesam:album` | `s` | `Track::albumTitle`, only when not empty |
| `mpris:artUrl` | `s` | `MediaControls::artUrl()`, only when not empty |

**Change notifications.** Qt D-Bus serves `Properties.Get`, `GetAll` and `Set` from the
`Q_PROPERTY` declarations but never emits `org.freedesktop.DBus.Properties.PropertiesChanged`.
`emitPropertiesChanged` sends it by hand from `/org/mpris/MediaPlayer2` with arguments
`(interface, changed, [])`: the invalidated list is always empty. Clients (the GNOME and KDE
panels, `playerctl --follow`) cache the properties and rely on these signals:

| `MediaControls` signal | Properties announced on `org.mpris.MediaPlayer2.Player` |
|---|---|
| `trackChanged` | `Metadata`, `CanSeek` |
| `artChanged` | `Metadata` |
| `statusChanged` | `PlaybackStatus`, `CanSeek` |
| `modesChanged` | `Shuffle`, `LoopStatus` |
| `volumeChanged` | `Volume` |
| `seeked` | none; the `Seeked` signal instead |

`Position` is never announced: the spec exempts it from `PropertiesChanged`, and `Seeked` covers
the jumps. A D-Bus write to `Shuffle`, `LoopStatus` or `Volume` is announced by the same path
(`Player::modesChanged`, the app's `volumeChanged`), not by the setter, and `Player` emits
`modesChanged` only when the value actually changes.

**Threads.** Qt D-Bus delivers incoming calls through the event loop of the thread that owns
`Mpris` (the GUI thread), so every slot, accessor and hook runs there; there are no locks.
`controls` must outlive the `Mpris`.

**Tests.** `tests/mpris_test.cpp` runs against a private session bus (CTest wraps it in
`dbus-run-session` when that is installed) and calls the methods from outside with `gdbus`, as a
desktop panel would. It covers the root properties and `Raise`, `Metadata`, `Next`/`Previous`
with the `Metadata` change announced, writes to `Volume`/`Shuffle`/`LoopStatus`, app-side changes
reaching clients, `Seek` being ignored while not seekable, and the `.instance` name of a second
copy. It skips itself without a session bus, and skips the `gdbus` cases without `gdbus`.

**Traps:**
- Renaming an adaptor property, slot or signal renames the D-Bus member and breaks every client.
  The naming rule of the code style does not apply there.
- Adding a property that can change is not enough: also announce it in one of the `connect`
  calls of the constructor, or clients show a stale value. The names in `emitPropertiesChanged`
  are plain strings (`"CanSeek"`) that nothing checks against the `Q_PROPERTY` list.
- The `.instance<pid>` fallback is a check (`isServiceRegistered`) followed by a registration, not
  one atomic step. Two copies started at the same moment can both go for the bare name; the loser
  warns "cannot register" and runs without MPRIS. A third `Mpris` with the same suffix in one
  process gets the same `.instance<pid>` name and fails the same way.
- The object path is per connection: a second `Mpris` on the same `QDBusConnection` fails at
  `registerObject` ("is taken on this connection"). The test simulates a second copy with its own
  connection (`QDBusConnection::connectToBus(..., "second")`).
- When registration fails after the name was chosen, `serviceName()` still returns that name.
  Check `isRegistered()`.
- Track paths are not unique: ids that differ only in characters outside `[A-Za-z0-9]` map to the
  same path, and `SetPosition` cannot tell them apart. Keep the `/io/github/kickoman/qiyaa` prefix;
  the spec reserves `/org/mpris` for `NoTrack`.
- `CanGoNext`, `CanGoPrevious`, `CanPlay` and `CanPause` stay true with an empty queue (the
  commands are then no-ops in `Core::Player`); `CanQuit` and `CanRaise` stay true with empty
  hooks.
- A client that sets `LoopStatus` to `"Track"` reads back `"Playlist"`: Winamp repeats the
  playlist, and that is the only repeat mode the player has.
- `Seek` past the end skips to the next track, but only while `CanSeek` is true; with a stopped
  player it does nothing (`seekIsIgnoredWhenNotSeekable`).

## `Smtc` (`smtc.h`)

```cpp
class Smtc : public QObject {
public:
    // window: the main window, not null
    Smtc(MediaControls* controls, QWidget* window, QObject* parent = nullptr);
    ~Smtc() override;

    bool isActive() const;  // false when SMTC could not be set up
};
```

- **Bound to a window.** A desktop (Win32) application gets its SMTC per window, through
  `ISystemMediaTransportControlsInterop::GetForWindow` on `window->winId()` (the HWND; `winId()`
  creates the native window if it doesn't exist yet). Windows 10 and later.
- **Buttons.** Play, Pause, Stop, Next and Previous are enabled and map to the `MediaControls`
  command of the same name. Other buttons (FastForward, Rewind, Record, ChannelUp/Down) are
  neither enabled nor handled.
- **Threads.** `ButtonPressed` handlers run on a WinRT thread, not the Qt thread. The handler
  only reads `arguments.Button()` and posts a queued call to `QCoreApplication::instance()`, so
  `handleButton` and everything after it run on the GUI thread. The posted lambda holds a
  `QPointer<Smtc>`: a press still queued when the `Smtc` is destroyed is dropped.
- **What it publishes.** `PlaybackStatus` from `MediaControls::status()` on every
  `statusChanged`. On every `trackChanged` the display updater is cleared (`ClearAll()`, which also
  drops the previous track's thumbnail), then `Type` = Music, `Title`, `Artist` (the artists
  joined with ", "), `AlbumTitle` and a `Thumbnail` made with
  `RandomAccessStreamReference::CreateFromUri(remoteArtUrl())`, then `Update()`. Without a
  current track the cleared updater is published empty. Both also run once at the end of a
  successful constructor.
- **What it does not publish:** timeline (position, duration), shuffle, repeat, rate. It uses
  no `Hooks` and connects neither `artChanged` nor `seeked`.
- **COM.** There is no `winrt::init_apartment()`: Qt has already initialised COM on the GUI
  thread, and that apartment is fine for these calls.
- **Destruction.** When active, the destructor removes the `ButtonPressed` handler and sets
  `IsEnabled(false)`, swallowing any exception.

**Traps:**
- Do not add `winrt::init_apartment()`: COM is already initialised on this thread (by Qt, as a
  single-threaded apartment), and asking for a different apartment type fails with
  `RPC_E_CHANGED_MODE`, which C++/WinRT throws.
- The thumbnail must stay `remoteArtUrl()`, not `artUrl()`: SMTC downloads it itself, and
  `CreateFromUri` refuses `file://` URIs. `smtc.cpp` keeps a one-line note about it.
- SMTC has no play/pause toggle button: a play/pause key arrives as `Play` or `Pause`, which
  Windows picks from the `PlaybackStatus` we published. A stale status makes the key do nothing.
- The binding is to the HWND at construction. The app creates `Smtc` after the main window and
  destroys the windows first (member order in `app/application.h`), so the destructor's calls
  may hit controls whose window is gone; its `catch (...)` covers that. If the main window's
  native handle were ever recreated, SMTC would stay bound to the old one.
- `window` is dereferenced unchecked.
- No test covers `Smtc`; the only check is that an MSVC build with C++/WinRT compiles it.

## `MacMediaControls` (`mac_media_controls.h`)

Registers play, pause, toggle, stop, next and previous handlers with
`MPRemoteCommandCenter`. Commands are queued onto the backend's Qt thread and use
`MediaControls` semantics. A `QPointer` guards pending commands during teardown.
The native handler tokens are retained with ARC and removed on destruction.

`MPNowPlayingInfoCenter` publishes title, artists, duration, elapsed time, playback rate
and playback state on track/status changes, seeks and position ticks. No current track
clears the metadata; destruction also clears it and marks playback stopped. macOS routes
media keys to the active Now Playing application; this backend does not capture global
keyboard events and needs no Accessibility permission. Artwork and system seeking are
not implemented. The app's `mediaIntegration` option controls its lifetime, as for MPRIS/SMTC.

## Errors

The module defines no exception type and lets none out of its API; there is no
`Integrations::Error`. Failures are data or log lines:

- `MediaControls::seekTo` returns false when it can't seek.
- `Mpris`: `isRegistered()` is false after any registration failure, logged as `qInfo`
  "MPRIS: no D-Bus session bus", `qWarning` "MPRIS: /org/mpris/MediaPlayer2 is taken on this
  connection" or `qWarning` "MPRIS: cannot register <name>: <bus error>". Nothing in it throws.
  Invalid or stale D-Bus requests (see the `Seek`, `SetPosition` and `OpenUri` rows) are ignored
  and answered with an empty reply, and an unknown `LoopStatus` string turns repeat on.
- `Smtc`: C++/WinRT reports failures by throwing `winrt::hresult_error`. Every WinRT call made on
  the Qt side sits in a `try`:
  - constructor: catches `hresult_error`, logs `qWarning` "SMTC unavailable: <message>", resets
    the controls to null (`isActive()` is false) and connects no signals;
  - `updateStatus`: catches `hresult_error` and ignores it;
  - `updateMetadata`: catches `hresult_error` and logs `qWarning` "SMTC metadata: <message>";
  - destructor: `catch (...)`.

  `handleButton` makes no WinRT call. The `ButtonPressed` handler body is not in a `try`, but it
  runs inside C++/WinRT's delegate, which turns an exception into an HRESULT returned to Windows,
  so nothing reaches Qt. Not caught anywhere: `std::bad_alloc`, which C++/WinRT throws instead of
  `hresult_error` for `E_OUTOFMEMORY`.

## Not here

- Whether media integration is on (`App::Application::Options::mediaIntegration`, off for
  `--screenshot`), the hook implementations, the `MainWindow::volumeChanged` connection and the
  choice of backend:
  [src/app](../app/README.md). So are Winamp's in-app transport keys (Z X C V B and the arrows).
- What play, pause, next, previous, shuffle, repeat and seeking actually do, and the cover cache
  (`Core::CoverCache`, its directory and download rules): [src/core](../core/README.md).
- Engine states and seeking within the downloaded part: [src/audio](../audio/README.md).
- Track fields, `Track::coverUrl()` and `Track::webUrl()`: [src/yandex](../yandex/README.md).
- The volume slider: [src/ui](../ui/README.md).
