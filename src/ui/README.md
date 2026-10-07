# `src/ui` — the skinned Winamp windows, the Yandex menus, the login dialog and the jam window

Namespace `Ui`. This folder holds the widgets the user sees: the main, equalizer and playlist
windows, the GEN.BMP-framed "Now playing", Milkdrop and jam windows, their common base class (frameless,
scaled, snapping, docking, shade mode), the pure geometry behind snapping, the Yandex part of the
context menus, and the login dialog. The windows paint sprites and turn clicks into calls on
`Core::Player` or into signals. They do not load skins or parse skin files
([src/skins](../skins/README.md)), draw the spectrum or run projectM ([src/vis](../vis/README.md)),
keep the queue or play audio ([src/core](../core/README.md), [src/audio](../audio/README.md)), talk
to Yandex ([src/yandex](../yandex/README.md)), or create, lay out, connect and save the windows
([src/app](../app/README.md)).

| File | Contains |
|---|---|
| `skinned_window.h/.cpp` | `SkinnedWindow` — abstract base of every skinned window: scaled painting, region mask, dragging with snapping, docking, shade mode that keeps the stack together |
| `snap.h/.cpp` | `kSnapDistance` and pure rectangle functions: snapping, choosing and clamping to a screen, docking (`Touching`, `ConnectedGroup`), `StackBelow` |
| `main_window.h/.cpp` | `MainWindow` — transport, time, marquee, visualizer, volume, balance, EQ/PL lights |
| `equalizer_window.h/.cpp` | `EqualizerWindow` — preamp and 10 bands, ON/AUTO, presets menu, `.eqf` load/save, spline graph |
| `playlist_window.h/.cpp` | `PlaylistWindow` — the player's queue: rows, selection, scrollbar, resize steps, bottom buttons |
| `gen_window.h/.cpp` | `GenWindow` — abstract GEN.BMP frame: title, close button, resize grip |
| `now_playing_window.h/.cpp` | `NowPlayingWindow` — cover and details of the current track |
| `milkdrop_window.h/.cpp` | `MilkdropWindow` — projectM view in a GEN frame: preset switching, black-preset skipping, full screen. Built only with Milkdrop |
| `library_menu.h/.cpp` | `AddLibraryActions`: the Yandex menu items, which call `Core::Sources` |
| `login_dialog.h/.cpp` | `LoginDialog` — Yandex login by device code or by a pasted token |
| `input_dialogs.h/.cpp` | `AskText`, `AskItem`: `QInputDialog` questions with the app's own OK and Cancel |
| `jam_window.h/.cpp` | `JamWindow` — the host's jam in a GEN frame: start, QR code and link, settings, guests, end, search that adds tracks. Built only with the jam |
| `jam_server_dialog.h/.cpp` | `JamServerDialog` — the jam server's address, "Teach the jam vibe" and "Guests may listen". Built only with the jam |

The windows' texts are English sources in `tr()` (in free functions
`QCoreApplication::translate` with the class-like context `Ui::LibraryMenu`, `Ui::InputDialogs`),
translated in [translations](../../translations/README.md). Painted texts follow the language at
the next repaint; `SkinnedWindow::changeEvent` answers `QEvent::LanguageChange` with
`retranslate()` (the window title) and `update()`.

## Dependencies

`qiyaa_ui` links PUBLIC `qiyaa_audio` (`equalizer_window.h` includes `audio/eq_presets.h` and
`audio/equalizer.h`), `qiyaa_vis` (`main_window.h` includes `vis/visualizer.h`,
`milkdrop_window.h` includes `vis/milkdrop_presets.h`) and `Qt6::Widgets`; PRIVATE `qiyaa_core`,
`qiyaa_skins` and `qiyaa_yandex` (the headers only forward-declare `Core::Player`,
`Core::CoverCache`, `Skins::Skin`, `Yandex::DeviceLogin`). `milkdrop_window.*` is added to the
target only when Milkdrop is built; `QIYAA_HAVE_MILKDROP` then comes PUBLIC from `qiyaa_vis`.
`jam_window.*` and `jam_server_dialog.*` are added only when the jam is built: `qiyaa_ui` then
links PUBLIC `qiyaa_jam` (`QIYAA_HAVE_JAM` comes with it) and PRIVATE `qiyaa_qrcodegen`
(`contrib/qrcodegen`, MIT).

It deliberately does not link `qiyaa_app` or `qiyaa_integrations` (both sit above it),
`libprojectM` or `Qt6::OpenGL` (the OpenGL window is `Vis::MilkdropView`; `Qt6::OpenGL` arrives only
through `qiyaa_vis`), `Qt6::Network` (`LoginDialog` receives a `QNetworkAccessManager*` and hands it
to `Yandex::DeviceLogin`), miniaudio or miniz.

Inside the module `skinned_window.h` is included by the main, equalizer, playlist and GEN windows,
`gen_window.h` by the "Now playing" and Milkdrop windows, and `snap.h` only by `skinned_window.cpp`
(and by `src/app`). `library_menu` and `login_dialog` include no other `ui/` header.

```bash
grep -rnE --include='*.h' --include='*.cpp' '#include "(app|integrations)/' src/ui/  # must print nothing
grep -rn --include='*.h' --include='*.cpp' 'throw' src/ui/                           # must print nothing
grep -rn --include='*.h' --include='*.cpp' 'catch' src/ui/ | grep -v 'Audio::Error'  # must print nothing
grep -rniE --include='*.h' --include='*.cpp' '#include <(projectM|QOpenGL)' src/ui/  # must print nothing
grep -nE '#include <Q(Widget|Screen|GuiApplication|Window|Painter)>' src/ui/snap.*   # must print nothing
```

## Window classes

- `QWidget`
  - [`SkinnedWindow`](#skinnedwindow--skinned_windowhcpp) — abstract: `paintSkin`, `isDragArea`
    - [`MainWindow`](#mainwindow--main_windowhcpp) — 275x116, shaded 275x14
    - [`EqualizerWindow`](#equalizerwindow--equalizer_windowhcpp) — 275x116, shaded 275x14
    - [`PlaylistWindow`](#playlistwindow--playlist_windowhcpp) — 275x116 plus 25x29 steps,
      shaded to a 14 px strip
    - [`GenWindow`](#genwindow--gen_windowhcpp) — abstract: `paintContent`; 275x116 plus 25x29
      steps, no shade mode
      - [`NowPlayingWindow`](#nowplayingwindow--now_playing_windowhcpp)
      - [`MilkdropWindow`](#milkdropwindow--milkdrop_windowhcpp) — only with Milkdrop
- `QDialog`
  - [`LoginDialog`](#logindialog--login_dialoghcpp)
- free functions: [Snap](#snap--snaphcpp) (`snap.h`),
  [library menu](#library-menu--library_menuhcpp) (`library_menu.h`)

Three coordinate spaces are in use:

| Space | Unit | Used for |
|---|---|---|
| skin | skin pixel, window top-left = (0, 0) | sprites, hit tests, `skinSize()`, `contentRect()`, `paintSkin` |
| widget | Qt logical pixel = skin × `scale()` | mouse events, `size()`, `update()` |
| screen | Qt logical pixel of the virtual desktop | `pos()`, `frameGeometry()`, snapping, screen rects |

`toSkin()` converts widget to skin with `static_cast<int>(floor(widget / scale))`; the windows
map back with `mapToGlobal(qRound(skin × scale()))` when they pop up a menu.

`src/app` creates one instance of each window, owns them, hands them one `const Skins::Skin*` (not
owned; replaced through `setSkin`) and connects their signals. The windows never call each other:
the equalizer's shade-mode volume, the EQ/PL lights, closing and the menus all go through the app.
Everything runs on the GUI thread; nothing here locks. `MainWindow`'s `closeEvent` accepts and emits
`closeRequested` (the app quits); every other window ignores the close event and emits
`closeRequested` (the app hides it and updates the matching light).

Tests: `tests/snap_test.cpp` (Snap), `tests/main_window_test.cpp` (drag, snap, clamp, scale),
`tests/windows_test.cpp` (docking, shade stack, scaling, playlist, equalizer, login dialog height,
"Now playing"), `tests/milkdrop_test.cpp` (preset order, switching, black presets, full screen),
`tests/screenshots_test.cpp` (golden images of the main and equalizer windows).

## SkinnedWindow — `skinned_window.h/.cpp`

```cpp
class SkinnedWindow : public QWidget {
public:
    SkinnedWindow(const Skins::Skin* skin, QSize skinSize, QWidget* parent = nullptr);

    void setSkin(const Skins::Skin* skin);           // not owned; keeps the size
    const Skins::Skin& skin() const;

    void setScale(double scale);                     // clamped to [1, 4], rounded to 0.05
    double scale() const;
    static bool IsIntegerScale(double scale);        // within 1e-6 of an integer

    QSize skinSize() const;                          // skin pixels, before scaling
    void setSkinSize(QSize size);

    void setDragsDockedWindows(bool on);             // a drag also moves the docked group
    void setSecondary();                             // Windows: Qt::Tool, no taskbar button
    void placeAt(QPoint pos);                        // clamped onto the screens; no-op on native Wayland
    void ensureVisible();                            // placeAt(pos())
    QList<SkinnedWindow*> dockedWindows() const;     // visible, touching directly or through others

    bool isShaded() const;
    virtual void setShaded(bool shaded);             // base: does nothing

    static bool CanPositionWindows();                // false on native Wayland
    static const QList<SkinnedWindow*>& AllWindows();  // every live instance, hidden ones too

Q_SIGNALS:
    void moveFinished();                             // left button released after a drag done here
    void shadeChanged(bool shaded);                  // after the resize and the stack move

protected:
    virtual void paintSkin(QPainter& painter) = 0;   // skin coordinates, painter already scaled
    virtual bool isDragArea(QPoint skinPos) const = 0;
    virtual bool skinMousePress(QPoint, Qt::MouseButton);        // true = consumed
    virtual void skinMouseMove(QPoint);
    virtual void skinMouseRelease(QPoint, Qt::MouseButton);
    virtual bool skinMouseDoubleClick(QPoint, Qt::MouseButton);  // false = replayed as a press
    virtual void skinChanged();
    virtual QString regionSection() const;           // region.txt section; empty = rectangle

    void resizeKeepingStack(QSize newSkinSize);
    void applyShade(bool shaded, QSize newSkinSize);
    QPoint toSkin(QPointF widgetPos) const;
    int wheelSteps(QWheelEvent* event);              // whole notches; positive = away from the user
    void applyMask();
    void updateSkinRect(const QRect& skinRect);
};
```

A top-level `Qt::Window | Qt::FramelessWindowHint` widget with `WA_OpaquePaintEvent` and
`WA_NoSystemBackground`, fixed at `round(skinSize × scale)`.

**Painting.** At an integer scale `paintSkin` gets a painter scaled by `scale()` with
`SmoothPixmapTransform` off, so every skin pixel is a square of whole pixels. At a fractional scale
nearest-neighbour would make some skin pixels 1 px and others 2 px wide, so the window is painted
"sharp bilinear": `paintSkin` draws at `bufferScale = ceil(scale × devicePixelRatio)` times the skin
size into an ARGB32-premultiplied buffer with nearest-neighbour, and that buffer is drawn into
`rect()` with smooth scaling. The buffer is kept between paints, dropped when the size changes and
reallocated when `bufferScale` changes. `updateSkinRect` repaints a skin rect scaled, aligned
outwards and grown by 1 px on each side (the smooth scaling bleeds into neighbours). An activation
change repaints everything (title bars have active and inactive sprites).

**Mask.** `applyMask` takes the polygons of `regionSection()` from `Skin::region()`, scales the
polygons themselves (not a `QRegion`, which would lose accuracy at fractional scales) and sets
`Skins::RegionFromPolygons` of them as the mask. An empty section name or a section the skin lacks
clears the mask. It runs on `setSkin`, `setScale`, `setSkinSize` and `applyShade`. Sections in use:
`normal`/`windowshade` (main), `equalizer`/`equalizerws` (equalizer); the playlist and GEN windows
are rectangular.

**Registry and screens.** The constructor appends the window to `AllWindows()` (construction
order), the destructor removes it. When a screen is removed, or the available geometry of any screen
changes (screens present at construction and those added later), each visible window calls
`ensureVisible()`; `setScale` calls it too. `placeAt` picks the screen with `PickScreen` among the
available geometries of all screens and moves the window there with `ClampInside`.

**Dragging.** A left press that `skinMousePress` does not consume and `isDragArea` accepts starts a
drag. On native Wayland (`CanPositionWindows()` is false when the platform name starts with
`wayland`) it is handed to `QWindow::startSystemMove()`: no snapping, no group, no `moveFinished`.
Otherwise the drag group is this window plus, with `setDragsDockedWindows(true)`, its
`dockedWindows()`. Every mouse move translates the bounding rect of the group by the cursor delta,
runs `ResolveDragPosition` against the visible windows outside the group and the screens' available
geometry, and moves every member by the same offset, so the group snaps and is clamped as one
rectangle. Members are held by `QPointer`; one destroyed mid-drag is skipped. The left-button
release ends the drag and emits `moveFinished` (the app saves positions).

**Shade and the stack.** `applyShade(shaded, newSkinSize)` sets the flag, calls
`resizeKeepingStack`, re-applies the mask and emits `shadeChanged` last, so listeners see final
positions. `resizeKeepingStack(newSkinSize)` snapshots the frame rects of all registered windows,
hidden ones too (so they are still docked when shown again), with their visibility as `solid`;
resizes; and moves the windows `StackBelow` returns by the height difference in widget pixels. If
the window grew and is visible, the moved windows and its docked group are then shifted together so
that their union stays on its screen (a stack growing near the bottom edge is lifted). On native
Wayland only the window itself resizes.

`wheelSteps` adds `angleDelta().y()` to an accumulator and returns whole 120-unit notches, keeping
the remainder, so touchpads and smooth-scrolling mice step like a wheel.

**Traps:**
- `paintSkin` must cover every pixel: nothing clears the window (opaque paint) or the fractional
  buffer between paints.
- At a fractional scale `paintSkin` paints the whole window on every paint event; `updateSkinRect`
  only narrows the final blit.
- `IsIntegerScale` looks at `scale()` alone, not at `devicePixelRatioF()`: at scale 1 or 2 on a
  125 % or 150 % display the nearest-neighbour path runs through Qt's device transform and skin
  pixels come out with uneven widths.
- Docking is not stored anywhere: two windows are docked exactly when their frame rects touch
  (`Touching`). A 1 px gap, e.g. rounding after a scale change, undocks them; the app re-docks after
  `setScale` ([src/app](../app/README.md)).
- Only `resizeKeepingStack`/`applyShade` move other windows. `setSkinSize` and `setScale` do not,
  so resizing the playlist or a GEN window leaves the windows docked below it in place.
- `skinMouseMove` is called only while no window drag runs, and mouse tracking is off: moves arrive
  only while a button is held.
- An unconsumed double click is replayed as a press, so a double click on a drag area starts a
  drag.
- `moveFinished` is not emitted by `placeAt`, `ensureVisible`, `resizeKeepingStack` or a system
  move.
- Hidden windows are not pulled back when screens change; call `ensureVisible()` when showing one.
- `setSecondary()` changes window flags: call it before the first `show()`, since changing flags
  hides a visible window.

## Snap — `snap.h/.cpp`

```cpp
inline constexpr int kSnapDistance = 15;  // in the units of the rects: logical screen pixels here

QPoint SnapToOthers(const QRect& moving, const QList<QRect>& others, int distance = kSnapDistance);
QPoint SnapWithin(const QRect& moving, const QRect& screen, int distance = kSnapDistance);
QRect PickScreen(const QRect& rect, const QList<QRect>& screens);  // empty QRect when screens is empty
QPoint ClampInside(const QRect& rect, const QRect& screen);
bool Touching(const QRect& first, const QRect& second);
QList<int> ConnectedGroup(int start, const QList<QRect>& rects);  // without start itself
QList<int> StackBelow(int self, const QList<QRect>& rects, int dy, const QList<bool>& solid = {});
QPoint ResolveDragPosition(
    const QRect& proposed,
    const QList<QRect>& others,
    const QList<QRect>& screens,
    int distance = kSnapDistance
);
```

Pure functions of their arguments, ported from webamp's `snapUtils.ts` (MIT, see
`THIRD_PARTY.md`). Callers: `SkinnedWindow` and `src/app` (re-docking after a scale change).
Positions are returned as the new top-left.

- **Edges are exclusive**, as in webamp: right = x + width, bottom = y + height. `QRect::right()`
  and `bottom()` (inclusive, x + width − 1) are never used.
- "Near" is strict: |a − b| < distance. At 15 a gap of 14 px snaps and a gap of 15 does not.
- `SnapToOthers`: for each rect of `others`, in order, whose extent on the other axis overlaps the
  moving rect's (with `distance` as slack), x snaps with this priority: our left to its right, our
  right to its left, left to left, right to right; y the same with top and bottom. A later rect
  overrides an earlier one on the same axis; an axis that snaps nowhere keeps its value.
- `SnapWithin`: when the left edge is nearer than `distance` to the screen's left edge, or past it,
  x becomes the screen's left; otherwise the same test on the right edge. Likewise for y.
- `PickScreen`: the screen with the largest intersection, ties to the earlier one; when none
  intersects, the nearest by squared distance between the rects.
- `ClampInside`: an empty screen leaves the position alone. Otherwise the rect is moved in from the
  right and bottom first, then from the left and top, so a rect larger than the screen keeps its
  top-left visible.
- `Touching`: a shared edge at distance 0 with a positive overlap along it; touching corners do not
  count. This is Winamp's "docked".
- `ConnectedGroup`: breadth-first over `Touching`, indices in discovery order; an out-of-range
  `start` gives an empty list.
- `StackBelow(self, rects, dy, solid)`: `rects[self]` changes height by `dy` with its top fixed;
  returns the indices (ascending) that move by `dy`. Only rects whose top is at or below the old
  bottom are candidates. A candidate follows when it hangs from `self` or from a follower (its top
  equals their bottom and they overlap horizontally). Growing, it also follows when the grown rect,
  or a follower moved by `dy`, would intersect it. Shrinking, a candidate stays when a staying solid
  rect also holds it up, and a follower that would land on a staying solid rect is blocked
  (together with what followed only through it); this repeats until nothing conflicts.
  `solid[i] == false` (hidden windows): may follow, never holds up or blocks. Empty = all solid.
- `ResolveDragPosition`: `SnapToOthers`, then `PickScreen` for the snapped rect, then `SnapWithin`
  and `ClampInside` on that screen. No screens: the snapped position.

**Traps:**
- `kSnapDistance` does not grow with `scale()`: `SkinnedWindow` passes frame rects in screen pixels.
- All rects of one call must be in one coordinate space; skin pixels mixed with screen pixels give
  meaningless results.
- `SnapWithin` also catches a rect that is past a screen edge, however far, and puts it back at
  the edge: one call can move it by far more than `distance`.

## MainWindow — `main_window.h/.cpp`

```cpp
class MainWindow : public SkinnedWindow {
public:
    enum class VisMode { Spectrum, Oscilloscope, Off };

    MainWindow(Core::Player* player, const Skins::Skin* skin, QWidget* parent = nullptr);

    void setVolume(int value);                  // clamped to 0..100
    void setBalance(int value);                 // clamped to -100..100; |value| < 8 becomes 0
    int volume() const;                         // 75 at start
    int balance() const;

    void setEqButton(bool on);                  // the owner reports whether EQ / playlist are shown
    void setPlaylistButton(bool on);

    VisMode visMode() const;
    void setVisMode(VisMode mode);
    bool showsRemainingTime() const;
    void setShowsRemainingTime(bool on);

    void setStatusText(const QString& text);    // replaces the marquee for 3 s
    void setTrackNote(std::function<QString(int row)> note);  // after the title: "+ Аня" in a jam

    void setShaded(bool shaded) override;

Q_SIGNALS:
    void eqToggleRequested();
    void playlistToggleRequested();
    void menuRequested(QPoint globalPos);          // options button or right click
    void sourcesMenuRequested(QPoint globalPos);   // eject button
    void closeRequested();                         // close button or window-manager close
    void minimizedChanged(bool minimized);
    void volumeChanged(int volume);                // only when the value changed
    void balanceChanged(int balance);
};
```

275x116 (`Skins::MainWindowSprites::kSize`), shaded 275x14. Drags its docked group. The `Player` is
not owned and must outlive the window. `setVolume`/`setBalance` push the value to the
`AudioEngine` every time, the signals fire only on a change. `Player::statusMessage` is connected to
`setStatusText`.

The timer runs only while something on screen changes (first matching row wins):

| State | Interval | Repaint |
|---|---|---|
| playing or buffering, visualizer on, visible, not minimized, not shaded | 33 ms | visualizer rect; whole window every 100 ms |
| playing or buffering otherwise | 100 ms | whole window |
| paused | 250 ms | whole window (the time blinks 1 s on, 1 s off) |
| status text shown, or marquee text wider than 155 px | 220 ms | whole window |
| anything else, or minimized | stopped | — |

Each visualizer frame reads 1024 samples per channel with `AudioEngine::readVisSamples`, feeds the
mono mix to `Vis::Analyzer` and hands both to the `Vis::Visualizer`; the visualizer is reset when it
stops animating.

The marquee shows, first match wins: the status text; `VOLUME: n%`, `BALANCE: CENTER` /
`BALANCE: n% LEFT|RIGHT` or `SEEK TO: m:ss/m:ss (p%)` while that slider is held;
`QiYaa <version>` without a track; `<n>. <title> (<m:ss>)`, followed by ` · <note>` when the track
note gives one for the current row (who added a jam item, HOST-34). Text wider than the marquee scrolls one
character per 220 ms with `  ***  ` between repeats, except while a status text is shown or a
control is held. The time shows minutes up to 99; remaining time is `ceil(duration − position)`.

Buttons act on release inside their rectangle and show the pressed sprite while the cursor is in
it. Volume, balance and position follow the mouse; while the position bar is held `seekPreview`
holds the 0..1 fraction (−1 otherwise) and the seek happens with `Player::seekFraction` on release
(a press on it while stopped is swallowed). Every spot that is not
a control, including the marquee, drags the window. A click on the visualizer cycles `VisMode`, a
click on the time toggles remaining time, a double click on the title bar (y < 14, not a control)
toggles shade, the wheel changes the volume by 4 per notch and shows `VOLUME: n%`.

**Traps:**
- Shade-mode hit rectangles (transport at x 169–224, mini position bar 226,4 17x7, mini time
  127,4 25x6) are literals in `main_window.cpp`, not in `skins/sprites.h`.
- The shuffle and repeat lights are read from `Player` when painting. The shuffle light is
  `Player::shuffleActive()`, so it is off during a wave even when the user's choice is on.
  `modesChanged` and `queueReplaced` repaint the window, so a change from MPRIS or a new queue
  shows at once.
- The eject menu position comes from the unshaded layout: in shade mode `sourcesMenuRequested`
  points at skin (136, 105), below the 14 px strip.

## EqualizerWindow — `equalizer_window.h/.cpp`

```cpp
struct EqualizerControl {
    enum class Kind {
        None,
        Close,
        Shade,
        ShadeVolume,
        ShadeBalance,
        On,
        Auto,
        Presets,
        Preamp,
        Band
    };

    Kind kind = Kind::None;
    int band = 0;

    bool operator==(const EqualizerControl&) const = default;
};

class EqualizerWindow : public SkinnedWindow {
public:
    explicit EqualizerWindow(const Skins::Skin* skin, QWidget* parent = nullptr);

    const Audio::EqSettings& settings() const;
    void setSettings(const Audio::EqSettings& settings);   // no signal
    bool autoOn() const;
    void setAutoOn(bool on);
    void setShaded(bool shaded) override;
    void setMixer(int volume, int balance);                 // what the shade-mode sliders show

    static QList<double> GraphCurve(const Audio::EqSettings& settings);  // 109 samples, graph pixels

Q_SIGNALS:
    void settingsChanged(const Audio::EqSettings& settings);
    void statusText(const QString& text);        // "EQ: 60HZ +3.0 DB", "EQ: ON", file errors
    void closeRequested();
    void volumeRequested(int volume);            // shade-mode slider, 0..100
    void balanceRequested(int balance);          // shade-mode slider, -100..100
};
```

275x116, shaded 275x14; everything that is not a control drags the window. The preamp and the 10
bands (`Audio::kEqBandHz`, 60 Hz … 16 kHz) hold dB in ±12 (`Audio::kEqMaxDb`):

- dragging: the 51 px of thumb travel map +12 … −12 dB, rounded to 0.1 dB; |value| < 0.6 becomes 0
  (centre detent);
- wheel over a slider: ±0.5 dB per notch, clamped, rounded to 0.1 dB, no detent;
- double click on a slider: 0 dB; double click on the title bar (y < 14, not a control): shade.

Every slider change emits `settingsChanged` and `statusText("EQ: <BAND> ±x.x DB")` with the band
named `60HZ`, `1KHZ`, … or `PREAMP`; the app shows it in the main window's marquee. ON toggles
`EqSettings::enabled` and says `EQ: ON`/`EQ: OFF`. AUTO toggles a flag and emits nothing; the app
stores `autoOn()` and nothing else acts on it. The presets button opens a menu: reset to 0 dB,
load `.eqf`, save `.eqf`, then `Audio::BuiltinEqPresets()`. A preset or a reset keeps the current
`enabled`.

In shade mode the volume and balance sliders belong to the main window: they emit
`volumeRequested`/`balanceRequested`, the app forwards them to `MainWindow` and mirrors the values
back with `setMixer`. Each thumb has three sprites, chosen by which third of the range the value is
in.

The graph is a natural cubic spline (port of webamp's `spline.js`; tridiagonal system for the
slopes, solved with the Thomas algorithm) through the 10 band values at x = 0, 12, …, 108, sampled
at every integer x; y is in graph pixels 0..18 with 0 = +12 dB. Each drawn pixel takes its colour
from the 1 px colour column `kGraphLineColors` of EQMAIN.BMP at that height (green when the sheet
is missing). The preamp line is drawn at the preamp's height.

**Traps:**
- `setSettings` emits nothing: the owner applies the settings to the engine itself.
- `pressedControl` is an `EqualizerControl`: its `band` means something only for `Kind::Band`,
  and `ShadeVolume`/`ShadeBalance` are the shade-mode sliders.
- The reset menu item emits `settingsChanged` but no `statusText`.

## PlaylistWindow — `playlist_window.h/.cpp`

```cpp
class PlaylistWindow : public SkinnedWindow {
public:
    PlaylistWindow(Core::Player* player, const Skins::Skin* skin, QWidget* parent = nullptr);

    QSize sizeSteps() const;                // (0, 4) at start: 275x232
    void setSizeSteps(QSize steps);         // each clamped to 0..40; 275x116 + steps × 25x29
    void setShaded(bool shaded) override;   // full width, 14 px high

    int visibleRows() const;                // max(1, list height / 13)
    int scrollOffset() const;               // first visible row
    void setScrollOffset(int row);          // clamped to 0..count − visibleRows
    void ensureRowVisible(int row);
    const QSet<int>& selection() const;     // queue indices
    int rowAt(QPoint skinPos) const;        // -1 outside the rows

Q_SIGNALS:
    void closeRequested();
    void sizeStepsChanged(QSize steps);     // only on a change
    void sourcesMenuRequested(QPoint globalPos);  // ADD button or right click outside a row
    void trackMenuRequested(QPoint globalPos, int row);  // right click on a track
};
```

Frame from PLEDIT.BMP: top 20, bottom 38, left 12, right 20 px; rows are 13 px and start 3 px
below the top of the list. Text uses the PLEDIT.TXT font at 9 px with 0.5 px letter spacing, the
current track in the `current` colour, selected rows on `selectedBackground`, durations right-aligned
(`h:mm:ss` from one hour). Rectangular (no region mask). The title bar (y < 20) drags the window; in
shade mode the whole strip does, except the close (width − 11, 3) and shade (width − 21, 3)
buttons, 9x9 each, which act on release inside.

- Selection: a click selects one row, Ctrl toggles a row, Shift selects the range from the anchor
  (the anchor stays). Up, Down, PgUp, PgDn, Home and End move the keyboard row (Shift extends),
  Enter plays it, Delete removes the selection (`Player::removeTracks`). The window takes
  `Qt::StrongFocus`.
- `setQueueHooks(QueueHooks)` is how a jam changes the playlist (HOST-21, HOST-34): `note(row)` is
  drawn right-aligned before the duration, in the row's colour, and the title is elided before it;
  Delete and REM's "remove selected" call `remove(rows)` (sorted) and REM's "clear" calls
  `clear()` first, and the `Player` is changed only when the hook is unset or returns false. The
  selection is cleared either way.
- `Player::queueReplaced` clears selection, anchor, keyboard row and scroll; `playlistChanged` drops
  selected indices past the end and re-clamps the scroll; `currentTrackChanged` scrolls the current
  row into view.
- A double click on a row plays it; on the title bar, or anywhere on the shaded strip left of the
  buttons, it toggles shade. The wheel scrolls 3 rows per notch. In the scrollbar (right frame) the
  handle drags and a click on the track jumps there. The bottom-right 20x20 grip resizes in steps
  of 25x29 px of mouse travel.
- Right click selects the row under the cursor unless it is already selected, then emits
  `trackMenuRequested` with that row, even within a larger selection. A keyboard context menu
  uses the focused row, falling back to the current track. A click outside the rows emits
  `sourcesMenuRequested`.
- Bottom bar (base = width − 150, height − 38): running time `selected/total` at base + (7, 10);
  elapsed time at base + (66, 23), repainted alone when `Player::positionTick` crosses a second.
  Buttons 22x18 at y = height − 30: ADD (x 14) emits `sourcesMenuRequested` at the button, REM
  (x 43) offers "remove selected" / "clear", SEL (x 72) "select all" / "none", MISC (x 101) and LIST
  (x = width − 44) do nothing (Winamp's MISC sorts and shows file info, which has no counterpart
  for a stream queue yet). Mini transport 8x10 at base + (3 | 11 | 20 | 29 | 37, 22): previous,
  play, pause, stop, next; the sixth (eject, base + 45) does nothing.

**Traps:**
- Selected rows are queue indices. Change the queue only through `Player`, whose signals fix the
  selection.
- `setScrollOffset(scrollRow)` is not a no-op: it re-clamps after the row count or the height
  changed (resize, shade, unshade, queue change).
- Resizing does not move the windows docked below (see `SkinnedWindow`).

```cpp
struct PlaylistWindow::QueueHooks {
    std::function<QString(int row)> note;
    std::function<bool(const QList<int>& rows)> remove;
    std::function<bool()> clear;
};
void PlaylistWindow::setQueueHooks(QueueHooks hooks);
```

## GenWindow — `gen_window.h/.cpp`

```cpp
class GenWindow : public SkinnedWindow {
public:
    GenWindow(const Skins::Skin* skin, const QString& title, QWidget* parent = nullptr);

    QSize sizeSteps() const;
    void setSizeSteps(QSize steps);        // each clamped to 0..40; 275x116 + steps × 25x29

Q_SIGNALS:
    void closeRequested();
    void sizeStepsChanged(QSize steps);    // only on a change

protected:
    QRect contentRect() const;             // skin pixels inside the frame: (11, 20, w − 19, h − 34)
    virtual void paintContent(QPainter& painter, const QRect& area) = 0;
    virtual bool contentMousePress(QPoint, Qt::MouseButton);  // true = consumed
};
```

Winamp's generic plugin frame from GEN.BMP: top 20 px, left 11, right 8, bottom 14; the top and
bottom are tiled in 25 px pieces, the sides in 29 px pieces with the bottom pieces anchored to the
bottom, and the bottom corners are 125 px wide. The title is drawn with GEN.BMP's letters (A–Z
only, see [src/skins](../skins/README.md)) between the two title end pieces, centred, with 4 px of
padding on the left and 3 px on the right; the top pieces have active and inactive sprites. The
content is painted first, clipped to `contentRect()`, and the frame over it.

The title bar (y < 20) drags the window. The close button (width − 11, 3, 9x9) acts on release
inside. The bottom-right 20x20 grip resizes: each 25 px (29 px) of mouse travel, rounded, is one
step. A press inside `contentRect()` goes to `contentMousePress`. No mask, no shade mode.

**Traps:**
- An unconsumed press in the content area does nothing: only the title bar drags.
- The size cannot go below 275x116 skin pixels.

## NowPlayingWindow — `now_playing_window.h/.cpp`

```cpp
class NowPlayingWindow : public GenWindow {
public:
    NowPlayingWindow(
        Core::Player* player,
        Core::CoverCache* covers,
        const Skins::Skin* skin,
        QWidget* parent = nullptr
    );
    QRect coverRect() const;   // skin pixels; see below
};
```

`coverRect()` is a square at the top-left of the content inset by 4 px, with side
min(inset height, inset width / 2).

Title `NOW PLAYING`. The content uses the PLEDIT.TXT colours and font (`Skin::playlistStyle()`).
The cover is requested at 400 px (`Track::coverUrl(400)`) from `Core::CoverCache` and drawn with
smooth scaling (a photo, unlike the sprites); until it arrives an outline stands in. Next to it: the
title (bold 11 px, `current` colour), artists, album with year, duration and `♥ In Liked`
for a liked track, each elided on the right; lines that do not fit are left out. Without a track:
`Nothing is playing`. Repaints on `currentTrackChanged`, `Library::likesChanged` and
`CoverCache::ready` for the current cover. A left click on the cover opens `Track::webUrl()` in the
browser. `Player` and `CoverCache` are not owned.

**Traps:**
- `CoverCache::get` starts the download as a side effect of painting; the repaint comes from
  `ready`. The URL includes the size, so another size is another cache entry.

## JamWindow — `jam_window.h/.cpp`

```cpp
class JamWindow : public GenWindow {
public:
    enum class Page { Jam, Search };
    JamWindow(Jam::HostSession* host, Yandex::Library* library, const Skins::Skin* skin, QWidget* parent = nullptr);
    void setHostName(const QString& name);       // offered on the start page until the user types
    void setServerName(const QString& server);   // shown on the start page; empty: "not set"
    void setTextFont(const std::optional<QFont>& font);  // tests; default: PLEDIT.TXT font, 9 px
    Page page() const;  void showPage(Page page);
    void search(const QString& text);
    struct Control { QRect rect; QString label; bool enabled; std::function<void()> action; };
    QList<Control> controls();                   // what can be clicked now, in skin pixels
Q_SIGNALS:
    void statusText(const QString& text);        // for the marquee
    void serverSettingsRequested();
};
```

The host's side of the jam on screen (spec/jam/host.md HOST-20, HOST-25, HOST-35), title `JAM`,
in the PLEDIT.TXT colours and font like the "Now playing" window. What it shows follows
`HostSession::phase()`:

- **None:** "New jam", what a jam is, "Your name" (a field), "Start the jam" (enabled with a name
  and a server), a problem line when `create` refused, the server and "Server settings…"
  (`serverSettingsRequested`).
- **Creating:** "Connecting to the jam server…" and "Cancel" (`cancelCreate`).
- **Active:** two tabs, "Jam" and "Add tracks", with the connection on the right ("Connected",
  "Connecting…", "No connection" and a light), and without a connection the line "No connection to
  the jam server · trying again by itself".
  - **Jam page:** the QR code of `joinUrl`, dark on white whatever the skin (not every camera
    reads an inverted code), ECC M with a 4-module quiet zone, the largest whole number of pixels
    per module within 120 px and half the width; "Link for the guests" with the link (up to three
    lines), "Copy" (to the clipboard, `statusText("The link is copied")`) and "New
    link" (`rotateLink`); the settings, each a label and a button with its value that flips
    it ("Order": "Taking turns" / "First come", "Guests can skip": "Yes" / "No", "New
    guests": "Let in" / "Closed"); "Guests · N" with a row per guest (light for online, name,
    kind, "waiting: N", "Remove" to kick), scrolled by the wheel; at the bottom "End the jam",
    which asks "Press again to end" and ends on a second click within 3 s.
  - **Search page:** a field and "Search" (Enter too), then `Library::searchTracks`: the available
    tracks, 20 at most, each with "To the jam" (`add`) and "Next" (`playNext`), and their
    `statusText`; "Searching…", "Nothing found", "The search failed: …". A newer search replaces an
    older one's answer.
- Without a connection (`!isConnected()`) "New link", the settings, "Remove", "To the jam" and
  "Next" are drawn dim and do nothing (HOST-25); the link, the tabs, the search and the end work.
  An action the session refuses says `No connection to the jam server`.

**How it is drawn.** One pass, `render(QPainter*)`, both paints the page (with a painter) and
lists its controls (without one), so a click always finds what is drawn. Controls are boxes in the
`normal` colour (dim when disabled; a selected tab on `selectedBackground`). Fields show the end of
their text and a caret while focused; a click on a field focuses it, a click elsewhere drops the
focus. Keys while a field is focused: text, Backspace, Ctrl+V (one line), Enter (start or search),
Esc (drop the focus); the name is cut to 24 and the search to 100 characters.

**Traps:**
- The app's shortcuts are actions on every window (Z, X, C, V, B, arrows…). While a field has the
  focus, the window takes `ShortcutOverride` for keys without Ctrl or Alt (and for Paste), or
  typing "x" would start playback.
- `qrcodegen::QrCode::encodeText` throws `data_too_long` for text no QR code holds; the window
  catches it there (a warning, no code). A join link is far below that limit.
- The pictures in `tests/data/golden/jam-*.png` are drawn with `setTextFont` set to the Tiny5
  pixel font without antialiasing (`tests/data/fonts`), the same on every system; the PLEDIT.TXT
  font is the system's and is not compared.

## JamServerDialog — `jam_server_dialog.h/.cpp`

```cpp
class JamServerDialog : public QDialog {
public:
    JamServerDialog(const QString& server, bool waveFeedback, bool shareAudio, const QString& defaultServer, QWidget* parent = nullptr);
    QString server() const;           // as typed, or the default for an empty field
    bool waveFeedback() const;
    bool shareAudio() const;
    static bool IsServerAddress(const QString& text);   // http(s) with a host
};
```

"Jam server": the address (empty shows and means the default, `QIYAA_JAM_URL`), with "An
address like https://jam.example.org is needed" and Save disabled until it is an http(s) URL with a host;
"Teach the jam vibe" (HOST-16) with what it means; "Guests may listen" with what it
does and that the files are the host's subscription's. The owner writes the settings.

## MilkdropWindow — `milkdrop_window.h/.cpp`

```cpp
class MilkdropWindow : public GenWindow {
public:
    enum class PresetOrigin { User, Automatic };

    MilkdropWindow(
        Audio::AudioEngine* engine,
        const QString& builtInPresetDirectory,  // bundled presets (":/milkdrop")
        const QString& userPresetDirectory,     // the user's .milk files and textures/
        const Skins::Skin* skin,
        QWidget* parent = nullptr
    );

    const Vis::MilkdropPresets& presets() const;
    int currentIndex() const;              // -1 before the first preset
    QString currentPreset() const;
    void selectPreset(
        int index,
        Vis::PresetTransition transition = Vis::PresetTransition::Blend,
        PresetOrigin origin = PresetOrigin::User
    );
    void selectPreset(const QString& name);
    void nextPreset();
    void previousPreset();
    void reloadPresets();

    bool shuffle() const;                  // true at start
    void setShuffle(bool on);
    bool locked() const;                   // no automatic switching
    void setLocked(bool on);
    int presetSeconds() const;             // 30 at start
    void setPresetSeconds(int seconds);    // clamped to 5..3600
    void setPlaying(bool playing);         // 60 fps and the black watch while playing, else 20 fps

    bool isFullScreenMode() const;
    void setFullScreenMode(bool on);

    Vis::MilkdropView* view() const;       // null before the first show and without OpenGL
    QString failure() const;               // why nothing can be shown; empty when fine

    void onSwitchRequested(Vis::PresetTransition transition);  // the view's signals, public for tests
    void onPresetFailed(const QString& message);
    void onStaysBlack();

    QStringList blackPresets() const;      // sorted names
    void setBlackPresets(const QStringList& names);
    bool isBlack(int index) const;
    QString userPresetDirectory() const;

Q_SIGNALS:
    void presetChanged(const QString& name, Ui::MilkdropWindow::PresetOrigin origin);
    void settingsChanged();                // preset, shuffle, lock, interval or black list changed
    void transportKey(int key);            // Z, X, C, V, B, Left, Right pressed in the view
};
```

Title `MILKDROP`. The preset list is loaded in the constructor (built-ins first, then the user's,
see [src/vis](../vis/README.md)); OpenGL is not touched until the window is first shown, because
probing it loads the GPU driver. On the first show `Vis::MilkdropView::OpenGlProblem()` is checked;
if OpenGL is usable a `MilkdropView` (a `QOpenGLWindow`) is embedded with
`QWidget::createWindowContainer` over `contentRect()` (scaled, focus on click). The container owns
that view. Texture search paths are `userPresetDirectory` and `userPresetDirectory/textures`. When
the view is ready it starts with the selected preset, or the first `followingPreset()`, without
blending, and emits `presetChanged(name, PresetOrigin::Automatic)`. When a view fails, the embedded
one is hidden and stops rendering, a full-screen one closes; `failure()` then returns the reason and
the frame shows `Milkdrop is not available: <reason>`.

Rendering: the embedded view renders while the window is visible and the view has not failed; in
full screen only the full-screen view renders. `setPlaying` switches between 60 and 20 fps. Full
screen is a second `MilkdropView`, a top-level window on this window's screen, covering its full
geometry with a blank cursor. It is refused when the embedded view is missing or failed. Leaving
full screen hides it, releases it to `deleteLater` and reloads the current preset in the embedded
view without blending; hiding the window leaves full screen.

**Switching.** `selectPreset(index, transition, origin)` ignores an index out of range or a preset
whose file reads as empty. Otherwise it records the previous index in the history (up to 100
entries), loads the preset into the active view (blended when `transition` is `Blend`) and emits
`presetChanged(name, origin)` and `settingsChanged`. `origin` is `Automatic` for every automatic
switch; the app shows the name in the main window's marquee only when it is `User`.
`followingPreset()`:
- shuffle: uniformly at random (`QRandomGenerator::global()`) among the presets that are not black
  and not the current one; drawing from that list, not retrying random picks, is what keeps a
  black preset from ever coming up;
- in order: the next preset after the current one that is not black, wrapping; before the first
  preset it starts at index 0;
- every other preset black: stay on the current one if it is not black; otherwise
  `MilkdropPresets::random`/`next` regardless, so switching never gets stuck.

`previousPreset()` in shuffle mode pops the history (without recording where it came from); with
an empty history, or in order mode, it steps back in list order skipping black presets.
`MilkdropView::switchRequested(transition)` (the preset's time is up) switches to
`followingPreset()` with that `transition`; ignored when locked or when there are no presets.
`presetFailed` (projectM keeps showing the previous preset) moves on to `followingPreset()`
without blending, and gives up after `min(10, number of presets)` failures in a row. `nextPreset`,
`previousPreset`, the H key, a pick from the menu, `switchRequested` and `staysBlack` reset that
count.

**Black presets.** Some GPUs or drivers show only black for some presets. While music plays the
view watches for that (`setBlackWatch`); silence may legitimately fade a preset to black, so the
watch is off otherwise. `staysBlack` adds the current preset's name to the black list, emits
`settingsChanged` (the app stores `blackPresets()` in its settings) and switches on without
blending. Black presets are skipped by every automatic choice and by `previousPreset` in order
mode, and stay selectable by hand; the preset menu marks them `(black here)`. A streak of black
presets means something else is wrong (no sound reaching the view, a driver problem), so the sixth
`staysBlack` in a row turns the watch off on both views and blacklists nothing: at most 5 presets
per streak. `drawsPicture`, `switchRequested` (a preset played its full time) and
`setBlackPresets` reset the streak. The menu item that restores the skipped presets clears the list
and turns the watch back on while music plays.

Keys arrive from `MilkdropView::keyPressed`: Space or N next, Backspace or P previous, H next
without blending, R shuffle, L or Scroll Lock lock, F or Alt+Enter full screen, Esc leaves full
screen, Ctrl+Shift+K leaves full screen and emits `closeRequested`, Z X C V B Left Right go out as
`transportKey` for the app to forward to the player. A double click on the view toggles full
screen; a right click in the view or in the frame's content opens the preset menu.

**Traps:**
- Built only with `QIYAA_HAVE_MILKDROP`; code that uses the class must be inside
  `#if defined(QIYAA_HAVE_MILKDROP)`.
- `view()` is null until the first show. `selectPreset(name)` before the view is ready only sets
  the preset to start with: no history, no signal.
- Leaving full screen can run inside the full-screen view's own key or double-click handler, hence
  `deleteLater` rather than deleting it.
- `previousPreset` in shuffle mode walks the history and does not skip presets blacklisted since.
- After the streak limit the watch stays off on the existing views even when the streak resets; it
  comes back with `setPlaying` changing, the restore menu item, or a new full-screen view.
- `reloadPresets` re-reads the folders, clears the history and keeps the current preset by name
  (index −1 if it is gone), but does not reload the view.

## Library menu — `library_menu.h/.cpp`

```cpp
void AddLibraryActions(
    QMenu* menu,
    Core::Player* player,                 // library and current track for the dislike label
    Core::Sources* sources,               // what every item plays
    const Yandex::Track* track,            // target of like/dislike/open; nullptr disables them
    QWidget* dialogParent,                // parent of the search input dialog
    std::function<void()> loginRequested  // the only item when not logged in
);
```

The menu only builds items; what they play, and every status they report, is `Core::Sources`
([src/core](../core/README.md#sourcesh-sources)).

- When logged in: My Vibe, Liked, submenus Wheel of vibes (the vibes around
  `Sources::lastWaveSeeds()`), For you, Playlists (Listen / Similar tracks), Artists, Albums,
  Stations (grouped by station type), Search…, then like/unlike, dislike and open in the browser;
  those three are disabled without a target track. The target's ID, liked state and browser URL
  are captured when the menu is built, so changing the queue does not retarget an open menu.
  The dislike label includes "(skip)" when the target is current at menu creation; `Sources`
  checks again when the action runs and skips only if that track is still current.
- Submenus load their lists straight from `Yandex::Library` on their first `aboutToShow`, once per
  menu instance: a disabled `Loading…`, then the items, `(empty)` or a disabled `Error: …`.

**Traps:**
- Menu actions capture `Player*`, `Sources*` and `Library*` unguarded; that is safe only because
  the app owns them for the whole run.
- Submenus never reload: build a new menu for fresh data.

## LoginDialog — `login_dialog.h/.cpp`

```cpp
class LoginDialog : public QDialog {
public:
    explicit LoginDialog(
        QNetworkAccessManager* networkManager,
        QWidget* parent = nullptr,
        const QString& oauthBase = {}     // empty = https://oauth.yandex.ru; tests pass a mock server
    );
    QString token() const;                // set when the dialog was accepted
};
```

Two ways in. By device code, the preferred one, shown first: the constructor starts
`Yandex::DeviceLogin`; on `codeReady` the code is shown large and copied to the clipboard, and a
button opens the verification URL; `succeeded` accepts the dialog with that token; `failed` shows
the reason and disables the button. By pasting: a `music.yandex.ru/#access_token=…` address or the
bare token, extracted with `Yandex::NormalizeToken`; nothing found shows a red message, otherwise
the dialog is accepted. Accepting cancels the device polling. The token is not stored here (the app
does that). Minimum width 460 px.

**Traps:**
- Constructing the dialog starts network traffic, before `exec()`.
- Qt's default size hint squeezes word-wrapped labels. `fitToContents()` computes the layout's
  height for the actual width (`totalHeightForWidth`) and raises the minimum height; call it after
  changing any label text.

## File formats

Nothing is parsed here: bitmaps, `region.txt` and `PLEDIT.TXT` arrive parsed in `Skins::Skin`
([src/skins](../skins/README.md)), `.milk` presets are read by `Vis::MilkdropPresets`
([src/vis](../vis/README.md)).

`EqualizerWindow` reads and writes Winamp `.eqf` files (layout in [src/audio](../audio/README.md)):

- Load: file dialog filter `*.eqf *.EQF *.q1`. A file above 1 MiB (1,048,576 bytes) is refused
  before it is read; a library of 1,000 presets is 268 KB. `Audio::ParseEqf` parses it; one preset
  is applied at once, several (a `.q1` library) are offered by name in an input dialog.
- Save: one preset, named by the user, default path `~/<name>.eqf`, written with `Audio::WriteEqf`.

## Errors

Nothing in `src/ui` throws. The only `catch` is `EqualizerWindow`'s around `Audio::ParseEqf`: an
`Audio::Error` becomes `statusText("EQ: <file>: " + what())`. Everything else is data:

| Failure | Reported as |
|---|---|
| `.eqf` cannot be opened, is above 1 MiB, cannot be saved | `EqualizerWindow::statusText`: `EQ: <file>: <reason>`, `EQ: <file> is <n> KB, and presets are 1024 KB at most`, `EQ: <file> не сохранён: <reason>` |
| Yandex request fails | a source's error goes out through `Core::Sources` as `Player::statusMessage`; a submenu's list shows a disabled `Error: …` item |
| Device login fails, pasted text holds no token | labels in `LoginDialog` |
| No usable OpenGL, projectM fails | `MilkdropWindow::failure()`, painted in the frame; `qWarning` |
| A preset fails or stays black | handled by switching (see `MilkdropWindow`); `qWarning` |

`Skins::Error` never reaches this module: skins are loaded by [src/app](../app/README.md), and the
windows only receive a `const Skins::Skin*`.

## Not here

- Loading skins, sprite coordinates (`skins/sprites.h`), `region.txt` and `PLEDIT.TXT` parsing, the
  TEXT.BMP and GEN.BMP fonts: [src/skins](../skins/README.md).
- Spectrum and oscilloscope drawing, `Vis::Analyzer`, `Vis::MilkdropView` (projectM, OpenGL, black
  detection timing), the preset list and its order: [src/vis](../vis/README.md).
- The queue, playback control, source tickets, covers: [src/core](../core/README.md).
- Yandex requests, the OAuth device flow, token parsing: [src/yandex](../yandex/README.md).
- The equalizer DSP, the `.eqf` layout, built-in presets, the audio engine:
  [src/audio](../audio/README.md).
- Creating and wiring the windows, the default layout, saving and restoring positions and sizes,
  scale changes with re-docking, always-on-top, the main and sources menus around the Yandex
  items, shortcuts and transport keys: [src/app](../app/README.md).
- MPRIS and SMTC: [src/integrations](../integrations/README.md).
- What the mouse and keys do, for users: [docs/](../../docs/) (Russian).
