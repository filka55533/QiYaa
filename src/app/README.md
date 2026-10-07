# `src/app` — composition root and the `qiyaa` entry point

`src/app` assembles the running player. `App::Application` creates and owns every long-lived
object (the audio engine, the Yandex API client and library, the player, the jam mode and, in a
jam build, the jam's host session, the cover cache, the media controls and the windows: main,
equalizer, playlist, now-playing and, in a Milkdrop build, Milkdrop), connects their signals, places and re-stacks the windows,
builds the context menus and keyboard shortcuts, keeps `settings.ini`, finds and saves the Yandex
token, swaps skins and shuts down in order. `paths.*` says where QiYaa's own files and the old
Yaamp's files live. `offline_sources.*` holds what `--play-file` and `--demo` play without
Yandex. `main.cpp` belongs to the `qiyaa` executable, not to the library: it picks the
Qt platform, parses the command line and runs the application. The folder only wires things
together. Drawing, dragging, snapping, shading and resizing a window are in
[src/ui](../ui/README.md); the queue, playback and wave reports in [src/core](../core/README.md);
HTTP, OAuth and the token file format in [src/yandex](../yandex/README.md); `.wsz` parsing in
[src/skins](../skins/README.md); MPRIS, SMTC and macOS MediaPlayer in [src/integrations](../integrations/README.md).

| File | Contains |
|---|---|
| `application.h/.cpp` | `App::Application` and its `Options`: ownership, wiring, layout, menus, shortcuts, settings, login, skins, quit, `snapshot()` |
| `paths.h/.cpp` | `ConfigDirectory`, `YaampDataDirectories`, `TokenFile`, `YaampTokenFiles`: where settings and tokens live |
| `translations.h/.cpp` | `Language`, `Languages`, `LanguageCode`, `LanguageFromCode`, `LanguageName`, `Translations`: the interface language |
| `offline_sources.h/.cpp` | `StreamLocalFile` (a local file fed to the engine like a download) and `DemoTracks` (nine sample entries) |
| `main.cpp` | `main()` of the `qiyaa` executable: platform choice, command-line options, `--screenshot`, exit codes |

## Dependencies

`qiyaa_app` is built from `application.*`, `offline_sources.*` and `paths.*`:

- PUBLIC `qiyaa_audio`, `qiyaa_core`, `qiyaa_skins`, `qiyaa_yandex`: `application.h` holds
  `Audio::AudioEngine`, `Core::Player`, `Skins::Skin`, `Yandex::ApiClient` and `Yandex::Library`
  by value.
- PRIVATE `qiyaa_integrations`, `qiyaa_ui`, `qiyaa_vis`: the header only forward-declares the
  windows, `Core::CoverCache` and `Integrations::MediaControls`.
- PRIVATE `qiyaa_jam` in a jam build (`QIYAA_HAVE_JAM`), with the define
  `QIYAA_JAM_URL="<the CMake option>"`, the default jam server. The header forward-declares
  `Jam::HostSession`; the code that calls it is under `#ifdef QIYAA_HAVE_JAM`.
- Resources `:/icons` (`qiyaa-16.png` … `qiyaa-256.png`, seven sizes), used by `main.cpp` for the
  window icon, and `:/i18n/qiyaa_be.qm`, `qiyaa_en.qm`, `qiyaa_ru.qm`, built from
  [translations](../../translations/README.md) by `qt_add_translations` (Qt LinguistTools is
  required).
- `QT_NO_KEYWORDS` and `-Wall -Wextra -Wshadow` (`/W4 /utf-8` on MSVC), like every target.

The executable `qiyaa` (output name `QiYaa`) is `main.cpp` linked PRIVATE with `qiyaa_app` and
`qiyaa_ui` (`main.cpp` calls `Ui::MainWindow::setStatusText`), with the define
`QIYAA_VERSION="<project version>"`, a `.rc` file from `packaging/windows/qiyaa.rc.in` on Windows
and `resources/icons/qiyaa.icns` on macOS.

`app` deliberately does not link the vendored `qiyaa_miniaudio`, `qiyaa_miniz` or projectM (it
reaches them only through `audio`, `skins` and `vis`), nor Qt D-Bus or WinRT (only through
`integrations`). Nothing links `qiyaa_app` except the executable and, among the tests,
`windows_test` and `jam_e2e_test`.

`app` is the top of the one-way order `audio`, `yandex`, `skins` → `vis`, `core` → `ui`,
`integrations` → `app` → `qiyaa`:

```bash
grep -rln --include='*.h' --include='*.cpp' '#include "app/' src/ | grep -v '^src/app/'   # must print nothing
grep -rln --include='*.h' --include='*.cpp' 'QSettings' src/ | grep -v '^src/app/'        # must print nothing: only app keeps settings
grep -rnw --include='*.h' --include='*.cpp' 'throw' src/app/                               # must print nothing: app only catches
```

## `application.h/.cpp` — `App::Application`

```cpp
class Application : public QObject {
public:
    struct Options {
        QString skinOverride;           // --skin; empty = the `skin` setting
        QString settingsFile;           // empty = <ConfigDirectory>/settings.ini
        bool offline = false;           // start() does not look for a token
        bool audio = true;              // false: AudioEngine::init() is never called
        bool readOnlySettings = false;  // settings, covers, Milkdrop presets in a temp dir
        bool mediaIntegration = true;   // MPRIS, SMTC or macOS MediaPlayer
        QNetworkAccessManager* network = nullptr;  // tests: one that reaches a mock server
        std::optional<Language> language;          // this run only; unset: the `language` setting
    };

    explicit Application(const Options& options, QObject* parent = nullptr);

    void start();

    Ui::MainWindow* mainWindow() const;
    Ui::EqualizerWindow* equalizerWindow() const;
    Ui::PlaylistWindow* playlistWindow() const;
    Ui::NowPlayingWindow* nowPlayingWindow() const;
    Ui::MilkdropWindow* milkdropWindow() const;  // null without QIYAA_HAVE_MILKDROP
    Ui::JamWindow* jamWindow() const;            // null without QIYAA_HAVE_JAM
    Core::CoverCache* covers() const;
    Core::Player* player();
    Audio::AudioEngine* engine();
    Yandex::ApiClient* api();
    Yandex::Library* library();
    Core::JamMode* jam();
    Jam::HostSession* jamHost() const;                 // null without QIYAA_HAVE_JAM

    bool loadSkin(const QString& path);                // false: message in the marquee
    enum class ScaleScope { Saved, ThisRun };
    void setScale(double scale, ScaleScope scope = ScaleScope::Saved);  // ThisRun for --scale
    void setAlwaysOnTop(bool on);
    void setEqualizerVisible(bool on);
    void setPlaylistVisible(bool on);
    void setNowPlayingVisible(bool on);
    void setMilkdropVisible(bool on);                  // no-op without Milkdrop
    void setJamWindowVisible(bool on);                 // no-op without the jam

    Language language() const;
    void setLanguage(Language language);               // applies now and saves `language`

    void login();                                      // modal dialog
    void logout();
    void applyToken(const QString& token, bool save);

    void saveState();
    void quit();
    QImage snapshot() const;                           // visible windows, as placed on screen
};
```

**Threads and ownership.** Everything runs on the GUI thread: `Application` starts no thread,
and every method must be called from the GUI thread (the audio engine's decoder thread and device
callback stay inside [src/audio](../audio/README.md)). `Application` owns everything it wires:
`QSettings`, the base and the current `Skins::Skin`, `ApiClient`, `Library`, `AudioEngine`,
`Player`, `Sources` and `JamMode` as members; its own `QNetworkAccessManager` unless
`Options::network` lends one; the `Jam::HostSession`, `CoverCache`, `MediaControls`, the `Mpris`, `MacMediaControls` or
`Smtc` object (held as `std::unique_ptr<QObject>`) and the windows (five, and the jam window in a
jam build) through `std::unique_ptr`.
The accessors return non-owning pointers, valid until the `Application` is destroyed.

Member order is load-bearing. Members are constructed in declaration order, and the initialiser
list relies on it: the settings file after the temporary directory, `ApiClient` after the network
manager, `Player` after the library and the engine, `JamMode` after the player. The host session
is declared after `JamMode` and destroyed before it. The windows are declared last, so they are
destroyed first, while the player, engine and skins they point at still exist. The `Mpris`/`Smtc`/`MacMediaControls`
object dies before the `MediaControls` it wraps, and `QSettings` before the temporary directory
that may hold its file.

**Construction**, in this order:

1. Picks the settings file (INI format): `settings.ini` in a new `QTemporaryDir` when
   `readOnlySettings`, else `settingsFile`, else `<ConfigDirectory>/settings.ini`.
   Then installs the interface language (`Options::language`, else the `language` setting, else
   Belarusian), before any window takes its texts.
2. Loads the built-in base skin `:/skins/base-2.91.wsz` (`Skins::Skin::BuiltinBase()`). It is also
   the fallback for bitmaps other skins lack.
3. With `audio`, calls `AudioEngine::init()`. On failure it logs `Audio: <message>` and the app
   runs without sound; on success it logs `Audio backend: <name>`.
4. Loads `skinOverride`, or else the `skin` setting. A `Skins::Error` logs
   `Skin: <what>; using the built-in one`.
5. Creates the main, equalizer, playlist and now-playing windows and the cover cache
   (`<temp dir>/covers` when `readOnlySettings`, else the default of
   [src/core](../core/README.md)). With `QIYAA_HAVE_MILKDROP` it also creates the Milkdrop window,
   with built-in presets from `:/milkdrop` and user presets from `<ConfigDirectory>/milkdrop`
   (`<temp dir>/milkdrop` when `readOnlySettings`). Every window gets the shortcuts listed below;
   all but the main window are "secondary" (no taskbar button on Windows).
6. Gives `Sources` the `JamMode` (a pick during a jam is refused, HOST-21). In a jam build
   (`setUpJam()`) creates the `Jam::HostSession`: the server and the wave feedback from the
   `jam/*` settings at each use, the queue title `Jam`, the app's version for `hello`, and the
   session file `jam-session.json` next to the settings file (so `readOnlySettings` and a test's
   `settingsFile` keep it in their own folder). Its signals go to the marquee: a refusal by its
   reason (below), the end of the jam (`The jam is over`, `The jam was ended`, `The jam closed while you
   were away`, `The jam is over: the server no longer knows it`), and, while the jam is on, a lost
   connection (`No connection to the jam server · trying again by itself`) and its return (`The jam is connected`). Then
   the jam window (`Ui::JamWindow`, steps `jamWindow/steps`, the server's host from `jam/server`;
   its `statusText` to the marquee, its "Server settings…" to `showJamServerDialog()`), and the
   jam's part in the other windows (HOST-34, HOST-21): the playlist's rows and the marquee get a
   note, `+ <name>` for a jam item (the participant whose `publicId` is the slot's `addedBy`) and
   `jam vibe` for a jam wave track; during a jam the playlist's remove sends `remove` for the
   selected jam items (`Select jam tracks to remove them` when none is one, `No connection to the
   jam server` when a send fails) and its clear only says `A jam is on: add tracks to
   the jam`; neither touches the `Player`. It is done here, before the loops below over `windows()`,
   which take the jam window in.
7. Restores the settings listed in [Settings](#settings) and connects the windows:
   - the equalizer's shade mode shows and sets the main window's volume and balance (`setMixer`,
     `volumeRequested`, `balanceRequested`);
   - the equalizer's `statusText` goes to the main window's marquee;
   - a Milkdrop preset picked by hand (`presetChanged` with `PresetOrigin::User`) shows `Milkdrop:
     <name>` in the marquee;
   - Milkdrop's `transportKey` (transport keys pressed while the visualization has the focus)
     goes to the same handler as the shortcuts;
   - Milkdrop is told whether the engine state is `Playing`.
8. Applies the saved shade state of the main, equalizer and playlist windows, then `scale` to every
   window, then `alwaysOnTop`.
9. With `mediaIntegration`, creates `Integrations::MediaControls` with four hooks: volume, set
   volume, raise (un-minimise the main window, raise every visible window, activate the main one)
   and quit (`quit()`). On top of it, `Mpris` in a build with `QIYAA_HAVE_MPRIS`, or `Smtc` in a
   build with `QIYAA_HAVE_SMTC`, or `MacMediaControls` with
   `QIYAA_HAVE_MAC_MEDIA_CONTROLS` on macOS.
10. With audio on and not offline, `watchNetwork()` (see the traps).
11. Connects `QApplication::aboutToQuit` to `saveState()` followed by `Player::stop()`.

The refusals of the jam server, by reason: `server-full` "The jam server is full, try
later", `rate-limited` "Too many requests, wait a little", `update-required` "Update
QiYaa to work with this jam server", `not-allowed` "The jam server does not allow this",
`queue-limit` "The jam queue is full", `duplicate` "This track is already in the jam queue", `stale`
"This track has already left the queue", any other "The jam server refused". The texts are the Android app's.

**`start()`** shows the main window, then each other window whose `*/visible` setting is true,
places them all (`layoutWindows()`), sets the main window's EQ and PL buttons to match, and
activates the main window. Unless `offline`, it then asks about a stored jam and looks for a
token (see [Login, logout and the token](#login-logout-and-the-token)).

**"Continue the jam?"** (HOST-23). When the host session has a stored jam, `start()` opens a
`QMessageBox` "A jam was on" / "Continue it? The guests' link stays the same." with "Continue"
and "End it". It is not modal (`open()`): playback and the windows work while it waits, and a
test on the offscreen platform does not block. "Continue" calls `continueStored()` and shows the jam window; "End it"
calls `discardStored()` and queues the likes like a login does; closing it without an answer keeps
the jam stored for the next start. While it is open, or a jam is on, `applyToken` does not queue
the likes.

**Layout.** `layoutWindows()`, called only from `start()`, places each window at its saved
`*/pos`, or at the Winamp default: the equalizer right below the main window, the playlist right
below the equalizer, now-playing to the right of the main window, Milkdrop at main position +
(main width, main height), the jam window to the right of the main window like now-playing. The defaults use the sizes the windows have at that moment,
after shade and scale. `SkinnedWindow::placeAt` clamps every position to the screens that exist
now.

**`setScale(scale, scope)`**:

1. Collects the windows docked to the main window through a chain of touching windows
   (`Ui::ConnectedGroup`, breadth-first, so nearest first). Hidden windows are included, so they
   are in place when shown. It records each one's offset from the main window in skin pixels
   (offset / old scale, rounded).
2. Calls `setScale` on every window. The window clamps the factor to [1, 4], rounds it to 0.05,
   and may move itself back onto the screen, so the main window's position is read only after
   this step.
3. Puts each docked window back at main position + offset × new scale, snapped with
   `Ui::SnapToOthers(frame, placed, 4)` to the windows already placed. Window sizes are rounded one
   by one, and the 4 px snap closes the resulting 1 px gaps.
4. Moves the visible group as a whole into the screen it is on (`Ui::PickScreen`,
   `Ui::ClampInside`). If the group is wider or taller than that screen, it calls
   `ensureVisible()` on every window so each one can still be reached.
5. With `ScaleScope::Saved`, stores `scale` (the factor the main window ended up with) and calls
   `saveState()`. Without it (`--scale`), it sets `transientScale`, which stops `saveState()` from
   writing positions.

Windows not docked to the main window keep their top-left corner and only change size.

**Visibility.** `setEqualizerVisible`, `setPlaylistVisible`, `setNowPlayingVisible` and
`setMilkdropVisible` show or hide the window, call `ensureVisible()` when showing it, and store
`*/visible`. The first two also update the main window's EQ/PL button. A window's own close button
calls the same setter with `false`. Minimising the main window hides the other four; restoring it
shows those whose `*/visible` is true. `setAlwaysOnTop` sets `Qt::WindowStaysOnTopHint` on every
window, then shows again those that were visible, because changing window flags hides a window.

**`saveState()`** always writes `volume`, `balance`, `vis/mode`, `time/remaining` and
`equalizer/auto`. It writes the five `*/pos` keys only when all three hold: the platform lets the
app place windows (`SkinnedWindow::CanPositionWindows()`, false on native Wayland), the main window
is visible, and no `--scale` override is active. It runs when any window finishes a move
(`moveFinished`), on a shade change, on a `setScale` with `ScaleScope::Saved`, on a choice in the
Visualization submenu, in `quit()` and on `aboutToQuit`. The other settings are written the moment
they change (see the table).

**`quit()`** runs once; a second call returns at once. It calls `saveState()`, then
`Player::shutDown()` (stops playback, which reports the open track to the wave as skipped, and
ignores anything that would start playback again), hides every window, and then quits the Qt
application with a queued call, so that a `quit()` made inside a nested event loop (a menu) ends
the main loop. The queued quit happens right away if `ApiClient::pendingPosts()` is 0, otherwise
on `ApiClient::postsSettled` or after 1500 ms, whichever comes first. `quit()` is reached from the
main window's close button (and the window manager's close), the menu item "Quit QiYaa" and the
media controls' quit.

**`snapshot()`** paints `grab()` of each visible window into an `ARGB32_Premultiplied` image the
size of the bounding rectangle of all visible windows, each at its offset in that rectangle. Gaps
stay transparent. With no visible window the image is null.

`tests/windows_test.cpp` drives a real `Application` with `offline`, `audio = false`,
`readOnlySettings` and no media integration, on a 2560×1440 offscreen screen (on Windows on Qt's
default offscreen screen, where the scale tests skip themselves).
`tests/jam_e2e_test.cpp` runs one with `offline`, the Null output, a `settingsFile` that points
`jam/server` at a real jam server and `Options::network` at the test's MockHttpServer.

**Traps:**
- Do not reorder the members in `application.h` (see above). A new window goes after the
  existing ones, and needs `installShortcuts()`, an entry in `windows()` and its settings keys.
- `Options::offline` only skips the token lookup in `start()`. The menu item "Log in to Yandex
  Music…" still logs in. `readOnlySettings` does not redirect the token file: a login during
  such a run writes the real `<ConfigDirectory>/token`.
- `Options::settingsFile` is set only by `jam_e2e_test`, to point `jam/server` at its server.
- After `--scale`, window positions are not saved for the rest of the run, unless the user picks a
  size from the menu (a `setScale` with `ScaleScope::Saved` clears `transientScale`).
- `saveState()` skips positions while the main window is hidden, which is why `quit()` saves before
  hiding the windows. The `saveState()` on `aboutToQuit` that follows only writes the other keys.
- Only `quit()` waits for pending POSTs. Any other way out (the session ends,
  `QApplication::quit()` called from elsewhere) goes straight to `aboutToQuit` → `Player::stop()`:
  the report for the open track is started, but the process may exit before it leaves.
- The shade state is applied in the constructor, before `start()` places the windows, because the
  saved positions were taken with the shaded heights.
- The shortcut shown next to a menu item is only a label; the key that works is the `QAction`
  added by `installShortcuts()`. Change both places together.
- `applyToken` leaves a rejected token set on `ApiClient`; the library just stays logged out. The
  result of `Yandex::SaveToken` is ignored.
- Settings values are converted with `QVariant::toInt`, `toDouble`, `toBool`, `toPoint`, `toSize`.
  A key that is present but malformed does not fall back to the default (a number becomes 0). Only
  `vis/mode` is clamped here before `static_cast<Ui::MainWindow::VisMode>`; saving uses
  `static_cast<int>`. Volume, balance, scale, resize steps and Milkdrop seconds are clamped
  by the windows. Equalizer values are clamped to ±12 dB by the engine only, so the equalizer
  window draws an out-of-range value as stored.
- A `skin` setting that fails to load is not cleared: the warning repeats at every start until
  another skin is loaded.
- The playlist's ADD button and right-click open the full main menu. Track rows emit
  `PlaylistWindow::trackMenuRequested`; ADD and clicks outside the rows emit
  `PlaylistWindow::sourcesMenuRequested`.
- With audio on and not offline, `watchNetwork()` loads a `QNetworkInformation` backend that can
  tell reachability and passes it to `Player::setNetworkOnline`: Online is true, Disconnected
  false, anything else (Local, Site, Unknown) "cannot tell". Online also reconnects the jam at
  once (`HostSession::networkBack`, HOST-26). Without such a backend the `Player`'s
  retry timer alone ends a wait for the network ([src/core](../core/README.md#failures-during-playback)).

### Login, logout and the token

Unless `offline`, `start()` calls `Yandex::FindToken(TokenFile(), YaampTokenFiles())`, which tries
`$QIYAA_TOKEN`, then QiYaa's own token file, then the old Yaamp `token.json` files. The order, the
accepted token formats and the rule that an empty own file means "logged out" are in
[src/yandex](../yandex/README.md).

- No token: the marquee shows `Log in: right click → Log in`, and `login()` is queued, so it runs
  once the event loop starts.
- A token: logs `Using Yandex token from <origin>` and calls `applyToken(token, save)`. `save` is
  true only when the origin is neither under `ConfigDirectory()` nor the environment variable (an
  origin starting with `environment`), so a token imported from Yaamp is copied into QiYaa's own
  token file once the account connects.

`applyToken(token, save)` sets the token on `ApiClient`, shows `Connecting to Yandex Music…`
and calls `Library::connectAccount`. On error the marquee shows `Login failed: <error>`. On
success it calls `Yandex::SaveToken(TokenFile(), token)` if `save`, shows
`Hello, <displayName>!`, and, if the play queue is empty and no jam is on or offered, calls
`Core::Sources::playLikes(false)` (queues the liked tracks without starting playback).

`login()` runs `Ui::LoginDialog` modally (`exec()`, a nested event loop) and, on accept, calls
`applyToken(dialog.token(), true)`. `logout()` first ends a jam being created or on (the guests
search through this account, HOST-32), then clears the play queue, calls `Library::logout()`
(which also clears the API token) and `Yandex::ForgetToken(TokenFile())` (leaves an empty token
file), and shows `You have logged out`.

### Skins

`loadSkin(path)` loads `path` with the base skin as the fallback. On `Skins::Error` it shows
`Cannot load the skin: <what>` in the marquee, keeps the current skin and returns false. On
success it points every window at the new skin first, and only then replaces (and frees) the old
one; it stores `skin` = `path` and returns true. The Skins submenu passes `:/skins/<file>.wsz` for a
bundled skin, or the path picked in the file dialog.

### Menus

`showMainMenu(globalPosition, track)` answers the main window's options button and right-click,
and the playlist's ADD button and right-click. The track is the playlist row under the cursor
for `trackMenuRequested`, and the current track for the other entry points.
`showSourcesMenu(globalPosition)` answers the main window's eject button with the current track.
Both build a `QMenu` parented to the main window, open it with `popup()` and delete it on close.

The sources menu holds the Yandex items from `Ui::AddLibraryActions` (only "Log in to Yandex
Music…" while logged out; it calls `login()`). The texts here are the English sources; the user sees
them in the interface language. The main menu, top to bottom:

- the same Yandex items;
- in a jam build, Jam: "Start…" (no jam) or "Jam window" (a jam is on or being created), which
  show the jam window; "Join…", disabled until Kickoman/QiYaa#16; "End it", enabled
  while a jam is on, which asks "End the jam? The guests will see that it is over." in a modal
  `QMessageBox::question` and then calls `HostSession::end()`; "Server settings…"
  (`showJamServerDialog()`: `Ui::JamServerDialog`, modal, which writes `jam/server` and
  `jam/waveFeedback` and `jam/shareAudio` on Save and tells the jam window the server's host);
- Equalizer (Alt+G), Playlist (Alt+E), Now playing (no shortcut), Milkdrop (Ctrl+Shift+K, only
  in a Milkdrop build): checkable, checked when the window is visible;
- Visualization: Spectrum, Oscilloscope, Off (exclusive; a choice calls `saveState()`);
- Shuffle: "Random track" or "Without repeats" (exclusive); chooses the preferred algorithm
  and saves `shuffle/algorithm`. It does not toggle the shuffle button or restart audio.
- Skins: every `*.wsz` in `:/skins`, sorted by name, shown without the extension; then "Load a
  skin…" (file dialog starting in the home directory, filter `*.wsz *.zip`);
- Size: 100 %, 125 %, 150 %, 175 %, 200 %, 250 %, 300 % (exclusive, checked when the current
  scale is within 1e-6), then "Double size" (Ctrl+D);
- the language: "Мова (Language)" in Belarusian, "Язык (Language)" in Russian, "Language" in
  English, so that a wrong pick can be undone; "Беларуская", "Русский", "English" (exclusive, each
  in its own language) call `setLanguage()`;
- Always on top: checkable, checked from the main window's current flag;
- "Log out (<displayName>)", only while logged in: `logout()`;
- "Quit QiYaa": `quit()`.

### Shortcuts

`installShortcuts(widget)` adds one `QAction` per key to each window (the jam window's fields
take the keys while they have the focus). `QAction`'s
default context is the window, so the keys work while any of the player's windows is active.

| Key | Action |
|---|---|
| Z, X, C, V, B | previous, play, pause, stop, next |
| Left, Right | seek 5 s back or forward from `AudioEngine::positionSeconds()` |
| Alt+G | show or hide the equalizer |
| Alt+E | show or hide the playlist |
| Ctrl+D | scale 2.0, or 1.0 when it is already 2.0 (saved) |
| Ctrl+W | shade or unshade the main window |
| Ctrl+Shift+K | show or hide Milkdrop (nothing without Milkdrop) |

### Settings

`Application` is the only place in `src/` that reads or writes settings. "ctor" is the
constructor; "setter" is the matching `set*Visible`.

| Key | Type | Default | Read | Written |
|---|---|---|---|---|
| `skin` | QString: a file path or `:/skins/<file>.wsz` | empty = built-in `base-2.91.wsz` | ctor, unless `--skin` | `loadSkin()` on success |
| `shuffle/algorithm` | QString: `random` or `without-repeats` | `without-repeats` (anything else too) | ctor | on `Player::modesChanged` |
| `language` | QString: `be`, `ru` or `en` | `be` (anything else too) | ctor, unless `Options::language` | `setLanguage()` |
| `scale` | double | 1.0 | ctor | `setScale(…, ScaleScope::Saved)` |
| `alwaysOnTop` | bool | false | ctor | `setAlwaysOnTop()` |
| `volume` | int, 0..100 | 75 | ctor | `saveState()` |
| `balance` | int, −100..100 | 0 | ctor | `saveState()` |
| `vis/mode` | int: 0 spectrum, 1 oscilloscope, 2 off | 0 | ctor (clamped to 0..2) | `saveState()` |
| `time/remaining` | bool: show remaining time | false | ctor | `saveState()` |
| `mainWindow/pos` | QPoint | (100, 100) | `start()` | `saveState()` |
| `mainWindow/shaded` | bool | false | ctor | on `shadeChanged` |
| `equalizer/visible` | bool | true | `start()`, un-minimise | setter |
| `equalizer/pos` | QPoint | main position + (0, main height) | `start()` | `saveState()` |
| `equalizer/shaded` | bool | false | ctor | on `shadeChanged` |
| `equalizer/enabled` | bool | true | ctor | on `EqualizerWindow::settingsChanged` |
| `equalizer/preamp` | double, dB | 0.0 | ctor | on `settingsChanged` |
| `equalizer/bands` | QStringList: 10 numbers in dB, one decimal, in `Audio::kEqBandHz` order (60 Hz first) | empty = all 0.0 | ctor | on `settingsChanged` |
| `equalizer/auto` | bool | false | ctor | `saveState()` |
| `playlist/visible` | bool | true | `start()`, un-minimise | setter |
| `playlist/pos` | QPoint | equalizer default + (0, equalizer height) | `start()` | `saveState()` |
| `playlist/shaded` | bool | false | ctor | on `shadeChanged` |
| `playlist/steps` | QSize: resize steps of 25×29 skin px beyond 275×116 | (0, 4) | ctor | on `sizeStepsChanged` |
| `nowPlaying/visible` | bool | false | `start()`, un-minimise | setter |
| `nowPlaying/pos` | QPoint | main position + (main width, 0) | `start()` | `saveState()` |
| `nowPlaying/steps` | QSize: resize steps of 25×29 skin px | (0, 0) | ctor | on `sizeStepsChanged` |
| `milkdrop/visible` | bool | false | `start()`, un-minimise | setter |
| `milkdrop/pos` | QPoint | main position + (main width, main height) | `start()` | `saveState()` |
| `milkdrop/steps` | QSize: resize steps of 25×29 skin px | (0, 4) | ctor | on `sizeStepsChanged` |
| `milkdrop/shuffle` | bool | true | ctor | on `MilkdropWindow::settingsChanged` |
| `milkdrop/locked` | bool: keep the current preset | false | ctor | on `settingsChanged` |
| `milkdrop/seconds` | int, seconds per preset (the window clamps to 5..3600) | 30 | ctor | on `settingsChanged` |
| `milkdrop/preset` | QString: preset name | empty = the window's own pick | ctor | on `settingsChanged` |
| `milkdrop/black` | QStringList: names of blacklisted presets | empty | ctor | on `settingsChanged` |

| `jam/server` | QString: the jam server's address, `https://…` | `QIYAA_JAM_URL` | each time the host session connects; ctor and the dialog for the jam window | "Server settings…" |
| `jam/waveFeedback` | bool: the jam wave learns from skips (HOST-12, HOST-16) | true | when a jam starts | "Server settings…" |
| `jam/shareAudio` | bool: guests may listen along (spec/jam/listen.md) | false | at each `playing` | "Server settings…" |
| `jamWindow/visible` | bool | false | un-minimise only: the jam window is shown by the menu and by "Continue" | `setJamWindowVisible()` |
| `jamWindow/pos` | QPoint | main position + (main width, 0) | `start()` | `saveState()` |
| `jamWindow/steps` | QSize: resize steps of 25×29 skin px | (1, 8): 300×348 | ctor | on `sizeStepsChanged` |

The `milkdrop/*` keys are read and written only in a build with `QIYAA_HAVE_MILKDROP`, the `jam/*`
and `jamWindow/*` keys only in a jam build. The resize
steps are clamped to 0..40 per axis by the windows.

## File format: `settings.ini`

`QSettings::IniFormat`, written by Qt. Keys without a group go under `[General]`; `a/b` is key `b`
in group `[a]`. Points and sizes are written as `@Point(x y)` and `@Size(w h)`, string lists as
comma-separated values, bools as `true`/`false`:

```ini
[General]
skin=:/skins/Vizor1-01.wsz
scale=1.5
volume=75
balance=0

[equalizer]
bands=3.0, 1.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.5, 3.0
pos=@Point(100 274)
visible=true

[playlist]
steps=@Size(0 4)
```

The loader refuses nothing. Unknown keys are ignored (and kept), missing keys take the defaults
from the table, and malformed values are converted as described in the traps above. For
`equalizer/bands`, the first 10 entries are used, and bands without an entry stay at 0.0 dB. The
module reads no other file itself: the token files are read by [src/yandex](../yandex/README.md),
skins by [src/skins](../skins/README.md).

## `translations.h/.cpp`

```cpp
enum class Language { Belarusian, Russian, English };
inline constexpr Language kDefaultLanguage = Language::Belarusian;
const QList<Language>& Languages();                     // in the menu's order
QString LanguageCode(Language);                         // "be", "ru", "en"
std::optional<Language> LanguageFromCode(const QString&);
QString LanguageName(Language);                         // "Беларуская", "Русский", "English"
class Translations {
    bool apply(Language language);                      // false: the app's .qm is missing
    Language language() const;
};
```

`apply` removes the translators it installed before, sets `QLocale::setDefault`, loads
`:/i18n/qiyaa_<code>.qm` (for English only the plural forms) and Qt's `qtbase_<code>.qm` from
`QLibraryInfo::TranslationsPath` when Qt has one (Russian; not Belarusian), and installs them on
the application. Qt then sends `QEvent::LanguageChange` to every widget: the skinned windows draw
their texts again and set their titles in `retranslate()` (see [src/ui](../ui/README.md)); menus
and dialogs take the new texts when they open next. A text already shown in the marquee stays
until the next one. The destructor removes the translators.

**Traps:**
- Install before the windows exist: a text set in a constructor (a window title, a dialog's
  label) is taken in the language of that moment, unless the widget sets it again on
  `LanguageChange`.
- Qt has no Belarusian translation: its standard buttons and its own file dialog stay in English
  there. The app's dialogs set their buttons' texts themselves (`Ui::AskText`, `Ui::AskItem`,
  the jam's questions).

## `paths.h/.cpp`

```cpp
namespace App {
QString ConfigDirectory();            // created if missing
QStringList YaampDataDirectories();    // old Yaamp data folders, may not exist
QString TokenFile();            // <ConfigDirectory>/token
QStringList YaampTokenFiles();  // <dir>/token.json for each YaampDataDirectories() entry, same order
}
```

`ConfigDirectory()` is `QStandardPaths::GenericConfigLocation` + `/QiYaa`. `YaampDataDirectories()` is where
Electron's `app.getPath('userData')` (`<appData>/<productName>`) put the old Yaamp's data, with the
product name spelled `Yaamp` and then `yaamp`:

| OS | `ConfigDirectory()` | `YaampDataDirectories()` |
|---|---|---|
| Linux, BSD | `$XDG_CONFIG_HOME/QiYaa`, default `~/.config/QiYaa` | `$XDG_CONFIG_HOME/Yaamp`, `…/yaamp`; `~/.config/…` when the variable is empty or unset |
| Windows | `%LOCALAPPDATA%\QiYaa` | `%APPDATA%\Yaamp`, `%APPDATA%\yaamp`; none when `APPDATA` is unset |
| macOS | `~/Library/Preferences/QiYaa` | `~/Library/Application Support/Yaamp`, `…/yaamp` |

What lives in `ConfigDirectory()`: `settings.ini`, `token`, in a Milkdrop build `milkdrop/` (the
user's own presets), and while a jam is on `jam-session.json` (next to `settings.ini`, see
[src/jam](../jam/README.md#session_storeh)). The cover cache is not there: [src/core](../core/README.md) chooses its
place.

**Traps:**
- `ConfigDirectory()` calls `QDir().mkpath()` on every call, `TokenFile()` included, and ignores a
  failure.
- On Windows QiYaa's folder is under Local (`%LOCALAPPDATA%`, Qt's choice), while Yaamp's is under
  Roaming (`%APPDATA%`); on macOS it is `Preferences`, not `Application Support`.
- The "imported" test in `start()` is a string-prefix test against `ConfigDirectory()`. If `TokenFile()`
  ever moves out of `ConfigDirectory()`, QiYaa's own token counts as imported and is saved again at every
  start.
- On case-insensitive file systems (Windows, macOS by default) `Yaamp` and `yaamp` are the same
  folder, so the same `token.json` may be read twice; `FindToken` stops at the first usable one.

## `offline_sources.h/.cpp`

```cpp
namespace App {
void StreamLocalFile(Audio::AudioEngine* engine, const QString& path);
QList<Yandex::Track> DemoTracks();
}
```

`StreamLocalFile` imitates a download: it opens the file (on failure it logs `Cannot open <path>`
and the app keeps running with nothing playing), calls `AudioEngine::beginStream()`, then a 20 ms
timer, parented to the engine, appends 64 KiB per tick until the end of the file, calls
`finishData()` and deletes itself. It bypasses `Core::Player`: the playlist does not show the
file.

`DemoTracks()` returns nine fixed "Artist - Title" entries with ids `1`..`9` and durations from
164 to 395 s. They have no cover and no audio.

**Traps:**
- `StreamLocalFile` feeds the engine's *current* stream (the `appendData`/`finishData` overloads
  without a stream id). If something starts another stream meanwhile (a demo track played with
  `--demo`), the rest of the file goes into that stream.

## `main.cpp`

```cpp
constexpr int kSuccess = 0;
constexpr int kFailure = 1;
constexpr int kInternalError = 2;

int main(int argc, char* argv[]);  // Run() inside try/catch
```

`Run()`, in this order:

1. `ChoosePlatform()`, before `QApplication` exists, because it sets `QT_QPA_PLATFORM`.
2. Creates `QApplication` and sets the application name `QiYaa` (also the folder name Qt uses in
   per-application locations such as the cache), the desktop file name `qiyaa` (must match
   `packaging/linux/qiyaa.desktop`: taskbar icon, MPRIS), the window icon from the seven
   `:/icons/qiyaa-<size>.png`, the version `QIYAA_VERSION`, and
   `setQuitOnLastWindowClosed(false)`: closing the equalizer, the playlist or any other window
   never quits; only `Application::quit()` or the end of the session does.
3. Parses the options, fills `App::Application::Options`, constructs the `Application` and calls
   `start()`.
4. Applies `--scale`, `--demo` and `--text`, after `start()`, when the windows exist and are placed.
5. Takes the screenshot, or starts `--play-file`, or runs the event loop.

**Options.** The user reference is [docs/cli.md](../../docs/cli.md). What each one does here:

| Option | Effect |
|---|---|
| `--help`, `--version` | Qt's; `QCommandLineParser::process` prints and exits |
| `--screenshot <png>` | `offline`, `audio` off (unless `--play-file`), `readOnlySettings`, no media integration; see below |
| `--skin <wsz>` | `Options::skinOverride`; not stored in `skin` |
| `--play-file <path>` | `offline`, `audio` on; `StreamLocalFile(engine, path)` |
| `--offline` | `Options::offline` |
| `--text <text>` | `MainWindow::setStatusText(text)` |
| `--demo` | `Player::setQueue(DemoTracks(), "Demo", false)` |
| `--scale <factor>` | `Application::setScale(value.toDouble(), ScaleScope::ThisRun)`: this run only |

Qt's own arguments (`-platform`, `-style`, …) are taken out by `QApplication` before the parser
runs. The mapping to `Options` is `offline = --screenshot || --offline || --play-file`,
`audio = !--screenshot || --play-file`, `readOnlySettings = --screenshot`,
`mediaIntegration = !--screenshot`.

**Platform choice** (Linux and FreeBSD only; elsewhere `ChoosePlatform()` does nothing). Wayland
does not let a client place its own windows, and the Winamp layout (separate top-level windows
snapped together) needs that. So:

1. `QT_QPA_PLATFORM` already set: left alone.
2. `QIYAA_NATIVE_WAYLAND=1` (exactly `1`): left alone, so Qt picks native Wayland on a Wayland
   session.
3. `WAYLAND_DISPLAY` set, or `XDG_SESSION_TYPE=wayland`: sets `QT_QPA_PLATFORM=xcb;wayland`, that
   is XWayland, with native Wayland as the fallback when xcb cannot start (for example, Qt's xcb
   plugin is not installed).

On native Wayland `SkinnedWindow::CanPositionWindows()` is false and window positions are not
saved.

**Screenshot.** Without `--play-file`: one `QApplication::processEvents()`, then `snapshot()` saved
to `<png>` (format from the extension, by `QImage::save`); `Run` returns `kSuccess`, or `kFailure`
when saving fails. The event loop never runs. There is no audio device, no network, no media
integration, and the settings are fresh defaults in a temporary directory deleted at exit, so the
image depends only on the options (`--skin`, `--scale`, `--demo`, `--text`), the build and the
platform. With `--play-file`, the file plays and the snapshot is taken 1500 ms later from the event
loop, then `QApplication::exit(kSuccess or kFailure)`, so the visualizer is moving in the image.
CI runs `QiYaa --screenshot <png> --offline` on the offscreen platform as a start-up check of every
build and package.

**Exit codes:**

| Code | Name | When |
|---|---|---|
| 0 | `kSuccess` | normal quit; screenshot saved; `--help`, `--version` |
| 1 | `kFailure` | the screenshot could not be saved (unwritable path, unknown extension, no visible window). `QCommandLineParser::process` also exits with 1 on an unknown option or a missing value |
| 2 | `kInternalError` | a `std::exception` escaped `Run`: `main` prints `QiYaa: internal error: <what>` to stderr |

**Traps:**
- `Run` takes `int& argc` because `QApplication` keeps a reference to it; it must not become a copy.
- The `try`/`catch` in `main` covers what runs outside Qt's event loop: construction, `start()`,
  the options and the screenshot without `--play-file`. Exceptions are never thrown inside the
  event loop (see `CLAUDE.md`), and one thrown there would not arrive here intact.
- `--scale` goes through `QString::toDouble`: a non-number gives 0.0, which the windows clamp to
  1.0.
- `--text` is only the latest status text. Without `--offline`, the login messages replace it.
- The 1500 ms screenshot delay with `--play-file` is wall-clock time; how much of the visualizer
  it shows depends on the machine.

## Errors

`src/app` defines no exception type and throws nothing. It catches `Skins::Error` in two places:

- the constructor, for the skin from `--skin` or the `skin` setting: logs
  `Skin: <what>; using the built-in one` and keeps the base skin;
- `loadSkin()`: shows `Cannot load the skin: <what>` and returns false.

`Skins::Skin::BuiltinBase()`, in the member initialiser, is not caught: a broken bundled
`:/skins/base-2.91.wsz` is a bug. The `Skins::Error` leaves `Run`, and `main` turns it into exit
code 2. `Audio::Error` (equalizer preset files) never reaches this module; [src/ui](../ui/README.md)
catches it.

Failures that are data:

| Failure | How it shows |
|---|---|
| audio device | `AudioEngine::InitResult{ok, message}`: logs `Audio: <message>`, the app runs without sound |
| account connection | error string in the `Library::connectAccount` callback: marquee `Login failed: <error>` |
| login dialog cancelled | nothing happens |
| saving the token | `Yandex::SaveToken` returns false; ignored |
| `--play-file` cannot open | logs `Cannot open <path>` |
| screenshot not saved | `QImage::save` returns false: exit code 1 |
| settings file | `QSettings::status()` is never checked; an unwritable `settings.ini` loses changes silently |
| config folder | the result of `mkpath` is ignored |

## Not here

- Drawing, dragging, docking during a drag, shade, window resizing, per-window scale limits, the
  login dialog, the Yandex menu items, Milkdrop's own keys: [src/ui](../ui/README.md).
- Snapping geometry (`ConnectedGroup`, `SnapToOthers`, `PickScreen`, `ClampInside`):
  [src/ui](../ui/README.md), `snap.h`.
- The play queue, `shutDown()`, wave and play reports, the cover cache and its folder:
  [src/core](../core/README.md).
- Token lookup order, token file formats and permissions, OAuth, API requests, pending POSTs:
  [src/yandex](../yandex/README.md).
- `.wsz` loading and the bundled skins: [src/skins](../skins/README.md).
- The audio backend (`QIYAA_AUDIO_BACKEND`), equalizer DSP, `.eqf` presets:
  [src/audio](../audio/README.md).
- MPRIS, SMTC and macOS MediaPlayer: [src/integrations](../integrations/README.md).
- Milkdrop rendering and the preset list: [src/vis](../vis/README.md).
- The user-facing command-line reference: [docs/cli.md](../../docs/cli.md).
- The `.desktop` file, the Windows resource file and the installers: `packaging/`.
