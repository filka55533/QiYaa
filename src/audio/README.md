# `src/audio` — playback engine, equalizer and Winamp EQ presets

Plays one audio file while it is still downloading: the bytes the caller appends go into an
in-memory `StreamBuffer`, a decoder thread turns them into float stereo PCM at the output device's
rate, a 2-second ring buffer carries that PCM to the miniaudio device callback, and the callback
applies the equalizer, feeds the visualization tap and applies volume and balance. A second stream
can be queued; it follows the current one in the same ring without a gap. The module also holds the
10-band equalizer DSP, Winamp's built-in presets and the `.eqf` reader and writer. It does not
download anything, decide what plays next or when to queue it, or poll itself: that is `Player` in
[src/core](../core/README.md). It draws no visualization ([src/vis](../vis/README.md)) and shows no
equalizer window ([src/ui](../ui/README.md)). Namespace `Audio`, library `qiyaa_audio`.

| File | Contains |
|---|---|
| `audio_engine.h/.cpp` | `AudioEngine` — streams, state, seeking, gapless chaining, volume, balance, `poll()`. Only in the `.cpp`: `StreamBuffer` (one downloading file) and `AudioEngine::Implementation` (miniaudio context and device, ring buffer, decoder thread, device callback) |
| `equalizer.h/.cpp` | `EqSettings`, `EqualizerDsp` — preamp and 10 peaking biquads per channel, coefficients handed to the audio thread through a triple buffer; `kEqBands`, `kEqBandHz`, `kEqMaxDb` |
| `eq_presets.h/.cpp` | `EqPreset`; `ParseEqf`, `WriteEqf` (`.eqf` and `.q1`); `EqfToDb`, `DbToEqf`; `BuiltinEqPresets` — Winamp's 17 presets |
| `vis_tap.h/.cpp` | `VisTap` — the last 4096 output frames, for visualizations |
| `error.h` | `Audio::Error` |
| `miniaudio_impl.c` | `#define MINIAUDIO_IMPLEMENTATION` + `#include "miniaudio.h"`: the one translation unit that compiles miniaudio. It is the source of `qiyaa_miniaudio`, not of `qiyaa_audio` |

## Dependencies

- `qiyaa_audio` links PUBLIC `Qt6::Core` (`QObject`, `QString`, `QStringList`, `QByteArray`,
  `QHash`, `QList`, `QStringConverter`, `QStringDecoder`) and PRIVATE `qiyaa_miniaudio`.
- `qiyaa_miniaudio` is a separate static library built from `src/audio/miniaudio_impl.c` against
  `contrib/miniaudio/miniaudio.h` (miniaudio 0.11.25) with `MA_NO_ENCODING MA_NO_GENERATION
  MA_NO_VORBIS`, so the decoders compiled in are WAV, FLAC and MP3. It links `Threads::Threads` and
  `${CMAKE_DL_LIBS}`; on Apple it defines `MA_NO_RUNTIME_LINKING` and links CoreFoundation,
  CoreAudio and AudioToolbox, on other Unix systems `m`. It does not get the project's warning
  flags (`qiyaa_target_defaults` is not applied to it).
- It deliberately links no other project module and neither `Qt6::Gui`, `Qt6::Widgets` nor
  `Qt6::Network`. It sits at the bottom of the module order; which modules link it is in
  [docs/architecture.md](../../docs/architecture.md#modules).
- miniaudio stays private: no header includes `miniaudio.h`. `AudioEngine` holds everything
  miniaudio-typed through `std::unique_ptr<Implementation>` and a forward-declared `StreamBuffer`.
- Inside the module: `audio_engine.h` and `eq_presets.h` include `equalizer.h`,
  `audio_engine.cpp` includes `equalizer.h` and `vis_tap.h`, `eq_presets.cpp` includes `error.h`.
  `equalizer.h`, `vis_tap.h` and `error.h` include no file of the module.

```bash
grep -rnE '#include "(app|core|integrations|skins|ui|vis|yandex)/' src/audio/ --include='*.h' --include='*.cpp'   # must print nothing
grep -rlE '#include [<"]miniaudio' src/ --include='*.h' --include='*.cpp' --include='*.c' \
    | grep -vxE 'src/audio/(audio_engine\.cpp|miniaudio_impl\.c)'                                  # must print nothing
grep -rnE '#include <Q' src/audio/ --include='*.h' --include='*.cpp' \
    | grep -vE '<Q(ByteArray|Hash|List|Object|String|StringConverter|StringDecoder|StringList)>'                              # must print nothing
grep -rnw 'throw' src/audio/ --include='*.h' --include='*.cpp' | grep -v '^src/audio/eq_presets.cpp:'   # must print nothing
```

The tests are `tests/audio_test.cpp` (the real engine on `tests/data/sine440_3s.mp3`) and
`tests/dsp_test.cpp` (equalizer response, presets, `.eqf`). CTest runs every test with
`QIYAA_AUDIO_BACKEND=null`.

## `audio_engine.h` — `AudioEngine`

```cpp
class AudioEngine : public QObject {
public:
    enum class State { Stopped, Buffering, Playing, Paused };

    struct InitResult {
        bool ok = false;
        QString message;
    };
    InitResult init();                 // opens the output device; after a success: {true, {}}
    QString backendName() const;       // miniaudio's name ("PulseAudio", "Null"...), "none" before init

    using TStreamId = quint64;         // 0 = none

    TStreamId beginStream(double startSeconds = 0);   // drops current and queued; 0 without a device
    TStreamId queueStream();           // 0 unless a decoder thread exists
    void clearQueued();
    TStreamId queuedStream() const;    // 0 when none, or the decoder could not open it
    TStreamId playQueuedNow();         // 0 when queuedStream() is 0

    void appendData(TStreamId stream, const QByteArray& bytes);   // unknown id: no-op
    void finishData(TStreamId stream);
    void failData(TStreamId stream);
    void appendData(const QByteArray& bytes);                     // the current stream
    void finishData();
    void failData();
    TStreamId currentStream() const;

    void pause();
    void resume();
    void stop();
    bool seek(double seconds);         // false until the first stream is open

    State state() const;
    double positionSeconds() const;
    int sourceSampleRate() const;      // the file's own format; 0 until the first stream is open
    int sourceChannels() const;

    void setVolume(int percent);       // clamped to 0..100, default 75
    void setBalance(int balance);      // clamped to -100 (left) .. 100 (right)
    void setEqualizer(const EqSettings& settings);

    void readVisSamples(std::span<float> left, std::span<float> right) const;   // <= 4096 frames
    VisReadResult readNewVisSamples(uint32_t cursor, std::span<float> stereo) const;
    uint32_t visCursor() const;
    int outputSampleRate() const;      // the device's rate; 44100 before init()

    void poll();                       // call from a UI timer

Q_SIGNALS:
    void stateChanged(Audio::AudioEngine::State state);
    void trackFinished();
    void trackAdvanced();
    void errorOccurred(const QString& message);
    void streamUndecodable(Audio::AudioEngine::TStreamId stream);
};
```

### Pipeline

```
appendData() ─> StreamBuffer ─(decoder thread: ma_decoder)─> ma_pcm_rb ─> device callback ─> device
                                                            2 s, f32      EQ → VisTap → gains, clip
```

Every frame count in the engine is in device-rate frames: each `ma_decoder` converts its source to
32-bit float, 2 channels, at the device's sample rate. `sourceSampleRate()` and `sourceChannels()`
report the file's own format, for display.

### Streams

- `beginStream()` stops the decoder thread, drops the current and the queued stream, creates the
  next id (ids start at 1 and are never reused by one engine), starts a decoder thread and the
  device, and sets `Buffering`. Without a ring (`init()` not called or failed) it sets `Stopped`,
  emits `errorOccurred("no audio output device")` and returns 0. With `startSeconds > 0` the
  decoder seeks there before any audio reaches the ring, and `positionSeconds()` shows that
  position from the start: how the `Player` resumes a track whose download broke off.
- `decoderStarted()` tells whether the current stream's decoder has opened it.
- `appendData()`, `finishData()`, `failData()` do nothing for an id that is neither current nor
  queued, so a late network callback cannot feed the wrong track. `failData()` ends a stream the
  way `finishData()` does: the engine plays up to the last byte received and reports
  `trackFinished` as usual. Telling the user is the caller's job.
- `queueStream()` replaces the queued stream (it calls `clearQueued()` first) and returns 0 unless
  a decoder thread exists, i.e. between `beginStream()`/`playQueuedNow()` and `stop()`.
- `playQueuedNow()` stops the decoder thread, drops the current stream, rewinds the queued one to
  byte 0 and starts it as the current stream (`Buffering`, no `trackAdvanced`).
- `stop()` drops every stream, stops the device (no callbacks run while stopped), resets the
  position to 0 and sets `Stopped`.
- Streams are held whole in memory: the current and the queued file. After a gapless advance the
  decoder thread releases the previous file on its next pass.

### State and signals

| State | Entered by |
|---|---|
| `Stopped` | construction, `stop()`, `poll()` after a decode failure, `beginStream()` without a device |
| `Buffering` | `beginStream()`, `playQueuedNow()`, `resume()` while the first stream is not open yet |
| `Playing` | `poll()` once the first stream is open and the ring holds a frame; `resume()` otherwise |
| `Paused` | `pause()` from `Playing` or `Buffering`; it stops the device, `resume()` starts it again |

All signals are emitted synchronously on the UI thread:

| Signal | From | When |
|---|---|---|
| `stateChanged` | `beginStream`, `playQueuedNow`, `pause`, `resume`, `stop`, `poll` | the state actually changes |
| `trackFinished` | `poll` | `Playing`, the decoder is done, the ring is empty, no seek is pending, nothing is chained; or playback reached the boundary of a cancelled chain. Once per decoder run |
| `trackAdvanced` | `poll` | playback crossed into the chained stream: `currentStream()` is now the former queued id, `queuedStream()` is 0, the position restarts at 0. No `stateChanged` |
| `errorOccurred` | `beginStream`, `poll` | no device; the first stream cannot be decoded (after `stop()`, so `stateChanged(Stopped)` comes first) |
| `streamUndecodable(stream)` | `poll` | right after that `errorOccurred`, with the id of the stream that could not be decoded |

`poll()` emits at most one of `trackFinished`, `trackAdvanced`, `errorOccurred` (with its
`streamUndecodable`) and returns right
after it, so a connected slot may call back into the engine (`Player` starts the next track from
its `trackFinished` slot). Nothing is reported between polls; how often `Player` polls is in
[src/core](../core/README.md).

### Position, volume, balance

`positionSeconds()` converts max(0, `frameOffset` + `framesPlayed`) with `static_cast<double>`
before dividing by the device rate. `framesPlayed` counts the frames the callback took out of
the ring since the last ring reset (start or seek);
`frameOffset` is the track position of `framesPlayed == 0`: 0 at the start, the target after a
seek, −`boundaryFrame` after a gapless advance. The position stands still during a pause, an
underrun and a pending seek.

Gain = (volume / 100)², so the default 75 gives 0.5625. With b = balance / 100, the left channel is
multiplied by 1 − b when b > 0 and the right by 1 + b when b < 0. After the gains every sample is
clamped to [−1, 1]. The equalizer and the tap come before the gains, so the tap does not follow the
volume.

### Threads

| Thread | Runs |
|---|---|
| UI thread: the thread that owns the engine (the Qt main thread in the app) | every public method and signal, `startDecoder()`, `stopDecoderThread()`, `ma_device_start()`/`ma_device_stop()`, `EqualizerDsp::publish()`, `VisTap::read()`/`readNew()` |
| Decoder thread: a `std::thread` on `Implementation::decoderMain`, one per `beginStream()`/`playQueuedNow()`, joined by the next of those, by `stop()` and by the destructor | `ma_decoder_*` on the current source, `StreamBuffer::read()`/`seek()` through `OnRead`/`OnSeek`, every write into the ring, the ring reset of a seek, gapless chaining |
| Device thread: miniaudio's, running `Implementation::DataCallback` | every read from the ring, `EqualizerDsp::process()`, `VisTap::write()`, the gains |

`AudioEngine`'s own members (`streams`, `lastId`, `currentId`, `queuedId`, `currentState`,
`volumePercent`, `balancePercent`) belong to the UI thread; `sourceRate` and `sourceChannelCount`
are atomics because the decoder thread sets them when the first stream opens. The callback takes no
mutex and allocates nothing (`DataCallback`, `EqualizerDsp::process`, `VisTap::write`); it relies
on its atomics being lock-free, which nothing asserts.

**The ring** (`ma_pcm_rb`, 2 s × device rate frames of f32 stereo, allocated in `init()`) is
single-producer, single-consumer: the decoder thread writes, the callback reads, and
`ma_pcm_rb_reset()` is safe only while neither of them is inside it. It is reset in two places:

1. `startDecoder()` on the UI thread. The old decoder thread has been joined and `outputEnabled` is
   false, but a callback that loaded `outputEnabled` before it turned false may still be reading.
   So the device is stopped around the reset (`ma_device_stop()` waits for the backend's worker
   thread before returning, per miniaudio's documentation) and started again before the new
   decoder thread exists and `outputEnabled` turns true.
2. The decoder thread, while it serves a seek. The callback leaves the ring alone while
   `seekRequest >= 0`, but it checks that once, at its start. So `seek()` stops the device before
   it stores the request and starts it after: a callback that began earlier has returned, and every
   later one sees the request and writes silence. The decoder thread resets the ring, sets
   `frameOffset`, `framesPlayed` and `written`, and hands the ring back with
   `compare_exchange_strong(target, -1)`. If a newer seek replaced the target meanwhile, the
   exchange fails and the loop serves the newer seek, still owning the ring.

**Atomics in `Implementation`:**

| Field | Written by | Read by | Meaning |
|---|---|---|---|
| `outputEnabled` | UI | callback | false: the callback writes silence and leaves the ring alone |
| `gainLeft`, `gainRight` | UI | callback | linear gain per channel, volume and balance included |
| `framesPlayed` | callback (+frames taken); decoder (0 at a seek); UI (0 in `startDecoder()`, `stop()`) | UI, callback | frames played since the last ring reset |
| `frameOffset` | decoder (seek target); UI (0; −`boundaryFrame` at an advance) | UI | track position of `framesPlayed == 0` |
| `seekRequest` | UI (target frame); decoder (back to −1) | decoder, callback, UI | ≥ 0: a seek is pending and the ring belongs to the decoder |
| `seekEpoch` | UI | decoder | `uiEpoch` when the seek was requested |
| `decoderStarted` | decoder | UI | the first stream is open |
| `decoderDone` | decoder | UI, decoder | the current source ended and nothing was chained |
| `decoderFailed` | decoder | UI (exchanged in `poll()`) | the first stream cannot be decoded |
| `finishedReported` | UI (true under `queueMutex`; false in `startDecoder()`) | decoder, UI | `trackFinished` was emitted in this run |
| `decoderEpoch` | decoder (under `queueMutex`) | UI, callback | the track being written into the ring |
| `uiEpoch` | UI (at an advance) | decoder, callback | the track `poll()` has announced |
| `boundaryFrame` | decoder (at a chain) | UI, callback | `framesPlayed` value where `decoderEpoch` starts |
| `haltAtBoundary` | UI (`clearQueued()`, `stopDecoderThread()`), decoder (undo); always under `queueMutex` | callback, UI, decoder | the chained stream was cancelled: stop at the boundary |
| `nextRate`, `nextChannels` | decoder (at a chain) | UI (at the advance) | source format of the chained stream |
| `failedQueuedId` | decoder; UI (0 in `startDecoder()`) | UI (exchanged in `poll()`, compared in `queuedStream()`) | id of a queued stream the decoder could not open |
| `stopDecoder` | UI | decoder | leave the loop |

`boundaryFrame` is stored (release) before `decoderEpoch` (release), so whoever loads the new epoch
(acquire) also sees its boundary. `startDecoder()` resets `framesPlayed`, `frameOffset`, the three
`decoder*` flags, `finishedReported`, `seekRequest`, both epochs and `failedQueuedId` before it
spawns the thread. The previous thread has been joined and the callback reads none of them while
`outputEnabled` is false, so those writes race with nothing.

**`queueMutex`** guards the plain fields of the queued-stream handoff:

| Field | Meaning | Written by |
|---|---|---|
| `queued`, `queuedId` | the queued stream, not yet taken by the decoder | UI (`queueStream()`, `clearQueued()`); decoder (takes it in `tryChain`; puts it back after an undo or a lost race with `trackFinished`) |
| `chainingId`, `dropChaining` | the stream `tryChain` is opening outside the lock; `dropChaining` asks it to discard the result | decoder; UI (`clearQueued()` sets `dropChaining`) |
| `chainedId` | chained, the UI has not passed the boundary | decoder; UI (0 at the advance) |
| `decoderHeld` | every buffer the decoder thread may read: current, tail, the one being opened | decoder (`publishHeld`, `tryChain`); UI (copied, then cleared, in `stopDecoderThread()`) |

The mutex also makes two check-then-act sequences atomic. `tryChain` checks `finishedReported`
before it takes the queued stream and again before it commits the chain; `poll()` checks
`decoderEpoch > uiEpoch` before it sets `finishedReported`. Either the chain commits first (then
`poll()` does not report the end, and the advance comes at a later poll) or the end is reported
first (then `tryChain` does not commit and puts the stream back into `queued`, rewound, for
`playQueuedNow()`). And `clearQueued()` finds the queued stream in exactly one of its three places.

Lock order: `queueMutex`, then a `StreamBuffer`'s mutex (`finished()` and the hand-back
`rewind()` in `tryChain`, `interrupt()` in `clearQueued()`, `rewind()` in the undo).
`StreamBuffer` never takes `queueMutex`. The decoder thread never holds `queueMutex` across a
read, a decode or a seek: `tryChain` opens the new source between two lock scopes.

**Stopping the decoder thread.** `stopDecoderThread()` runs on the UI thread from `beginStream()`
and `stop()` (through `dropStreams()`), from `playQueuedNow()` and from the destructor, each time
after `outputEnabled = false`:

1. `stopDecoder = true`; copy `decoderHeld` under `queueMutex`.
2. `interrupt()` every buffer in `streams` and in the copy. `streams` too, because the first stream
   enters `decoderHeld` only after it opened, and a thread still opening it is blocked in its read.
   `decoderHeld` too, because it can hold buffers the UI already dropped (a cancelled chained
   stream, the tail).
3. Join. Interrupted reads return at once and the thread's sleeps are 10 or 20 ms, but a decode or
   seek already running inside miniaudio finishes first. `audio_test`
   (`clearingAChainedStreamThenRestartingDoesNotHang`, `stopReturnsPromptlyWhileQueuedDataIsMissing`) requires
   `stop()` and `beginStream()` to return within 1000 ms.
4. `stopDecoder = false`; `resume()` the same buffers; under `queueMutex` clear `queued`,
   `queuedId`, `chainingId`, `chainedId`, `dropChaining`, `haltAtBoundary`, `decoderHeld`.

### Decoder thread

`openSource()` creates an `ma_decoder` (f32, 2 channels, device rate) over a `StreamBuffer`. It
first forces the MP3 decoder; if that fails and the thread is not stopping, it seeks the buffer
back to byte 0 and tries again with miniaudio's format detection (WAV, FLAC, MP3). The decoder
lives in a heap-allocated `Source` because an `ma_decoder` must not move. If the first stream
cannot be opened, the thread sets `decoderFailed` (unless it is stopping) and returns; `poll()`
then stops the engine and reports the error.

Each pass of the loop, in this order:

1. A pending seek (see [Seeking](#seeking)).
2. If the UI has passed the boundary (`uiEpoch >= epoch`), drop the tail.
3. If `decoderDone`: try to chain (no tail, not `finishedReported`), otherwise sleep 20 ms.
4. If the ring has fewer than 1024 free frames, sleep 10 ms.
5. Decode up to 1024 frames and write them into the ring. At the end of the source
   (`MA_AT_END`, or an error with no frames, which includes `MA_CANCELLED` from an interrupted
   buffer): try to chain if there is no tail; set `decoderDone` when there is a tail or nothing
   was chained.

The thread decodes ahead of playback by at most the ring, 2 s. After the end it keeps looping
(20 ms sleeps) until the UI stops it.

### Gapless chaining

Each decoder run numbers its tracks from 0. `decoderEpoch` is the track being written into the
ring, `uiEpoch` the one `poll()` has announced. The decoder chains only when it holds no tail (the
previous track's source, kept until the UI passes the boundary), so the two differ by at most 1.

**Chain** (`tryChain`, decoder thread). Happens when the current source ends and all of these hold:
there is no tail, a stream is waiting in `Implementation::queued`, that stream is `finished()`, and
`finishedReported` is false. `finished()` means the download ended, successfully or not. Only a
complete stream is chained, so opening and decoding it never wait for the network (an ID3 tag with a
large cover alone can exceed any "enough to start" threshold), and a stream that is not complete in
time is started by the caller the usual way. Steps: under `queueMutex` take the stream out of
`queued`, set `chainingId`, add it to `decoderHeld`; open it outside the lock; under `queueMutex`
again either discard it or commit. It is discarded if the open failed, if `clearQueued()` set
`dropChaining`, if the thread is stopping, or if `poll()` reported the end meanwhile. Unless it was
cancelled or the thread is stopping, a failed open sets `failedQueuedId = id`, and a stream that
opened but lost the race with `trackFinished` is rewound and put back into `queued` for
`playQueuedNow()`. Commit: `chainedId = id`, the old source becomes the tail, `epoch + 1`,
`nextRate`/`nextChannels`, `boundaryFrame = written`, `decoderEpoch = epoch`. The new source's
frames follow the old ones in the same ring. When the queued stream completes after the current
source ended, the decoder, now `decoderDone`, retries every 20 ms until `finishedReported`
(`queuedStreamArrivingLateStillChains`).

**Advance** (`poll()`, UI thread). When `decoderEpoch > uiEpoch`, no seek is pending, the end was
not reported and `framesPlayed >= boundaryFrame`: `chainedId = 0` (under `queueMutex`),
`frameOffset = −boundaryFrame`, `uiEpoch = decoderEpoch`, the old current stream is dropped,
`currentId` becomes the queued id, the source format comes from `nextRate`/`nextChannels`, and
`trackAdvanced` is emitted. The decoder drops the tail on its next pass. The audio has no gap
because the first frame of the new track follows the last frame of the old one in the ring; the
signal comes up to one poll interval later.

**Undo.** A seek requested before the advance targets the old track (`seekEpoch < epoch`). The
decoder then, under `queueMutex`, rewinds the chained stream and puts it back into `queued` (unless
it was cancelled), clears `chainedId` and `haltAtBoundary`, makes the tail current again and
decrements the epoch. The queued stream chains again later from its beginning
(`seekingBackUndoesTheChain`).

**Cancel** (`clearQueued()`, UI thread). The id is removed from `streams` at once, and under
`queueMutex`:
- still waiting in `Implementation::queued`: it is removed, nothing else happens;
- being opened (`chainingId`): `dropChaining = true` and its buffer is interrupted, so `tryChain`
  discards it;
- chained (`chainedId`): `haltAtBoundary = true` and its buffer is interrupted, which ends its
  decoding. The callback then plays only up to `boundaryFrame` and writes silence after it, and
  `poll()` reports `trackFinished` instead of `trackAdvanced`. The chained frames stay in the ring
  until the next reset.

**Undecodable queued stream.** `failedQueuedId` makes `queuedStream()` return 0 at once; `poll()`
then drops the stream. The current track ends with `trackFinished`
(`undecodableQueuedStreamIsSkipped`).

### Seeking

`seek(seconds)` returns false when there is no decoder thread, when the first stream is not open
yet, or when `seconds < 0`. Otherwise it stops the device if it runs, stores
`seekEpoch = uiEpoch` and `seekRequest = int64(seconds × device rate)`, starts the device again and
returns true without waiting. The target is not clamped to the track or to the downloaded part.

The decoder thread serves the request at the top of its loop: it undoes a chain the UI has not
passed or drops a tail it has passed, calls `ma_decoder_seek_to_pcm_frame()`, which can block
until the download reaches that part, then resets the ring and releases it (see the ring rules
above). If the thread is stopped during the seek, it leaves without touching the ring. Until the
request is cleared the callback writes silence, the position shows the old value and the state
does not change.

### `StreamBuffer` (in `audio_engine.cpp`)

```cpp
class StreamBuffer {
public:
    void append(const char* data, size_t byteCount);
    void finish(bool failed);
    void interrupt();                                        // until resume()
    void resume();
    void rewind();                                           // read cursor back to byte 0
    size_t size() const;
    bool finished() const;                                   // the download ended, either way
    ma_result read(void* destination, size_t bytesToRead, size_t* bytesRead);        // blocks
    ma_result seek(ma_int64 offset, ma_seek_origin origin);  // blocks
};
```

- The whole file in one growing `std::vector<char>`, with one mutex and one condition variable.
  No size limit.
- `read()` waits until the buffer is interrupted, finished, or has bytes past the cursor. It
  returns `MA_CANCELLED` when interrupted, `MA_AT_END` at the end of a finished stream, and
  otherwise copies what is there, up to `bytesToRead` bytes (a short read does not wait for more).
- `seek()` refuses a negative target and a seek from the end before the download finished
  (`MA_BAD_SEEK`: the size is unknown until then, and waiting would stall streaming). A target
  past the downloaded bytes waits until the download reaches it (`MA_SUCCESS`), finishes short of
  it (`MA_BAD_SEEK`) or the buffer is interrupted (`MA_CANCELLED`).
- `interrupt()` is sticky until `resume()`: a read or seek that starts later returns
  `MA_CANCELLED` at once.
- `finish(true)` stores `endedWithError`, which nothing reads.
- Threads: `append`, `finish`, `interrupt`, `resume` and `size` from the UI thread; `read`, `seek`
  and `finished` from the decoder thread; `rewind` from the decoder thread under `queueMutex`, or
  from the UI thread in `playQueuedNow()` after the join.

### Output device and `QIYAA_AUDIO_BACKEND`

`init()` opens a playback device: f32, 2 channels, `sampleRate = 0` (the device's native rate),
miniaudio's conservative performance profile (default period 100 ms instead of 10 ms,
`MA_DEFAULT_PERIOD_SIZE_IN_MILLISECONDS_CONSERVATIVE`; the backend may choose another size). It
tries the candidate backends one by one, each with its own `ma_context` holding only that backend,
and keeps the first whose device opens:

| Platform | Candidates, in order |
|---|---|
| Linux, FreeBSD | PulseAudio (PipeWire answers through its PulseAudio server), ALSA, Null. No JACK: it writes errors to stderr when no JACK server runs |
| Windows | WASAPI, DirectSound, WinMM, Null |
| anything else (macOS, other BSDs) | Core Audio, Null |

`QIYAA_AUDIO_BACKEND` replaces the list with one backend. Its value is compared with
`ma_get_backend_name()` of every miniaudio backend, case-insensitively and with spaces removed on
both sides: `wasapi`, `directsound`, `winmm`, `coreaudio`, `sndio`, `audio(4)`, `oss`,
`pulseaudio`, `alsa`, `jack`, `aaudio`, `opensl|es`, `webaudio`, `custom`, `null`. `null` plays
nothing but runs in real time, so positions advance; the tests use it. An unknown name logs
`QIYAA_AUDIO_BACKEND=<value>: no such audio backend, using the default ones` (`qWarning`) and keeps
the default list. A known name whose backend does not open fails `init()`; there is no Null
fallback then.

After a device opens, `init()` sets the equalizer's sample rate and allocates the ring. It does not
start the device: `beginStream()` and `playQueuedNow()` do. Failures are returned in
`InitResult::message`: `no audio output device opens (tried PulseAudio, ALSA, Null)` or
`cannot allocate a 2-second ring buffer at 48000 Hz` (with the real names and rate). The destructor
stops the decoder thread, then uninitialises the device, the context and the ring, in that order.

**Traps:**
- Nothing happens without `poll()`: no `trackFinished`, no `trackAdvanced`, no decode error, and
  `Buffering` never becomes `Playing`.
- `trackFinished` does not stop anything: the state stays `Playing`, the device keeps running
  (silence) and the decoder thread keeps looping until the caller calls `beginStream()`,
  `playQueuedNow()` or `stop()`.
- `trackFinished` fires once per decoder run: `finishedReported` is reset only by `startDecoder()`.
  A seek after it plays the track again but never reports its end, and nothing chains any more.
- `Buffering` covers only the start of a stream. An underrun later, or a seek waiting for the
  download, is silence with the state `Playing` and a frozen position.
- A stream is chained only when completely downloaded, and `finished()` is also true after
  `failData()`: a failed queued download is chained and plays up to its last byte.
- Between the boundary and the next `poll()`, `positionSeconds()` runs past the old track's
  length and `currentStream()` is still the old id.
- `clearQueued()` on a chained stream ends the current track at the boundary with
  `trackFinished`; it never produces `trackAdvanced`, and a stream queued after it does not chain
  in this run (the halted boundary is never passed) unless a seek back undoes the chain.
- A seek is served only at the top of the decoder loop: while the decoder waits in a read for the
  download (it decodes up to 2 s ahead), a seek waits too.
- The result of `ma_decoder_seek_to_pcm_frame()` is ignored: when it fails, the ring is reset and
  the position set to the target anyway, and decoding continues wherever the decoder was left.
- `seek()`, `pause()`, `resume()`, `stop()`, `beginStream()` and `playQueuedNow()` stop or start
  the device on the UI thread; miniaudio documents that stopping can wait for the current period on
  some backends.
- `frameOffset` and `framesPlayed` are two atomics: a `positionSeconds()` call while the decoder
  thread serves a seek can combine the new offset with the old count once.
- An undecodable current stream can be reported only when its download finishes: the MP3 attempt
  reads on looking for a frame, and reads wait for the download.
- The default lists end with Null: when no real backend opens, `init()` succeeds without sound.
  Check `backendName()`.
- The atomics the callback loads include `std::atomic<ma_uint64>`, `<ma_int64>` and `<float>`;
  on a 32-bit target they may not be lock-free, and nothing checks.

## `equalizer.h` — `EqualizerDsp`

```cpp
inline constexpr int kEqBands = 10;
inline constexpr std::array<double, kEqBands> kEqBandHz = {60,   170,  310,   600,   1000,
                                                           3000, 6000, 12'000, 14'000, 16'000};
inline constexpr double kEqMaxDb = 12.0;

struct EqSettings {
    bool enabled = true;
    double preampDb = 0.0;                   // clamped to ±kEqMaxDb when coefficients are computed
    std::array<double, kEqBands> bandsDb{};  // same

    bool operator==(const EqSettings&) const = default;
};

class EqualizerDsp {
public:
    EqualizerDsp();                                   // 44100 Hz, flat
    void setSampleRate(uint32_t rate);                // 0 means 44100; only while process() cannot run
    void publish(const EqSettings& settings);         // UI thread
    void process(std::span<float> stereoFrames); // audio thread: interleaved stereo, in place
    static double ResponseDb(const EqSettings& settings, double hz, double sampleRate);
};
```

- The filter: preamp gain 10^(dB/20), then 10 peaking biquads per channel from the RBJ audio EQ
  cookbook, Q = 1.2 for every band (roughly an octave wide, so neighbouring bands overlap), run in
  transposed direct form II on `float`. A band closer than 0.05 dB to 0, or at or above 0.49 × the
  sample rate, is skipped (at 22 050 Hz: the 12, 14 and 16 kHz bands). Filter state below 1e-15 is
  flushed to 0 after each block (denormals).
- `publish()` computes the coefficients on the calling thread and hands them over through a triple
  buffer: the writer owns slot `back`, the reader owns slot `front`, and the atomic `middle` holds
  the third slot's index with bit 2 (`kDirty`) set when it is newer than `front`. `publish()`
  exchanges `back` into `middle`; `process()` swaps `middle` into `front` at the start of a block
  when it is dirty. Neither side waits; several publishes between two blocks leave only the last.
- `setSampleRate()` recomputes the last published settings at the new rate and swaps them into
  `front` itself, acting as writer and reader at once. The engine calls it once, in `init()`,
  before the device is ever started; the rate is fixed for the device's lifetime. Settings
  published before `init()` survive it.
- `ResponseDb()` evaluates the magnitude of the product of the float coefficients at `hz`; 0 when
  disabled. Only `dsp_test` calls it.

**Traps:**
- `publish()` is single-writer. Two threads calling it corrupt `back`.
- There is no limiter: preamp and a band together can boost by 24 dB. Samples leave `process()`
  above 1.0 and are only clamped after the volume, in the engine's callback.
- `enabled = false` makes `process()` return before touching anything, so the filter state of the
  moment is kept and re-enabling resumes from it. A band that becomes flat, instead, has its state
  zeroed at once and restarts from rest when it becomes active again.
- The filter state is not reset by seeks or track changes.

## `eq_presets.h` — presets and `.eqf`

```cpp
struct EqPreset {
    QString name;
    EqSettings settings;
};

double EqfToDb(int value);   // Winamp scale 1..64 (clamped) to dB; 33 is exactly 0 dB
int DbToEqf(double db);      // dB to 1..64, rounded to nearest

QList<EqPreset> ParseEqf(const QByteArray& data);   // throws Audio::Error
QByteArray WriteEqf(const QList<EqPreset>& presets);

QList<EqPreset> BuiltinEqPresets();   // 17 presets, all with preamp 0 dB
```

- The scale: dB = (value − 1) / 63 × 24 − 12, so 1 is −12 dB and 64 is +12 dB, except 33,
  Winamp's centre notch, which is exactly 0 dB (the line gives +0.19 dB). `DbToEqf(0)` is 33.
- `BuiltinEqPresets()` is a port of webamp's `presets/builtin.json` (MIT), stored in the 1..64
  scale: Classical, Club, Dance, Laptop speakers/headphones, Large hall, Party, Pop, Reggae, Rock,
  Soft, Ska, Full Bass, Soft Rock, Full Treble, Full Bass & Treble, Live, Techno.
- `WriteEqf()` writes the layout below: the header, then one 268-byte record per preset, nothing
  after. A name is written in the local 8-bit encoding on Windows when it survives the round trip
  (that is the ANSI code page Winamp used), otherwise in UTF-8, cut to 256 bytes so the 257-byte
  field always ends with a NUL; a cut in UTF-8 never splits a character.
- `ParseEqf()` reads names as UTF-8 when they are valid UTF-8, otherwise as the local 8-bit
  encoding on Windows and as Latin-1 elsewhere, and rounds every value to 0.1 dB.

**Traps:**
- A round trip is lossy: 64 levels over 24 dB are steps of 0.38 dB (`eqfRoundTripKeepsNamesAndLevels` allows 0.25).
- `BuiltinEqPresets()` values are not rounded to 0.1 dB, parsed ones are (value 20 is −4.76 dB
  built in, −4.8 dB after a parse).
- A Winamp preset named in cp1251 reads as Latin-1 mojibake outside Windows.
- The UTF-8 cut may drop one whole character more than needed.
- `ParseEqf()` does not bound its input; the caller does (see [Formats read](#formats-read)).

## `vis_tap.h` — `VisTap`

```cpp
struct VisReadResult {
    uint32_t frames = 0;
    uint32_t cursor = 0;
};

class VisTap {
public:
    static constexpr uint32_t kSize = 4096;   // frames; a power of two

    // Audio thread: interleaved stereo frames.
    void write(std::span<const float> stereoFrames);
    // UI thread; at most kSize samples per channel.
    void read(std::span<float> left, std::span<float> right) const;
    // UI thread.
    VisReadResult readNew(uint32_t cursor, std::span<float> stereo) const;
    uint32_t position() const;                                   // frames written, wraps at 2^32
    void clear();
};
```

- Two arrays of 4096 floats (left, right) and a free-running `std::atomic<uint32_t>` frame counter;
  slot = counter & 4095. `write()` stores the samples, then the counter (release); readers load the
  counter (acquire) and copy. Nothing is locked, and the arrays never move.
- `read()` copies the latest min(`left.size()`, `right.size()`, `kSize`) frames, oldest first.
  `readNew()` copies the frames written since `cursor`, at most `kSize` and at most `stereo.size() /
  2` (the newest ones), interleaved into `stereo`, and returns their number in `frames` and the
  current counter in `cursor`, the value to pass next time. The unsigned difference `end - cursor`
  stays correct across the wrap at 2^32.
- In the engine the tap receives what the callback took from the ring, after the equalizer and
  before volume, balance and clamping. `AudioEngine::readVisSamples()` and `readNewVisSamples()`
  pass through to `read()` and `readNew()`;
  `visCursor()` is `position()`. A reader that wants every sample once (Milkdrop) starts its cursor
  from `visCursor()`, so it does not replay what played while it was off.

**Traps:**
- Readers race with the writer on plain floats: a copy can mix old and new samples. That is a
  data race in C++ terms (ThreadSanitizer can flag it); it cannot crash because the arrays never
  move.
- One callback can write more than 4096 frames (a 100 ms period at 48 kHz is 4800); only the last
  4096 survive. A `readNew()` reader that falls more than 4096 frames behind loses the older ones
  silently.
- The counter stops during pause, underruns, pending seeks and after a halted boundary: `read()`
  keeps returning the last frames, `readNew()` returns 0 frames.
- Samples can exceed ±1 (equalizer boost) and do not follow the volume.
- `clear()` is not safe while `write()` runs, and nothing calls it.

## `error.h`, `miniaudio_impl.c`

`error.h` declares `Audio::Error` (see [Errors](#errors)). `miniaudio_impl.c` is the only place
`MINIAUDIO_IMPLEMENTATION` is defined; including `miniaudio.h` anywhere else gives declarations
only.

## Formats read

### Winamp EQ presets: `.eqf` and `.q1`

A `.q1` library has the same layout as an `.eqf`, with more than one record. Offsets in bytes:

| Offset | Size | Content |
|---|---|---|
| 0 | 27 | ASCII `Winamp EQ library file v1.1`, no terminator |
| 27 | 1 | `0x1A` (^Z) |
| 28 | 3 | ASCII `!--` |
| 31 + 268·k | 257 | name of preset k, NUL-padded; it ends at the first NUL, or is all 257 bytes when there is none |
| 31 + 268·k + 257 | 10 | bands 60 Hz … 16 kHz in `kEqBandHz` order, one byte each |
| 31 + 268·k + 267 | 1 | preamp |

Each value byte `b` stores 64 − value, value 1..64: `b = 0` is +12 dB, `b = 31` is 0 dB, `b = 63`
is −12 dB. The format is the one webamp's `winamp-eqf` reads.

`ParseEqf()` throws `Audio::Error` when:
- the data does not start with the 27 header characters, or is shorter than 31 bytes:
  `not a Winamp EQ file: <n> bytes that do not start with "Winamp EQ library file v1.1"`;
- no complete 268-byte record follows offset 31:
  `Winamp EQ file of <n> bytes holds no preset (one takes 268 bytes after the 31-byte header)`.

It does not check bytes 27–30, ignores a trailing partial record, and clamps value bytes instead of
refusing them: any byte from 64 to 255 reads as −12 dB. It returns ⌊(size − 31) / 268⌋ presets and
does not bound the input; the equalizer window refuses files over 1 MiB before reading them
([src/ui](../ui/README.md)), which allows at most 3912 presets.

## Errors

`Audio::Error` (`std::runtime_error`, no children) is the module's only exception and only
`ParseEqf()` throws it, for the two cases above. The equalizer window catches it and shows `what()`
in the status line.

Everything else is data:
- `init()` returns `InitResult{ok, message}`; see [Output
  device](#output-device-and-qiyaa_audio_backend).
- `beginStream()` without a device returns 0, sets `Stopped` and emits
  `errorOccurred("no audio output device")`.
- A first stream that cannot be decoded: `poll()` calls `stop()` and emits
  `errorOccurred("cannot decode the audio stream: <n> bytes received, not mp3, flac or wav")`.
- A queued stream that cannot be decoded is dropped silently: `queuedStream()` returns 0 and the
  current track ends with `trackFinished`.
- A failed download (`failData()`) is not an error for the engine: the stream ends where its data
  ends.
- `queueStream()` and `playQueuedNow()` return 0, `seek()` returns false, when they cannot act.
- An unknown `QIYAA_AUDIO_BACKEND` is a `qWarning`, not a failure.
- miniaudio results inside the decoder thread (seek, read errors) are not reported; a read error
  ends the source like its end does.

The device callback and the decoder thread throw nothing of their own. Nothing catches in the
decoder thread, so an allocation failure there (`std::bad_alloc`) would end the process.

## Not here

- Which track plays next, when the next one is queued (`maybePreload`), the downloads that feed
  `appendData()`, the messages for failed downloads, and the timer that calls `poll()`:
  `Player` in [src/core](../core/README.md).
- Resolving a track id into a download link: [src/yandex](../yandex/README.md).
- Spectrum, oscilloscope and Milkdrop, which read the tap through `readVisSamples()` and
  `readNewVisSamples()`: [src/vis](../vis/README.md) and the main and Milkdrop windows in
  [src/ui](../ui/README.md).
- The equalizer window, its sliders and preset menu, loading and saving `.eqf` files from disk:
  [src/ui](../ui/README.md).
- Calling `init()` (skipped in screenshot mode), saving the equalizer settings, and streaming a
  local file into the engine (`StreamLocalFile` in `offline_sources.cpp`, 64 KiB every 20 ms):
  [src/app](../app/README.md).
- miniaudio itself: `contrib/miniaudio/`, built by the top-level `CMakeLists.txt`. The user-facing
  description of `QIYAA_AUDIO_BACKEND` is in [docs/cli.md](../../docs/cli.md).
