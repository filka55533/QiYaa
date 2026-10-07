#include "core/player.h"

#include "yandex/api_client.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <utility>

namespace Core {

using Audio::AudioEngine;
using Yandex::Track;

namespace {
constexpr int kLoadMoreWhenLeft = 2;
constexpr int kDownloadTimeoutMs = 30'000;

QString ShuffleOffInWaveText() {
    return QCoreApplication::translate("Core::Player", "Shuffle does not apply to a vibe");
}

bool IsHttpError(const QNetworkReply& reply) {
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400;
}
}  // namespace

Player::Player(Yandex::Library* library, Audio::AudioEngine* engine, QObject* parent)
    : QObject(parent)
    , yandexLibrary(library)
    , audioEngine(engine) {
    connect(audioEngine, &Audio::AudioEngine::trackFinished, this, [this] {
        if (streamFailure == FailureKind::Network && streamId) {
            // ERR-03: the download broke and the audio that arrived has run out. The track
            // stays open and waits for the network.
            return handleFailure(FailureKind::Network, {});
        }
        if (openTrack && openTrackEvents) {
            openTrackEvents(
                streamFailure ? TrackEvent::Skipped : TrackEvent::Finished, *openTrack,
                accumulatePlayedSeconds()
            );
        }
        openTrack.reset();
        if (repeatEnabled && queuedTracks.size() == 1 && !loadMore) {  // WAVE-12: a wave waits
            return playIndex(playingIndex);
        }
        next();
    });
    connect(audioEngine, &Audio::AudioEngine::trackAdvanced, this, [this] {
        if (openTrack && openTrackEvents) {
            openTrackEvents(
                streamFailure ? TrackEvent::Skipped : TrackEvent::Finished, *openTrack,
                accumulatePlayedSeconds()
            );
        }
        openTrack.reset();
        if (!preload || preload->stream != audioEngine->currentStream()
            || preload->index >= queuedTracks.size()) {
            const int nextIndex = pickNext();
            nextIndex >= 0 ? playIndex(nextIndex) : stop();
            return;
        }
        const Preload upcoming = *std::exchange(preload, std::nullopt);
        ++generation;
        download = upcoming.reply;
        currentLinkUrl = upcoming.link;
        streamId = upcoming.stream;
        playingIndex = upcoming.index;
        navigator->select(playingIndex);
        currentDownloaded = upcoming.downloadDone;
        streamFailure.reset();
        currentBytes = 1;  // a preload that chained has audio
        waitingForMore = false;
        Q_EMIT currentTrackChanged();
        maybeLoadMore();
        trackStarted(queuedTracks[playingIndex], upcoming.bitrate);
    });
    connect(audioEngine, &Audio::AudioEngine::errorOccurred, this, [this](const QString& message) {
        Q_EMIT statusMessage(tr("Audio error: %1").arg(message));
    });
    connect(audioEngine, &Audio::AudioEngine::streamUndecodable, this, [this](TStreamId stream) {
        if (!stream || stream != streamId) {
            return;
        }
        // Cut short by the network, the bytes may simply be too few; otherwise the track is
        // broken (ERR-04).
        handleFailure(
            streamFailure == FailureKind::Network ? FailureKind::Network : FailureKind::Track,
            tr("the sound could not be decoded")
        );
    });
    retryTimer.setSingleShot(true);
    connect(&retryTimer, &QTimer::timeout, this, &Player::retryAfterNetwork);
    pollTimer.setInterval(100);
    connect(&pollTimer, &QTimer::timeout, this, [this] {
        audioEngine->poll();
        accumulatePlayedSeconds();
        Q_EMIT positionTick();
    });
    connect(
        audioEngine, &Audio::AudioEngine::stateChanged, this,
        [this](Audio::AudioEngine::State state) {
            if (state == Audio::AudioEngine::State::Playing) {
                failuresInRow = 0;  // ERR-06: a track really plays
            }
            if (state == Audio::AudioEngine::State::Stopped) {
                pollTimer.stop();
            } else if (!pollTimer.isActive()) {
                pollTimer.start();
            }
        }
    );
}

void Player::setQueue(
    const QList<Yandex::Track>& tracks,
    const QString& title,
    bool autoplay,
    TLoadMoreCallback more,
    TEventCallback events
) {
    stop();
    reportEvent = std::move(events);
    queuedTracks.clear();
    for (const Yandex::Track& track : tracks) {
        if (track.available) {
            queuedTracks << track;
        }
    }
    titleText = title;
    loadMore = std::move(more);
    queueRules = {};
    loadingMore = false;
    waitingForMore = false;
    failuresInRow = 0;  // ERR-06
    ++queueGeneration;
    playingIndex = queuedTracks.isEmpty() ? -1 : 0;
    resetNavigator();
    Q_EMIT queueReplaced();
    Q_EMIT playlistChanged();
    Q_EMIT currentTrackChanged();
    if (loadMore && shuffleEnabled) {
        Q_EMIT statusMessage(ShuffleOffInWaveText());
    }
    if (autoplay && playingIndex >= 0) {
        playIndex(0);
    }
}

void Player::appendTracks(const QList<Yandex::Track>& tracks) {
    bool added = false;
    for (const Yandex::Track& track : tracks) {
        if (track.available) {
            queuedTracks << track;
            added = true;
        }
    }
    if (!added) {
        return;
    }
    if (playingIndex < 0) {
        playingIndex = 0;
    }
    resetNavigator();
    Q_EMIT playlistChanged();
    refreshPreload();
}

void Player::insertTracks(int index, const QList<Yandex::Track>& tracks) {
    QList<Yandex::Track> added;
    for (const Yandex::Track& track : tracks) {
        if (track.available) {
            added << track;
        }
    }
    if (added.isEmpty()) {
        return;
    }
    const int at = std::clamp(index, 0, static_cast<int>(queuedTracks.size()));
    for (qsizetype i = 0; i < added.size(); ++i) {
        queuedTracks.insert(at + i, added[i]);
    }
    if (playingIndex < 0) {
        playingIndex = 0;
    } else if (at <= playingIndex) {
        playingIndex += static_cast<int>(added.size());
    }
    resetNavigator();
    Q_EMIT playlistChanged();
    refreshPreload();
}

void Player::changeSource(
    const QString& title,
    TLoadMoreCallback more,
    TEventCallback events,
    QueueRules rules
) {
    titleText = title;
    loadMore = std::move(more);
    reportEvent = std::move(events);
    queueRules = rules;
    loadingMore = false;
    waitingForMore = false;
    ++queueGeneration;
    resetNavigator();
    Q_EMIT playlistChanged();
    Q_EMIT modesChanged();
    refreshPreload();
}

void Player::removeTracks(QList<int> indices) {
    std::sort(indices.begin(), indices.end(), std::greater<>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    bool removedCurrent = false;
    for (int index : indices) {
        if (index < 0 || index >= queuedTracks.size()) {
            continue;
        }
        queuedTracks.removeAt(index);
        if (index == playingIndex) {
            removedCurrent = true;
        } else if (index < playingIndex) {
            --playingIndex;
        }
    }
    if (removedCurrent) {
        stop();
        playingIndex = std::min<int>(playingIndex, static_cast<int>(queuedTracks.size()) - 1);
        Q_EMIT currentTrackChanged();
    }
    if (queuedTracks.isEmpty()) {
        playingIndex = -1;
    }
    resetNavigator();
    Q_EMIT playlistChanged();
    refreshPreload();
}

void Player::clearQueue() {
    newSourceRequest();  // a load still in flight must not refill the cleared list
    setQueue({}, {}, false);
}

const Yandex::Track* Player::currentTrack() const {
    return (playingIndex >= 0 && playingIndex < queuedTracks.size()) ? &queuedTracks[playingIndex]
                                                                     : nullptr;
}

double Player::durationSeconds() const {
    const auto* track = currentTrack();
    return track ? static_cast<double>(track->durationMs) / 1000.0 : 0.0;
}

void Player::play() {
    if (networkWait) {
        networkWait->resume = true;
        return retryAfterNetwork();
    }
    switch (audioEngine->state()) {
        case Audio::AudioEngine::State::Paused: audioEngine->resume(); return;
        case Audio::AudioEngine::State::Playing:
            playIndex(playingIndex);
            return;  // Winamp: Play restarts the track
        case Audio::AudioEngine::State::Buffering: return;
        case Audio::AudioEngine::State::Stopped:
            playIndex(playingIndex < 0 ? 0 : playingIndex);
            return;
    }
}

void Player::pause() {
    if (networkWait) {  // the engine stays parked; this decides whether a retry plays on
        networkWait->resume = !networkWait->resume;
        return;
    }
    if (audioEngine->state() == Audio::AudioEngine::State::Paused) {
        audioEngine->resume();
    } else {
        audioEngine->pause();
    }
}

void Player::closeOpenTrack() {
    if (openTrack && openTrackEvents) {
        openTrackEvents(TrackEvent::Skipped, *openTrack, accumulatePlayedSeconds());
    }
    openTrack.reset();
}

double Player::accumulatePlayedSeconds() {
    const double position = audioEngine->positionSeconds();
    const double step = position - lastPosition;
    // Normal progress between two polls is ~0.1 s; bigger jumps are seeks.
    if (audioEngine->state() == Audio::AudioEngine::State::Playing && step > 0 && step < 1.0) {
        playedSeconds += step;
    }
    lastPosition = position;
    return playedSeconds;
}

void Player::setShuffle(bool on) {
    if (on == shuffleEnabled) {
        return;
    }
    shuffleEnabled = on;
    resetNavigator();
    Q_EMIT modesChanged();
    if (on && loadMore) {
        Q_EMIT statusMessage(ShuffleOffInWaveText());
    }
    refreshPreload();
}

void Player::setShuffleAlgorithm(ShuffleAlgorithm algorithm) {
    if (preferredShuffleAlgorithm == algorithm) {
        return;
    }
    preferredShuffleAlgorithm = algorithm;
    if (shuffleActive()) {
        resetNavigator();
        refreshPreload();
    }
    Q_EMIT modesChanged();
}

void Player::setRepeat(bool on) {
    if (on == repeatEnabled) {
        return;
    }
    repeatEnabled = on;
    Q_EMIT modesChanged();
    refreshPreload();
}

void Player::stop() {
    cancelNetworkWait();
    closeOpenTrack();
    resolvingLink = false;
    waitingForMore = false;
    ++generation;
    cancelPreload();
    abortDownload();
    audioEngine->stop();
    streamId = 0;
}

void Player::resetNavigator() {
    navigator = MakeTrackNavigator(
        shuffleActive() ? std::optional(preferredShuffleAlgorithm) : std::nullopt
    );
    navigator->reset(static_cast<int>(queuedTracks.size()), playingIndex);
}

int Player::pickNext() const {
    return navigator->next(repeatEnabled && !loadMore);
}

void Player::next() {
    if (queuedTracks.isEmpty()) {
        return;
    }
    if (preload) {
        return playIndex(preload->index);
    }
    const int nextIndex = pickNext();
    if (nextIndex >= 0) {
        return playIndex(nextIndex);
    }
    if (loadMore) {
        stop();
        waitingForMore = true;
        maybeLoadMore();
        Q_EMIT statusMessage(tr("Loading more tracks…"));
        return;
    }
    stop();
}

int PreviousTarget(double positionSeconds, int cursor, int size, bool repeat) {
    if (positionSeconds > kPreviousRestartsAfterSeconds) {
        return -1;
    }
    return cursor > 0 ? cursor - 1 : (repeat ? size - 1 : 0);
}

void Player::previous() {
    if (queuedTracks.isEmpty()) {
        return;
    }
    if (queueRules.previousRestartsOnly) {  // HOST-07: a jam never goes back
        if (!seekTo(0)) {
            playIndex(playingIndex);
        }
        return;
    }
    const bool active = audioEngine->state() != Audio::AudioEngine::State::Stopped;
    const int target = active && audioEngine->positionSeconds() > kPreviousRestartsAfterSeconds
        ? -1
        : navigator->previous(repeatEnabled);
    if (target >= 0) {
        playIndex(target);
    } else if (!seekTo(0)) {  // the restart is a seek: no new start, no events
        playIndex(playingIndex);
    }
}

bool Player::seekFraction(double fraction) {
    const double duration = durationSeconds();
    return duration > 0 && seekTo(std::clamp(fraction, 0.0, 1.0) * duration);
}

bool Player::seekTo(double seconds) {
    const double duration = durationSeconds();
    const double target =
        duration > 0 ? std::clamp(seconds, 0.0, duration) : std::max(0.0, seconds);
    if (!audioEngine->seek(target)) {
        return false;
    }
    lastPosition = target;
    Q_EMIT seeked(target);
    return true;
}

void Player::maybeLoadMore() {
    if (!loadMore || loadingMore || queuedTracks.size() - playingIndex > kLoadMoreWhenLeft) {
        return;
    }
    loadingMore = true;
    const quint64 requestGeneration = queueGeneration;
    QPointer<Player> self(this);
    loadMore([self, requestGeneration](const QList<Yandex::Track>& tracks) {
        if (!self || requestGeneration != self->queueGeneration) {
            return;
        }
        self->loadingMore = false;
        const bool wasWaiting = self->waitingForMore;
        self->waitingForMore = false;
        self->appendTracks(tracks);
        if (wasWaiting) {
            if (self->playingIndex + 1 < self->queuedTracks.size()) {
                self->playIndex(self->playingIndex + 1);
            } else {
                self->waitingForMore = true;  // nothing came: still at the end
            }
        }
    });
}

void Player::shutDown() {
    newSourceRequest();  // loads in flight are stale now
    stop();
    isShutDown = true;
}

void Player::playIndex(int index) {
    if (isShutDown || index < 0 || index >= queuedTracks.size()) {
        return;
    }
    waitingForMore = false;
    cancelNetworkWait();
    closeOpenTrack();
    resolvingLink = false;
    const quint64 requestGeneration = ++generation;
    abortDownload();
    playingIndex = index;
    navigator->select(index);
    bitrateKbps = 0;
    currentLinkUrl.clear();
    currentDownloaded = false;
    streamFailure.reset();
    currentBytes = 0;
    const Yandex::Track track = queuedTracks[index];

    if (preload && preload->stream && preload->trackId == track.id
        && preload->stream == audioEngine->queuedStream()) {
        const Preload upcoming = *std::exchange(preload, std::nullopt);
        streamId = audioEngine->playQueuedNow();
        download = upcoming.reply;
        currentLinkUrl = upcoming.link;
        currentDownloaded = upcoming.downloadDone;
        currentBytes = 1;  // the preload's bytes went to its own stream id
        Q_EMIT currentTrackChanged();
        maybeLoadMore();
        trackStarted(track, upcoming.bitrate);
        return;
    }
    cancelPreload();
    streamId = audioEngine->beginStream();
    resolvingLink = true;
    Q_EMIT currentTrackChanged();
    maybeLoadMore();

    yandexLibrary->api()->resolveTrackUrl(
        track.id,
        [this, self = QPointer<Player>(this), requestGeneration,
         track](const Yandex::ResolvedUrl& link, const Yandex::RequestError& error) {
            if (!self || requestGeneration != generation) {
                return;
            }
            resolvingLink = false;
            if (error.isError()) {
                if (!streamId) {  // no audio output: nothing to wait on or to skip to
                    Q_EMIT statusMessage(tr("Cannot get the link: %1").arg(error.text));
                    audioEngine->stop();
                    return;
                }
                return handleFailure(KindOf(error), error.text);
            }
            currentLinkUrl = link.url;
            download = startDownload(link.url, streamId);
            trackStarted(track, link.bitrateKbps);
        }
    );
}

void Player::trackStarted(const Yandex::Track& track, int bitrate) {
    bitrateKbps = bitrate;
    if (queueRules.playReports) {
        yandexLibrary->api()->reportPlayStarted(
            yandexLibrary->account(), track, QUuid::createUuid().toString(QUuid::WithoutBraces)
        );
    }
    openTrack = track;
    openTrackEvents = reportEvent;
    playedSeconds = 0;
    lastPosition = 0;
    if (reportEvent) {
        reportEvent(TrackEvent::Started, track, 0);
    }
    Q_EMIT currentTrackChanged();
    maybePreload();
}

QNetworkReply* Player::startDownload(const QUrl& url, TStreamId stream) {
    QNetworkRequest request(url);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy
    );
    request.setTransferTimeout(kDownloadTimeoutMs);
    QNetworkReply* reply = yandexLibrary->api()->network()->get(request);
    // The engine ignores data for streams it has dropped meanwhile. An error page is not audio.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, stream] {
        const QByteArray bytes = reply->readAll();
        if (!IsHttpError(*reply)) {
            audioEngine->appendData(stream, bytes);
            if (stream == streamId) {
                currentBytes += bytes.size();
            }
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, stream] {
        reply->deleteLater();
        Yandex::RequestError error = Yandex::ClassifyReply(*reply);
        if (error.isError()) {
            error.text = reply->errorString();
            audioEngine->failData(stream);
        } else {
            audioEngine->appendData(stream, reply->readAll());
            audioEngine->finishData(stream);
        }
        downloadFinished(stream, error);
    });
    return reply;
}

void Player::downloadFinished(TStreamId stream, const Yandex::RequestError& error) {
    if (stream && stream == streamId) {
        if (error.isError()) {
            download.clear();  // finished: acting on it must not abort it
            const FailureKind kind = KindOf(error);
            if (currentBytes == 0) {  // no audio arrived: act now
                return handleFailure(kind, error.text);
            }
            // Some audio arrived: it plays out, then the end of the track decides (a network
            // failure waits, ERR-03; anything else ends the track as a skip). Too few bytes to
            // decode end the same way, through streamUndecodable.
            streamFailure = kind;
            if (kind != FailureKind::Network) {
                Q_EMIT statusMessage(tr("Download failed: %1").arg(error.text));
            }
            return;
        }
        currentDownloaded = true;
        maybePreload();
    } else if (preload && preload->stream == stream) {
        if (error.isError()) {
            // Not chained half-done: the track is fetched again when its turn comes.
            cancelPreload();
            return;
        }
        preload->downloadDone = true;
    }
}

void Player::handleFailure(FailureKind kind, const QString& text) {
    const FailureAction action =
        DecideOnFailure(kind, failuresInRow, pickNext() >= 0, static_cast<bool>(loadMore));
    switch (action) {
        case FailureAction::WaitForNetwork: return waitForNetwork();
        case FailureAction::Next:  // ERR-04; in a wave at its end next() waits for more (ERR-07)
            ++failuresInRow;
            Q_EMIT statusMessage(tr("The track does not play: %1").arg(text));
            return next();
        case FailureAction::Stop:
            if (kind == FailureKind::Auth) {  // ERR-08: not the track's fault
                Q_EMIT statusMessage(tr("Access error: %1").arg(text));
            } else {  // ERR-07: no track follows
                ++failuresInRow;
                Q_EMIT statusMessage(tr("The track does not play: %1").arg(text));
            }
            return stop();
        case FailureAction::StopAfterLimit:  // ERR-05
            failuresInRow = 0;
            Q_EMIT statusMessage(
                tr("Stopped: %n track(s) in a row did not play", nullptr, kMaxTrackFailuresInRow)
            );
            return stop();
    }
}

void Player::waitForNetwork() {
    NetworkWait wait;
    if (networkWait) {
        wait = *networkWait;
        ++wait.attempt;
    } else {
        const AudioEngine::State state = audioEngine->state();
        wait.resume = state != AudioEngine::State::Paused;
        wait.position =
            state == AudioEngine::State::Stopped ? lastPosition : audioEngine->positionSeconds();
        Q_EMIT statusMessage(tr("No network — waiting for it…"));
    }
    ++generation;
    abortDownload();
    cancelPreload();
    // Parked: a new stream at the kept position, paused, so every view shows "paused at X".
    // The track stays open: no skip now and no new start later (ERR-01).
    streamId = audioEngine->beginStream(wait.position);
    streamFailure.reset();
    currentBytes = 0;
    if (!streamId) {
        return stop();
    }
    audioEngine->pause();
    lastPosition = wait.position;
    networkWait = wait;
    retryTimer.stop();
    if (networkOnline != false) {
        retryTimer.start(std::min(retryMaxMs, retryFirstMs << std::min(wait.attempt, 16)));
    }
}

void Player::retryAfterNetwork() {
    if (!networkWait || !currentTrack()) {
        return;
    }
    retryTimer.stop();
    const quint64 requestGeneration = ++generation;
    const Track track = *currentTrack();
    yandexLibrary->api()->resolveTrackUrl(
        track.id,
        [this, self = QPointer<Player>(this), requestGeneration,
         track](const Yandex::ResolvedUrl& link, const Yandex::RequestError& error) {
            if (!self || requestGeneration != generation || !networkWait) {
                return;
            }
            if (error.isError()) {
                return handleFailure(KindOf(error), error.text);
            }
            const NetworkWait wait = *std::exchange(networkWait, std::nullopt);
            currentLinkUrl = link.url;
            download = startDownload(link.url, streamId);
            if (openTrack) {
                bitrateKbps = link.bitrateKbps;
                Q_EMIT currentTrackChanged();
            } else {  // the link had failed before the track ever started
                trackStarted(track, link.bitrateKbps);
            }
            if (wait.resume) {
                audioEngine->resume();
            }
        }
    );
}

void Player::cancelNetworkWait() {
    networkWait.reset();
    retryTimer.stop();
}

void Player::setNetworkOnline(std::optional<bool> online) {
    networkOnline = online;
    if (!networkWait) {
        return;
    }
    if (online == true) {  // back: try at once, and from the shortest delay again
        networkWait->attempt = 0;
        retryAfterNetwork();
    } else if (online == false) {
        retryTimer.stop();  // nothing to try until the network is back
    }
}

void Player::setNetworkRetryDelays(int firstMs, int maxMs) {
    retryFirstMs = std::max(1, firstMs);
    retryMaxMs = std::max(retryFirstMs, maxMs);
}

void Player::abortDownload() {
    if (QNetworkReply* reply = download) {
        download.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

int Player::preloadedIndex() const {
    return preload && preload->stream ? preload->index : -1;
}

void Player::maybePreload() {
    if (preload || isShutDown || !currentDownloaded || !openTrack
        || audioEngine->state() == Audio::AudioEngine::State::Stopped) {
        return;
    }
    const int index = pickNext();
    if (index < 0) {
        return;
    }
    Preload upcoming;
    upcoming.index = index;
    upcoming.trackId = queuedTracks[index].id;
    upcoming.generation = ++preloadGeneration;
    preload = upcoming;
    QPointer<Player> self(this);
    yandexLibrary->api()->resolveTrackUrl(
        upcoming.trackId,
        [self, requestGeneration = upcoming.generation](
            const Yandex::ResolvedUrl& link, const Yandex::RequestError& error
        ) {
            if (!self || !self->preload || self->preload->generation != requestGeneration) {
                return;
            }
            const TStreamId stream = error.isError() ? 0 : self->audioEngine->queueStream();
            if (!stream) {
                self->preload.reset();
                return;
            }
            self->preload->stream = stream;
            self->preload->bitrate = link.bitrateKbps;
            self->preload->link = link.url;
            self->preload->reply = self->startDownload(link.url, stream);
        }
    );
}

void Player::cancelPreload() {
    if (!preload) {
        return;
    }
    const Preload upcoming = *std::exchange(preload, std::nullopt);
    if (QNetworkReply* reply = upcoming.reply) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    if (upcoming.stream && upcoming.stream == audioEngine->queuedStream()) {
        audioEngine->clearQueued();
    }
}

void Player::refreshPreload() {
    if (preload) {
        int index = -1;
        if (const int nextIndex = pickNext();
            nextIndex >= 0 && queuedTracks[nextIndex].id == preload->trackId) {
            index = nextIndex;
        }
        if (index >= 0) {
            preload->index = index;
            return;
        }
        cancelPreload();
    }
    maybePreload();
}

}  // namespace Core
