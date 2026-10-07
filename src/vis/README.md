# `src/vis` — visualizations: Winamp's spectrum and oscilloscope, the FFT, Milkdrop through projectM

This folder turns the player's output PCM into pictures. It holds the `Visualizer` interface with
Winamp's two classic visualizations for the main window's 76×16 area (a 19-bar spectrum analyzer
and an oscilloscope), the `Analyzer` that computes their spectrum, and the Milkdrop parts:
`MilkdropView`, a `QOpenGLWindow` that runs projectM 4 on the audio engine's PCM, and
`MilkdropPresets`, the list of `.milk` files. It does not decide when to draw, which visualization
is on or which preset comes next, and it has no widgets: the main window's timer and vis area, and
the skinned Milkdrop window (`MilkdropWindow`) that owns the views and chooses presets, are in
[src/ui](../ui/README.md). The PCM it reads comes from [src/audio](../audio/README.md), the
colours from [src/skins](../skins/README.md).

| File | Contains |
|---|---|
| `visualizer.h` | `VisFrame`, the `Visualizer` interface, `MakeSpectrum`, `MakeOscilloscope`, `SpectrumBands`, `SpectrumLevels`, `Analyzer` |
| `visualizer.cpp` | `Analyzer` (Hann window + radix-2 FFT), the file-local `Spectrum` and `Oscilloscope` |
| `milkdrop_presets.h/.cpp` | `PresetTransition` (cut or blend); `MilkdropPresets` — built-in and user `.milk` files in a fixed order, read with a 1 MiB bound |
| `milkdrop_view.h/.cpp` | `MilkdropView` — projectM 4 in a `QOpenGLWindow`: OpenGL 3.3 probe, PCM feed, deferred preset loads, black-picture watch, frame capture. Built only with Milkdrop |

## Dependencies

Module order: `audio`, `yandex`, `skins` → **`vis`**, `core` → `ui`, `integrations` → `app`.

- `qiyaa_vis` links PUBLIC `Qt6::Gui`, PRIVATE `qiyaa_audio` and `qiyaa_skins`. Those two stay
  private because the headers include no other module's header and only forward-declare
  `Audio::AudioEngine` and `Skins::Skin`:
  `audio/audio_engine.h` is included by `milkdrop_view.cpp` alone, `skins/skin.h` by
  `visualizer.cpp` alone.
- With Milkdrop (CMake finds `Qt6::OpenGL` and projectM 4.1 or newer, or downloads projectM v4.1.7
  when `QIYAA_FETCH_PROJECTM=ON`): PUBLIC `Qt6::OpenGL`, PRIVATE `libprojectM::projectM` (LGPL-2.1,
  linked dynamically, see [THIRD_PARTY.md](../../THIRD_PARTY.md)), PUBLIC define
  `QIYAA_HAVE_MILKDROP`, and the resource prefix `:/milkdrop` with the 150 `.milk` files of
  `resources/milkdrop`. Only then are `milkdrop_view.*` compiled; `MilkdropPresets` is always built.
- It deliberately does not link `Qt6::Widgets` (the view is a `QWindow`; `src/ui` embeds it with
  `QWidget::createWindowContainer`), `Qt6::Network`, `qiyaa_core`, `qiyaa_yandex` or `qiyaa_ui`.
  projectM's header is private to one source file.

Checks, from the repository root:

```bash
grep -rnE --include='*.h' --include='*.cpp' '#include "(core|ui|integrations|app|yandex)/' src/vis/   # must print nothing
grep -n '#include "' src/vis/*.h | grep -v '"vis/'                                                  # must print nothing
grep -rl --include='*.h' --include='*.cpp' 'projectM-4/' src/vis/ | grep -v 'milkdrop_view.cpp$'      # must print nothing
grep -rnw --include='*.h' --include='*.cpp' throw src/vis/                                            # must print nothing
```

## `visualizer.h`: `VisFrame` and `Visualizer`

```cpp
struct VisFrame {
    std::span<const float> left, right;  // latest PCM, -1..1, same length
    std::span<const float> spectrum;     // magnitudes in dBFS, bins 0..fftSize/2
    int sampleRate = 44'100;
    int fftSize = 1024;
};

class Visualizer {
public:
    virtual ~Visualizer() = default;
    virtual QString name() const = 0;
    virtual void update(const VisFrame& frame) = 0;
    virtual void render(QPainter& painter, const QRect& area, const Skins::Skin& skin) const = 0;
    virtual void reset() { }
};

std::unique_ptr<Visualizer> MakeSpectrum();      // name() "Spectrum"
std::unique_ptr<Visualizer> MakeOscilloscope();  // name() "Oscilloscope"

inline constexpr int kSpectrumBars = 19;
struct SpectrumBand { double lowHz, highHz; int firstBin, endBin; };   // bins [firstBin, endBin)
std::array<SpectrumBand, kSpectrumBars> SpectrumBands(int sampleRate, int fftSize);
std::array<float, kSpectrumBars> SpectrumLevels(std::span<const float> spectrumDb, int sampleRate, int fftSize);
```

- `update` is called once per animation frame with fresh data. `render` is `const` and draws only
  the state the last `update` left, so it may run any number of times in between (every repaint).
  The base `reset` does nothing; both visualizations here clear their state in it, and draw nothing
  until the next `update`.
- The spans in `VisFrame` do not own; a visualizer copies what it needs during `update`. Spectrum
  bin `k` is at `k * sampleRate / fftSize` Hz.
- Each factory call returns a new instance with empty state. Both visualizations draw with
  `QPainter::fillRect` in rectangles one pixel high, in skin pixels, and paint no background: the
  caller paints the skin under them and clips to the area.
- GUI thread only; nothing is locked.
- `MilkdropView` does not implement `Visualizer`: it reads the audio engine itself.

Colours are indices into `Skins::Skin::visColors()` (the skin's `VISCOLOR.TXT`, 24 entries). An
index the skin does not have is drawn `Qt::green`.

| Index | Used for |
|---|---|
| 0, 1 | not used here (background, grid dots) |
| 2..17 | spectrum bars by row: `2 + rowFromTop * 16 / areaHeight`, so 2 at the top of the area, 17 at the bottom |
| 18..21 | oscilloscope by distance from the centre line: `18 + min(4, static_cast<int>(abs(row - 7.5)) / 2)` |
| 22 | never reached: the largest distance on 16 rows is 7 |
| 23 | spectrum peaks |

**Spectrum** (`MakeSpectrum`, Winamp's 19-bar analyzer with falling peaks):

- 19 bars on a log frequency scale from `lowestHz` = 60 Hz to `highestHz` = `min(16000, sampleRate /
  2)` Hz. Bar `bar` covers `60 * r^(bar/19)` to `60 * r^((bar+1)/19)` Hz with `r = highestHz / 60`,
  and shows the loudest bin in that range: at least one bin, never bin 0 (DC).
- −72 dBFS is an empty bar, −6 dBFS a full one, linear in dB between.
- `SpectrumBands` and `SpectrumLevels` are this computation for one frame, before fall-off and
  peaks: the bands and the 0..1 target of each bar. The spectrum's `update` uses them, and they
  produce `spec/dsp/spectrum.json`, the vectors the Android spectrum is checked against.
- A bar jumps up at once and falls by 0.07 of the height per `update` (Winamp's "fast" falloff):
  a full bar empties in 15 updates.
- A peak is pushed up by its bar. Otherwise it drops by `0.0004 * t²` of the height per `update`,
  `t` being the number of updates since it was last pushed (0 the first time): it holds for one
  update, then falls faster and faster.
- Bar `bar` is 3 px wide at `area.x() + 4 * bar` (the 19 bars span 75 px) and `ceil(level *
  areaHeight)` rows high, `areaHeight = area.height()`. The peak is one row at `areaHeight -
  ceil(peak * areaHeight)` in colour 23, drawn while above 0.

**Oscilloscope** (`MakeOscilloscope`, Winamp's "lines" style):

- Uses the last `shownSamples = min(sampleCount, 576)` samples of the frame (`sampleCount =
  left.size()`), one mono sample `(L + R) / 2` per column over 75 columns (`start + x * shownSamples
  / 75`, no averaging).
- Row `clamp(lround(7.5 - 8 * mono), 0, 15)`: +1 at the top (row 0), −1 at the bottom (row 15),
  silence on row 8.
- Each column fills the vertical run from the previous column's row to its own, so the trace is
  continuous. Nothing is drawn before the first `update` or after `reset`.

**Traps:**

- Both are made for the 76×16 area. The spectrum ignores `area.width()` (always 75 px); the
  oscilloscope ignores `area.height()` (always rows 0..15) and draws `min(75, area.width())`
  columns. The caller clips.
- Falloff is per `update`, not per second: calling `update` at another rate changes how fast bars
  and peaks fall.
- Neither checks its input. The oscilloscope indexes `left` and `right` up to `left.size() - 1`,
  so `left` must not be empty and `right` must be at least as long. The spectrum needs at least 2
  bins (with fewer, `std::clamp` gets `hi < lo`, which is undefined behaviour).

## `visualizer.h`: `Analyzer`

```cpp
class Analyzer {
public:
    explicit Analyzer(int fftSize = 1024);  // a power of two, >= 2 (not checked)
    int fftSize() const;
    const std::vector<float>& analyze(std::span<const float> mono);  // fftSize()/2 + 1 values, dBFS
};
```

- Hann window `0.5 - 0.5 * cos(2πi / (N - 1))`, then an in-place iterative radix-2 FFT.
  Magnitudes are scaled by `4 / N`, so a full-scale sine reads 0 dBFS. `analyzerFindsTheTone`
  (`tests/dsp_test.cpp`) pins it: 1 kHz at 44.1 kHz with N = 1024 peaks at bin 23, within 1.5 dB
  of 0 dBFS.
- Values are `20 * log10(max(magnitude, 1e-9))`: the floor is −180 dBFS.
- Reads the first `fftSize()` samples of `mono`; missing ones count as 0, extra ones are ignored.
- The constructor allocates every buffer. `analyze` allocates nothing and returns a reference to
  its own vector, valid until the next `analyze` or the end of the `Analyzer`.

**Traps:**

- `fftSize` is not checked: anything but a power of two gives a wrong spectrum without a word, and
  1 gives NaN (the window divides by `N - 1`).
- `analyze` writes the object's buffers: one `Analyzer` per thread.

## `milkdrop_presets.h/.cpp`: `MilkdropPresets`

```cpp
enum class PresetTransition { Cut, Blend };  // how a preset replaces the one on screen

class MilkdropPresets {
public:
    struct Preset {
        QString name;  // file name without ".milk"
        QString path;  // ":/milkdrop/..." or a file on disk
        bool builtIn = true;
    };

    void load(const QString& builtInDirectory, const QString& userDirectory);

    int size() const;
    bool isEmpty() const;
    const Preset& at(int index) const;
    int indexOf(const QString& name) const;  // -1 when absent
    QByteArray data(int index) const;        // empty when unreadable or over 1 MiB

    int next(int current) const;      // wraps around; -1 when empty
    int previous(int current) const;  // wraps around; -1 when empty
    int random(int current) const;    // a different one when there are several; -1 when empty
};
```

- `PresetTransition` is what `MilkdropView::loadPreset` takes and `switchRequested` carries: `Cut`
  replaces the picture at once, `Blend` fades over projectM's soft-cut time (3 s). It lives in this
  header, which is built with or without Milkdrop, and `milkdrop_view.h` includes it.
- `load` replaces the whole list: the built-in folder's presets first, then the user's. A folder
  contributes its readable, non-hidden files matching `*.milk`, top level only. Qt name filters
  ignore case unless `QDir::CaseSensitive` is given, so `X.MILK` counts too.
- Within a folder the order is by `name` ignoring case, ties broken by `path` (case-sensitive), so
  `B.milk` and `b.milk` keep one order on every file system (`namesDifferingInCaseKeepAFixedOrder`
  in `tests/milkdrop_test.cpp`). The file system's listing order never shows.
- A missing folder, or an empty string, contributes nothing: `""` is not taken as the current
  directory.
- `name` is `QFileInfo::completeBaseName()`: everything before the last dot.
- `data(index)` opens the file on every call (nothing is cached) and returns its bytes. It returns
  empty for an index out of range, a file that cannot be opened, or one larger than 1 MiB
  (1,048,576 bytes; exactly 1 MiB is still read, 1 MiB + 1 is refused by
  `oversizedPresetIsNotRead`). The size is checked before anything is read. The result is
  NUL-terminated because `QByteArray` always keeps a `\0` after its data, so `constData()` goes to
  projectM as a C string.
- `indexOf` compares exactly (case-sensitive) and returns the first match.
- `next(-1)` is 0: with no current preset, "next" is the first one.
- `random` draws from `QRandomGenerator::global()` until it gets an index other than `current`;
  it is 0 with one preset. Not reproducible: there is no seed.

**Traps:**

- A user preset with the same name as a built-in one is listed twice, and `indexOf` finds only
  the built-in one. Anything that remembers presets by name (the ui's settings) can't reach the
  user's copy.
- Indices change with every `load`; carry names across a reload, not indices.
- `at` does no range check of its own (`QList::at` asserts only in debug builds).
- An empty file and an unreadable one look the same: `data` returns empty for both.
- `previous(-1)` is `size() - 2`, not the last preset.
- projectM reads the data as a C string: a preset stops at its first `\0` byte.
- `data` reads up to 1 MiB on the calling thread, the GUI thread in practice.

## `milkdrop_view.h/.cpp`: `MilkdropView`

```cpp
class MilkdropView : public QOpenGLWindow {
public:
    explicit MilkdropView(Audio::AudioEngine* engine);  // not owned; not null; must outlive the view

    static QString OpenGlProblem();  // empty when OpenGL 3.3 core is available

    void loadPreset(const QByteArray& milk, PresetTransition transition);  // at the next frame
    void setPresetDuration(double seconds);                 // then switchRequested(); default 30
    void setLocked(bool locked);                            // projectM requests no switches
    void setTextureSearchPaths(const QStringList& paths);   // at the next frame
    void setRendering(bool on, int fps = 60);
    bool isRendering() const;

    bool isReady() const;         // projectM runs; false until the first paint
    QString failure() const;      // why it doesn't; empty otherwise
    qint64 framesRendered() const;
    QString glInfo() const;       // "<GL_VERSION> | <GL_RENDERER>", for logs and reports

    void setBlackWatch(bool on);
    void setBlackWatchTiming(int graceMs, int intervalMs, int checks);  // defaults 5000, 1000, 4
    void captureNextFrame();      // then frameCaptured(); for tests

Q_SIGNALS:
    void ready();
    void failed(const QString& reason);
    void switchRequested(Vis::PresetTransition transition);
    void presetFailed(const QString& message);
    void staysBlack();
    void drawsPicture();
    void frameCaptured(const QImage& frame);
    void doubleClicked();
    void contextMenuRequested(const QPoint& globalPosition);
    void keyPressed(int key, Qt::KeyboardModifiers modifiers);
};
```

The header forward-declares `struct projectm`, the type behind projectM's `projectm_handle`, so
projectM's headers stay inside `milkdrop_view.cpp`.

### Why a `QOpenGLWindow`, and framebuffer 0

projectM draws its final image into framebuffer 0: `projectm_opengl_render_frame` takes no target,
and projectM 4.1.7 has no other render call. A `QOpenGLWindow` built with `NoPartialUpdate` paints
straight into the window's own framebuffer, so here framebuffer 0 is the window. A `QOpenGLWidget`
renders into an offscreen FBO, and projectM's picture would never reach the screen. The view's own
GL code (alpha fix, black probe, capture) binds `defaultFramebufferObject()`, which in this mode is
that same framebuffer.

### OpenGL

- Surface format: OpenGL 3.3 core profile, no depth, stencil or alpha buffer, swap interval 1.
- `OpenGlProblem()` creates a context with that format (no surface), once per process, and caches
  the answer: empty, or one of the reasons under **Errors**. OpenGL ES is refused because projectM
  is built for desktop OpenGL.
- `initializeGL` (the first paint after the window is exposed) runs the same checks on the real
  context, then `projectm_create()`. On success it sends the window size in device pixels, turns
  aspect correction on, sets the soft-cut (blend) time to 3 s and projectM's fps value to 60,
  installs the two callbacks, applies duration, lock and texture paths, starts the black-watch
  clocks, logs `Milkdrop: OpenGL <glInfo>` with `qInfo`, and emits `ready()`. On failure it stops
  the timer, stores the reason and emits `failed(reason)`; from then on `paintGL` only clears the
  window to black.

### Frames

The timer (`Qt::PreciseTimer`, interval `1000 / max(1, fps)` ms in integer division: 60 fps is
16 ms) calls `update()`. Each `paintGL` with projectM running does, in this order:

1. sends the size in device pixels (`size() * devicePixelRatio()`) to projectM if it changed;
2. applies the texture search paths if they changed;
3. loads the pending preset, if any (the latest `loadPreset` wins), with
   `projectm_load_preset_data`, and restarts the black watch for it;
4. feeds the PCM that played since the last frame: `AudioEngine::readNewVisSamples`, at most 4096
   stereo frames, the newest ones, into `projectm_pcm_add_float`;
5. `projectm_opengl_render_frame`;
6. sets alpha to 1 everywhere (below);
7. counts the frame in `framesRendered()`;
8. runs the black-picture watch;
9. captures the frame if asked.

- `setRendering(true)` from off starts reading at the audio engine's current position: what played
  while the view was off is not replayed.
- `setPresetDuration` and `setLocked` pass plain values to projectM at once (no GL, so any context
  may be current); before `ready()` they are stored and applied by `initializeGL`.
- `loadPreset` and `setTextureSearchPaths` only record the request and call `update()`. Both are GL
  work (a preset creates GL objects; new texture paths replace projectM's texture manager), so they
  run in `paintGL` with this view's context current, and never from inside a projectM call.

### Opaque alpha

The format asks for no alpha buffer, but the system may give the window one anyway (an ARGB visual
on X11/XWayland, a translucent surface elsewhere). projectM leaves whatever alpha a preset
produced, often 0, and the compositor would show those pixels as see-through, i.e. black over the
picture. After every frame the view clears only the alpha channel to 1 (`glColorMask` with alpha
only, then `glClear`). `rendersWithProjectM` checks that a captured pixel has alpha 255.

### Black-picture watch

Some presets draw nothing on some GPUs or drivers. While the watch is on (`setBlackWatch(true)`),
the view samples the frame it has just drawn, before the swap:

- not before `graceMs` (5000) after the start, the last preset load, the last `setBlackWatch`
  change or the last report, because a new preset fades in and the blend alone takes 3 s; after
  that at most once per `intervalMs` (1000);
- a sample is a `glBlitFramebuffer` with `GL_LINEAR` from framebuffer 0 (the whole window) into a
  32×18 RGBA8 texture on the view's own FBO (created on first use, freed with projectM), then a
  `glReadPixels` of those 32×18 pixels (2,304 bytes). It is black when the brightest of R, G, B is
  below 12 (of 255) in every pixel; alpha is ignored;
- `checks` (4) black samples in a row emit `staysBlack()`, reset the count and restart the grace
  period: a preset that stays black is reported at most once per `graceMs + (checks - 1) *
  intervalMs` (8 s with the defaults), not on every sample;
- the first sample after a load that is not black emits `drawsPicture()`, once per load, and resets
  the count.

The view does not know whether music plays. Silence can legitimately fade a preset to black, so
the watch belongs on only while music plays; the caller decides that. `blackPictureIsNoticed` runs
it on real OpenGL with 300 / 150 / 3.

### Capture

`captureNextFrame()` reads the next frame back as drawn, before the swap, and delivers it with
`frameCaptured`: `QImage::Format_RGBA8888`, in device pixels, top row first (the view mirrors
OpenGL's bottom-up rows), alpha 255. `QOpenGLWindow::grabFramebuffer()` can't do this: it reads the
back buffer after the swap, whose content is undefined in `NoPartialUpdate` mode.

### Signals

| Signal | Fires when | Delivered |
|---|---|---|
| `ready()` | `initializeGL` started projectM | directly, inside `initializeGL`, context current |
| `failed(reason)` | `initializeGL` could not; the timer is stopped | directly, inside `initializeGL` |
| `switchRequested(transition)` | projectM wants the next preset: the time is up (`Blend`) or a beat-driven hard cut (`Cut`) | queued; projectM calls back from inside `projectm_opengl_render_frame` |
| `presetFailed(message)` | projectM could not load the data given to `loadPreset`; the previous preset stays | queued; from inside `projectm_load_preset_data` |
| `staysBlack()`, `drawsPicture()` | the black-picture watch, above | queued, from `paintGL` |
| `frameCaptured(frame)` | the frame after `captureNextFrame()` | queued, from `paintGL` |
| `doubleClicked()` | left-button double click | directly |
| `contextMenuRequested(globalPosition)` | right button pressed | directly |
| `keyPressed(key, modifiers)` | every key press; the view acts on none | directly |

Queued signals are posted with the view as the context object: if the view is deleted first, they
are dropped.

### Threads and lifetime

- GUI thread only. All GL work runs in `initializeGL`, `paintGL` and the destructor with the view's
  own context current; the destructor makes it current to free projectM (which owns GL objects)
  and the probe FBO and texture.
- One projectM instance per view, from `initializeGL` to the destructor. Several views may read one
  engine: each keeps its own read cursor (the ui runs a second view full screen).

**Traps:**

- Call `OpenGlProblem()` first and create no view when it returns a reason: `QOpenGLWindow` itself
  breaks without an OpenGL context (Qt's `offscreen` platform has none). It needs a
  `QGuiApplication`, loads the GPU driver, and keeps its answer for the life of the process.
- `ready()` and `failed()` arrive inside `initializeGL`: a slot must not delete the view
  (`deleteLater` is fine) or do GL work of its own.
- `isReady()` stays false until the window is shown and exposed. A `loadPreset` before then waits;
  after `failed()` it is never applied.
- `setRendering(false)` only stops the timer. `loadPreset`, `setTextureSearchPaths`,
  `captureNextFrame` and expose events still run `paintGL`, and every such frame feeds PCM and
  renders.
- projectM's fps value is always 60 (`projectm_set_fps`), whatever `fps` goes to `setRendering`;
  projectM hands that value to presets.
- `switchRequested` fires once per loaded preset. A receiver that ignores it gets no reminder until
  the next load, or until `setLocked` or `setPresetDuration` (both call
  `projectm_set_preset_locked`, which in projectM 4.1.7 re-arms the request when unlocked).
  projectM 4.1.7 also varies the time: it draws it from a normal distribution around the duration
  (σ = 1 s, its default "easter egg"), at least 1 s.
- Hard cuts stay at projectM's default, off in 4.1.7, so `transition` is `Blend` unless someone
  enables them.
- More than 4096 frames between two paints (about 93 ms at 44.1 kHz): the older ones are skipped,
  not queued. 4096 is also the size of the audio engine's tap.
- The probe samples, it does not average: a `GL_LINEAR` blit reads about 2×2 source pixels per
  probe pixel, so on a large window a thin bright line can go unseen and the frame counts as black.
- Don't turn on multisampling in the surface format: a scaled `glBlitFramebuffer` from a
  multisampled framebuffer is `GL_INVALID_OPERATION`, and the probe would read stale or undefined
  pixels.
- `setBlackWatchTiming` takes any values; `checks` of 1 or less reports on the first black sample.
- `setBlackWatch` does not reset "saw a picture": switching the watch off and on brings no second
  `drawsPicture()` for the same preset.
- Keep `NoPartialUpdate` (see "Why a `QOpenGLWindow`"): in a partial-update mode
  `defaultFramebufferObject()` is an FBO, and projectM would still draw into framebuffer 0.
- The size is re-sent in every `paintGL`, not only in `resizeGL`: moving to a screen with another
  scale factor changes the device-pixel size without a resize.
- After each frame the view leaves `GL_SCISSOR_TEST` disabled and the clear colour at (0, 0, 0, 1).
- `captureNextFrame()` before `ready()` or after `failed()` stays pending; `frameCaptured` comes
  only once projectM renders.
- `presetFailed` carries projectM's message only; the view drops projectM's file-name argument
  (the preset came as data).

## File format: `.milk`

The module reads Milkdrop presets as opaque text and leaves parsing to projectM. The line layout,
as in the bundled presets (ASCII, `\n` line ends):

```
MILKDROP_PRESET_VERSION=201                 optional header lines, key=value
PSVERSION=2
[preset00]                                  section header
fDecay=0.98                                 parameters, key=value, one per line
per_frame_init_1=...                        numbered code lines: per_frame_init_N, per_frame_N,
per_frame_1=wave_r = 0.5 + 0.5*sin(time);   per_pixel_N, warp_N and comp_N (shader text)
wavecode_0_enabled=1                        custom waves and shapes: wavecode_K_*, shapecode_K_*,
wave_0_per_point1=...                       wave_K_per_pointN, shape_K_per_frameN
```

`MilkdropPresets` does not list files that are not named `*.milk`, are unreadable or hidden, or sit
in a subfolder. It lists but does not read files over the 1 MiB limit (see `data` above). It does
not look at the content; what projectM refuses arrives as `presetFailed`. For scale: the largest
preset of the Cream of the Crop collection is about 60 KB, the largest of the 150 bundled ones
28,241 bytes.

## Errors

This module defines no exception type, throws nothing (the last check above) and catches nothing;
the project's error policy is in [docs/architecture.md](../../docs/architecture.md#errors). Every
failure is data:

| Where | Failure | The caller gets |
|---|---|---|
| `MilkdropPresets::load` | missing or unreadable folder | no entries from it |
| `MilkdropPresets::data` | bad index, file can't be opened, over 1 MiB | empty `QByteArray` |
| `indexOf`, `next`, `previous`, `random` | not found, empty list | −1 |
| `MilkdropView::OpenGlProblem`, `failure()`, `failed(reason)` | no usable OpenGL, or projectM didn't start | a reason for the user, in Russian (below) |
| `MilkdropView` | projectM can't load a preset | `presetFailed(message)` with projectM's text; the previous preset stays |
| `MilkdropView` | the picture stays black | `staysBlack()` |
| `Spectrum`, `Oscilloscope` | the skin has fewer colours than an index | that colour drawn `Qt::green` |

| Reason | When |
|---|---|
| `no OpenGL` | the probe context can't be created; or in `initializeGL` there is no context, it is invalid, or it isn't the current one |
| `only OpenGL ES is here, and OpenGL 3.3 is needed` | the context is OpenGL ES |
| `OpenGL 3.3 is needed, and X.Y is here` | the context is older than 3.3 |
| `projectM did not start` | `projectm_create()` returned null (`initializeGL` only) |

## Not here

- The skinned Milkdrop window: which preset comes next, shuffle and history, lock and switch
  interval, the remembered presets that stay black, full screen, keys, menu, the frame rate, and
  when the black watch is on — [src/ui](../ui/README.md) (`MilkdropWindow`).
- The main window's visualization area, its timer, which visualization is on, reading PCM and
  building `VisFrame` — [src/ui](../ui/README.md) (`MainWindow`).
- The PCM tap (`readVisSamples`, `readNewVisSamples`, `visCursor`) and its thread rules —
  [src/audio](../audio/README.md).
- Parsing `VISCOLOR.TXT` and the fallback to the base skin's colours —
  [src/skins](../skins/README.md).
- Where the user's preset folder is, and the saved Milkdrop settings —
  [src/app](../app/README.md).
- Where the 150 bundled presets come from, and their license —
  [resources/milkdrop](../../resources/milkdrop/README.md).
- The build switches `QIYAA_WITH_MILKDROP`, `QIYAA_FETCH_PROJECTM`, `QIYAA_REQUIRE_MILKDROP` — the
  top-level `CMakeLists.txt`.
