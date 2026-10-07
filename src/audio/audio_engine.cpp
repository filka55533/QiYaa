#include "audio/audio_engine.h"

#include "audio/equalizer.h"
#include "audio/vis_tap.h"

#include <QStringList>
#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Audio {

namespace {

constexpr ma_uint32 kChannels = 2;
constexpr ma_uint32 kRingSeconds = 2;

}  // namespace

class StreamBuffer {
public:
    void append(const char* data, size_t byteCount) {
        {
            std::lock_guard lock(mutex);
            bytes.insert(bytes.end(), data, data + byteCount);
        }
        wakeUp.notify_all();
    }
    void finish(bool failed) {
        {
            std::lock_guard lock(mutex);
            isFinished = true;
            endedWithError = failed;
        }
        wakeUp.notify_all();
    }
    void interrupt() {
        {
            std::lock_guard lock(mutex);
            interrupted = true;
        }
        wakeUp.notify_all();
    }
    void resume() {
        std::lock_guard lock(mutex);
        interrupted = false;
    }
    void rewind() {
        std::lock_guard lock(mutex);
        readOffset = 0;
    }
    size_t size() const {
        std::lock_guard lock(mutex);
        return bytes.size();
    }
    bool finished() const {
        std::lock_guard lock(mutex);
        return isFinished;
    }

    ma_result read(void* destination, size_t bytesToRead, size_t* bytesRead) {
        std::unique_lock lock(mutex);
        wakeUp.wait(lock, [&] { return interrupted || isFinished || bytes.size() > readOffset; });
        *bytesRead = 0;
        if (interrupted) {
            return MA_CANCELLED;
        }
        const size_t available = bytes.size() - std::min(readOffset, bytes.size());
        const size_t bytesToCopy = std::min(bytesToRead, available);
        if (bytesToCopy == 0) {
            return MA_AT_END;
        }
        std::memcpy(destination, bytes.data() + readOffset, bytesToCopy);
        readOffset += bytesToCopy;
        *bytesRead = bytesToCopy;
        return MA_SUCCESS;
    }

    ma_result seek(ma_int64 offset, ma_seek_origin origin) {
        std::unique_lock lock(mutex);
        ma_int64 target = 0;
        switch (origin) {
            case ma_seek_origin_start: target = offset; break;
            case ma_seek_origin_current: target = static_cast<ma_int64>(readOffset) + offset; break;
            case ma_seek_origin_end:
                // Size is unknown until the download completes; don't stall streaming for it.
                if (!isFinished) {
                    return MA_BAD_SEEK;
                }
                target = static_cast<ma_int64>(bytes.size()) + offset;
                break;
        }
        if (target < 0) {
            return MA_BAD_SEEK;
        }
        wakeUp.wait(lock, [&] {
            return interrupted || isFinished || static_cast<ma_int64>(bytes.size()) >= target;
        });
        if (interrupted) {
            return MA_CANCELLED;
        }
        if (target > static_cast<ma_int64>(bytes.size())) {
            return MA_BAD_SEEK;
        }
        readOffset = static_cast<size_t>(target);
        return MA_SUCCESS;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable wakeUp;
    std::vector<char> bytes;
    size_t readOffset = 0;
    bool isFinished = false;
    bool endedWithError = false;
    bool interrupted = false;
};

struct AudioEngine::Implementation {
    ma_context context{};
    bool contextReady = false;
    ma_device device{};
    bool deviceReady = false;
    ma_uint32 sampleRate = 44'100;
    ma_pcm_rb ring{};
    bool ringReady = false;

    std::thread decoderThread;
    std::atomic<bool> stopDecoder{false};

    std::atomic<bool> outputEnabled{false};
    std::atomic<float> gainLeft{1.0f};
    std::atomic<float> gainRight{1.0f};
    std::atomic<ma_uint64> framesPlayed{0};
    EqualizerDsp eq;
    VisTap visTap;
    std::atomic<ma_int64> frameOffset{0};

    std::atomic<bool> decoderStarted{false};
    std::atomic<bool> decoderDone{false};
    std::atomic<bool> decoderFailed{false};
    std::atomic<ma_int64> seekRequest{-1};
    std::atomic<int> seekEpoch{0};
    std::atomic<bool> finishedReported{false};

    std::atomic<int> decoderEpoch{0};
    std::atomic<int> uiEpoch{0};
    std::atomic<ma_uint64> boundaryFrame{0};
    std::atomic<bool> haltAtBoundary{false};
    std::atomic<int> nextRate{0};
    std::atomic<int> nextChannels{0};
    std::atomic<TStreamId> failedQueuedId{0};

    // Guarded by queueMutex.
    std::mutex queueMutex;
    std::shared_ptr<StreamBuffer> queued;
    TStreamId queuedId = 0;
    TStreamId chainingId = 0;
    bool dropChaining = false;
    TStreamId chainedId = 0;
    std::vector<std::shared_ptr<StreamBuffer>> decoderHeld;

    static void
    DataCallback(ma_device* outputDevice, void* output, const void*, ma_uint32 frameCount) {
        auto* self = static_cast<Implementation*>(outputDevice->pUserData);
        auto* outputSamples = static_cast<float*>(output);
        ma_uint32 written = 0;
        // While a seek is pending the decoder thread owns the ring (see seek()).
        if (self->outputEnabled.load(std::memory_order_acquire)
            && self->seekRequest.load(std::memory_order_acquire) < 0) {
            ma_uint32 limit = frameCount;
            if (self->haltAtBoundary.load(std::memory_order_acquire)
                && self->decoderEpoch.load(std::memory_order_acquire)
                    > self->uiEpoch.load(std::memory_order_acquire)) {
                const ma_uint64 played = self->framesPlayed.load(std::memory_order_relaxed);
                const ma_uint64 boundary = self->boundaryFrame.load(std::memory_order_acquire);
                limit = played >= boundary
                    ? 0
                    : static_cast<ma_uint32>(std::min<ma_uint64>(frameCount, boundary - played));
            }
            while (written < limit) {
                ma_uint32 regionFrames = limit - written;
                void* ringRegion = nullptr;
                if (ma_pcm_rb_acquire_read(&self->ring, &regionFrames, &ringRegion) != MA_SUCCESS
                    || regionFrames == 0) {
                    break;
                }
                std::memcpy(
                    outputSamples + written * kChannels, ringRegion,
                    regionFrames * kChannels * sizeof(float)
                );
                ma_pcm_rb_commit_read(&self->ring, regionFrames);
                written += regionFrames;
            }
            self->eq.process({outputSamples, written * kChannels});
            self->visTap.write({outputSamples, written * kChannels});
            const float leftGain = self->gainLeft.load(std::memory_order_relaxed);
            const float rightGain = self->gainRight.load(std::memory_order_relaxed);
            for (ma_uint32 i = 0; i < written; ++i) {
                outputSamples[i * 2] = std::clamp(outputSamples[i * 2] * leftGain, -1.0f, 1.0f);
                outputSamples[i * 2 + 1] =
                    std::clamp(outputSamples[i * 2 + 1] * rightGain, -1.0f, 1.0f);
            }
            self->framesPlayed.fetch_add(written, std::memory_order_relaxed);
        }
        if (written < frameCount) {
            std::memset(
                outputSamples + written * kChannels, 0,
                (frameCount - written) * kChannels * sizeof(float)
            );
        }
    }

    static ma_result
    OnRead(ma_decoder* decoder, void* destination, size_t bytesToRead, size_t* bytesRead) {
        return static_cast<StreamBuffer*>(decoder->pUserData)
            ->read(destination, bytesToRead, bytesRead);
    }
    static ma_result OnSeek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) {
        return static_cast<StreamBuffer*>(decoder->pUserData)->seek(offset, origin);
    }

    // Heap-allocated: ma_decoder must not move.
    struct Source {
        std::shared_ptr<StreamBuffer> buffer;
        ma_decoder decoder{};
        int rate = 0;
        int channels = 0;
        bool initialised = false;

        Source() = default;
        Source(const Source&) = delete;
        Source& operator=(const Source&) = delete;
        ~Source() {
            if (initialised) {
                ma_decoder_uninit(&decoder);
            }
        }
    };

    std::unique_ptr<Source> openSource(std::shared_ptr<StreamBuffer> buffer) {
        auto source = std::make_unique<Source>();
        source->buffer = std::move(buffer);
        ma_decoder_config decoderConfig =
            ma_decoder_config_init(ma_format_f32, kChannels, sampleRate);
        decoderConfig.encodingFormat = ma_encoding_format_mp3;
        ma_result result = ma_decoder_init(
            &Implementation::OnRead, &Implementation::OnSeek, source->buffer.get(), &decoderConfig,
            &source->decoder
        );
        if (result != MA_SUCCESS && !stopDecoder) {
            source->buffer->seek(0, ma_seek_origin_start);
            decoderConfig.encodingFormat = ma_encoding_format_unknown;
            result = ma_decoder_init(
                &Implementation::OnRead, &Implementation::OnSeek, source->buffer.get(),
                &decoderConfig, &source->decoder
            );
        }
        if (result != MA_SUCCESS) {
            return nullptr;
        }
        source->initialised = true;
        ma_format format;
        ma_uint32 channels = 0, rate = 0;
        if (ma_data_source_get_data_format(
                source->decoder.pBackend, &format, &channels, &rate, nullptr, 0
            )
            == MA_SUCCESS) {
            source->rate = static_cast<int>(rate);
            source->channels = static_cast<int>(channels);
        }
        return source;
    }

    void decoderMain(
        std::shared_ptr<StreamBuffer> first,
        std::atomic<int>* sourceRate,
        std::atomic<int>* sourceChannelCount
    ) {
        std::unique_ptr<Source> current = openSource(std::move(first));
        if (!current) {
            if (!stopDecoder) {
                decoderFailed = true;
            }
            return;
        }
        *sourceRate = current->rate;
        *sourceChannelCount = current->channels;
        decoderStarted = true;

        std::unique_ptr<Source> tail;
        int epoch = 0;
        ma_uint64 written = 0;  // frames since the last ring reset, the origin of framesPlayed

        // Call with queueMutex held.
        auto publishHeld = [&] {
            decoderHeld.clear();
            if (current) {
                decoderHeld.push_back(current->buffer);
            }
            if (tail) {
                decoderHeld.push_back(tail->buffer);
            }
        };
        {
            std::lock_guard lock(queueMutex);
            publishHeld();
        }

        auto tryChain = [&]() -> bool {
            std::shared_ptr<StreamBuffer> buffer;
            TStreamId id = 0;
            {
                std::lock_guard lock(queueMutex);
                if (!queued || !queued->finished() || finishedReported) {
                    return false;
                }
                buffer = std::move(queued);
                id = std::exchange(queuedId, 0);
                chainingId = id;
                dropChaining = false;
                decoderHeld.push_back(buffer);
            }
            std::unique_ptr<Source> next = openSource(buffer);
            std::lock_guard lock(queueMutex);
            chainingId = 0;
            if (!next || dropChaining || stopDecoder || finishedReported) {
                if (!next && !dropChaining && !stopDecoder) {
                    failedQueuedId = id;
                } else if (next && finishedReported && !dropChaining && !stopDecoder) {
                    // Already reported finished: hand it back for playQueuedNow().
                    buffer->rewind();
                    queued = buffer;
                    queuedId = id;
                }
                dropChaining = false;
                publishHeld();
                return false;
            }
            chainedId = id;
            tail = std::move(current);
            current = std::move(next);
            ++epoch;
            nextRate = current->rate;
            nextChannels = current->channels;
            boundaryFrame.store(written, std::memory_order_release);
            decoderEpoch.store(epoch, std::memory_order_release);
            publishHeld();
            return true;
        };

        std::vector<float> chunk(1024 * kChannels);
        while (!stopDecoder) {
            if (ma_int64 target = seekRequest.load(std::memory_order_acquire); target >= 0) {
                // seekRequest >= 0: the callback leaves the ring to this thread.
                if (tail && seekEpoch.load(std::memory_order_acquire) < epoch) {
                    std::lock_guard lock(queueMutex);
                    if (!haltAtBoundary) {
                        current->buffer->rewind();
                        queued = current->buffer;
                        queuedId = chainedId;
                    }
                    chainedId = 0;
                    haltAtBoundary = false;
                    current = std::move(tail);
                    --epoch;
                    decoderEpoch.store(epoch, std::memory_order_release);
                    publishHeld();
                } else if (tail) {
                    tail.reset();
                    std::lock_guard lock(queueMutex);
                    publishHeld();
                }
                // May block until that part is downloaded.
                ma_decoder_seek_to_pcm_frame(&current->decoder, static_cast<ma_uint64>(target));
                if (stopDecoder) {
                    break;
                }
                ma_pcm_rb_reset(&ring);
                frameOffset = target;
                framesPlayed = 0;
                written = 0;
                decoderDone = false;
                // Fails if a newer seek arrived meanwhile; the loop then serves that one.
                seekRequest.compare_exchange_strong(target, -1, std::memory_order_acq_rel);
                continue;
            }
            if (tail && uiEpoch.load(std::memory_order_acquire) >= epoch) {
                tail.reset();
                std::lock_guard lock(queueMutex);
                publishHeld();
            }
            if (decoderDone) {
                if (!tail && !finishedReported && tryChain()) {
                    decoderDone = false;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            ma_uint32 space = ma_pcm_rb_available_write(&ring);
            if (space < 1024) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            ma_uint64 framesRead = 0;
            const ma_result result =
                ma_decoder_read_pcm_frames(&current->decoder, chunk.data(), 1024, &framesRead);
            ma_uint32 remaining = static_cast<ma_uint32>(framesRead);
            const float* remainingSamples = chunk.data();
            while (remaining > 0) {
                ma_uint32 regionFrames = remaining;
                void* ringRegion = nullptr;
                if (ma_pcm_rb_acquire_write(&ring, &regionFrames, &ringRegion) != MA_SUCCESS
                    || regionFrames == 0) {
                    break;
                }
                std::memcpy(ringRegion, remainingSamples, regionFrames * kChannels * sizeof(float));
                ma_pcm_rb_commit_write(&ring, regionFrames);
                remainingSamples += regionFrames * kChannels;
                remaining -= regionFrames;
                written += regionFrames;
            }
            if (result == MA_AT_END || (result != MA_SUCCESS && framesRead == 0)) {
                if (tail || !tryChain()) {
                    decoderDone = true;
                }
            }
        }
    }

    void stopDecoderThread(const QHash<TStreamId, std::shared_ptr<StreamBuffer>>& streams) {
        stopDecoder = true;
        std::vector<std::shared_ptr<StreamBuffer>> held;
        {
            std::lock_guard lock(queueMutex);
            held = decoderHeld;
        }
        for (const auto& buffer : streams) {
            buffer->interrupt();
        }
        for (const auto& buffer : held) {
            buffer->interrupt();
        }
        if (decoderThread.joinable()) {
            decoderThread.join();
        }
        stopDecoder = false;
        for (const auto& buffer : held) {
            buffer->resume();
        }
        for (const auto& buffer : streams) {
            buffer->resume();
        }
        std::lock_guard lock(queueMutex);
        queued.reset();
        queuedId = chainingId = chainedId = 0;
        dropChaining = false;
        haltAtBoundary = false;
        decoderHeld.clear();
    }
};

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
    , implementation(std::make_unique<Implementation>()) {
    updateGains();
}

AudioEngine::~AudioEngine() {
    implementation->outputEnabled = false;
    implementation->stopDecoderThread(streams);
    if (implementation->deviceReady) {
        ma_device_uninit(&implementation->device);
    }
    if (implementation->contextReady) {
        ma_context_uninit(&implementation->context);
    }
    if (implementation->ringReady) {
        ma_pcm_rb_uninit(&implementation->ring);
    }
}

AudioEngine::InitResult AudioEngine::init() {
    if (implementation->deviceReady) {
        return {true, {}};
    }
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = kChannels;
    deviceConfig.sampleRate = 0;  // device native
    deviceConfig.dataCallback = &Implementation::DataCallback;
    deviceConfig.pUserData = implementation.get();
    deviceConfig.performanceProfile = ma_performance_profile_conservative;
#if defined(__linux__) || defined(__FreeBSD__)
    // No JACK: it spams stderr when no JACK server runs.
    std::vector<ma_backend> candidates = {ma_backend_pulseaudio, ma_backend_alsa, ma_backend_null};
#elif defined(_WIN32)
    std::vector<ma_backend> candidates = {
        ma_backend_wasapi, ma_backend_dsound, ma_backend_winmm, ma_backend_null
    };
#else
    std::vector<ma_backend> candidates = {ma_backend_coreaudio, ma_backend_null};
#endif
    if (QString requestedBackend =
            qEnvironmentVariable("QIYAA_AUDIO_BACKEND").remove(QLatin1Char(' '));
        !requestedBackend.isEmpty()) {
        bool known = false;
        for (int i = 0; i < MA_BACKEND_COUNT && !known; ++i) {
            const auto backend = ma_backend(i);
            if (QString::fromLatin1(ma_get_backend_name(backend))
                    .remove(QLatin1Char(' '))
                    .compare(requestedBackend, Qt::CaseInsensitive)
                == 0) {
                candidates = {backend};
                known = true;
            }
        }
        if (!known) {
            qWarning(
                "QIYAA_AUDIO_BACKEND=%s: no such audio backend, using the default ones",
                qPrintable(requestedBackend)
            );
        }
    }
    // A context is bound to one backend, so try them one by one until a device opens.
    bool opened = false;
    for (ma_backend backend : candidates) {
        if (ma_context_init(&backend, 1, nullptr, &implementation->context) != MA_SUCCESS) {
            continue;
        }
        if (ma_device_init(&implementation->context, &deviceConfig, &implementation->device)
            == MA_SUCCESS) {
            implementation->contextReady = true;
            opened = true;
            break;
        }
        ma_context_uninit(&implementation->context);
    }
    if (!opened) {
        QStringList tried;
        for (ma_backend backend : candidates) {
            tried << QString::fromLatin1(ma_get_backend_name(backend));
        }
        return {
            false, QStringLiteral("no audio output device opens (tried %1)").arg(tried.join(u", "))
        };
    }
    implementation->deviceReady = true;
    implementation->sampleRate = implementation->device.sampleRate;
    implementation->eq.setSampleRate(implementation->sampleRate);
    if (ma_pcm_rb_init(
            ma_format_f32, kChannels, implementation->sampleRate * kRingSeconds, nullptr, nullptr,
            &implementation->ring
        )
        != MA_SUCCESS) {
        return {
            false,
            QStringLiteral("cannot allocate a %1-second ring buffer at %2 Hz")
                .arg(kRingSeconds)
                .arg(implementation->sampleRate)
        };
    }
    implementation->ringReady = true;
    return {true, {}};
}

QString AudioEngine::backendName() const {
    if (!implementation->deviceReady) {
        return QStringLiteral("none");
    }
    return QString::fromLatin1(ma_get_backend_name(implementation->device.pContext->backend));
}

void AudioEngine::dropStreams() {
    implementation->outputEnabled.store(false, std::memory_order_release);
    implementation->stopDecoderThread(streams);
    streams.clear();
    currentId = queuedId = 0;
}

void AudioEngine::startDecoder(double startSeconds) {
    // Stopping the device guarantees the callback isn't inside the ring right now.
    if (implementation->deviceReady && ma_device_is_started(&implementation->device)) {
        ma_device_stop(&implementation->device);
    }
    ma_pcm_rb_reset(&implementation->ring);
    if (implementation->deviceReady) {
        ma_device_start(&implementation->device);
    }

    // A start position is a seek the decoder serves before any audio reaches the ring.
    const ma_int64 startFrame =
        startSeconds > 0 ? static_cast<ma_int64>(startSeconds * implementation->sampleRate) : -1;
    implementation->framesPlayed = 0;
    implementation->frameOffset = std::max<ma_int64>(0, startFrame);
    implementation->decoderStarted = false;
    implementation->decoderDone = false;
    implementation->decoderFailed = false;
    implementation->finishedReported = false;
    implementation->seekRequest = startFrame;
    implementation->decoderEpoch = 0;
    implementation->uiEpoch = 0;
    implementation->failedQueuedId = 0;
    sourceRate = 0;
    sourceChannelCount = 0;

    implementation->decoderThread = std::thread(
        &Implementation::decoderMain, implementation.get(), streams.value(currentId), &sourceRate,
        &sourceChannelCount
    );
    implementation->outputEnabled.store(true, std::memory_order_release);
    setState(State::Buffering);
}

AudioEngine::TStreamId AudioEngine::beginStream(double startSeconds) {
    dropStreams();
    if (!implementation->ringReady) {
        setState(State::Stopped);
        Q_EMIT errorOccurred(QStringLiteral("no audio output device"));
        return 0;
    }
    currentId = ++lastId;
    streams.insert(currentId, std::make_shared<StreamBuffer>());
    startDecoder(startSeconds);
    return currentId;
}

AudioEngine::TStreamId AudioEngine::queueStream() {
    clearQueued();
    if (!implementation->decoderThread.joinable()) {
        return 0;
    }
    queuedId = ++lastId;
    auto buffer = std::make_shared<StreamBuffer>();
    streams.insert(queuedId, buffer);
    std::lock_guard lock(implementation->queueMutex);
    implementation->queued = std::move(buffer);
    implementation->queuedId = queuedId;
    return queuedId;
}

void AudioEngine::clearQueued() {
    const TStreamId id = std::exchange(queuedId, 0);
    if (!id) {
        return;
    }
    std::shared_ptr<StreamBuffer> buffer = streams.take(id);
    std::lock_guard lock(implementation->queueMutex);
    if (implementation->queuedId == id) {
        implementation->queued.reset();
        implementation->queuedId = 0;
    } else if (implementation->chainingId == id) {
        implementation->dropChaining = true;
        if (buffer) {
            buffer->interrupt();
        }
    } else if (implementation->chainedId == id) {
        implementation->haltAtBoundary = true;
        if (buffer) {
            buffer->interrupt();
        }
    }
}

AudioEngine::TStreamId AudioEngine::queuedStream() const {
    return queuedId && implementation->failedQueuedId.load() != queuedId ? queuedId : 0;
}

AudioEngine::TStreamId AudioEngine::playQueuedNow() {
    const TStreamId id = queuedStream();
    if (!id) {
        return 0;
    }
    std::shared_ptr<StreamBuffer> buffer = streams.value(id);
    implementation->outputEnabled.store(false, std::memory_order_release);
    implementation->stopDecoderThread(streams);
    streams.clear();
    buffer->rewind();
    streams.insert(id, buffer);
    currentId = id;
    queuedId = 0;
    startDecoder(0);
    return id;
}

void AudioEngine::appendData(TStreamId stream, const QByteArray& bytes) {
    if (const auto buffer = streams.value(stream)) {
        buffer->append(bytes.constData(), static_cast<size_t>(bytes.size()));
    }
}

void AudioEngine::finishData(TStreamId stream) {
    if (const auto buffer = streams.value(stream)) {
        buffer->finish(false);
    }
}

void AudioEngine::failData(TStreamId stream) {
    if (const auto buffer = streams.value(stream)) {
        buffer->finish(true);
    }
}

void AudioEngine::pause() {
    if (currentState != State::Playing && currentState != State::Buffering) {
        return;
    }
    if (implementation->deviceReady) {
        ma_device_stop(&implementation->device);
    }
    setState(State::Paused);
}

void AudioEngine::resume() {
    if (currentState != State::Paused) {
        return;
    }
    if (implementation->deviceReady) {
        ma_device_start(&implementation->device);
    }
    setState(implementation->decoderStarted ? State::Playing : State::Buffering);
}

void AudioEngine::stop() {
    dropStreams();
    if (implementation->deviceReady && ma_device_is_started(&implementation->device)) {
        ma_device_stop(&implementation->device);
    }
    implementation->framesPlayed = 0;
    implementation->frameOffset = 0;
    setState(State::Stopped);
}

bool AudioEngine::seek(double seconds) {
    if (!implementation->decoderThread.joinable() || !implementation->decoderStarted
        || seconds < 0) {
        return false;
    }
    // ma_device_stop() waits for a running callback; after the restart the callback sees
    // seekRequest >= 0 and leaves the ring alone until the decoder thread clears it.
    const bool wasRunning =
        implementation->deviceReady && ma_device_is_started(&implementation->device);
    if (wasRunning) {
        ma_device_stop(&implementation->device);
    }
    // The track the UI shows; the decoder may already be in the next one.
    implementation->seekEpoch.store(implementation->uiEpoch.load(), std::memory_order_release);
    implementation->seekRequest.store(
        static_cast<ma_int64>(seconds * implementation->sampleRate), std::memory_order_release
    );
    if (wasRunning) {
        ma_device_start(&implementation->device);
    }
    return true;
}

bool AudioEngine::decoderStarted() const {
    return implementation->decoderStarted.load();
}

double AudioEngine::positionSeconds() const {
    if (implementation->sampleRate == 0) {
        return 0;
    }
    const ma_int64 frames = implementation->frameOffset.load()
        + static_cast<ma_int64>(implementation->framesPlayed.load());
    return static_cast<double>(std::max<ma_int64>(0, frames)) / implementation->sampleRate;
}

void AudioEngine::setEqualizer(const EqSettings& settings) {
    implementation->eq.publish(settings);
}

void AudioEngine::readVisSamples(std::span<float> left, std::span<float> right) const {
    implementation->visTap.read(left, right);
}

VisReadResult AudioEngine::readNewVisSamples(uint32_t cursor, std::span<float> stereo) const {
    return implementation->visTap.readNew(cursor, stereo);
}

uint32_t AudioEngine::visCursor() const {
    return implementation->visTap.position();
}

int AudioEngine::outputSampleRate() const {
    return static_cast<int>(implementation->sampleRate);
}

void AudioEngine::setVolume(int percent) {
    volumePercent = std::clamp(percent, 0, 100);
    updateGains();
}

void AudioEngine::setBalance(int balance) {
    balancePercent = std::clamp(balance, -100, 100);
    updateGains();
}

void AudioEngine::updateGains() {
    const float volume = static_cast<float>(volumePercent) / 100.0f;
    const float gain = volume * volume;
    const float balance = static_cast<float>(balancePercent) / 100.0f;
    implementation->gainLeft = gain * (balance > 0 ? 1.0f - balance : 1.0f);
    implementation->gainRight = gain * (balance < 0 ? 1.0f + balance : 1.0f);
}

void AudioEngine::poll() {
    if (implementation->decoderFailed.exchange(false)) {
        const TStreamId failedId = currentId;
        const std::shared_ptr<StreamBuffer> buffer = streams.value(failedId);
        const qulonglong received = buffer ? buffer->size() : 0;
        stop();
        Q_EMIT errorOccurred(
            QStringLiteral("cannot decode the audio stream: %1 bytes received, not mp3, flac or wav"
            )
                .arg(received)
        );
        Q_EMIT streamUndecodable(failedId);
        return;
    }
    if (const TStreamId failedId = implementation->failedQueuedId.exchange(0);
        failedId && failedId == queuedId) {
        streams.remove(failedId);
        queuedId = 0;
    }
    const int chainedEpoch = implementation->decoderEpoch.load(std::memory_order_acquire);
    if (chainedEpoch > implementation->uiEpoch.load() && implementation->seekRequest.load() < 0
        && !implementation->finishedReported
        && implementation->framesPlayed.load()
            >= implementation->boundaryFrame.load(std::memory_order_acquire)) {
        if (implementation->haltAtBoundary) {
            {
                std::lock_guard lock(implementation->queueMutex);
                implementation->finishedReported = true;
            }
            Q_EMIT trackFinished();
            return;
        }
        {
            std::lock_guard lock(implementation->queueMutex);
            implementation->chainedId = 0;
        }
        implementation->frameOffset = -static_cast<ma_int64>(implementation->boundaryFrame.load());
        implementation->uiEpoch.store(chainedEpoch, std::memory_order_release);
        streams.remove(currentId);
        currentId = std::exchange(queuedId, 0);
        sourceRate = implementation->nextRate.load();
        sourceChannelCount = implementation->nextChannels.load();
        Q_EMIT trackAdvanced();
        return;
    }
    if (currentState == State::Buffering && implementation->decoderStarted
        && ma_pcm_rb_available_read(&implementation->ring) > 0) {
        setState(State::Playing);
    }
    if (currentState == State::Playing && implementation->decoderDone
        && ma_pcm_rb_available_read(&implementation->ring) == 0
        && implementation->seekRequest.load() < 0 && !implementation->finishedReported) {
        {
            // Under the lock: a chain in progress re-checks finishedReported before committing.
            std::lock_guard lock(implementation->queueMutex);
            if (implementation->decoderEpoch.load() > implementation->uiEpoch.load()) {
                return;
            }
            implementation->finishedReported = true;
        }
        Q_EMIT trackFinished();
    }
}

void AudioEngine::setState(State state) {
    if (currentState == state) {
        return;
    }
    currentState = state;
    Q_EMIT stateChanged(state);
}

}  // namespace Audio
